"""
Resume and sessions commands for CAI REPL.

This module provides commands for managing and resuming sessions:
- /resume: Pick or resume a session (replaces former ``cai --resume`` CLI flags)
- /sessions: List recent sessions
"""

from pathlib import Path
from typing import Optional

from rich.console import Console

from cai.repl.commands.base import Command, register_command
from cai.repl.session_resume import (
    DEFAULT_RECENT_SESSION_COUNT,
    find_jsonl_by_token_in_dir,
    find_jsonl_by_token_in_logs,
    find_last_session_log,
    find_newest_cai_jsonl_by_filename_prefix,
    get_session_metadata,
    list_recent_sessions,
    list_recent_sessions_in_directory,
    load_session_into_agent,
    prompt_pick_session_path,
    resume_session,
    sessions_table_from_metadatas,
)
from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
from cai.util.config_utils import get_session_logs_dir

console = Console()


class ResumeCommand(Command):
    """Command for resuming a previous session."""

    def __init__(self):
        """Initialize the resume command."""
        super().__init__(
            name="/resume",
            description="Resume a session: pick from recent logs, last, path, token, or directory",
            aliases=["/r"],
        )

    def handle(self, args: Optional[list[str]] = None) -> bool:
        """Handle the resume command.

        - No args: same recent list as ``/sessions`` (10) + numbered pick.
        - ``last``: most recent log with messages (session logs under ``~/.cai/logs``).
        - ``<path.jsonl>``: resume that file.
        - ``<dir>``: pick among up to 10 newest JSONL under that directory.
        - ``<dir> <token>``: newest JSONL under dir whose name contains token.
        - ``<token>`` (no slashes, not a file): substring match in session ``cai_*.jsonl`` files.
        """
        args = args or []
        positionals: list[str] = []
        for arg in args:
            if arg.startswith("-"):
                console.print(f"[yellow]Unknown option: {arg}[/yellow]")
            else:
                positionals.append(arg)

        log_path: Optional[str] = None

        if not positionals:
            sessions = list_recent_sessions(DEFAULT_RECENT_SESSION_COUNT)
            if not sessions:
                console.print(
                    f"[yellow]No sessions found under {get_session_logs_dir()}[/yellow]"
                )
                return True
            table = sessions_table_from_metadatas(
                sessions,
                title=f"Recent Sessions (last {len(sessions)}) — same list as /sessions",
                include_pick_hint_caption=True,
            )
            console.print(table)
            log_path = prompt_pick_session_path(sessions)
            if not log_path:
                console.print("[dim]Resume cancelled.[/dim]")
                return True

        elif positionals[0].lower() == "last":
            log_path = find_last_session_log()
            if not log_path:
                console.print("[yellow]No previous session found to resume.[/yellow]")
                console.print("[dim]Start a new session or use /sessions.[/dim]")
                return True

        else:
            first = Path(positionals[0]).expanduser()
            if first.is_file():
                log_path = str(first)
            elif first.is_dir():
                if len(positionals) >= 2:
                    token = positionals[1]
                    found = find_jsonl_by_token_in_dir(first, token)
                    if not found:
                        console.print(
                            f"[red]No .jsonl under {first} with '{token}' in the filename.[/red]"
                        )
                        return False
                    log_path = found
                else:
                    sessions = list_recent_sessions_in_directory(
                        first, limit=DEFAULT_RECENT_SESSION_COUNT
                    )
                    if not sessions:
                        console.print(
                            f"[yellow]No sessions with messages found under {first}[/yellow]"
                        )
                        return True
                    table = sessions_table_from_metadatas(
                        sessions,
                        title=f"Sessions under {first} (up to {len(sessions)})",
                        include_pick_hint_caption=True,
                    )
                    console.print(table)
                    log_path = prompt_pick_session_path(sessions)
                    if not log_path:
                        console.print("[dim]Resume cancelled.[/dim]")
                        return True
            else:
                token = positionals[0]
                found = find_jsonl_by_token_in_logs(token)
                if not found:
                    console.print(
                        f"[red]No session log under {get_session_logs_dir()} "
                        f"matching: {token}[/red]"
                    )
                    console.print("[dim]Use /sessions or pass a .jsonl path.[/dim]")
                    return False
                log_path = found

        messages, used_path, _parallel = resume_session(log_path)

        if not messages:
            console.print("[yellow]No messages to load from session[/yellow]")
            return True

        current_agent = AGENT_MANAGER.get_active_agent()
        if not current_agent:
            console.print("[red]No active agent to load history into[/red]")
            return False

        success = load_session_into_agent(current_agent, messages, log_path=used_path)

        if success:
            console.print(
                "[green]Session resumed successfully. You can continue the conversation.[/green]"
            )

        return success


class SessionsCommand(Command):
    """Command for listing recent sessions."""

    def __init__(self):
        """Initialize the sessions command."""
        super().__init__(
            name="/sessions",
            description="List recent sessions available for resuming",
            aliases=["/sess"],
        )

    def handle(self, args: Optional[list[str]] = None) -> bool:
        """Handle the sessions command.

        - No args: last 10 sessions (same set as ``/resume`` with no args).
        - ``<n>``: show last n sessions.
        - Otherwise: show details for a session id / path.
        """
        args = args or []
        limit = DEFAULT_RECENT_SESSION_COUNT

        if args:
            if args[0].isdigit():
                limit = int(args[0])
            else:
                return self._show_session_details(args[0])

        sessions = list_recent_sessions(limit)

        if not sessions:
            console.print(
                f"[yellow]No sessions found under {get_session_logs_dir()}[/yellow]"
            )
            return True

        table = sessions_table_from_metadatas(
            sessions,
            title=f"Recent Sessions (last {len(sessions)})",
            include_pick_hint_caption=True,
        )
        console.print(table)
        console.print()
        z = "#00ff9d"
        console.print("[dim #9aa0a6]Usage:[/dim #9aa0a6]")
        console.print(
            f"  [bold {z}]/resume[/bold {z}]            - Pick from these "
            f"{DEFAULT_RECENT_SESSION_COUNT} (or last 10)"
        )
        console.print(
            f"  [bold {z}]/resume last[/bold {z}]       - Resume the most recent session"
        )
        console.print(
            f"  [bold {z}]/resume <id|path>[/bold {z}]  - Resume by id token or .jsonl path"
        )
        console.print(f"  [bold {z}]/sessions <n>[/bold {z}]      - Show last n sessions")
        return True

    def _show_session_details(self, session_arg: str) -> bool:
        """Show details for a specific session."""
        log_path = None

        if session_arg.endswith(".jsonl") or "/" in session_arg or "\\" in session_arg:
            log_path = session_arg
        else:
            log_path = find_newest_cai_jsonl_by_filename_prefix(session_arg)

        if not log_path or not Path(log_path).exists():
            console.print(
                f"[bold #ff6b6b]Session not found:[/bold #ff6b6b] "
                f"[white]{session_arg}[/white]"
            )
            return False

        metadata = get_session_metadata(log_path)
        m = "#9aa0a6"
        v = "#00ff9d"

        def _kv(label: str, value: object) -> None:
            console.print(f"[dim {m}]{label}:[/dim {m}] [bold {v}]{value}[/bold {v}]")

        console.print(f"\n[bold {v}]Session details[/bold {v}]")
        console.print(f"[dim {m}]{'─' * 52}[/dim {m}]")
        _kv("File", log_path)
        _kv("Session ID", metadata.get("session_id", "N/A"))
        _kv("Start", metadata.get("start_time", "N/A"))
        _kv("End", metadata.get("end_time", "N/A"))
        _kv("Model", metadata.get("model", "N/A"))
        _kv("Agent", metadata.get("agent_name", "N/A"))
        _kv("Messages", metadata.get("message_count", 0))
        _kv("Total cost", f"${metadata.get('total_cost', 0.0):.4f}")

        active = metadata.get("active_time", 0)
        idle = metadata.get("idle_time", 0)
        _kv("Active time", f"{active:.1f}s")
        _kv("Idle time", f"{idle:.1f}s")

        console.print()
        console.print(
            f"[dim {m}]Resume with[/dim {m}] [bold {v}]/resume {session_arg}[/bold {v}]"
            f"[dim {m}] or a full `.jsonl` path.[/dim {m}]"
        )

        return True


register_command(ResumeCommand())
register_command(SessionsCommand())
