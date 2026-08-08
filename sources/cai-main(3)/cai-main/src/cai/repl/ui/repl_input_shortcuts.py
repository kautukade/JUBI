"""
Canonical REPL input shortcuts (CLI headless, prompt_toolkit).

Single source for the bare ``/help`` «Quick shortcuts» panel and the ``?`` command.

An empty-line ``?`` keypress is handled in ``keybindings`` (no Enter); submitting ``?``
alone via Enter still runs the registered ``?`` command for the same panel.
"""

from __future__ import annotations

from typing import List, Tuple

from rich.console import Console
from rich.panel import Panel
from rich.table import Table
from rich.text import Text

_CAI_GREEN = "#00ff9d"
_GREY = "dim white"


def repl_input_shortcut_rows() -> List[Tuple[str, str]]:
    """Return (label, description) pairs for REPL key / prefix behaviour."""
    return [
        ("/", "Slash commands — first token"),
        ("$", "Shell — first character of line"),
        ("Tab", "Completion / history fill"),
        ("Enter", "Submit input"),
        ("↑ / ↓", "Command history"),
        ("Ctrl+L", "Clear screen"),
        ("Ctrl+D", "Delete forward (non-empty buffer)"),
        ("Ctrl+C", "Interrupt / exit"),
        ("Alt+↵ / Shift+↵ / Ctrl+J", "Insert newline"),
    ]


def quick_shortcuts_text(accent_style: str, desc_style: str) -> Text:
    """Rich ``Text`` body for the quick-guide «Quick shortcuts» subpanel."""
    parts: List[Tuple[str, str]] = []
    for label, desc in repl_input_shortcut_rows():
        parts.append((f"  {label}", accent_style))
        parts.append((f" — {desc}\n", desc_style))
    return Text.assemble(*parts)


def print_repl_input_shortcuts(console: Console) -> None:
    """Two-column table + panel (palette aligned with ``banner`` subpanels)."""
    from cai.repl.ui.banner import _quick_guide_subpanel_title

    rows = repl_input_shortcut_rows()
    mid = (len(rows) + 1) // 2
    left = rows[:mid]
    right = rows[mid:]

    tbl = Table(show_header=False, box=None, pad_edge=False, collapse_padding=True)
    tbl.add_column(vertical="top")
    tbl.add_column(vertical="top", min_width=2)
    tbl.add_column(vertical="top")

    def cell(label: str, desc: str) -> Text:
        return Text.assemble(
            (label, f"bold {_CAI_GREEN}"),
            (" — ", _GREY),
            (desc, "white"),
        )

    n = max(len(left), len(right))
    for i in range(n):
        lc = left[i] if i < len(left) else None
        rc = right[i] if i < len(right) else None
        lcell = cell(*lc) if lc else Text("")
        rcell = cell(*rc) if rc else Text("")
        tbl.add_row(lcell, Text("  "), rcell)

    console.print(
        Panel(
            tbl,
            title=_quick_guide_subpanel_title("Input shortcuts"),
            title_align="left",
            border_style=_CAI_GREEN,
            padding=(1, 1),
        )
    )
    console.print(
        Text.assemble(
            ("Full guide: ", "dim"),
            ("/help", f"bold {_CAI_GREEN}"),
            "\n",
        )
    )
