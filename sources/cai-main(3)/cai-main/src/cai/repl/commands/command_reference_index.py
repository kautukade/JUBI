"""
Categorized slash-command index for REPL help (aligned with public Commands Reference).

Rows are resolved against ``COMMANDS`` after lazy-load so the UI stays in sync with
``register_command`` without hand-maintaining every command in multiple files.
"""

from __future__ import annotations

# (section title, primary keys as stored in ``COMMANDS``)
CLI_COMMAND_CATEGORIES: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("Agent Management", ("/agent", "/queue")),
    ("Model Management", ("/model", "/temperature", "/topp")),
    (
        "Memory & History",
        ("/memory", "/history", "/compact", "/flush", "/load", "/save", "/merge"),
    ),
    ("Environment & Configuration", ("/env", "/workspace", "/virtualization", "/config")),
    ("Tools & Integration", ("/mcp", "/shell")),
    ("Parallel Execution", ("/parallel",)),
    (
        "Session, cost & utilities",
        (
            "/resume",
            "/sessions",
            "continue",
            "/cost",
            "/context",
            "/replay",
            "/quickstart",
            "/graph",
            "/help",
            "/settings",
            "/metadebug",
            "/ctr",
            "/api",
            "/auth",
            "/exit",
        ),
    ),
)


def _display_cmd_name(primary: str) -> str:
    # Bare ``?`` is the shortcuts command; ``/?`` is an alias of ``/help``, not the same token.
    if primary == "?":
        return "?"
    return primary if primary.startswith("/") else f"/{primary}"


def _aliases_for_primary(primary: str, alias_map: dict[str, str]) -> str:
    als = sorted(a for a, p in alias_map.items() if p == primary and a != primary)
    return ", ".join(als)


def _collect_rows() -> tuple[
    list[tuple[str, list[tuple[str, str, str]]]], list[tuple[str, str, str]]
]:
    from cai.repl.commands import _ensure_all_commands_loaded
    from cai.repl.commands.base import COMMAND_ALIASES, COMMANDS

    _ensure_all_commands_loaded()

    assigned: set[str] = set()
    blocks: list[tuple[str, list[tuple[str, str, str]]]] = []

    for title, keys in CLI_COMMAND_CATEGORIES:
        rows: list[tuple[str, str, str]] = []
        seen_primary: set[str] = set()
        for key in keys:
            cmd = COMMANDS.get(key)
            if cmd is None:
                continue
            primary = cmd.name
            if primary in seen_primary:
                continue
            seen_primary.add(primary)
            if primary in assigned:
                continue
            rows.append(
                (
                    _display_cmd_name(primary),
                    _aliases_for_primary(primary, COMMAND_ALIASES),
                    cmd.description,
                )
            )
            assigned.add(primary)
        if rows:
            blocks.append((title, rows))

    other: list[tuple[str, str, str]] = []
    for primary in sorted(COMMANDS.keys(), key=lambda x: _display_cmd_name(x).lower()):
        if primary in assigned:
            continue
        cmd = COMMANDS[primary]
        other.append(
            (
                _display_cmd_name(primary),
                _aliases_for_primary(primary, COMMAND_ALIASES),
                cmd.description,
            )
        )
    if other:
        blocks.append(("Other", other))

    flat: list[tuple[str, str, str]] = []
    for _, rs in blocks:
        flat.extend(rs)
    return blocks, flat


def categorized_command_tables() -> list[tuple[str, list[tuple[str, str, str]]]]:
    """For ``/help commands`` — (category, [(command, aliases, description), ...])."""
    blocks, _ = _collect_rows()
    return blocks


def help_topic_rows_by_category() -> list[tuple[str, list[tuple[str, str]]]]:
    """For ``/help topics`` — slash commands by category ``[(category, [(cmd, desc), ...]), ...]``."""
    blocks, _ = _collect_rows()
    return [(title, [(a, c) for a, _, c in rows]) for title, rows in blocks]
