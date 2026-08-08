"""One-shot LLM extraction for continuous-ops onboarding (structured mission plan)."""

from __future__ import annotations

import json
import logging
import os
import random
import re
import time
from dataclasses import dataclass, replace
from typing import Any

_LOG = logging.getLogger(__name__)

# Transient gateway / overload — same family as ``httpx_client`` retry policy.
_PLANNER_RETRY_HTTP = frozenset({429, 502, 503, 504, 529})


def _env_int(name: str, default: int, lo: int, hi: int) -> int:
    try:
        v = int((os.getenv(name) or str(default)).strip())
    except ValueError:
        return default
    return max(lo, min(hi, v))


def _env_float(name: str, default: float, lo: float, hi: float) -> float:
    try:
        v = float((os.getenv(name) or str(default)).strip())
    except ValueError:
        return default
    return max(lo, min(hi, v))


_PLANNER_MAX_ATTEMPTS = _env_int("CAI_MISSION_PLANNER_MAX_ATTEMPTS", 3, 1, 8)
_PLANNER_READ_TIMEOUT = _env_float("CAI_MISSION_PLANNER_TIMEOUT", 240.0, 30.0, 600.0)

from cai.continuous_ops.rate_plan import compute_base_tick_seconds, min_allowed_tick_seconds, resolve_rate_tier
from cai.util.llm_api_base import resolve_llm_openai_compatible_base


@dataclass
class MissionPlan:
    tasks_markdown: str
    tick_seconds: int | None
    use_tmux: bool | None
    auth_required: bool | None
    estimated_tokens_per_iteration: int
    refined_tick_prompt: str
    tier: str
    #: Optional discrete tasks from the model JSON ``tasks`` key (preferred for summaries).
    structured_tasks: tuple[str, ...] | None = None
    #: ``planner_api`` = JSON from the Alias/CAI chat planner; ``fallback_*`` = local heuristic plan.
    planner_origin: str = "unspecified"
    #: Last HTTP status from the planner ``/chat/completions`` call (if any); set on fallback paths.
    planner_http_status: int | None = None
    #: Short operator-facing reason (HTTP body snippet, timeout, missing key, JSON parse, etc.).
    planner_failure_summary: str | None = None

    @property
    def base_tick(self) -> float:
        return compute_base_tick_seconds(self.estimated_tokens_per_iteration, tier=self.tier)

    @property
    def min_tick(self) -> float:
        return min_allowed_tick_seconds(self.estimated_tokens_per_iteration, tier=self.tier)


def _normalize_assistant_content(raw: Any) -> str:
    """OpenAI-style ``message.content`` may be a string or a list of typed parts (multimodal)."""
    if raw is None:
        return ""
    if isinstance(raw, str):
        return raw.strip()
    if isinstance(raw, list):
        chunks: list[str] = []
        for part in raw:
            if isinstance(part, str):
                chunks.append(part)
            elif isinstance(part, dict):
                tx = part.get("text")
                if isinstance(tx, str):
                    chunks.append(tx)
        return "\n".join(chunks).strip()
    return str(raw).strip()


def _completion_choice_assistant_text(body: dict[str, Any]) -> str:
    """Best-effort assistant string from a ``/chat/completions`` JSON body."""
    try:
        ch0 = (body.get("choices") or [None])[0] or {}
    except (TypeError, IndexError):
        return ""
    if not isinstance(ch0, dict):
        return ""
    msg = ch0.get("message")
    if not isinstance(msg, dict):
        msg = {}
    text = _normalize_assistant_content(msg.get("content"))
    if text:
        return text
    legacy = ch0.get("text")
    if isinstance(legacy, str) and legacy.strip():
        return legacy.strip()
    refusal = msg.get("refusal")
    if refusal:
        _LOG.debug("Mission planner: refusal field present: %s", str(refusal)[:400])
    rc = msg.get("reasoning_content")
    if isinstance(rc, str) and rc.strip():
        _LOG.debug("Mission planner: using reasoning_content as assistant text (non-standard gateway)")
        return rc.strip()
    fr = ch0.get("finish_reason")
    _LOG.debug(
        "Mission planner: empty assistant text; finish_reason=%s message_keys=%s",
        fr,
        sorted(msg.keys()),
    )
    return ""


def _mission_is_short_or_vague(user_text: str) -> bool:
    """Heuristic: user gave a one-liner mission without an explicit multi-item checklist."""
    t = (user_text or "").strip()
    if not t:
        return True
    lines = [ln for ln in t.splitlines() if ln.strip()]
    bulletish = sum(1 for ln in lines if re.match(r"^\s*(?:[-*+]|\d+[\.)])\s+", ln)) >= 2
    if bulletish:
        return False
    if len(lines) >= 3 and len(t) > 140:
        return False
    return len(t) < 170 or len(t.split()) < 20


def _expanded_monitoring_tasks(user_summary: str) -> tuple[str, str, tuple[str, ...]]:
    """Concrete periodic checks when the API planner is unavailable and the mission is vague."""
    core = (user_summary or "").strip() or "host monitoring"
    bullets = (
        f"Regarding «{core}»: capture OS/kernel identity, hostname, and distribution using read-only commands.",
        "Report uptime, load averages, memory pressure, and disk space on paths readable without elevated privileges.",
        "Enumerate listening ports and associated processes visible to the current user (non-intrusive).",
        "Review user-readable logs (e.g. authentication/session) for anomalies or failures without using sudo.",
        "If visible without root, summarize containers/runtimes or user-level services relevant to security posture.",
    )
    md = "\n".join(f"- {b}" for b in bullets)
    refined = (
        f"[Continuous ops — operator goal: {core}]\n"
        "Each tick, run the checklist below and end the reply with [STATUS: OK] or [STATUS: INCIDENT].\n"
        f"{md}"
    )
    return md, refined, bullets


def _apply_local_mission_expansion_if_needed(plan: MissionPlan, user_text: str) -> MissionPlan:
    """When the remote planner failed and the mission is a short phrase, attach a concrete checklist."""
    if plan.planner_origin == "planner_api":
        return plan
    if not _mission_is_short_or_vague(user_text):
        return plan
    md, refined, structured = _expanded_monitoring_tasks(user_text)
    return replace(
        plan,
        tasks_markdown=md,
        refined_tick_prompt=refined,
        structured_tasks=structured,
    )


def _fallback_plan(
    user_text: str,
    *,
    origin: str = "fallback",
    http_status: int | None = None,
    failure_summary: str | None = None,
) -> MissionPlan:
    tier = resolve_rate_tier()
    est = 12_000
    stripped = user_text.strip()
    plan = MissionPlan(
        tasks_markdown=stripped,
        tick_seconds=None,
        use_tmux=None,
        auth_required=None,
        estimated_tokens_per_iteration=est,
        refined_tick_prompt=stripped,
        tier=tier,
        structured_tasks=None,
        planner_origin=origin,
        planner_http_status=http_status,
        planner_failure_summary=failure_summary,
    )
    return _apply_local_mission_expansion_if_needed(plan, stripped)


def parse_task_lines_from_markdown(text: str) -> list[str]:
    """Split *text* into tasks: markdown bullets / ordered lines, else one block."""
    raw = (text or "").strip()
    if not raw:
        return []
    lines = raw.splitlines()
    items: list[str] = []
    bullet_re = re.compile(r"^\s*(?:[-*+]|\d+[\.)])\s+(.+)$")
    for line in lines:
        m = bullet_re.match(line)
        if m:
            items.append(m.group(1).strip())
    if items:
        return [t for t in items if t]
    return [raw]


def normalized_tasks(plan: MissionPlan) -> list[str]:
    """Concrete task strings for UI and summaries."""
    if plan.structured_tasks:
        return [t for t in plan.structured_tasks if str(t).strip()]
    return parse_task_lines_from_markdown(plan.tasks_markdown)


def summary_iteration_tasks(plan: MissionPlan) -> list[str]:
    """Task lines for the final operator summary — pick the richest structured view."""

    def _score(lines: list[str]) -> tuple[int, int]:
        n = len(lines)
        ch = sum(len(x) for x in lines)
        # Prefer real multi-item checklists over one huge paragraph when scores tie on length.
        boosted = n + (10_000 if n >= 2 else 0)
        return (boosted, ch)

    md = parse_task_lines_from_markdown((plan.tasks_markdown or "").strip())
    norm = normalized_tasks(plan)
    # Model JSON often echoes the short user phrase in ``tasks`` while the real checklist lives only
    # in ``tasks_markdown`` bullets — treat that markdown as canonical for the summary.
    if len(md) >= 2 and len(norm) == 1:
        norm = list(md)
    refined = (plan.refined_tick_prompt or "").strip()
    ref = parse_task_lines_from_markdown(refined) if refined else []
    candidates = [c for c in (md, ref, norm) if c]
    if not candidates:
        return ["(not specified)"]
    return max(candidates, key=_score)


def needs_task_collection(plan: MissionPlan) -> bool:
    """True when no actionable periodic task is available (empty prompt, model gap, etc.)."""
    tasks = normalized_tasks(plan)
    if not tasks:
        return True
    if all(len(t.strip()) < 3 for t in tasks):
        return True
    return False


def _coerce_bool(val: Any) -> bool | None:
    if val is None:
        return None
    if isinstance(val, bool):
        return val
    if isinstance(val, str):
        s = val.strip().lower()
        if s in ("true", "yes", "y", "1"):
            return True
        if s in ("false", "no", "n", "0"):
            return False
    return None


def _strip_markdown_json_fence(text: str) -> str:
    """Remove leading ``` / ```json and trailing ``` so ``json.loads`` can run."""
    t = text.strip()
    if not t.startswith("```"):
        return t
    first_nl = t.find("\n")
    if first_nl != -1:
        t = t[first_nl + 1 :]
    t = t.rstrip()
    if t.endswith("```"):
        t = t[: -3].rstrip()
    return t.strip()


def _extract_json_object(text: str) -> dict[str, Any] | None:
    """Parse a single JSON object from model output (raw JSON, fenced block, or prose-wrapped)."""
    raw = (text or "").strip()
    if not raw:
        return None
    candidates: list[str] = []
    seen: set[str] = set()
    for cand in (raw, _strip_markdown_json_fence(raw)):
        if cand and cand not in seen:
            seen.add(cand)
            candidates.append(cand)

    for cand in candidates:
        try:
            data = json.loads(cand)
            if isinstance(data, dict):
                return data
        except json.JSONDecodeError:
            pass
        m = re.search(r"\{[\s\S]*\}\s*$", cand)
        if m:
            try:
                data = json.loads(m.group(0))
                if isinstance(data, dict):
                    return data
            except json.JSONDecodeError:
                pass
        lb, rb = cand.find("{"), cand.rfind("}")
        if lb != -1 and rb > lb:
            chunk = cand[lb : rb + 1]
            try:
                data = json.loads(chunk)
                if isinstance(data, dict):
                    return data
            except json.JSONDecodeError:
                continue
    return None


@dataclass
class _PlannerHttpOutcome:
    """Result of one planner POST series (may include retries)."""

    content: str | None
    http_status: int | None
    error_snippet: str | None
    transport_error: str | None = None


def _retry_after_seconds(resp: Any) -> float | None:
    h = getattr(resp, "headers", None) or {}
    raw = h.get("retry-after") or h.get("Retry-After")
    if not raw:
        return None
    try:
        return float(str(raw).strip())
    except ValueError:
        return None


def _retry_delay_seconds(attempt: int) -> float:
    base = min(30.0, 1.5 * (2**attempt))
    return base + random.uniform(0, 0.75)


def _alias_planner_completion(
    *,
    model: str,
    system: str,
    user: str,
    max_tokens: int,
    temperature: float,
    timeout: float | None = None,
) -> _PlannerHttpOutcome:
    """POST ``/chat/completions`` on the Alias gateway with retries on transient HTTP failures."""
    read_timeout = float(timeout) if timeout is not None else _PLANNER_READ_TIMEOUT
    try:
        import httpx
    except ImportError:
        return _PlannerHttpOutcome(
            content=None,
            http_status=None,
            error_snippet=None,
            transport_error="httpx is not installed",
        )
    api_key = (os.getenv("ALIAS_API_KEY") or "").strip()
    if not api_key:
        return _PlannerHttpOutcome(
            content=None,
            http_status=None,
            error_snippet=None,
            transport_error="ALIAS_API_KEY is not set",
        )
    base = resolve_llm_openai_compatible_base(os.getenv("CAI_MODEL")).rstrip("/")
    url = f"{base}/chat/completions"
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": system},
            {"role": "user", "content": user},
        ],
        "max_tokens": max_tokens,
        "temperature": temperature,
    }
    headers = {"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"}
    timeout_cfg = httpx.Timeout(connect=30.0, read=read_timeout, write=30.0, pool=30.0)

    last_status: int | None = None
    last_snippet: str | None = None
    for attempt in range(_PLANNER_MAX_ATTEMPTS):
        try:
            r = httpx.post(url, headers=headers, json=payload, timeout=timeout_cfg)
        except Exception as exc:
            _LOG.debug("Mission planner request failed (attempt %s/%s): %s", attempt + 1, _PLANNER_MAX_ATTEMPTS, exc)
            if attempt + 1 >= _PLANNER_MAX_ATTEMPTS:
                return _PlannerHttpOutcome(
                    content=None,
                    http_status=None,
                    error_snippet=None,
                    transport_error=str(exc)[:400],
                )
            time.sleep(min(30.0, _retry_delay_seconds(attempt)))
            continue
        last_status = r.status_code
        if r.status_code == 200:
            try:
                data = r.json()
                if not isinstance(data, dict):
                    raise TypeError("completion body is not an object")
                text = _completion_choice_assistant_text(data)
                return _PlannerHttpOutcome(content=text, http_status=200, error_snippet=None, transport_error=None)
            except (KeyError, IndexError, TypeError, ValueError) as exc:
                _LOG.debug("Mission planner: bad JSON envelope from %s: %s", url, exc)
                return _PlannerHttpOutcome(
                    content=None,
                    http_status=200,
                    error_snippet="response missing usable choices[0] assistant text",
                    transport_error=None,
                )

        try:
            last_snippet = (r.text or "")[:500].replace("\n", " ").strip()
        except Exception:
            last_snippet = None
        _LOG.debug(
            "Mission planner HTTP %s from %s (attempt %s/%s) — %s",
            r.status_code,
            url,
            attempt + 1,
            _PLANNER_MAX_ATTEMPTS,
            last_snippet or "(empty body)",
        )
        if r.status_code in _PLANNER_RETRY_HTTP and attempt + 1 < _PLANNER_MAX_ATTEMPTS:
            ra = _retry_after_seconds(r)
            delay = ra if ra is not None else _retry_delay_seconds(attempt)
            time.sleep(delay)
            continue
        return _PlannerHttpOutcome(
            content=None, http_status=last_status, error_snippet=last_snippet, transport_error=None
        )

    return _PlannerHttpOutcome(
        content=None, http_status=last_status, error_snippet=last_snippet, transport_error=None
    )


def parse_mission_with_planner(user_text: str) -> MissionPlan:
    """Call the Alias chat API for a JSON mission plan; fall back on failure.

    Uses ``ALIAS_API_KEY`` and ``CAI_MODEL`` (e.g. ``alias1``, ``alias2-mini``, ``alias3``) — the same
    gateway as the rest of CAI (``CSI_CUSTOM_ENDPOINT`` / ``ALIAS_API_URL`` for qualifying ``CAI_MODEL``,
    else ``OPENAI_API_BASE`` / default).
    """
    tier = resolve_rate_tier()
    model = os.getenv("CAI_MODEL", "alias1")
    system = (
        "You are a scheduling assistant for a cybersecurity AI CLI. "
        "Return ONLY a compact JSON object with keys: "
        "tasks (array of strings, REQUIRED unless the user text is truly empty): each string is ONE concrete "
        "actionable iteration task the worker can run every tick without extra human Q&A. "
        "When the user mission is vague (e.g. \"monitor this host\", \"check security\"), you MUST expand it into "
        "several specific, repeatable checks for that tick loop: e.g. OS/kernel identity, uptime/load, disk space, "
        "listening services (unprivileged), relevant user-readable logs, failed SSH/auth hints if readable without root, "
        "installed package snapshot where unprivileged, container/runtime surface if user can read it — each as its own task line. "
        "When the user already gave a precise checklist, keep tasks aligned to that wording. "
        "tasks_markdown (string): the same tasks as a markdown bullet list (lines starting with '- '). "
        "tick_seconds (integer or null if unspecified), "
        "use_tmux (boolean or null), "
        "auth_required (boolean or null — whether typical steps need sudo/root), "
        "estimated_tokens_per_iteration (integer, total prompt+expected completion tokens), "
        "refined_tick_prompt (string): one self-contained block the worker will receive EVERY tick — it must embed "
        "the full expanded mission (assumptions, scope, and ordered checklist) so each iteration is well-oriented "
        "even without the original short user phrase. "
        "Be conservative with estimated_tokens_per_iteration to avoid API rate limits."
    )
    user = f"User mission:\n{user_text}\n\nRespond with JSON only."
    out = _alias_planner_completion(
        model=model,
        system=system,
        user=user,
        max_tokens=2000,
        temperature=0.2,
        timeout=None,
    )
    if (not out.content) and out.http_status == 200:
        _LOG.debug("Mission planner: empty assistant payload on HTTP 200 — one structured retry")
        out = _alias_planner_completion(
            model=model,
            system=system,
            user=(
                user
                + "\n\nYour last response had no usable assistant text in the API payload. "
                "Reply with ONLY one JSON object (no markdown fences, no commentary) using keys: "
                "tasks (array of strings), tasks_markdown, tick_seconds, use_tmux, auth_required, "
                "estimated_tokens_per_iteration, refined_tick_prompt."
            ),
            max_tokens=2800,
            temperature=0.15,
            timeout=None,
        )
    if out.transport_error:
        _LOG.debug(
            "Mission planner: %s (local fallback for onboarding).",
            (out.transport_error or "")[:220],
        )
        return _fallback_plan(
            user_text,
            origin="fallback_exception",
            failure_summary=out.transport_error,
        )
    if not out.content:
        if out.http_status == 200:
            _LOG.debug("Mission planner: empty assistant content (HTTP 200); local fallback for onboarding.")
        elif out.http_status is not None:
            _LOG.debug(
                "Mission planner: no model reply (HTTP %s); local fallback for onboarding.",
                out.http_status,
            )
        else:
            _LOG.debug("Mission planner: empty model reply; local fallback for onboarding.")
        parts: list[str] = []
        if out.http_status is not None:
            parts.append(f"HTTP {out.http_status}")
        else:
            parts.append("empty assistant reply")
        if out.error_snippet:
            parts.append(out.error_snippet[:280])
        summary = " — ".join(parts) if parts else None
        return _fallback_plan(
            user_text,
            origin="fallback_exception",
            http_status=out.http_status,
            failure_summary=summary,
        )
    data = _extract_json_object(out.content) or {}
    if not data:
        _LOG.debug("Mission planner: model reply was not parseable JSON; local fallback for onboarding.")
        return _fallback_plan(
            user_text,
            origin="fallback_exception",
            http_status=out.http_status,
            failure_summary="Assistant reply was not parseable as a JSON object.",
        )

    tasks_raw = str(data.get("tasks_markdown") or user_text).strip()
    structured: tuple[str, ...] | None = None
    tasks_arr = data.get("tasks")
    md_bullets = parse_task_lines_from_markdown(tasks_raw)
    if isinstance(tasks_arr, list) and tasks_arr:
        cleaned = [str(x).strip() for x in tasks_arr if str(x).strip()]
        if cleaned:
            # Model sometimes returns one vague ``tasks`` entry but a rich ``tasks_markdown`` list — prefer bullets.
            if len(cleaned) == 1 and len(md_bullets) >= 2:
                structured = tuple(md_bullets)
                tasks = tasks_raw
            else:
                structured = tuple(cleaned)
                tasks = "\n".join(f"- {t}" for t in cleaned)
        else:
            tasks = tasks_raw
    else:
        tasks = tasks_raw
    tick = data.get("tick_seconds")
    tick_i: int | None
    try:
        tick_i = int(tick) if tick is not None else None
    except (TypeError, ValueError):
        tick_i = None

    est_raw = data.get("estimated_tokens_per_iteration", 12_000)
    try:
        est = max(256, int(est_raw))
    except (TypeError, ValueError):
        est = 12_000

    return MissionPlan(
        tasks_markdown=tasks,
        tick_seconds=tick_i,
        use_tmux=_coerce_bool(data.get("use_tmux")),
        auth_required=_coerce_bool(data.get("auth_required")),
        estimated_tokens_per_iteration=est,
        refined_tick_prompt=str(data.get("refined_tick_prompt") or tasks).strip(),
        tier=tier,
        structured_tasks=structured,
        planner_origin="planner_api",
    )


# Backwards-compatible name (historical); same as ``parse_mission_with_planner``.
parse_mission_with_openai = parse_mission_with_planner
