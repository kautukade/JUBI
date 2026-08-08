"""Command execution helpers for the CAI API backend."""

from __future__ import annotations

import io
from contextlib import redirect_stderr, redirect_stdout
from dataclasses import dataclass
from typing import Any, Dict, List

from cai.repl.commands import (
    COMMANDS,
    get_all_commands,
    get_command_descriptions,
    handle_command_with_autocorrect,
)


@dataclass
class CommandMetadata:
    """Describe a CAI command along with its subcommands and aliases."""

    name: str
    description: str
    aliases: List[str]
    subcommands: List[str]


@dataclass
class CommandExecutionResult:
    """Result payload returned after executing a command."""

    handled: bool
    suggested_command: str | None
    stdout: str
    stderr: str
    exit_code: int | None


class CommandExecutor:
    """Utility responsible for running REPL commands in a safe, capture-friendly way."""

    def __init__(self) -> None:
        # Warm up the registry so metadata is available on demand.
        get_all_commands()

    def describe_commands(self) -> List[CommandMetadata]:
        descriptions = get_command_descriptions()
        all_cmds = get_all_commands()
        response: List[CommandMetadata] = []
        for name, subcommands in all_cmds.items():
            cmd_obj = COMMANDS.get(name)
            aliases = cmd_obj.aliases if cmd_obj else []
            response.append(
                CommandMetadata(
                    name=name,
                    description=descriptions.get(name, ""),
                    aliases=aliases,
                    subcommands=subcommands,
                )
            )
        return response

    def run(self, command_name: str, args: List[str] | None = None, auto_correct: bool = True) -> CommandExecutionResult:
        """Execute a slash command and capture its textual output."""
        if command_name == "?":
            normalized = "?"
        else:
            normalized = command_name if command_name.startswith('/') else f'/{command_name.lstrip("/")}'
        args = args or []
        stdout_buffer = io.StringIO()
        stderr_buffer = io.StringIO()
        exit_code: int | None = None
        with redirect_stdout(stdout_buffer), redirect_stderr(stderr_buffer):
            try:
                handled, suggestion = handle_command_with_autocorrect(
                    normalized,
                    args,
                    auto_correct=auto_correct,
                )
            except SystemExit as exc:  # pragma: no cover - defensive
                handled = True
                suggestion = None
                exit_code = int(exc.code) if exc.code is not None else 0
            except Exception:  # pragma: no cover - defensive
                handled = False
                suggestion = None
        return CommandExecutionResult(
            handled=handled,
            suggested_command=suggestion,
            stdout=stdout_buffer.getvalue(),
            stderr=stderr_buffer.getvalue(),
            exit_code=exit_code,
        )
