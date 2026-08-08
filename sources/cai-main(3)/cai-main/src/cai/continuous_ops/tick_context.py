"""Rolling operator context between continuous-ops ticks (loop_runner).

The worker subprocess has no memory across ticks. Operators can end each tick
with a delimited block in the tick log; the next tick prepends accumulated
context to ``current_tick_prompt.txt`` so the worker model sees prior state.
"""

from __future__ import annotations

import os
import re
from datetime import datetime, timezone
from pathlib import Path

# Markers must appear on their own lines in the tick log (stdout/stderr capture).
MARKER_BEGIN = "<<<COPS_TICK_CONTEXT>>>"
MARKER_END = "<<<END_COPS_TICK_CONTEXT>>>"

STATE_RELATIVE = Path("state") / "tick_operator_context.md"


def _max_context_chars() -> int:
    raw = (os.environ.get("CAI_COPS_TICK_CONTEXT_MAX_CHARS") or "").strip()
    if raw.isdigit():
        return max(4096, int(raw))
    return 120_000


def extract_tick_context_block(log_text: str) -> str | None:
    """Return inner text between markers, or None if missing."""
    pattern = re.compile(
        re.escape(MARKER_BEGIN) + r"\s*\n(.*?)\n\s*" + re.escape(MARKER_END),
        re.DOTALL,
    )
    m = pattern.search(log_text)
    if not m:
        return None
    inner = (m.group(1) or "").strip()
    return inner or None


def extract_tick_context_from_file(log_path: Path) -> str | None:
    try:
        text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    return extract_tick_context_block(text)


def _trim_accumulated(body: str, max_chars: int) -> str:
    if len(body) <= max_chars:
        return body
    removed = len(body) - max_chars
    return (
        f"[… {removed:,} older characters dropped; cap CAI_COPS_TICK_CONTEXT_MAX_CHARS …]\n\n"
        + body[-max_chars:]
    )


def persist_tick_context(run_dir: Path, new_block: str) -> None:
    """Append a tick section to ``state/tick_operator_context.md`` and trim to max size."""
    path = run_dir.resolve() / STATE_RELATIVE
    path.parent.mkdir(parents=True, exist_ok=True)
    max_chars = _max_context_chars()
    stamp = datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")
    section = f"\n\n### Tick context ({stamp})\n\n{new_block.strip()}\n"
    prev = ""
    if path.is_file():
        try:
            prev = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            prev = ""
    merged = (prev.rstrip() + section).strip() + "\n"
    path.write_text(_trim_accumulated(merged, max_chars), encoding="utf-8")


def rolling_context_for_prompt(run_dir: Path) -> str:
    """Plain text to inject after the base tick prompt (may be empty)."""
    path = run_dir.resolve() / STATE_RELATIVE
    if not path.is_file():
        return ""
    try:
        body = path.read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return ""
    if not body:
        return ""
    return (
        "\n\n--- Prior continuous-ops context (saved from previous ticks; delimited logs) ---\n\n"
        + body
        + "\n\n--- End prior context ---\n"
    )


def format_context_instruction_footer() -> str:
    """Help text operators can paste into ``tick_prompt`` or mission docs."""
    return (
        f"To carry state to the next tick, end your tick output with these lines "
        f"(replace the middle with a concise summary the next run should read):\n"
        f"{MARKER_BEGIN}\n"
        f"<your summary here>\n"
        f"{MARKER_END}\n"
        "\nTo enqueue new discrete tasks for future ticks (parsed by the worker), emit:\n"
        "<<<COPS_TASK_APPEND>>>\n"
        '[{"id": "optional-id", "text": "Do X", "repeat_after_seconds": 3600}]\n'
        "<<<END_COPS_TASK_APPEND>>>\n"
    )
