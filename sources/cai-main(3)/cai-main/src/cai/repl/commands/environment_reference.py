"""
Full environment-variable reference tables (shown after bare ``/help`` only).

Visual chrome matches ``display_quick_guide``: green outer border, badge-style subsection
titles, CAI palette (see ``banner.py``).
"""

from __future__ import annotations

from collections import defaultdict
from typing import Any, Dict, List, Optional, Tuple

from rich.console import Console, Group
from rich.padding import Padding
from rich.panel import Panel
from rich.table import Table
from rich.text import Text

from cai.repl.commands.env_catalog import ENV_VARS, HELP_REFERENCE_MATCH_TABLE_KWARGS
from cai.repl.commands.env_info_catalog import (
    CATEGORY_DISPLAY,
    EXTRA_ENV_VARS,
    INTRO_MARKUP,
    category_title_for_number,
    constraints_line,
    effective_label,
)
from cai.repl.commands.env_var_help import usage_markup_bold
from cai.repl.ui.banner import _CAI_GREEN, _GREY, _quick_guide_subpanel_title, environment_reference_outer_title


def _group_env_vars_by_category() -> List[Tuple[str, List[Tuple[int, dict]]]]:
    """Return (category_title, [(num, var_info), ...]) sorted by category then number."""
    buckets: Dict[str, List[Tuple[int, dict]]] = defaultdict(list)
    for num, var_info in sorted(ENV_VARS.items()):
        title = category_title_for_number(int(num))
        buckets[title].append((int(num), var_info))
    preferred = [
        "Core agent & model",
        "Streaming & debug output",
        "Parallelization & queue",
        "Execution limits & timeouts",
        "Memory & context",
        "Workspace & containers",
        "Support & meta agent",
        "CTR / G-CTR",
        "Tracing & telemetry",
        "Security & planning",
        "Pricing & cost",
        "Reporting & continuation",
        "HTTP API server",
        "Authentication service",
        "MCP (Model Context Protocol)",
        "TUI",
        "Advanced / misc",
        "Provider keys & runtime",
        "Per-agent model overrides",
        "CTF (capture-the-flag)",
    ]
    ordered: List[Tuple[str, List[Tuple[int, dict]]]] = []
    seen = set()
    for p in preferred:
        if p in buckets:
            ordered.append((p, buckets[p]))
            seen.add(p)
    for k, v in buckets.items():
        if k not in seen:
            ordered.append((k, v))
    return ordered


def _category_vars_table(rows: List[Tuple[int, dict]]) -> Table:
    """Borderless data table for one category (used inside the green section panel)."""
    table = Table(**HELP_REFERENCE_MATCH_TABLE_KWARGS)
    table.add_column("#", justify="right", width=4, no_wrap=True)
    table.add_column("Variable", no_wrap=True, min_width=16)
    table.add_column("Default", min_width=8, max_width=22, no_wrap=True)
    table.add_column("Values", min_width=10, ratio=2)
    table.add_column("When", min_width=8, no_wrap=True, ratio=1)
    table.add_column("Description", min_width=18, ratio=4)

    for idx, (num, var_info) in enumerate(rows):
        name = var_info["name"]
        default = var_info.get("default")
        default_s = "—" if default is None else str(default)
        desc = var_info.get("description") or ""
        body_style = "white" if idx % 2 == 0 else _GREY
        table.add_row(
            Text(str(num), style=body_style),
            Text(name, style=f"bold {_CAI_GREEN}"),
            Text(default_s, style=_CAI_GREEN),
            Text(constraints_line(name, desc), style=body_style),
            Text(effective_label(name), style=body_style),
            Text(desc, style=body_style),
        )
    return table


def _category_section_panel(
    category: str,
    *,
    body_chunks: List[Any],
) -> Panel:
    """Single green-bordered panel: optional prose (above) + table + optional footnote."""
    if not body_chunks:
        body_chunks = [Text("")]
    inner: Any = Group(*body_chunks) if len(body_chunks) > 1 else body_chunks[0]
    return Panel(
        inner,
        title=_quick_guide_subpanel_title(category),
        title_align="left",
        border_style=_CAI_GREEN,
        padding=(1, 1),
    )


def _dependency_satisfied(dependency_id: Optional[str]) -> bool:
    if not dependency_id:
        return True
    if dependency_id == "pentestperf":
        try:
            from cai import is_pentestperf_available

            return is_pentestperf_available()
        except Exception:
            return False
    return True


def _category_block_parts(category: str, rows: List[Tuple[int, dict]]) -> List[Any]:
    """One panel per category: overview / dependency copy inside the border, then the table."""
    cfg: Dict[str, Any] = CATEGORY_DISPLAY.get(category) or {}
    chunks: List[Any] = []

    overview = cfg.get("overview")
    if overview:
        chunks.append(Padding(Text.from_markup(overview), (0, 0, 1, 0)))

    dep_id: Optional[str] = cfg.get("dependency_id")
    omit_without = bool(cfg.get("omit_table_without_dependency"))
    dep_ok = _dependency_satisfied(dep_id) if dep_id else True
    show_table = not (dep_id and omit_without and not dep_ok)

    if dep_id and not dep_ok and cfg.get("missing_dependency_note"):
        chunks.append(Padding(Text.from_markup(cfg["missing_dependency_note"]), (0, 0, 1, 0)))

    if show_table:
        chunks.append(_category_vars_table(rows))

    if show_table and dep_id and dep_ok and cfg.get("present_dependency_note"):
        chunks.append(Padding(Text.from_markup(cfg["present_dependency_note"]), (1, 0, 0, 0)))

    return [_category_section_panel(category, body_chunks=chunks)]


def _extra_vars_panel() -> Optional[Panel]:
    """Only if an ``EXTRA_ENV_VARS`` row is not merged into the live catalog (should be rare)."""
    catalog_names = {str(v["name"]) for v in ENV_VARS.values()}
    remaining = [e for e in EXTRA_ENV_VARS if str(e["name"]) not in catalog_names]
    if not remaining:
        return None

    table = Table(**HELP_REFERENCE_MATCH_TABLE_KWARGS)
    table.add_column("Variable", no_wrap=True, min_width=18)
    table.add_column("Default", min_width=8, max_width=22, no_wrap=True)
    table.add_column("Values", min_width=10, ratio=2)
    table.add_column("When", min_width=8, no_wrap=True, ratio=1)
    table.add_column("Description", min_width=18, ratio=4)

    for idx, entry in enumerate(remaining):
        body_style = "white" if idx % 2 == 0 else _GREY
        name = str(entry["name"])
        desc = str(entry.get("description", ""))
        table.add_row(
            Text(name, style=f"bold {_CAI_GREEN}"),
            Text(str(entry.get("default", "—")), style=_CAI_GREEN),
            Text(constraints_line(name, desc), style=body_style),
            Text(effective_label(name), style=body_style),
            Text(desc, style=body_style),
        )

    return Panel(
        table,
        title=_quick_guide_subpanel_title("Additional (not merged into catalog)"),
        title_align="left",
        border_style=_CAI_GREEN,
        padding=(1, 1),
    )


def print_environment_reference(console: Optional[Console] = None) -> None:
    """Print the large environment-variable reference panel."""
    out = console or Console()
    intro = Padding(Text.from_markup(INTRO_MARKUP), (0, 0, 1, 2))
    parts: List[Any] = [intro]
    for category, rows in _group_env_vars_by_category():
        parts.append(Text(""))
        parts.extend(_category_block_parts(category, rows))
    extra_panel = _extra_vars_panel()
    if extra_panel is not None:
        parts.append(Text(""))
        parts.append(extra_panel)
    parts.append(Text(""))
    parts.append(
        Text.from_markup(
            "[dim]Tip: [bold]/env list[/bold] shows numbers and live values; "
            "[bold]/env set <#|NAME> <value...>[/bold] during a session. "
            f"{usage_markup_bold()} opens long-form help for one or more variables.[/dim]"
        )
    )

    out.print(
        Panel(
            Group(*parts),
            title=environment_reference_outer_title(),
            title_align="left",
            border_style=_CAI_GREEN,
            padding=(1, 1),
        )
    )
