"""Ctrl+O expand popup for the compact REPL.

Lists every task in the current turn (running, completed, errored) — q23=d —
and prints the full captured output of the selected task to the scrollback.
Keyboard-only; no mouse capture (q11=b, q22).

Implementation
--------------
* :class:`prompt_toolkit.shortcuts.radiolist_dialog` for arrow-key navigation.
* The dialog is shown via :func:`prompt_toolkit.application.run_in_terminal`
  from inside the main REPL key binding (see :mod:`cai.repl.ui.keybindings`).
* The full ``TaskRecord.output`` lives in :data:`TASK_REGISTRY`; nothing is
  re-fetched from disk.
"""

from __future__ import annotations

from typing import Optional

from rich.console import Console
from rich.padding import Padding
from rich.panel import Panel
from rich.text import Text

from cai.output import TASK_REGISTRY, TaskRecord
from cai.repl.ui.compact_renderer import format_secs
from cai.util.cli_palette import (
    CAI_GREEN,
    COMPLETED_PILL,
    ERROR_PILL,
    GREY_HINT,
    GREY_TEXT,
)


def _status_glyph(record: TaskRecord) -> str:
    if record.status == "completed":
        return "✓"
    if record.status == "error":
        return "✗"
    return "⋯"


def _choice_label(record: TaskRecord) -> str:
    """One-line label for the radiolist row (no Rich markup; plain text)."""
    parts: list[str] = [_status_glyph(record)]
    name = record.agent_name or "Agent"
    if record.agent_id and f"[{record.agent_id}]" not in name:
        name = f"{name} [{record.agent_id}]"
    parts.append(name)
    parts.append("—")
    parts.append(record.label or record.tool_name or "task")
    parts.append(f"({format_secs(record.duration_seconds or 0.0)})")
    return " ".join(parts)


def _print_task_detail(record: TaskRecord, console: Console | None = None) -> None:
    """Render the full task output to scrollback as a Rich panel."""
    console = console or Console()
    title = Text()
    title.append("● ", style=f"bold {CAI_GREEN}")
    title.append(record.agent_name or "Agent", style=f"bold {CAI_GREEN}")
    if record.agent_id and f"[{record.agent_id}]" not in (record.agent_name or ""):
        title.append(f" [{record.agent_id}]", style=f"bold {GREY_HINT}")
    title.append(" ─ ", style="dim white")
    title.append(record.label or record.tool_name or "task", style="white")

    if record.status == "completed":
        title.append("   ✓ OK", style=COMPLETED_PILL)
    elif record.status == "error":
        title.append("   ✗ ERROR", style=ERROR_PILL)
    else:
        title.append("   ⋯ RUNNING", style="bold yellow")

    title.append(f"   ⏱ {format_secs(record.duration_seconds or 0.0)}", style=GREY_TEXT)

    body_text = record.output or "(no captured output)"
    body = Text(body_text, style=GREY_TEXT)
    if record.status == "error" and record.error:
        err_block = Text()
        err_block.append("\n\n", style="")
        err_block.append("error: ", style=ERROR_PILL)
        err_block.append(record.error, style="bold white")
        if record.error_type:
            err_block.append(f"  ({record.error_type})", style=GREY_HINT)
        body.append_text(err_block)

    console.print(Panel(Padding(body, (0, 1)), title=title, border_style=GREY_HINT, expand=True))


def open_expand_popup(console: Console | None = None) -> None:
    """Show the radiolist dialog and print the selected task to scrollback.

    Designed to be called from a prompt_toolkit key binding via
    :func:`run_in_terminal`. Safe to call when no tasks exist (prints a hint
    and returns).
    """
    console = console or Console()
    tasks = TASK_REGISTRY.for_turn() or []
    if not tasks:
        console.print(Text("Ctrl+O — no tasks in the current turn yet.", style=GREY_HINT))
        return

    # Build the choices in display order (oldest first, mirrors the live block).
    values: list[tuple[str, str]] = [(t.task_id, _choice_label(t)) for t in tasks]

    try:
        # Imported lazily so the popup module stays usable in headless tests
        # that don't install prompt_toolkit's dialogs.
        from prompt_toolkit.shortcuts import radiolist_dialog
    except Exception:
        # Prompt-toolkit dialogs unavailable: fall back to a plain numbered menu.
        return _open_fallback_menu(values, console)

    try:
        selected: Optional[str] = radiolist_dialog(
            title="Expand task",
            text="↑/↓ to move · Enter to expand · Esc to cancel",
            values=values,
        ).run()
    except Exception:
        # Any failure (e.g. nested event loops) → graceful fallback
        return _open_fallback_menu(values, console)

    if not selected:
        return
    record = TASK_REGISTRY.get(selected)
    if record is None:
        return
    _print_task_detail(record, console=console)


def _open_fallback_menu(values: list[tuple[str, str]], console: Console) -> None:
    """Plain numbered menu used when ``radiolist_dialog`` is unavailable."""
    console.print(Text("Expand task — select a number, then press Enter:", style=f"bold {CAI_GREEN}"))
    for i, (_id, label) in enumerate(values, start=1):
        line = Text()
        line.append(f"  {i:>2}. ", style=GREY_HINT)
        line.append(label, style=GREY_TEXT)
        console.print(line)
    try:
        raw = input("> ").strip()
    except (EOFError, KeyboardInterrupt):
        return
    if not raw:
        return
    try:
        idx = int(raw)
    except ValueError:
        return
    if not (1 <= idx <= len(values)):
        return
    record = TASK_REGISTRY.get(values[idx - 1][0])
    if record is None:
        return
    _print_task_detail(record, console=console)


__all__ = ["open_expand_popup"]
