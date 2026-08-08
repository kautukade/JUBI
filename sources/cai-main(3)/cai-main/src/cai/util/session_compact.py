"""Session-wide compaction summary and explicit agent handoff.

After auto-compact (Phase 2), the summary is stored globally so every agent
sees the same ``<compacted_context>`` via :func:`get_compacted_summary` and
``CAI_SESSION_COMPACT_SUMMARY``. On agent switch, :func:`prepare_agent_handoff`
builds a one-shot handoff block (last compact + recent findings).
"""

from __future__ import annotations

import os
import re
from typing import Any

ENV_SESSION_SUMMARY = "CAI_SESSION_COMPACT_SUMMARY"
ENV_RECENT_FINDINGS = "CAI_SESSION_RECENT_FINDINGS"
ENV_HANDOFF = "CAI_AGENT_HANDOFF_CONTEXT"
ENV_KEEP_RECENT = "CAI_COMPACT_KEEP_RECENT"

DEFAULT_KEEP_RECENT = 10
MIN_KEEP_RECENT = 4
MAX_KEEP_RECENT = 16
MAX_ENV_CHARS = 28_000
FINDINGS_DEFAULT_N = 6

_COMPACTED_BLOCK_RE = re.compile(
    r"<compacted_context>\s*(.*?)\s*</compacted_context>",
    re.DOTALL | re.IGNORECASE,
)

# Pentest-oriented patterns for recent-findings extraction
_IP_RE = re.compile(
    r"\b(?:(?:25[0-5]|2[0-4]\d|[01]?\d\d?)\.){3}(?:25[0-5]|2[0-4]\d|[01]?\d\d?)\b"
)
_VULN_RE = re.compile(
    r"(?i)(?:vuln(?:erability)?|finding|CVE-\d{4}-\d+|exploit|misconfigur)",
)
_ARTIFACT_RE = re.compile(
    r"(?i)(?:/[\w./-]+\.(?:pcap|csv|png|txt|json|xml|har)|packet_captures/|screenshots/)",
)
_CMD_RE = re.compile(
    r"(?i)(?:pending|next\s+(?:step|command)|run\s+(?:nmap|curl|sqlmap|gobuster))",
)


def get_keep_recent_messages() -> int:
    """Messages to keep verbatim after compaction (env ``CAI_COMPACT_KEEP_RECENT``)."""
    raw = os.getenv(ENV_KEEP_RECENT, "").strip()
    if not raw:
        return DEFAULT_KEEP_RECENT
    try:
        n = int(raw)
    except ValueError:
        return DEFAULT_KEEP_RECENT
    return max(MIN_KEEP_RECENT, min(MAX_KEEP_RECENT, n))


def _truncate_for_env(text: str, limit: int = MAX_ENV_CHARS) -> str:
    if len(text) <= limit:
        return text
    return text[: limit - 80] + "\n\n[...truncated for CAI_SESSION_COMPACT_SUMMARY env...]"


def extract_compact_block(system_instructions: str | None) -> str | None:
    if not system_instructions:
        return None
    m = _COMPACTED_BLOCK_RE.search(system_instructions)
    if not m:
        return None
    body = (m.group(1) or "").strip()
    return body or None


def _message_text(msg: dict[str, Any]) -> str:
    content = msg.get("content", "")
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts = []
        for item in content:
            if isinstance(item, dict) and item.get("text"):
                parts.append(str(item["text"]))
            elif isinstance(item, str):
                parts.append(item)
        return "\n".join(parts)
    return str(content) if content else ""


def extract_recent_findings(
    message_history: list[dict[str, Any]] | None,
    *,
    n: int = FINDINGS_DEFAULT_N,
) -> list[str]:
    """Last *n* pentest-relevant snippets from recent messages."""
    if not message_history:
        return []

    findings: list[str] = []
    seen: set[str] = set()

    for msg in reversed(message_history):
        if msg.get("role") not in ("assistant", "user", "tool"):
            continue
        text = _message_text(msg).strip()
        if not text or len(text) < 12:
            continue

        lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
        for line in lines:
            if len(line) > 400:
                line = line[:400] + "…"
            key = line[:120]
            if key in seen:
                continue
            if (
                _IP_RE.search(line)
                or _VULN_RE.search(line)
                or _ARTIFACT_RE.search(line)
                or _CMD_RE.search(line)
                or re.search(r"(?i)\b(?:port|service|credential|flag\{)\b", line)
            ):
                seen.add(key)
                role = (msg.get("role") or "?").upper()
                findings.append(f"[{role}] {line}")
                if len(findings) >= n:
                    return list(reversed(findings))

    return list(reversed(findings))


def set_session_compact_summary(
    summary: str,
    *,
    agent_name: str | None = None,
    recent_findings: list[str] | None = None,
) -> None:
    """Persist global session summary after Phase 2 compaction."""
    summary = (summary or "").strip()
    if not summary:
        return

    os.environ[ENV_SESSION_SUMMARY] = _truncate_for_env(summary)

    if recent_findings:
        os.environ[ENV_RECENT_FINDINGS] = _truncate_for_env(
            "\n".join(f"- {f}" for f in recent_findings[-FINDINGS_DEFAULT_N :]),
            limit=8000,
        )

    try:
        from cai.repl.commands.memory import COMPACTED_SUMMARIES

        COMPACTED_SUMMARIES["__global__"] = [summary]
        if agent_name:
            existing = COMPACTED_SUMMARIES.get(agent_name)
            if isinstance(existing, list):
                if not existing or existing[-1] != summary:
                    existing.append(summary)
            else:
                COMPACTED_SUMMARIES[agent_name] = [summary]
    except Exception:
        pass


def get_session_compact_summary() -> str | None:
    env = os.getenv(ENV_SESSION_SUMMARY, "").strip()
    if env:
        return env
    try:
        from cai.repl.commands.memory import COMPACTED_SUMMARIES

        global_summaries = COMPACTED_SUMMARIES.get("__global__")
        if isinstance(global_summaries, list) and global_summaries:
            return global_summaries[-1]
        if isinstance(global_summaries, str) and global_summaries:
            return global_summaries
    except Exception:
        pass
    return None


def get_session_recent_findings() -> str | None:
    env = os.getenv(ENV_RECENT_FINDINGS, "").strip()
    if env:
        return env
    return None


def record_compaction_result(
    summary: str,
    agent_name: str | None,
    message_history: list[dict[str, Any]],
) -> None:
    findings = extract_recent_findings(message_history)
    set_session_compact_summary(
        summary,
        agent_name=agent_name,
        recent_findings=findings,
    )


def prepare_agent_handoff(
    from_agent_name: str,
    message_history: list[dict[str, Any]] | None,
    system_instructions: str | None,
    *,
    to_agent_name: str | None = None,
) -> str:
    """Build one-shot handoff for the next agent (sets ``CAI_AGENT_HANDOFF_CONTEXT``)."""
    compact = extract_compact_block(system_instructions) or get_session_compact_summary()
    findings = extract_recent_findings(message_history)

    parts = [
        "<agent_handoff>",
        f"Previous agent: {from_agent_name}",
    ]
    if to_agent_name:
        parts.append(f"Active agent: {to_agent_name}")
    parts.append(
        "Continue the same engagement. Do not claim there is no prior history — "
        "use the compacted context and recent findings below."
    )

    if compact:
        parts.append("\n<compacted_context>\n" + compact + "\n</compacted_context>")
    else:
        parts.append(
            "\n(No compacted_context block yet — use SHARED SESSION CONTEXT and recent messages.)"
        )

    if findings:
        parts.append("\n## Recent findings (last exchanges)\n")
        for f in findings:
            parts.append(f"- {f}")

    env_findings = get_session_recent_findings()
    if env_findings and env_findings not in "\n".join(findings):
        parts.append("\n## Session findings snapshot\n" + env_findings)

    parts.append("</agent_handoff>")
    handoff = "\n".join(parts)
    os.environ[ENV_HANDOFF] = _truncate_for_env(handoff, limit=MAX_ENV_CHARS)
    return handoff


def consume_agent_handoff() -> str | None:
    """Return and clear pending handoff (one shot per switch)."""
    text = os.environ.pop(ENV_HANDOFF, "").strip()
    return text or None


def get_handoff_for_system_prompt() -> str:
    """Handoff block for system prompt injection (does not consume)."""
    return os.getenv(ENV_HANDOFF, "").strip()


def shared_context_supplement() -> str:
    """Session summary for :meth:`AGENT_MANAGER.get_shared_context_injection`.

    Explicit agent handoff is injected once via :func:`consume_agent_handoff` in the
    CLI (user turn), not duplicated here.
    """
    session = get_session_compact_summary()
    if not session:
        return ""
    return "\n## SESSION COMPACT SUMMARY (all agents)\n" + session[:12000]
