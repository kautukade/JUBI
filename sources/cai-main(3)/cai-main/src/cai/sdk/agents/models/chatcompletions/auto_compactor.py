"""Smart auto-compaction logic for context window management (Option E).

Two-phase compaction strategy:
  Phase 1 — Tool Output Truncation (no LLM cost):
      Truncates large tool outputs in OLD items of `message_history` while
      keeping the last K messages completely intact.  This alone typically
      reduces 40-60% of tokens because tool outputs (nmap, ip addr, file
      contents, etc.) are by far the largest items in the history.

  Phase 2 — Hybrid Summary (LLM call, only if still over threshold):
      Summarises the OLD portion of `message_history` via a Summary Agent,
      then removes those old items and injects a compact
      <compacted_context> block into system instructions.  The last K
      messages are preserved verbatim so the model keeps its recent
      working memory.

ARCHITECTURE NOTE:
  After the first turn, _fetch_response uses ONLY message_history
  (it ignores `input` to avoid duplication — see line ~2807 of
  openai_chatcompletions.py).  Therefore compaction MUST operate on
  message_history, not on `input`.

Extracted from openai_chatcompletions.py [F] to reduce monolith size.
Improved from "nuclear" (clear-all) strategy to preserve recent context.
"""

from __future__ import annotations

import json
import os
import re
import time
from typing import TYPE_CHECKING, Any

import tiktoken

from cai.config import AUTO_COMPACT_THRESHOLD_MAX, get_config
from cai.output import OUTPUT, StatusEvent
from .token_counter import count_tokens_with_tiktoken

if TYPE_CHECKING:
    from cai.sdk.agents.items import TResponseInputItem

# ---------------------------------------------------------------------------
# Number of recent messages to keep verbatim after compaction.
# Override via CAI_COMPACT_KEEP_RECENT (default 10, range 4–16).
# ---------------------------------------------------------------------------
KEEP_RECENT_MESSAGES = 10  # default; use get_keep_recent_messages() at runtime

# Auto-compact must not run for the nested LLM used in Phase 2 (only that agent).
_AUTOCOMPACT_SKIP_AGENT_NAMES = frozenset({"summary agent"})

# Minimum tokens in old messages before Phase 2 (summary) is worthwhile.
# Below this, the summary would be as large (or larger) than the original.
# Lowered from 1500→800 after deep benchmark showed Phase 2 being skipped
# too often (12/64 events), leaving tokens_after ~9.7k vs ~6.7k when P2 ran.
MIN_OLD_TOKENS_FOR_SUMMARY = 800

# Maximum characters to keep per tool output during Phase 1 truncation.
TOOL_OUTPUT_MAX_CHARS = 800

# Tiktoken encoder — lazy singleton
_ENCODER = None


def _get_encoder():
    global _ENCODER
    if _ENCODER is None:
        try:
            _ENCODER = tiktoken.get_encoding("cl100k_base")
        except Exception:
            pass
    return _ENCODER


def _count_tokens(text: str) -> int:
    """Count tokens in a string via tiktoken, fallback to char/4."""
    enc = _get_encoder()
    if enc:
        return len(enc.encode(text))
    return len(text) // 4


def _set_context_usage_env(current_tokens: float | int, max_tokens: int) -> None:
    """Persist toolbar ratio; clamp so delta-estimation bugs never report >100%."""
    if max_tokens <= 0:
        os.environ["CAI_CONTEXT_USAGE"] = "0.0"
        return
    bounded = min(max(0, int(current_tokens)), max_tokens)
    os.environ["CAI_CONTEXT_USAGE"] = str(bounded / max_tokens)


def get_model_max_tokens(model_name: str) -> int:
    """Maximum input/context tokens for *model_name*.

    Uses the same source as the pricing footer (:func:`cai.util.tokens.get_model_input_tokens`)
    so ``CAI_CONTEXT_USAGE``, auto-compact thresholds, and the sticky footer stay aligned.

    ``alias1`` keeps a dedicated cap when the generic lookup would undershoot.
    """
    # Runtime override: pin effective context window when the provider reports
    # a smaller limit than our pricing heuristics (prevents missing the 80% cap).
    override = (os.getenv("CAI_MODEL_MAX_INPUT_TOKENS") or "").strip()
    if override:
        try:
            n = int(float(override))
            if n > 0:
                return n
        except Exception:
            pass
    name = str(model_name).strip()
    if name == "alias1":
        return 150_000
    try:
        from cai.util.tokens import get_model_input_tokens

        n = int(get_model_input_tokens(name))
        if n > 0:
            return n
    except Exception:
        pass
    try:
        import pathlib

        pricing_path = pathlib.Path("pricing.json")
        if pricing_path.exists():
            with open(pricing_path, encoding="utf-8") as f:
                pricing_data = json.load(f)
                model_info = pricing_data.get(name, {})
                raw = model_info.get("max_input_tokens", 0)
                if isinstance(raw, int) and raw > 0:
                    return raw
    except Exception:
        pass
    return 200_000


# ─── Compaction terminal UI (flat style: A header + B phase checklist + A footer) ───
CAI_GREEN_COMPACT = "#00ff9d"
_GRAY_COMPACT = "#aaaaaa"


def _compact_sep_w(console) -> int:
    try:
        w = console.size.width
    except Exception:
        w = 72
    return max(16, min(w - 2, 80))


def _print_compact_inicio(
    console,
    *,
    context_pct: float,
    estimated: int,
    max_tok: int,
    n_msgs: int,
) -> None:
    """Option A — header + subtitle."""
    from rich.console import Group
    from rich.text import Text

    w = _compact_sep_w(console)
    sep = Text("─" * w, style=f"dim {CAI_GREEN_COMPACT}")
    title = Text()
    title.append("● ", style=f"bold {CAI_GREEN_COMPACT}")
    title.append("Context compact", style=f"bold {CAI_GREEN_COMPACT}")
    title.append(" — ", style="dim")
    title.append(
        f"{context_pct:.1f}% · {estimated:,}/{max_tok:,} · {n_msgs} msgs",
        style=_GRAY_COMPACT,
    )
    sub = Text()
    sub.append("    ", style="")
    sub.append("shrinking history before next model call", style=f"italic dim {_GRAY_COMPACT}")
    console.print()
    console.print(Group(sep, title, sub))


def _build_compact_during_renderable(
    console,
    *,
    p1: str,
    p2: str,
    p1_detail: str = "",
    p2_detail: str = "",
):
    """p1 / p2 each: 'run' | 'wait' | 'done' | 'skip' | 'fail'."""
    from rich.console import Group
    from rich.spinner import Spinner
    from rich.table import Table
    from rich.text import Text

    w = _compact_sep_w(console)
    sep = Text("─" * w, style=f"dim {CAI_GREEN_COMPACT}")

    def _row_phase(
        *,
        running: bool,
        done: bool,
        failed: bool,
        name: str,
        sub: str,
    ):
        if running:
            row = Table.grid(padding=0)
            sp = Spinner("dots", style=f"bold {CAI_GREEN_COMPACT}")
            rest = Text()
            rest.append(f"{name}", style=f"bold {CAI_GREEN_COMPACT}")
            rest.append(" — ", style="dim")
            rest.append(sub, style="white")
            row.add_row(Text("  "), sp, Text(" "), rest)
            return row
        t = Text()
        if done:
            t.append("  ● ", style=f"bold {CAI_GREEN_COMPACT}")
            t.append(f"{name}", style=f"bold {CAI_GREEN_COMPACT}")
            t.append(" — ", style="dim")
            t.append(sub, style=_GRAY_COMPACT)
            return t
        if failed:
            t.append("  ⚠ ", style="bold red")
            t.append(f"{name}", style="bold red")
            t.append(" — ", style="dim")
            t.append(sub, style=_GRAY_COMPACT)
            return t
        t.append("  ○ ", style="dim")
        t.append(f"{name}", style="dim")
        t.append(" — ", style="dim")
        t.append(sub, style=f"italic dim {_GRAY_COMPACT}")
        return t

    # Phase 1
    if p1 == "run":
        line1 = _row_phase(
            running=True,
            done=False,
            failed=False,
            name="Phase 1",
            sub="truncating tool outputs",
        )
    elif p1 == "done":
        line1 = _row_phase(
            running=False,
            done=True,
            failed=False,
            name="Phase 1",
            sub=p1_detail or "tool outputs truncated",
        )
    else:
        line1 = _row_phase(
            running=False,
            done=False,
            failed=False,
            name="Phase 1",
            sub="pending",
        )

    # Phase 2
    if p2 == "run":
        line2 = _row_phase(
            running=True,
            done=False,
            failed=False,
            name="Phase 2",
            sub="summarizing older turns…",
        )
    elif p2 == "done":
        line2 = _row_phase(
            running=False,
            done=True,
            failed=False,
            name="Phase 2",
            sub=p2_detail or "summary injected",
        )
    elif p2 == "fail":
        line2 = _row_phase(
            running=False,
            done=False,
            failed=True,
            name="Phase 2",
            sub=p2_detail or "summary failed",
        )
    elif p2 == "skip":
        line2 = _row_phase(
            running=False,
            done=False,
            failed=False,
            name="Phase 2",
            sub=p2_detail or "not required",
        )
    else:
        line2 = _row_phase(
            running=False,
            done=False,
            failed=False,
            name="Phase 2",
            sub="pending",
        )

    sep2 = Text("─" * w, style=f"dim {CAI_GREEN_COMPACT}")
    return Group(sep, line1, line2, sep2)


def _print_compact_fin(
    console,
    *,
    headline: str,
    tokens_before: int,
    tokens_after: int,
    reduction_pct: float,
    n_msgs: int,
    elapsed: float,
    tag: str = "",
    style: str = f"bold {CAI_GREEN_COMPACT}",
    success_footer: bool = True,
) -> None:
    """Option A — completion line."""
    from rich.console import Group
    from rich.text import Text

    w = _compact_sep_w(console)
    sep = Text("─" * w, style=f"dim {CAI_GREEN_COMPACT}")
    line = Text()
    line.append("  ● ", style=f"bold {CAI_GREEN_COMPACT}")
    line.append(headline, style=style)
    line.append("  ", style="")
    line.append(f"{tokens_before:,}", style="bold white")
    line.append(" → ", style="dim")
    line.append(f"~{tokens_after:,}", style="bold white")
    line.append(" tokens · ", style="dim")
    line.append(f"{reduction_pct:.1f}%", style=_GRAY_COMPACT)
    line.append(" reduction · ", style="dim")
    line.append(f"{n_msgs} msgs", style="white")
    line.append(" · ", style="dim")
    line.append(f"{elapsed:.1f}s", style=_GRAY_COMPACT)
    if tag:
        line.append(f" · {tag}", style=f"dim {CAI_GREEN_COMPACT}")
    parts = [sep, line]
    if success_footer:
        foot = Text()
        foot.append("    ", style="")
        foot.append("context reduced · ready to continue", style="italic dim white")
        parts.append(foot)
    console.print(Group(*parts))
    console.print()


# ===================================================================
# Phase 1 — tool output truncation on message_history (zero LLM cost)
# ===================================================================

def _compaction_keep_start_index(message_history: list, target_tail: int) -> int:
    """Return the index of the first message to keep after Phase 2.

    The kept suffix ``message_history[index:]`` must:

    * Not *start* with ``role: tool`` (invalid without a preceding ``assistant``
      with matching ``tool_calls`` on OpenAI-compatible APIs).

    * Contain at least one ``role: user`` whenever any user message exists in
      ``message_history``.  Otherwise LiteLLM / alias1 rejects the request with
      ``No user query found in messages`` — a common case when ``target_tail``
      is small and the last turns are only assistant/tool pairs.
    """
    n = len(message_history)
    if n == 0:
        return 0
    if n <= target_tail:
        start = 0
    else:
        start = n - target_tail
        while start > 0 and message_history[start].get("role") == "tool":
            start -= 1

    if not any(message_history[i].get("role") == "user" for i in range(start, n)):
        for i in range(start - 1, -1, -1):
            if message_history[i].get("role") == "user":
                start = i
                break
    return start


def _ensure_user_message_in_history_inplace(message_history: list) -> None:
    """Append a minimal user turn if the history has none (edge-case APIs)."""
    if any(m.get("role") == "user" for m in message_history):
        return
    message_history.append(
        {
            "role": "user",
            "content": (
                "Continue with the task using the system context and prior "
                "assistant or tool output."
            ),
        }
    )


def _repair_message_history_inplace(message_history: list) -> None:
    """Align tool/assistant sequencing with ``sanitize_message_list`` / API rules."""
    try:
        from cai.util import fix_message_list
    except ImportError:
        return
    if not message_history:
        return
    copies = [m.copy() if isinstance(m, dict) else m for m in message_history]
    repaired = fix_message_list(copies)
    message_history.clear()
    message_history.extend(repaired)


def _truncate_text(content: str, max_chars: int) -> str:
    """Truncate a string, keeping head + tail with a marker."""
    if len(content) <= max_chars:
        return content
    head = int(max_chars * 0.6)
    tail = int(max_chars * 0.3)
    removed = len(content) - head - tail
    return (
        content[:head]
        + f"\n\n[... {removed:,} characters truncated ...]\n\n"
        + content[-tail:]
    )


def _phase1_truncate_message_history(
    message_history: list[dict],
    keep_start: int,
    max_chars: int = TOOL_OUTPUT_MAX_CHARS,
) -> tuple[int, int]:
    """Truncate large tool outputs in-place in OLD message_history items.

    Only indices ``i < keep_start`` are modified; the kept suffix is untouched.

    Returns:
        (truncated_count, tokens_saved)
    """
    if keep_start <= 0 or len(message_history) <= keep_start:
        return 0, 0

    truncated_count = 0
    tokens_saved = 0

    for i in range(keep_start):
        msg = message_history[i]
        role = msg.get("role")
        content = msg.get("content")

        if not isinstance(content, str):
            continue

        # Truncate tool messages
        if role == "tool" and len(content) > max_chars:
            old_tokens = _count_tokens(content)
            msg["content"] = _truncate_text(content, max_chars)
            new_tokens = _count_tokens(msg["content"])
            tokens_saved += old_tokens - new_tokens
            truncated_count += 1
        # Truncate very large assistant messages (more generous limit)
        elif role == "assistant" and len(content) > max_chars * 4:
            old_tokens = _count_tokens(content)
            msg["content"] = _truncate_text(content, max_chars * 4)
            new_tokens = _count_tokens(msg["content"])
            tokens_saved += old_tokens - new_tokens
            truncated_count += 1

    return truncated_count, tokens_saved


# ===================================================================
# Phase 2 — hybrid summary of old messages
# ===================================================================

def _build_old_messages_text(
    message_history: list[dict],
    first_kept_index: int,
) -> str:
    """Format messages ``[0:first_kept_index)`` as plain text for the summariser."""
    if first_kept_index <= 0:
        return ""

    parts = []
    for msg in message_history[:first_kept_index]:
        role = (msg.get("role") or "unknown").upper()
        content = msg.get("content", "")

        if isinstance(content, str) and content.strip():
            # Truncate for summary input to keep LLM cost low
            if len(content) > 2000:
                content = content[:1500] + "\n[...truncated for summary...]"
            parts.append(f"{role}: {content}")
        elif msg.get("tool_calls"):
            # Capture tool call info
            tc_info = []
            for tc in msg["tool_calls"]:
                if isinstance(tc, dict) and "function" in tc:
                    fn = tc["function"]
                    tc_info.append(f"{fn.get('name', '?')}({fn.get('arguments', '')[:200]})")
            if tc_info:
                parts.append(f"ASSISTANT (tools): {'; '.join(tc_info)}")

    return "\n\n".join(parts)


def _estimate_old_messages_tokens(
    message_history: list[dict],
    first_kept_index: int,
) -> int:
    """Estimate tokens in messages ``[0:first_kept_index)`` (removed in Phase 2)."""
    if first_kept_index <= 0:
        return 0

    total = 0
    for msg in message_history[:first_kept_index]:
        total += 5  # per-message overhead (role + structure)
        content = msg.get("content", "")
        if isinstance(content, str) and content:
            total += _count_tokens(content)
        # Tool calls contribute tokens too
        for tc in msg.get("tool_calls", []):
            if isinstance(tc, dict) and "function" in tc:
                fn = tc["function"]
                fn_str = f"{fn.get('name', '')}({fn.get('arguments', '')})"
                total += _count_tokens(fn_str) + 3
    return total


_GENERIC_SUMMARY_PROMPT = """You are a context compaction assistant for security testing sessions.
Create a concise but COMPLETE summary of the following conversation history.

CRITICAL RULES:
1. Preserve ALL technical details: IPs, ports, credentials, paths, hostnames, flags, commands, error messages.
2. Preserve the user's original request and constraints.
3. Note what worked and what failed (with specific error messages).
4. Number every vulnerability/finding (Vuln 1, Vuln 2, …) — never drop mid-sequence IDs.
5. List concrete artifacts on disk (paths under workspace, PCAP/CSV/screenshot dirs).
6. List commands still pending or explicitly requested but not yet run.
7. Be factual and precise — do NOT invent information.
8. Keep the summary under 2000 tokens.

MANDATORY SECTIONS (use these exact headings):

## Objective
[User's original request and scope]

## Targets & IPs
[Every IP/hostname/CIDR and role: target, pivot, attacker, etc.]

## Vulnerabilities & Findings (numbered)
1. [First finding with evidence]
2. [Second finding]
(continue numbering — include vulns 4–10 if present in history)

## Commands Executed
[What ran, exit outcome, key output snippets]

## Pending Commands / Next Steps
[Explicit queue: what to run next, with exact command lines if known]

## Artifacts on Disk
[Paths: reports, packet_captures/, screenshots/, CSV inventories, logs]

## Current State
[Where the engagement stopped; blockers]

CONVERSATION TO SUMMARIZE:
"""


async def _phase2_summarize(
    old_text: str,
    model_name: str,
    agent_name: str | None,
    *,
    quiet: bool = False,
) -> str | None:
    """Summarise old messages via a lightweight LLM call."""
    if not old_text.strip():
        return None

    from rich.console import Console

    console = Console(highlight=False)

    try:
        from openai import AsyncOpenAI
        from cai.util.llm_api_base import resolve_llm_openai_compatible_api_key
        from cai.sdk.agents import Agent, Runner
        from cai.sdk.agents.models.openai_chatcompletions import OpenAIChatCompletionsModel

        if len(old_text) > 30000:
            _pre_len = len(old_text)
            old_text = old_text[-30000:]
            if not quiet:
                from rich.text import Text as _Tx

                console.print(
                    _Tx(
                        f"Truncating for summary ({_pre_len:,} → 30,000 chars)…",
                        style=f"dim {_GRAY_COMPACT}",
                    )
                )

        summary_agent = Agent(
            name="Summary Agent",
            instructions=_GENERIC_SUMMARY_PROMPT,
            model=OpenAIChatCompletionsModel(
                model=model_name,
                openai_client=AsyncOpenAI(
                    api_key=resolve_llm_openai_compatible_api_key(model_name)
                ),
                agent_name="Summary Agent",
            ),
        )

        if not quiet:
            from rich.text import Text as _Tx

            console.print(
                _Tx(
                    f"Generating summary via {model_name}…",
                    style=f"dim {CAI_GREEN_COMPACT}",
                )
            )

        # In quiet mode, hide Summary Agent render output in CLI while still
        # running the summarization call synchronously.
        if quiet:
            import contextlib
            import io

            with contextlib.redirect_stdout(io.StringIO()):
                result = await Runner.run(
                    starting_agent=summary_agent,
                    input=old_text,
                    max_turns=1,
                )
        else:
            result = await Runner.run(
                starting_agent=summary_agent,
                input=old_text,
                max_turns=1,
            )

        if result.final_output:
            return str(result.final_output)
        return None

    except Exception as e:
        console.print(f"[red]Summary generation failed: {e}[/red]")
        return None


# ===================================================================
# Main entry point
# ===================================================================

async def auto_compact_if_needed(
    *,
    estimated_tokens: int,
    input: str | list[Any],
    system_instructions: str | None,
    model_name: str,
    agent_name: str | None,
    message_history: list,
    converter: Any,
    compaction_in_progress_flag: bool,
    set_compaction_flag: Any,
) -> tuple[str | list[Any], str | None, bool]:
    """Smart two-phase auto-compaction (Option E).

    Operates on **message_history** (the authoritative conversation store).
    After the first turn, _fetch_response only uses message_history and
    ignores `input`, so we must compact message_history directly.

    Phase 1: Truncate large tool outputs in old messages (zero LLM cost).
    Phase 2: Summarise old messages and keep the last K verbatim.

    Token re-estimation uses delta arithmetic (chars removed / added)
    instead of re-calling converter.items_to_messages(), which has
    side-effects (console printing, stack inspection, history mutation).

    Returns:
        (input, system_instructions, compaction_occurred)
    """
    cfg = get_config()

    def _debug_skip(reason: str) -> None:
        try:
            dbg = int(getattr(cfg, "debug", 0) or 0)
        except (TypeError, ValueError):
            dbg = 0
        if dbg >= 2:
            from rich.console import Console

            Console(stderr=True, highlight=False).print(
                f"[dim]auto_compact_if_needed: skip — {reason}[/dim]"
            )

    if not cfg.auto_compact:
        _debug_skip("auto-compact disabled (config / CAI_AUTO_COMPACT)")
        return input, system_instructions, False

    if compaction_in_progress_flag:
        _debug_skip(
            "compaction already in progress (nested compact blocked; if stuck, restart session)"
        )
        return input, system_instructions, False

    # Only skip the internal Phase-2 summarizer, not user agents named e.g. "Executive Summary".
    if (agent_name or "").strip().lower() in _AUTOCOMPACT_SKIP_AGENT_NAMES:
        _debug_skip('agent is internal "Summary Agent" (avoid recursive compaction)')
        return input, system_instructions, False

    max_tokens = get_model_max_tokens(model_name)
    threshold_percent = min(cfg.auto_compact_threshold, AUTO_COMPACT_THRESHOLD_MAX)
    threshold = max_tokens * threshold_percent

    if estimated_tokens <= threshold:
        _debug_skip(
            f"context {estimated_tokens:,} tok ≤ threshold {int(threshold):,} "
            f"({threshold_percent * 100:.0f}% of {max_tokens:,} max)"
        )
        return input, system_instructions, False

    # ---------------------------------------------------------------
    # Auto-compaction needed
    # ---------------------------------------------------------------
    from rich.console import Console
    from rich.live import Live

    from cai.util.streaming import register_compaction_live, unregister_compaction_live

    console = Console(highlight=False)

    context_pct = (estimated_tokens / max_tokens) * 100
    _set_context_usage_env(estimated_tokens, max_tokens)

    OUTPUT.emit(
        StatusEvent(
            message=f"Compacting context ({len(message_history)} msgs)…",
            level="info",
            agent_id=agent_name,
        )
    )
    _print_compact_inicio(
        console,
        context_pct=context_pct,
        estimated=estimated_tokens,
        max_tok=max_tokens,
        n_msgs=len(message_history),
    )

    set_compaction_flag(True)
    t0 = time.time()

    ui: dict[str, str] = {"p1": "run", "p2": "wait", "p1d": "", "p2d": ""}

    def _dur():
        return _build_compact_during_renderable(
            console,
            p1=ui["p1"],
            p2=ui["p2"],
            p1_detail=ui["p1d"],
            p2_detail=ui["p2d"],
        )

    try:
        tokens_before = estimated_tokens
        current_tokens = tokens_before

        from cai.util.session_compact import get_keep_recent_messages

        keep_recent = get_keep_recent_messages()
        keep_start = _compaction_keep_start_index(message_history, keep_recent)

        summary = None
        attempted_p2 = False

        with Live(_dur(), console=console, refresh_per_second=12, transient=False) as live:
            register_compaction_live(live)
            try:
                live.update(_dur())

                truncated_count, tokens_saved_p1 = _phase1_truncate_message_history(
                    message_history,
                    keep_start=keep_start,
                    max_chars=TOOL_OUTPUT_MAX_CHARS,
                )

                if tokens_saved_p1 > 0:
                    current_tokens = max(current_tokens - tokens_saved_p1, 0)

                phase1_time = time.time() - t0
                p1_reduction = (tokens_saved_p1 / tokens_before * 100) if tokens_before > 0 else 0

                ui["p1"] = "done"
                ui["p1d"] = (
                    f"{truncated_count} outputs · ~{tokens_saved_p1:,} tok freed · "
                    f"{phase1_time:.2f}s"
                )
                live.update(_dur())

                if current_tokens <= threshold:
                    ui["p2"] = "skip"
                    ui["p2d"] = "not required — within threshold after phase 1"
                    live.update(_dur())
                    elapsed = time.time() - t0
                    current_tokens = min(int(current_tokens), max_tokens)
                    _set_context_usage_env(current_tokens, max_tokens)
                    _log_compaction_metrics(
                        agent_name=agent_name,
                        tokens_before=tokens_before,
                        tokens_after=current_tokens,
                        phase="phase1_only",
                        msgs_preserved=len(message_history),
                        elapsed=elapsed,
                        llm_cost=0.0,
                    )
                    set_compaction_flag(False)
                    _print_compact_fin(
                        console,
                        headline="Compaction complete",
                        tokens_before=tokens_before,
                        tokens_after=current_tokens,
                        reduction_pct=p1_reduction,
                        n_msgs=len(message_history),
                        elapsed=elapsed,
                        tag="Phase 1",
                    )
                    return input, system_instructions, True

                t1 = time.time()
                if keep_start > 0:
                    old_text = _build_old_messages_text(message_history, keep_start)
                    old_tokens = _estimate_old_messages_tokens(message_history, keep_start)
                else:
                    old_text = ""
                    old_tokens = 0

                if old_text.strip() and old_tokens >= MIN_OLD_TOKENS_FOR_SUMMARY:
                    attempted_p2 = True
                    ui["p2"] = "run"
                    live.update(_dur())
                    # Nested Runner / streaming prints Rich panels to the same console.
                    # While Live is active, stdout is proxied and the render hook stacks
                    # output on the compaction block — stop Live first so the Summary
                    # Agent panel starts on a fresh line (no extra ENTER needed).
                    live.stop()
                    console.print()
                    summary = await _phase2_summarize(
                        old_text, model_name, agent_name, quiet=True
                    )
                    phase2_time = time.time() - t1
                    if summary:
                        stok = _count_tokens(summary) + 80
                        ui["p2"] = "done"
                        ui["p2d"] = f"{old_tokens:,} → {stok:,} summary tok · {phase2_time:.1f}s"
                    else:
                        ui["p2"] = "fail"
                        ui["p2d"] = "no summary produced"
                    if live.is_started:
                        live.update(_dur())
                elif old_text.strip():
                    ui["p2"] = "skip"
                    ui["p2d"] = (
                        f"skipped — old segment {old_tokens} tok < min "
                        f"{MIN_OLD_TOKENS_FOR_SUMMARY}"
                    )
                    live.update(_dur())
                else:
                    ui["p2"] = "skip"
                    ui["p2d"] = "nothing to summarise"
                    live.update(_dur())
            finally:
                unregister_compaction_live(live)

        if summary:
            try:
                from cai.repl.commands.memory import COMPACTED_SUMMARIES
                from cai.util.session_compact import record_compaction_result

                record_compaction_result(summary, agent_name, message_history)
                if agent_name:
                    existing = COMPACTED_SUMMARIES.get(agent_name)
                    if isinstance(existing, list):
                        if not existing or existing[-1] != summary:
                            existing.append(summary)
                    else:
                        COMPACTED_SUMMARIES[agent_name] = [summary]
            except Exception:
                pass

            recent = [
                m.copy() if isinstance(m, dict) else m
                for m in message_history[keep_start:]
            ]
            message_history.clear()
            message_history.extend(recent)
            _repair_message_history_inplace(message_history)
            _ensure_user_message_in_history_inplace(message_history)
            keep = len(message_history)

            new_si = system_instructions or ""
            new_si = re.sub(
                r'<compacted_context>.*?</compacted_context>\s*',
                "",
                new_si,
                flags=re.DOTALL,
            )
            if new_si:
                new_si += "\n\n"
            new_si += f"""<compacted_context>
This is a summary of previous conversation context that has been compacted to save tokens:

{summary}

Use this summary as context for earlier work. The recent messages below are preserved verbatim — continue from where you left off without re-doing already completed steps.
</compacted_context>"""

            summary_tokens = _count_tokens(summary) + 80
            tokens_delta_p2 = old_tokens - summary_tokens
            current_tokens = max(current_tokens - tokens_delta_p2, 0)
            current_tokens = min(int(current_tokens), max_tokens)
            total_reduction = (
                (tokens_before - current_tokens) / tokens_before * 100
            ) if tokens_before > 0 else 0
            elapsed = time.time() - t0

            _set_context_usage_env(current_tokens, max_tokens)

            _log_compaction_metrics(
                agent_name=agent_name,
                tokens_before=tokens_before,
                tokens_after=current_tokens,
                phase="phase1+phase2",
                msgs_preserved=keep,
                elapsed=elapsed,
                llm_cost=-1.0,
            )
            set_compaction_flag(False)
            _print_compact_fin(
                console,
                headline="Compaction complete",
                tokens_before=tokens_before,
                tokens_after=current_tokens,
                reduction_pct=total_reduction,
                n_msgs=keep,
                elapsed=elapsed,
                tag="Phase 1+2",
            )
            return input, new_si, True

        elapsed = time.time() - t0
        current_tokens = min(int(current_tokens), max_tokens)
        _set_context_usage_env(current_tokens, max_tokens)

        if attempted_p2:
            _log_compaction_metrics(
                agent_name=agent_name,
                tokens_before=tokens_before,
                tokens_after=current_tokens,
                phase="phase1_only_p2_failed",
                msgs_preserved=len(message_history),
                elapsed=elapsed,
                llm_cost=0.0,
            )
            set_compaction_flag(False)
            _print_compact_fin(
                console,
                headline="Compaction partial — Phase 2 failed",
                tokens_before=tokens_before,
                tokens_after=current_tokens,
                reduction_pct=p1_reduction,
                n_msgs=len(message_history),
                elapsed=elapsed,
                tag="Phase 1 only",
                style="bold yellow",
                success_footer=False,
            )
            return input, system_instructions, True

        _log_compaction_metrics(
            agent_name=agent_name,
            tokens_before=tokens_before,
            tokens_after=current_tokens,
            phase="phase1_only_p2_skipped",
            msgs_preserved=len(message_history),
            elapsed=elapsed,
            llm_cost=0.0,
        )
        set_compaction_flag(False)
        _print_compact_fin(
            console,
            headline="Compaction complete",
            tokens_before=tokens_before,
            tokens_after=current_tokens,
            reduction_pct=p1_reduction,
            n_msgs=len(message_history),
            elapsed=elapsed,
            tag="Phase 1 (Phase 2 skipped)",
        )
        return input, system_instructions, True

    except Exception as e:
        console.print(f"[red]Auto-compaction failed: {e}[/red]")
        console.print("[yellow]Continuing with full context...[/yellow]\n")
    finally:
        set_compaction_flag(False)

    return input, system_instructions, False


# ===================================================================
# Metrics logger
# ===================================================================

def _log_compaction_metrics(
    *,
    agent_name: str | None,
    tokens_before: int,
    tokens_after: int,
    phase: str,
    msgs_preserved: int,
    elapsed: float,
    llm_cost: float,
) -> None:
    """Append one compaction event to a JSONL file for benchmark analysis."""
    try:
        import pathlib
        log_dir = pathlib.Path.home() / ".cai" / "compaction_logs"
        log_dir.mkdir(parents=True, exist_ok=True)
        log_path = log_dir / "compaction_metrics.jsonl"

        entry = {
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "agent_name": agent_name or "unknown",
            "tokens_before": tokens_before,
            "tokens_after": tokens_after,
            "reduction_pct": round(
                (1 - tokens_after / tokens_before) * 100, 2
            ) if tokens_before > 0 else 0,
            "phase": phase,
            "messages_preserved": msgs_preserved,
            "elapsed_secs": round(elapsed, 2),
            "llm_cost": llm_cost,
        }
        with open(log_path, "a", encoding="utf-8") as f:
            f.write(json.dumps(entry) + "\n")
    except Exception:
        pass
