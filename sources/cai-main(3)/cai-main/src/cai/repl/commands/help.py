"""
Help command for CAI REPL.
This module provides commands for displaying help information.
"""

from typing import List, Optional

try:
    from rich.console import Console
    from rich.panel import Panel
    from rich.table import Table
    from rich.text import Text
except ImportError as exc:
    raise ImportError(
        "The 'rich' package is required. Please install it with: pip install rich"
    ) from exc

from cai.repl.commands.base import COMMAND_ALIASES, COMMANDS, Command, register_command
from cai.repl.commands.command_reference_index import categorized_command_tables
from cai.repl.commands.config import print_config_deprecated_message
from cai.repl.commands.settings_cli_catalog import settings_help_panel_subcommand_bullets
from cai.repl.ui.banner import _CAI_GREEN, _quick_guide_subpanel_title

console = Console()


def _h_panel_desc(text: str) -> str:
    """One-line intro for ``/h <topic>`` panels (below the green title bar; body, not dim)."""
    return f"[white]{text}[/white]\n\n"


def create_styled_table(
    title: Optional[str],
    headers: List[tuple[str, str]],
    header_style: str = "bold white",
) -> Table:
    """Create a styled table with consistent formatting.

    Args:
        title: The table title
        headers: List of (header_name, style) tuples
        header_style: Style for the header row

    Returns:
        A configured Table instance
    """
    table = Table(title=title, show_header=True, header_style=header_style)
    for header, style in headers:
        table.add_column(header, style=style)
    return table


def create_notes_panel(
    notes: List[str], title: str = "Notes", border_style: str | None = None
) -> Panel:
    """Create a notes panel with consistent formatting.

    Args:
        notes: List of note strings
        title: Panel title
        border_style: Style for the panel border (defaults to CAI green)

    Returns:
        A configured Panel instance
    """
    notes_text = Text.from_markup("\n".join(f"• {note}" for note in notes))
    return Panel(
        notes_text,
        title=_quick_guide_subpanel_title(title),
        title_align="left",
        border_style=border_style or _CAI_GREEN,
        padding=(1, 1),
    )


def model_help_panel_markup() -> str:
    """Rich markup for ``/h model`` (authoritative model CLI syntax)."""
    z = _CAI_GREEN
    return (
        _h_panel_desc(
            "Model selection: browse and set CAI_MODEL from the short table or the full catalog."
        )
        + f"[bold {z}]Syntax[/bold {z}]\n"
        f"• [bold {z}]/model[/bold {z}] — current model and short table\n"
        f"• [bold {z}]/model show[/bold {z}] — full LiteLLM catalog\n"
        f"• [bold {z}]/model show supported[/bold {z}] — function-calling models only\n"
        f"• [bold {z}]/model show <term>[/bold {z}] — filter by name\n"
        f"• [bold {z}]/model show supported <term>[/bold {z}] — filter supported set\n"
        f"• [bold {z}]/model <name>[/bold {z}] or [bold {z}]/model <n>[/bold {z}] — set "
        f"[bold]CAI_MODEL[/bold] if the id exists in the loaded catalog (applies next turn)\n\n"
        f"[bold {z}]Notes[/bold {z}]\n"
        f"• API keys: [bold {z}]/env list[/bold {z}]\n"
        f"• Row numbers match [bold {z}]/model show[/bold {z}]; the short table skips "
        "LiteLLM-only slots\n\n"
        f"[dim]Alias: /mod[/dim]"
    )


def graph_help_panel_markup() -> str:
    """Rich markup for ``/h graph`` (aligned with ``GraphCommand`` subcommands)."""
    z = _CAI_GREEN
    return (
        _h_panel_desc(
            "Graph views show user, assistant, and tool messages; export to json, dot, or mermaid."
        )
        + f"[bold {z}]Available Commands:[/bold {z}]\n"
        f"• [bold {z}]/graph[/bold {z}] or [bold {z}]/g[/bold {z}] — "
        "multi-agent layout when [bold]CAI_PARALLEL[/bold]>1 or multiple parallel slots exist; "
        "otherwise the active agent\n"
        f"• [bold {z}]/graph show[/bold {z}] — same as bare [bold {z}]/graph[/bold {z}]\n"
        f"• [bold {z}]/graph P1[/bold {z}] — graph for parallel agent by id (e.g. P2, P3)\n"
        f"• [bold {z}]/graph <agent_name>[/bold {z}] — graph for a specific agent (name may include spaces)\n"
        f"• [bold {z}]/graph all[/bold {z}] — graphs for every agent that has history\n"
        f"• [bold {z}]/graph timeline[/bold {z}] — table of messages per agent (by message index)\n"
        f"• [bold {z}]/graph stats[/bold {z}] — per-agent message and tool-call counts\n"
        f"• [bold {z}]/graph export <format>[/bold {z}] — export data (optional filename)\n\n"
        f"[bold {z}]Examples:[/bold {z}]\n"
        f"• [bold {z}]/graph[/bold {z}] — current context graph\n"
        f"• [bold {z}]/graph P2[/bold {z}] — graph for agent P2\n"
        f"• [bold {z}]/graph red_teamer[/bold {z}] — graph for that agent\n"
        f"• [bold {z}]/graph timeline[/bold {z}] — message table\n"
        f"• [bold {z}]/graph stats[/bold {z}] — statistics\n"
        f"• [bold {z}]/graph export mermaid graph.md[/bold {z}] — write a Mermaid file\n"
        f"• [bold {z}]/g timeline[/bold {z}] — same via alias\n\n"
        f"[bold {z}]Features:[/bold {z}]\n"
        "• Multi-agent panels in parallel mode\n"
        "• User, assistant, and tool-call flow in the graph\n"
        "• Timeline table for cross-agent review (index order, not wall-clock)\n"
        "• Stats across agents\n"
        "• Export full tracked histories to a file\n\n"
        "[dim]Exports: json (full messages), dot (Graphviz), mermaid (diagram text). "
        "Optional path defaults to a timestamped name in the cwd.[/dim]\n\n"
        f"[dim]Alias: /g[/dim]"
    )


def cost_help_panel_markup() -> str:
    """Rich markup for ``/h cost`` (aligned with ``CostCommand`` subcommands)."""
    z = _CAI_GREEN
    return (
        _h_panel_desc(
            "Current session spend and tokens, plus global totals from ~/.cai/usage.json when "
            "usage tracking is enabled."
        )
        + f"[bold {z}]Syntax[/bold {z}]\n"
        f"• [bold {z}]/cost[/bold {z}] or [bold {z}]/cost summary[/bold {z}]: "
        "session + global panels, top models snippet, hints for other views\n"
        f"• [bold {z}]/cost models[/bold {z}] — per-model costs and share\n"
        f"• [bold {z}]/cost daily[/bold {z}] — last 30 days and weekly rollup\n"
        f"• [bold {z}]/cost sessions[/bold {z}] — recent sessions (default 10 rows); "
        f"optional numeric arg limits rows (e.g. [bold {z}]/cost sessions 5[/bold {z}])\n"
        f"• [bold {z}]/cost reset[/bold {z}] — clear persisted stats (type RESET to confirm; backup first)\n\n"
        f"[bold {z}]Related[/bold {z}]\n"
        f"• [bold {z}]/context[/bold {z}] — where context tokens go (per-role estimates + heavy messages)\n\n"
        f"[bold {z}]Notes[/bold {z}]\n"
        "• Cache tokens (read/write) are shown when the backend reports them (provider-dependent)\n"
        "• Some views use local estimation; provider tokenization can differ by model\n\n"
        f"[dim]Aliases: /costs, /usage. Global file tracking off: CAI_DISABLE_USAGE_TRACKING=true[/dim]"
    )


def auth_help_panel_markup() -> str:
    """Rich markup for ``/h auth`` (aligned with ``AuthCommand`` subcommands)."""
    z = _CAI_GREEN
    return (
        _h_panel_desc(
            "Persisted API users (`AuthManager`): add a named account or pair a device by IP."
        )
        + f"[bold {z}]Syntax[/bold {z}]\n"
        f"• [bold {z}]/auth add-user <username> <password>[/bold {z}] — register user\n"
        f"• [bold {z}]/auth add-ip <ip[:port]>[/bold {z}] — random user + session, JSON over TCP to "
        "the device listener (device port from [bold]CAI_AUTH_DEVICE_PORT[/bold])\n\n"
        f"[dim]Device [bold]base_url[/bold]: [bold]CAI_AUTH_BASE_URL[/bold] if set, else public host/"
        f"port envs and [bold]CAI_API_*[/bold]. Bare [bold {z}]/auth[/bold {z}] lists required "
        "subcommands.[/dim]"
    )


def commands_reference_panel_markup() -> str:
    """Full Rich markup for ``/h commands`` (single bordered panel, like ``/h agent``)."""
    z = _CAI_GREEN
    parts: list[str] = [
        _h_panel_desc(
            "All available commands"
        )
    ]
    for category, commands in categorized_command_tables():
        parts.append(f"[bold {z}]{category}[/bold {z}]\n")
        for cmd, aliases, desc in commands:
            alias_suffix = (
                f" [dim]({aliases})[/dim]" if aliases and aliases.strip() else ""
            )
            parts.append(
                f"• [bold {z}]{cmd}[/bold {z}]{alias_suffix} — [white]{desc}[/white]\n"
            )
        parts.append("\n")
    parts.append(
        "[dim]• /h <topic> — e.g. agent, env, model (command index: /help topics)[/dim]"
    )
    return "".join(parts)


class HelpCommand(Command):
    """Command for displaying help information."""

    def __init__(self):
        """Initialize the help command."""
        super().__init__(
            name="/help",
            description=("Display help information about commands and features"),
            aliases=["/h", "/?"],
        )

        # Add subcommands organized by category
        # Agent Management
        self.add_subcommand("agent", "Display help for agent commands", self.handle_agent)
        self.add_subcommand("parallel", "Display help for parallel execution", self.handle_parallel)
        self.add_subcommand("queue", "Display help for queue command", self.handle_queue)

        # Memory & History
        self.add_subcommand("memory", "Display help for memory persistence", self.handle_memory)
        self.add_subcommand("history", "Display help for conversation history", self.handle_history)
        self.add_subcommand(
            "compact", "Display help for conversation compaction", self.handle_compact
        )
        self.add_subcommand("flush", "Display help for clearing histories", self.handle_flush)
        self.add_subcommand("load", "Display help for loading JSONL files", self.handle_load)
        self.add_subcommand("save", "Display help for saving conversation JSONL", self.handle_save)
        self.add_subcommand(
            "merge", "Display help for merging agent histories", self.handle_merge_help
        )

        self.add_subcommand("config", "Deprecated — same notice as /config; use /env", self.handle_config)
        self.add_subcommand("env", "Display help for environment variables", self.handle_env)
        self.add_subcommand(
            "var",
            "Long-form help for environment variables (/help var NAME)",
            self.handle_var,
        )
        self.add_subcommand(
            "workspace", "Display help for workspace management", self.handle_workspace
        )
        self.add_subcommand(
            "virtualization", "Display help for Docker containers", self.handle_virtualization
        )

        # Tools & Integration
        self.add_subcommand("mcp", "Display help for Model Context Protocol", self.handle_mcp)
        self.add_subcommand("shell", "Display help for shell commands", self.handle_shell)

        # Utilities
        self.add_subcommand("model", "Display help for model selection", self.handle_model)
        self.add_subcommand("graph", "Display help for visualization", self.handle_graph)
        self.add_subcommand(
            "aliases",
            "List registered command shortcuts",
            self.handle_aliases,
        )

        # Session & Cost
        self.add_subcommand("cost", "Display help for cost tracking", self.handle_cost)
        self.add_subcommand("context", "Display help for context usage", self.handle_context)
        self.add_subcommand("exit", "Display help for exiting CAI", self.handle_exit)
        self.add_subcommand("resume", "Display help for session resume", self.handle_resume)
        self.add_subcommand("sessions", "Display help for session listing", self.handle_sessions)
        self.add_subcommand("replay", "Display help for session replay", self.handle_replay)
        self.add_subcommand(
            "continue", "Display help for continuation mode", self.handle_continue
        )

        # Model Tuning
        self.add_subcommand(
            "temperature", "Display help for temperature adjustment", self.handle_temperature
        )
        self.add_subcommand("topp", "Display help for top-p adjustment", self.handle_topp)

        # Advanced
        self.add_subcommand(
            "settings", "Help for /settings (alias /set)", self.handle_settings
        )
        self.add_subcommand("auth", "Display help for API authentication", self.handle_auth)
        self.add_subcommand("ctr", "Display help for CTR security analysis", self.handle_ctr)
        self.add_subcommand("api", "Help for /api: ALIAS_API_KEY in .env (Alias / CAI PRO)", self.handle_api)
        self.add_subcommand(
            "metadebug", "Display help for meta-agent debugging", self.handle_metadebug
        )

        # General
        self.add_subcommand("commands", "List all available commands", self.handle_commands)
        self.add_subcommand(
            "topics",
            "Slash commands by category + /help <topic> hints; bare /help adds env tables",
            self.handle_help_topics,
        )

    def handle_unknown_subcommand(self, subcommand: str) -> bool:
        """Legacy help tokens ``quick`` / ``quickstart`` → point to ``/quickstart``."""
        if subcommand in ("quick", "quickstart"):
            console.print(
                "[dim]There is no /help quick or /help quickstart; use [bold]/quickstart[/bold] "
                "(aliases: /qs, /quick).[/dim]"
            )
            return False
        return super().handle_unknown_subcommand(subcommand)

    def handle_memory(self, _: Optional[List[str]] = None) -> bool:
        """Show help for memory commands."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc("Memory: save, restore, and manage agent conversation snapshots.")
                + f"[bold {z}]Available Commands:[/bold {z}]\n"
                "• [bold #00ff9d]/memory list[/bold #00ff9d] - List saved memory snapshots\n"
                "• [bold #00ff9d]/memory save [name] [agent][/bold #00ff9d] - Save current agent history\n"
                "• [bold #00ff9d]/memory apply <ID|name> [agent|all][/bold #00ff9d] - Apply memory to an agent\n"
                "• [bold #00ff9d]/memory show <ID|name>[/bold #00ff9d] - Show memory contents\n"
                "• [bold #00ff9d]/memory delete <ID|name>[/bold #00ff9d] - Delete a saved memory\n"
                "• [bold #00ff9d]/memory merge <ID1> <ID2> [name][/bold #00ff9d] - Merge memories into one\n"
                "• [bold #00ff9d]/memory status[/bold #00ff9d] - Show currently applied memories\n"
                "• [bold #00ff9d]/memory compact <agent|all>[/bold #00ff9d] - Compact and save agent history\n"
                "• [bold #00ff9d]/memory remove <memory_id> <agent>[/bold #00ff9d] - Remove a specific memory from agent\n"
                "• [bold #00ff9d]/memory clear <agent>[/bold #00ff9d] - Clear all memories from an agent\n"
                "• [bold #00ff9d]/memory list-applied [agent][/bold #00ff9d] - Show which memories are applied\n\n"
                f"[bold {z}]Examples:[/bold {z}]\n"
                "• [bold #00ff9d]/memory save pentest_login_flow[/bold #00ff9d] - Save with a custom name\n"
                "• [bold #00ff9d]/memory save[/bold #00ff9d] - Save with auto-generated name\n"
                "• [bold #00ff9d]/memory show M001[/bold #00ff9d] - Inspect memory by ID\n"
                "• [bold #00ff9d]/memory apply M001 P1[/bold #00ff9d] - Apply a memory to agent P1\n"
                "• [bold #00ff9d]/memory delete M001[/bold #00ff9d] - Delete memory by ID\n"
                "• [bold #00ff9d]/memory compact red_teamer[/bold #00ff9d] - Compact a single agent\n"
                "• [bold #00ff9d]/memory remove M001 red_teamer[/bold #00ff9d] - Remove one memory from agent\n"
                "• [bold #00ff9d]/memory clear red_teamer[/bold #00ff9d] - Clear all memories from agent\n"
                "• [bold #00ff9d]/memory list-applied[/bold #00ff9d] - Show all applied memories\n\n"
                f"[bold {z}]Recommended flow:[/bold {z}]\n"
                "• 1. Select/use an agent (e.g. /agent red_teamer)\n"
                "• 2. Send at least one normal prompt (non-command)\n"
                "• 3. Run /memory save <name>\n"
                "• 4. Use /memory list and /memory show to verify\n"
                "• 5. Apply with /memory apply when needed\n\n"
                f"[bold {z}]Notes:[/bold {z}]\n"
                "• If '/memory save' reports no history, send a prompt first\n"
                "• Memory IDs (e.g., M001) are the safest way to reference memories\n"
                "• Use '/memory status' to see what is currently applied\n\n"
                "[dim]Alias: /mem[/dim]",
                title=_quick_guide_subpanel_title("Memory Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_agent(self, _: Optional[List[str]] = None) -> bool:
        """Show help for agent management."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "Agents are autonomous AI assistants. Default CLI entry is "
                    "[bold]selection_agent[/bold] (handoff-only router); "
                    "[bold]orchestration_agent[/bold] [bold white on bright_red] BETA [/] adds "
                    "breadth-first routing with specialist tools. "
                    "See [bold]/help var CAI_AGENT_TYPE[/bold] and [bold]/help var CAI_ORCHESTRATION_*[/bold]."
                )
                + f"[bold {z}]Available Commands:[/bold {z}]\n"
                "• [bold #00ff9d]/agent list[/bold #00ff9d] - List all available agents\n"
                "• [bold #00ff9d]/agent select <name>[/bold #00ff9d] - Switch to a specific agent\n"
                "• [bold #00ff9d]/agent info <name>[/bold #00ff9d] - Show agent details and tools\n"
                "• [bold #00ff9d]/agent current[/bold #00ff9d] - Show current agent configuration\n\n"
                f"[bold {z}]Examples:[/bold {z}]\n"
                "• [bold #00ff9d]/agent list[/bold #00ff9d] - See all available agents\n"
                "• [bold #00ff9d]/agent select [/bold #00ff9d][red]red_teamer[/red]"
                " [dim]- Switch to offensive security agent[/dim]\n"
                "• [bold #00ff9d]/agent info [/bold #00ff9d][red]bug_bounter[/red]"
                " [dim]- View bug bounty agent details[/dim]\n"
                "• [bold #00ff9d]/a select [/bold #00ff9d][red]2[/red] [dim]- Select agent by number (alias)[/dim]\n\n"
                f"[bold {z}]Available agents:[/bold {z}]\n"
                "• [bold #00ff9d]selection_agent[/bold #00ff9d] - Default entry: handoff-only router "
                "(no orchestration specialist tools)\n"
                "• [bold #00ff9d]orchestration_agent[/bold #00ff9d] [bold white on bright_red] BETA [/] - "
                "Routing plus [bold]run_specialist[/bold], dual contest, and "
                "[bold]run_parallel_specialists[/bold] "
                "(tune workers with [bold]CAI_ORCHESTRATION_WORKER_MAX_TURNS[/bold]; optional multi-front "
                "nudge: [bold]CAI_ORCHESTRATION_MAS_HINT[/bold])\n"
                "• [bold #00ff9d]one_tool_agent[/bold #00ff9d] - Basic CTF solver\n"
                "• [bold #00ff9d]red_teamer[/bold #00ff9d] - Offensive security specialist\n"
                "• [bold #00ff9d]blue_teamer[/bold #00ff9d] - Defensive security specialist\n"
                "• [bold #00ff9d]bug_bounter[/bold #00ff9d] - Bug bounty hunter\n"
                "• [bold #00ff9d]dfir[/bold #00ff9d] - Digital forensics & incident response\n"
                "• [bold #00ff9d]network_traffic_analyzer[/bold #00ff9d] - Network analysis\n"
                "• [bold #00ff9d]flag_discriminator[/bold #00ff9d] - CTF flag extraction\n"
                "• [bold #00ff9d]codeagent[/bold #00ff9d] - Code generation and analysis\n"
                "• [bold #00ff9d]thought[/bold #00ff9d] - Strategic planning\n\n"
                "[dim]Alias: /a[/dim]",
                title=_quick_guide_subpanel_title("Agent Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_context(self, _: Optional[List[str]] = None) -> bool:
        """Show help for context usage breakdown."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "Context usage: inspect where tokens are going (per-role estimates and heavy messages). "
                    "Useful for diagnosing fast token growth and deciding when to compact."
                )
                + f"[bold {z}]Available Commands:[/bold {z}]\n"
                "• [bold #00ff9d]/context[/bold #00ff9d] - Show per-role context estimate (system/user/assistant/tool)\n"
                "• [bold #00ff9d]/context top[/bold #00ff9d] - Show biggest messages by estimated tokens (default 8)\n"
                "• [bold #00ff9d]/context top 20[/bold #00ff9d] - Show top 20 heavy messages\n\n"
                f"[bold {z}]Notes:[/bold {z}]\n"
                "• Estimates count message role+content only; system prompts and tool schemas add extra overhead\n"
                "• Provider tokenization can differ from local estimates depending on the model\n\n"
                "[dim]Alias: /ctx[/dim]",
                title=_quick_guide_subpanel_title("Context Usage"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_graph(self, _: Optional[List[str]] = None) -> bool:
        """Show help for graph visualization."""
        console.print(
            Panel(
                graph_help_panel_markup(),
                title=_quick_guide_subpanel_title("Graph Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_shell(self, _: Optional[List[str]] = None) -> bool:
        """Show help for shell command execution."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "Shell: run commands in the workspace cwd, or in the container when "
                    "CAI_ACTIVE_CONTAINER is set."
                )
                + f"[bold {z}]Available Commands:[/bold {z}]\n"
                "• [bold #00ff9d]/shell <command>[/bold #00ff9d] - Execute a shell command\n\n"
                f"[bold {z}]Aliases:[/bold {z}]\n"
                "• [bold #00ff9d]/s[/bold #00ff9d], [bold #00ff9d]$[/bold #00ff9d] - Shorthand for /shell\n\n"
                f"[bold {z}]Examples:[/bold {z}]\n"
                "• [bold #00ff9d]/shell ls -la[/bold #00ff9d] [dim]- List files[/dim]\n"
                "• [bold #00ff9d]/s pwd[/bold #00ff9d] [dim]- Show current directory[/dim]\n"
                "• [bold #00ff9d]$ git status[/bold #00ff9d] [dim]- Git status[/dim]",
                title=_quick_guide_subpanel_title("Shell Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_env(self, _: Optional[List[str]] = None) -> bool:
        """Show help for environment variables."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "Environment: session keys and full catalog use the same tables as bare /help."
                )
                + f"[bold {z}]Available Commands:[/bold {z}]\n"
                "• [bold #00ff9d]/env[/bold #00ff9d] — "
                "only [dim]CAI_[/dim] / [dim]CTF_[/dim] keys currently set in the process "
                "(same as before)\n"
                "• [bold #00ff9d]/env list[/bold #00ff9d] — "
                "every catalog variable (#, current, default, values, when, description)\n"
                "• [bold #00ff9d]/env get <n|NAME>[/bold #00ff9d] — show one catalog entry\n"
                "• [bold #00ff9d]/env set <n|NAME> <value>[/bold #00ff9d] — set by number or "
                "name (value may include spaces; no quotes)\n"
                "• [bold #00ff9d]/env default[/bold #00ff9d] — restore all catalog variables to "
                "registered defaults\n\n"
                f"[bold {z}]Notes:[/bold {z}]\n"
                "• Example catalog entry: [bold]CAI_MODEL[/bold] ([bold]/env list[/bold], "
                "[bold]/help var CAI_MODEL[/bold]).\n"
                "• Bare [bold]/help[/bold] still includes the full environment reference tables.\n\n"
                "[dim]Alias: /e[/dim]",
                title=_quick_guide_subpanel_title("Environment Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_var(self, args: Optional[List[str]] = None) -> bool:
        """Long-form help for one or more environment variables."""
        from cai.repl.commands.env_var_help import example_cyan_line, usage_markup_bold, render_variable_help
        from cai.repl.ui.banner import environment_reference_outer_title

        if not args or not any(a.strip() for a in args):
            console.print(
                Panel(
                    Text.from_markup(
                        _h_panel_desc(
                            "Long-form rows for one catalog variable (full tables stay on bare /help)."
                        )
                        + f"{usage_markup_bold()}  [dim](one or more names)[/dim]\n\n"
                        "Detailed help for a single variable from the tables under "
                        "[bold]/help[/bold]: "
                        "type, when it applies, default, and copy-paste examples.\n\n"
                        "[bold]Examples[/bold]\n"
                        f"{example_cyan_line('CAI_MODEL')}\n"
                        f"{example_cyan_line('CAI_DEBUG')}\n\n"
                        "[dim]All documented variables (including former “Additional”) are in the /env catalog.[/dim]"
                    ),
                    title=environment_reference_outer_title(),
                    title_align="left",
                    border_style=_CAI_GREEN,
                    padding=(1, 1),
                )
            )
            return True

        all_ok = True
        for token in args:
            raw = token.strip()
            if not raw:
                continue
            ok, canonical, body = render_variable_help(raw)
            if not ok:
                all_ok = False
            title_text = _quick_guide_subpanel_title(
                f"Variable — {canonical}" if ok else f"Unknown — {canonical}"
            )
            style = _CAI_GREEN if ok else "red"
            console.print(
                Panel(
                    Text.from_markup(body),
                    title=title_text,
                    title_align="left",
                    border_style=style,
                    padding=(1, 1),
                )
            )
        return all_ok

    def handle_aliases(self, _: Optional[List[str]] = None) -> bool:
        """Show all command aliases."""
        return self.handle_help_aliases()

    def handle_model(self, _: Optional[List[str]] = None) -> bool:
        """Show help for model selection."""
        console.print(
            Panel(
                Text.from_markup(model_help_panel_markup(), overflow="fold"),
                title=_quick_guide_subpanel_title("Model Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_config(self, _: Optional[List[str]] = None) -> bool:
        """Legacy ``/help config`` — deprecation notice only (use ``/env``)."""
        print_config_deprecated_message(console)
        return True

    def handle_no_args(self) -> bool:
        """Show the full quick guide (startup scaffolding panel; on demand only)."""
        from cai.repl.commands.environment_reference import print_environment_reference
        from cai.repl.ui.banner import display_quick_guide

        display_quick_guide(console)
        console.print()
        print_environment_reference(console)
        return True

    def _print_command_table(
        self,
        title: str,
        commands: List[tuple[str, str, str]],
        header_style: str | None = None,
        command_style: str | None = None,
        alias_column: str = "Alias",
    ) -> None:
        """Print a table of commands with consistent formatting."""
        z = _CAI_GREEN
        hs = header_style if header_style is not None else f"bold {z}"
        cs = command_style if command_style is not None else f"bold {z}"
        table = create_styled_table(
            title,
            [
                ("Command", cs),
                (alias_column, "#9aa0a6"),
                ("Description", "white"),
            ],
            hs,
        )

        for cmd, alias, desc in commands:
            table.add_row(cmd, alias, desc)

        console.print(table)

    def handle_help_topics(self, _: Optional[List[str]] = None) -> bool:
        """Topic index: intro, categorized commands, tips (no environment-variable tables)."""
        from cai.repl.ui.banner import display_help_topics_index

        display_help_topics_index(console)
        return True

    def handle_help(self) -> bool:
        """Same output as bare ``/help``: two-column quick guide + env reference."""
        return self.handle_no_args()

    def handle_help_aliases(self) -> bool:
        """Show all command aliases in a well-formatted table."""
        z = _CAI_GREEN
        accent = f"bold {z}"
        intro = (
            f"[bold]Command Aliases[/bold]"
        )
        console.print(
            Panel(
                Text.from_markup(intro, overflow="fold"),
                title=_quick_guide_subpanel_title("Aliases"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )

        alias_table = create_styled_table(
            None,
            [
                ("Alias", accent),
                ("Command", accent),
                ("Description", "white"),
            ],
            accent,
        )

        for alias, command in sorted(COMMAND_ALIASES.items()):
            cmd = COMMANDS.get(command)
            description = cmd.description if cmd else ""
            alias_table.add_row(alias, command, description)

        console.print(alias_table)

        tips = [
            "Use the alias as the first token of the line, same as the full command name.",
            (
                f"Example: [bold {z}]/a list[/bold {z}] for [bold {z}]/agent list[/bold {z}], or "
                f"[bold {z}]/mem list[/bold {z}] for [bold {z}]/memory list[/bold {z}]."
            ),
            "[dim]Shell: `$` only works at the start of the line (same routing rule as `/`).[/dim]",
        ]
        console.print("\n")
        console.print(create_notes_panel(tips, "Tips"))

        return True

    def handle_parallel(self, _: Optional[List[str]] = None) -> bool:
        """Show help for parallel execution."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "Parallel execution: run several agents at once with isolated histories, then merge."
                )
                + f"[bold {z}]Available Commands:[/bold {z}]\n"
                "• [bold #00ff9d]/parallel[/bold #00ff9d] - Show current configuration\n"
                "• [bold #00ff9d]/parallel add <agent>[/bold #00ff9d] - Add agent to parallel config\n"
                "• [bold #00ff9d]/parallel run[/bold #00ff9d] - Execute configured parallel agents\n"
                "• [bold #00ff9d]/parallel list[/bold #00ff9d] - List configured agents\n"
                "• [bold #00ff9d]/parallel clear[/bold #00ff9d] - Clear all configurations\n"
                "• [bold #00ff9d]/parallel remove <index>[/bold #00ff9d] - Remove specific agent\n"
                "• [bold #00ff9d]/parallel override-models[/bold #00ff9d] - Use global model for all\n"
                "• [bold #00ff9d]/parallel merge <indices>[/bold #00ff9d] - Merge agent histories\n"
                "• [bold #00ff9d]/parallel prompt <index> <text>[/bold #00ff9d] - Set custom prompt\n\n"
                f"[bold {z}]Examples:[/bold {z}]\n"
                "• [bold #00ff9d]/parallel add red_teamer[/bold #00ff9d] - Add red team agent\n"
                "• [bold #00ff9d]/parallel prompt P1 Analyze service exposure[/bold #00ff9d]\n"
                "• [bold #00ff9d]/parallel run[/bold #00ff9d] - Execute configured parallel agents\n"
                "• [bold #00ff9d]/merge[/bold #00ff9d] - Merge and auto-exit parallel mode\n"
                "• [bold #00ff9d]/p list[/bold #00ff9d] - Show all configured agents\n\n"
                f"[bold {z}]Notes:[/bold {z}]\n"
                "• Agents run independently with isolated contexts\n"
                "• Each agent gets a unique ID (P1, P2, etc.)\n"
                "• Results are displayed side-by-side\n"
                "• /merge merges all parallel agent contexts and exits parallel mode\n"
                "• /parallel clear exits parallel mode without merging contexts\n"
                "• /parallel add <agent> --model alias1 sets a custom model\n"
                "• Use CAI_PARALLEL env var to set default count\n\n"
                "[dim]Aliases: /par, /p[/dim]",
                title=_quick_guide_subpanel_title("Parallel Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_queue(self, _: Optional[List[str]] = None) -> bool:
        """Show help for queue commands."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc("Queue: line up prompts for sequential runs on the active or chosen agent.")
                + f"[bold {z}]Available Commands:[/bold {z}]\n"
                "• [bold #00ff9d]/queue or /queue show[/bold #00ff9d] - Show queue status\n"
                "• [bold #00ff9d]/queue add <prompt>[/bold #00ff9d] - Queue a prompt (active agent)\n"
                "• [bold #00ff9d]/queue add --agent <name> <prompt>[/bold #00ff9d] - Queue with specific agent\n"
                "• [bold #00ff9d]/queue list[/bold #00ff9d] - List queued prompts\n"
                "• [bold #00ff9d]/queue clear[/bold #00ff9d] - Clear queued prompts\n"
                "• [bold #00ff9d]/queue remove <index>[/bold #00ff9d] - Remove one queued prompt\n"
                "• [bold #00ff9d]/queue move <from> <to>[/bold #00ff9d] - Move prompt to new position\n"
                "• [bold #00ff9d]/queue next[/bold #00ff9d] - Show the next prompt in queue\n"
                "• [bold #00ff9d]/queue load <file>[/bold #00ff9d] - Load prompts from file\n"
                "• [bold #00ff9d]/queue run[/bold #00ff9d] - Execute all queued prompts\n\n"
                f"[bold {z}]Examples:[/bold {z}]\n"
                '• [bold #00ff9d]/queue add Analyze this target service[/bold #00ff9d]\n'
                "• [bold #00ff9d]/queue add --agent red_teamer scan target[/bold #00ff9d]\n"
                "• [bold #00ff9d]/queue move 3 1[/bold #00ff9d] - Move item #3 to position #1\n"
                "• [bold #00ff9d]/queue clear[/bold #00ff9d] - Clear the queue\n"
                "• [bold #00ff9d]/queue load prompts.txt[/bold #00ff9d] - Load prompts from file\n\n"
                "[dim]Alias: /que[/dim]",
                title=_quick_guide_subpanel_title("Queue Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_history(self, _: Optional[List[str]] = None) -> bool:
        """Show help for conversation history."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "History: browse, search, and drill into per-agent transcripts and parallel slots."
                )
                + f"[bold {z}]Available Commands:[/bold {z}]\n"
                "• [bold #00ff9d]/history[/bold #00ff9d] - Show control panel (tree view of agents)\n"
                "• [bold #00ff9d]/history all[/bold #00ff9d] - Display all agent histories chronologically\n"
                "• [bold #00ff9d]/history <agent>[/bold #00ff9d] or [bold #00ff9d]/history <ID>[/bold #00ff9d]"
                " - Show specific agent history\n"
                "• [bold #00ff9d]/history agent <name>[/bold #00ff9d] - Show history by agent name\n"
                "• [bold #00ff9d]/history search <term>[/bold #00ff9d] - Search messages across all agents\n"
                "• [bold #00ff9d]/history index <agent> <index> [role][/bold #00ff9d]"
                " - Show specific message by index\n\n"
                f"[bold {z}]Examples:[/bold {z}]\n"
                "• [bold #00ff9d]/history[/bold #00ff9d] - View agent control panel\n"
                "• [bold #00ff9d]/history P1[/bold #00ff9d] - Show P1's conversation\n"
                "• [bold #00ff9d]/history agent red_teamer[/bold #00ff9d] - Show red_teamer's history\n"
                '• [bold #00ff9d]/history search "password"[/bold #00ff9d] - Search for term\n'
                "• [bold #00ff9d]/history index red_teamer 5[/bold #00ff9d] - Show message #5\n"
                "• [bold #00ff9d]/history index P1 3 user[/bold #00ff9d] - Show 3rd user message from P1\n\n"
                f"[bold {z}]Features:[/bold {z}]\n"
                "• Message count and role breakdown per agent\n"
                "• Message role visualization (color-coded)\n"
                "• Tool call details\n"
                "• Use [bold #00ff9d]/save <file>.jsonl[/bold #00ff9d] for snapshots ([bold #00ff9d]/load[/bold #00ff9d]); "
                "[bold #00ff9d].md[/bold #00ff9d] for readable exports\n"
                "• Memory status indicator\n"
                "• Parallel agent support (isolated histories)\n\n"
                "[dim]Alias: /his[/dim]",
                title=_quick_guide_subpanel_title("History Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_compact(self, _: Optional[List[str]] = None) -> bool:
        """Show help for conversation compaction."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "Compaction: use a summarization model to shrink long threads and free context."
                )
                + f"[bold {z}]Available Commands:[/bold {z}]\n"
                "• [bold #00ff9d]/compact[/bold #00ff9d] - Compact current conversation\n"
                "• [bold #00ff9d]/compact model <name>[/bold #00ff9d] - Set compaction model by name\n"
                "• [bold #00ff9d]/compact model <number>[/bold #00ff9d] - Set compaction model by table number\n"
                "• [bold #00ff9d]/compact model default[/bold #00ff9d] - Reset to current agent model\n"
                "• [bold #00ff9d]/compact prompt <text>[/bold #00ff9d] - Set custom summarization prompt\n"
                "• [bold #00ff9d]/compact prompt reset[/bold #00ff9d] - Reset to default prompt\n"
                "• [bold #00ff9d]/compact status[/bold #00ff9d] - Show current settings\n\n"
                f"[bold {z}]Inline flags (one-time override):[/bold {z}]\n"
                "• [bold #00ff9d]/compact --model <model>[/bold #00ff9d] - Compact with a specific model\n"
                "• [bold #00ff9d]/compact --prompt <text>[/bold #00ff9d] - Compact with a custom prompt\n\n"
                f"[bold {z}]Examples:[/bold {z}]\n"
                "• [bold #00ff9d]/compact model o3-mini[/bold #00ff9d] - Set O3 Mini as compaction model\n"
                "• [bold #00ff9d]/compact model 3[/bold #00ff9d] - Set model by number from table\n"
                "• [bold #00ff9d]/compact model default[/bold #00ff9d] - Use current agent model\n"
                '• [bold #00ff9d]/compact prompt "Focus on vulnerabilities"[/bold #00ff9d] - Set custom prompt\n'
                "• [bold #00ff9d]/compact prompt reset[/bold #00ff9d] - Reset to default prompt\n"
                "• [bold #00ff9d]/cmp status[/bold #00ff9d] - Check configuration\n"
                "• [bold #00ff9d]/compact --model o3-mini[/bold #00ff9d] - One-time compaction with specific model\n"
                '• [bold #00ff9d]/compact --prompt "Focus on credentials"[/bold #00ff9d] - One-time custom prompt\n\n'
                f"[bold {z}]Features:[/bold {z}]\n"
                "• Preserves important context\n"
                "• Reduces token usage\n"
                "• Saves to memory (M-prefixed)\n"
                "• Clears history after compaction\n\n"
                "[dim]Alias: /cmp[/dim]",
                title=_quick_guide_subpanel_title("Compact Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_flush(self, _: Optional[List[str]] = None) -> bool:
        """Show help for clearing histories."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "Flush: drop stored messages while keeping agents, tools, and MCP as configured."
                )
                + f"[bold {z}]Available Commands:[/bold {z}]\n"
                "• [bold #00ff9d]/flush[/bold #00ff9d] - Clear current agent's history\n"
                "• [bold #00ff9d]/flush all[/bold #00ff9d] - Clear all agent histories\n"
                "• [bold #00ff9d]/flush <agent>[/bold #00ff9d] - Clear specific agent\n"
                "• [bold #00ff9d]/flush P1[/bold #00ff9d] - Clear parallel agent P1\n\n"
                f"[bold {z}]Examples:[/bold {z}]\n"
                "• [bold #00ff9d]/flush[/bold #00ff9d] - Clear active agent\n"
                "• [bold #00ff9d]/flush all[/bold #00ff9d] - Reset all agents\n"
                "• [bold #00ff9d]/flush red_teamer[/bold #00ff9d] - Clear red team agent\n"
                "• [bold #00ff9d]/clear P2[/bold #00ff9d] - Clear parallel agent P2\n\n"
                f"[bold {z}]Effects:[/bold {z}]\n"
                "• Removes all messages\n"
                "• Resets token counts\n"
                "• Preserves agent configuration\n"
                "• Keeps MCP connections\n\n"
                "[dim]Alias: /clear[/dim]",
                title=_quick_guide_subpanel_title("Flush Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_load(self, _: Optional[List[str]] = None) -> bool:
        """Show help for loading JSONL files."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc("Load JSONL transcripts from /save (or elsewhere) back into agents.")
                + f"[bold {z}]Available Commands:[/bold {z}]\n"
                "• [bold #00ff9d]/load <file>[/bold #00ff9d] - Load for current agent\n"
                "• [bold #00ff9d]/load <file> agent <name>[/bold #00ff9d] - Load for specific agent\n"
                "• [bold #00ff9d]/load <file> all[/bold #00ff9d] - Distribute across all agents\n"
                "• [bold #00ff9d]/load <file> parallel[/bold #00ff9d] - Smart parallel distribution\n\n"
                f"[bold {z}]Examples:[/bold {z}]\n"
                "• [bold #00ff9d]/load session.jsonl[/bold #00ff9d] - Load to current agent ([dim]use .jsonl from /save, not .md[/dim])\n"
                "• [bold #00ff9d]/load ctf.jsonl agent red_teamer[/bold #00ff9d] - Load to red team\n"
                "• [bold #00ff9d]/load scan.jsonl all[/bold #00ff9d] - Split across agents\n"
                "• [bold #00ff9d]/l pentest.jsonl parallel[/bold #00ff9d] - Pattern-based loading\n\n"
                "Use [bold #00ff9d]/save file.jsonl[/bold #00ff9d] to reload with [bold #00ff9d]/load[/bold #00ff9d]; "
                "[bold #00ff9d].md[/bold #00ff9d] exports are readable only (not for /load).\n"
                "[dim]/history export[/dim] is deprecated; use [bold #00ff9d]/save[/bold #00ff9d].\n\n"
                f"[bold {z}]Distribution modes:[/bold {z}]\n"
                "• [bold #00ff9d]agent[/bold #00ff9d] - Load all to one agent\n"
                "• [bold #00ff9d]all[/bold #00ff9d] - Round-robin distribution\n"
                "• [bold #00ff9d]parallel[/bold #00ff9d] - Match by agent patterns\n\n"
                "[dim]Alias: /l[/dim]",
                title=_quick_guide_subpanel_title("Load Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_save(self, _: Optional[List[str]] = None) -> bool:
        """Show help for saving conversation JSONL or Markdown."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "Save: export JSONL for /load or Markdown for humans; use after /flush to archive safely."
                )
                + f"[bold {z}]Formats:[/bold {z}]\n"
                "• [bold #00ff9d].jsonl[/bold #00ff9d] — machine format, one JSON object per line "
                "([dim]agent, role, content[/dim], tool fields). Use with [bold]/load[/bold].\n"
                "• [bold #00ff9d].md[/bold #00ff9d] or [bold #00ff9d].markdown[/bold #00ff9d] — human-readable "
                "report (headings per agent and role). Not loaded by [bold]/load[/bold].\n\n"
                f"[bold {z}]Examples:[/bold {z}]\n"
                "• [bold #00ff9d]/save session.jsonl[/bold #00ff9d]\n"
                "• [bold #00ff9d]/save findings.md[/bold #00ff9d]\n"
                "• [bold #00ff9d]/save ~/notes/cai_thread.jsonl[/bold #00ff9d]\n\n"
                "Tilde paths ([bold]~/...[/bold]) are expanded; parent directories are created if needed.\n"
                "Not the same as [bold]/memory save[/bold] (summarized memory under [dim].cai/memory[/dim]).\n\n"
                "[dim]Deprecated: /history export — use /save instead.[/dim]",
                title=_quick_guide_subpanel_title("Save Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_workspace(self, _: Optional[List[str]] = None) -> bool:
        """Show help for workspace management."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "Workspace labels and paths for CAI_WORKSPACE; bare /workspace or /workspace get shows status."
                )
                + f"[bold {z}]Subcommands[/bold {z}] ([dim]alias[/dim] [bold #00ff9d]/ws[/bold #00ff9d])\n"
                "• [bold #00ff9d]/workspace set <name>[/bold #00ff9d] — set workspace label\n"
                "• [bold #00ff9d]/workspace get[/bold #00ff9d] — same as bare [bold]/ws[/bold]\n"
                "• [bold #00ff9d]/workspace ls[/bold #00ff9d] [dim](optional path in workspace)[/dim] — list files\n"
                "• [bold #00ff9d]/workspace exec <cmd>[/bold #00ff9d] — run a shell command in the workspace cwd\n"
                "• [bold #00ff9d]/workspace copy <src> <dst>[/bold #00ff9d] — host ↔ container via [bold]docker cp[/bold]; "
                "[bold]container:[/bold] on exactly one path; needs [dim]CAI_ACTIVE_CONTAINER[/dim] "
                "([dim]/h virtualization[/dim])\n\n"
                "[dim]Host base path:[/dim] [dim]CAI_WORKSPACE_DIR[/dim]\n\n"
                "[dim]Alias: /ws[/dim]",
                title=_quick_guide_subpanel_title("Workspace Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_virtualization(self, _: Optional[List[str]] = None) -> bool:
        """Show help for Docker container management."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "Virtualization: attach CAI to Docker containers for isolated security tool runs."
                )
                + f"[bold {z}]Available Commands:[/bold {z}]\n"
                "• [bold #00ff9d]/virtualization[/bold #00ff9d] or [bold #00ff9d]/virtualization info[/bold #00ff9d] — full status (alias [bold #00ff9d]/virt[/bold #00ff9d])\n"
                "• [bold #00ff9d]/virtualization list[/bold #00ff9d] - List Docker containers\n"
                "• [bold #00ff9d]/virtualization set <container_id>[/bold #00ff9d] - Set active container (prefix if unique)\n"
                "• [bold #00ff9d]/virtualization clear[/bold #00ff9d] - Return to host\n"
                "• [bold #00ff9d]/virtualization pull <image>[/bold #00ff9d] - Pull Docker image\n"
                "• [bold #00ff9d]/virtualization run <image|id>[/bold #00ff9d] - New container from image, or activate if <id> matches one container prefix\n"
                "• [bold #00ff9d]/virtualization <image_or_pen_id>[/bold #00ff9d] - Switch (same as bare shortcut)\n\n"
                f"[bold {z}]Examples:[/bold {z}]\n"
                "• [bold #00ff9d]/virt pull kalilinux/kali-rolling[/bold #00ff9d] - Pull Kali\n"
                "• [bold #00ff9d]/virt run parrotsec/security[/bold #00ff9d] - Run Parrot OS\n"
                "• [bold #00ff9d]/virt set abc123def456[/bold #00ff9d] - Activate container by ID\n"
                "• [bold #00ff9d]/virt abc123def456[/bold #00ff9d] - Same (bare shortcut)\n\n"
                f"[bold {z}]Supported images:[/bold {z}]\n"
                "• [bold #00ff9d]kalilinux/kali-rolling[/bold #00ff9d] - Kali Linux\n"
                "• [bold #00ff9d]parrotsec/security[/bold #00ff9d] - Parrot Security\n"
                "• [bold #00ff9d]Any security-focused image[/bold #00ff9d]\n\n"
                f"[bold {z}]Features:[/bold {z}]\n"
                "• Host networking enabled\n"
                "• Workspace mounting\n"
                "• Interactive TTY\n"
                "• Sets CAI_ACTIVE_CONTAINER\n\n",
                title=_quick_guide_subpanel_title("Virtualization Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_mcp(self, _: Optional[List[str]] = None) -> bool:
        """Show help for Model Context Protocol."""
        from cai.repl.commands.mcp import mcp_help_panel_markup

        console.print(
            Panel(
                mcp_help_panel_markup(),
                title=_quick_guide_subpanel_title("MCP Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_cost(self, _: Optional[List[str]] = None) -> bool:
        """Show help for cost tracking."""
        console.print(
            Panel(
                cost_help_panel_markup(),
                title=_quick_guide_subpanel_title("Cost Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_exit(self, _: Optional[List[str]] = None) -> bool:
        """Show help for exiting CAI."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "Leave the REPL: session is tidied up and a short summary is shown "
                    "(same path as Ctrl+C)."
                )
                + f"[bold {z}]Command:[/bold {z}]\n"
                f"• [bold {z}]/exit[/bold {z}] — quit CAI\n\n"
                f"[bold {z}]Aliases:[/bold {z}] [dim]/q, /quit[/dim]\n\n"
                f"[bold {z}]Also:[/bold {z}] [dim]Ctrl+C at the prompt[/dim]\n",
                title=_quick_guide_subpanel_title("Exit Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_resume(self, _: Optional[List[str]] = None) -> bool:
        """Show help for session resume."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "Resume replaces the old ``cai --resume`` / ``--logpath`` flags: pick a log, "
                    "replay it, then load history into the active agent."
                )
                + f"[bold {z}]Commands:[/bold {z}]\n"
                "• [bold #00ff9d]/resume[/bold #00ff9d] — same [bold #00ff9d]10[/bold #00ff9d] recent sessions as "
                "[bold #00ff9d]/sessions[/bold #00ff9d]; enter a number to load one\n"
                "• [bold #00ff9d]/resume last[/bold #00ff9d] — newest log under [dim]logs/[/dim] with messages\n"
                "• [bold #00ff9d]/resume <file.jsonl>[/bold #00ff9d] — load that capture\n"
                "• [bold #00ff9d]/resume <dir>[/bold #00ff9d] — pick among up to 10 newest [dim].jsonl[/dim] under "
                "[dim]dir[/dim] (recursive)\n"
                "• [bold #00ff9d]/resume <dir> <token>[/bold #00ff9d] — newest [dim].jsonl[/dim] under "
                "[dim]dir[/dim] whose name contains [dim]token[/dim]\n"
                "• [bold #00ff9d]/resume <token>[/bold #00ff9d] — match in [dim]logs/cai_*.jsonl[/dim] filenames "
                "(not a path)\n\n"
                f"[bold {z}]Related:[/bold {z}]\n"
                "• [bold #00ff9d]/sessions[/bold #00ff9d], [bold #00ff9d]/sessions <n>[/bold #00ff9d]\n\n"
                "[dim]Alias: /r[/dim]",
                title=_quick_guide_subpanel_title("Resume Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_sessions(self, _: Optional[List[str]] = None) -> bool:
        """Show help for session listing."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "Sessions: browse on-disk JSONL logs. Default list matches the first step of "
                    "[bold #00ff9d]/resume[/bold #00ff9d] (10 newest with messages)."
                )
                + f"[bold {z}]Commands:[/bold {z}]\n"
                "• [bold #00ff9d]/sessions[/bold #00ff9d] — last [bold #00ff9d]10[/bold #00ff9d] sessions "
                "([dim]logs/cai_*.jsonl[/dim])\n"
                "• [bold #00ff9d]/sessions <n>[/bold #00ff9d] — last [dim]n[/dim] sessions\n"
                "• [bold #00ff9d]/sessions <id|path>[/bold #00ff9d] — metadata for one log\n\n"
                f"[bold {z}]Related:[/bold {z}]\n"
                "• [bold #00ff9d]/resume[/bold #00ff9d] — pick and replay into the agent\n\n"
                "[dim]Alias: /sess[/dim]",
                title=_quick_guide_subpanel_title("Sessions Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_replay(self, _: Optional[List[str]] = None) -> bool:
        """Show help for session replay."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc("Replay a JSONL capture with optional delay; stop from TUI if needed.")
                + f"[bold {z}]Available Commands:[/bold {z}]\n"
                "• [bold #00ff9d]/replay <file.jsonl>[/bold #00ff9d] - Replay a conversation\n"
                "• [bold #00ff9d]/replay <file.jsonl> <delay>[/bold #00ff9d] - Replay with custom delay\n"
                "• [bold #00ff9d]/replay stop[/bold #00ff9d] - Cancel active replay (TUI)\n\n"
                f"[bold {z}]Examples:[/bold {z}]\n"
                "• [bold #00ff9d]/replay session.jsonl[/bold #00ff9d] - Replay at default speed\n"
                "• [bold #00ff9d]/replay session.jsonl 2[/bold #00ff9d] - 2s delay between steps\n"
                "• [bold #00ff9d]/replay stop[/bold #00ff9d] - Stop current replay\n\n"
                f"[bold {z}]Features:[/bold {z}]\n"
                "• Shows user prompts, assistant panels, and tool outputs\n"
                "• Live step-by-step playback\n"
                "• Session recording is disabled during replay",
                title=_quick_guide_subpanel_title("Replay Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_continue(self, _: Optional[List[str]] = None) -> bool:
        """Show help for continuation mode."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "Continuation mode: let the agent keep working turn-by-turn until you turn it off."
                )
                + f"[bold {z}]Available Commands:[/bold {z}]\n"
                "• [bold #00ff9d]/continue[/bold #00ff9d] - Enable and continue current task\n"
                "• [bold #00ff9d]/continue on[/bold #00ff9d] - Enable continuation mode\n"
                "• [bold #00ff9d]/continue off[/bold #00ff9d] - Disable continuation mode\n"
                "• [bold #00ff9d]/continue status[/bold #00ff9d] - Check current mode status\n\n"
                f"[bold {z}]How it works:[/bold {z}]\n"
                "• When enabled, the agent automatically continues\n"
                "  working on the current task after each response\n"
                "• Useful for long-running multi-step tasks\n\n"
                f"[bold {z}]Examples:[/bold {z}]\n"
                "• [bold #00ff9d]/continue[/bold #00ff9d] - Start continuing\n"
                "• [bold #00ff9d]/continue off[/bold #00ff9d] - Stop auto-continuation",
                title=_quick_guide_subpanel_title("Continue Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_temperature(self, _: Optional[List[str]] = None) -> bool:
        """Show help for ``/temperature`` (matches command: no subcommands, optional value)."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "Sampling temperature (0.0–2.0). Bare command shows the current value; "
                    "with a number it sets [bold]CAI_TEMPERATURE[/bold] and the active REPL agent's "
                    "model_settings for the next turn."
                )
                + f"[bold {z}]Syntax[/bold {z}]\n"
                f"• [bold {z}]/temperature[/bold {z}] [dim]— show current[/dim]\n"
                f"• [bold {z}]/temperature <value>[/bold {z}] [dim]— set a float in 0.0–2.0[/dim]\n\n"
                f"[dim]Env: CAI_TEMPERATURE. Some models may ignore or clamp the parameter.[/dim]\n\n"
                "[dim]Alias: /temp[/dim]",
                title=_quick_guide_subpanel_title("Temperature"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_topp(self, _: Optional[List[str]] = None) -> bool:
        """Show help for ``/topp`` (matches command: no subcommands, optional value)."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "Nucleus sampling top_p (0.0–1.0). Bare command shows the current value; "
                    "with a number it sets [bold]CAI_TOP_P[/bold] and the active REPL agent's "
                    "model_settings for the next turn."
                )
                + f"[bold {z}]Syntax[/bold {z}]\n"
                f"• [bold {z}]/topp[/bold {z}] [dim]— show current[/dim]\n"
                f"• [bold {z}]/topp <value>[/bold {z}] [dim]— set a float in 0.0–1.0[/dim]\n\n"
                f"[bold {z}]Value guide:[/bold {z}]\n"
                "• [bold #00ff9d]0.1[/bold #00ff9d] - Very narrow (top 10% probability mass)\n"
                "• [bold #00ff9d]0.5[/bold #00ff9d] - Moderate (top 50%)\n"
                "• [bold #00ff9d]1.0[/bold #00ff9d] - Default (consider all tokens)\n\n"
                f"[bold {z}]Examples:[/bold {z}]\n"
                "• [bold #00ff9d]/topp 0.5[/bold #00ff9d] - More focused sampling\n"
                "• [bold #00ff9d]/topp 1.0[/bold #00ff9d] - Default behavior\n\n"
                f"[dim]Env: CAI_TOP_P. Some models may ignore or clamp the parameter.[/dim]",
                title=_quick_guide_subpanel_title("Top-P"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_settings(self, _: Optional[List[str]] = None) -> bool:
        """Show help for interactive settings."""
        z = _CAI_GREEN
        subs = settings_help_panel_subcommand_bullets()
        console.print(
            Panel(
                _h_panel_desc(
                    "Settings: edit .env variables, FAQ, API checks, language, and Ollama."
                )
                + f"[bold {z}]Commands:[/bold {z}]\n"
                "• [bold #00ff9d]/settings[/bold #00ff9d] - Interactive menu\n"
                f"{subs}\n\n"
                "[dim]Alias: /set[/dim]",
                title=_quick_guide_subpanel_title("Settings"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_auth(self, _: Optional[List[str]] = None) -> bool:
        """Show help for API authentication."""
        console.print(
            Panel(
                auth_help_panel_markup(),
                title=_quick_guide_subpanel_title("Auth Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_ctr(self, _: Optional[List[str]] = None) -> bool:
        """Show help for CTR security analysis."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "CTR: game-theoretic analysis on the session; artifacts under run_* folders "
                    "(base directory or one nested level, same discovery for list/show/graph/use)."
                )
                + f"[bold {z}]Commands:[/bold {z}]\n"
                f"• [bold {z}]/ctr[/bold {z}] — run full analysis\n"
                f"• [bold {z}]/ctr show[/bold {z}] — print equilibrium and strategies\n"
                f"• [bold {z}]/ctr graph[/bold {z}] — open graph image; node/edge summary when data exists\n"
                f"• [bold {z}]/ctr list[/bold {z}] — list runs (newest first; # matches [bold {z}]/ctr use <n>[/bold {z}])\n"
                f"• [bold {z}]/ctr use[/bold {z}] — [dim]list index[/dim], [dim]run folder name under base[/dim], "
                f"or [dim]absolute path[/dim]\n"
                f"• [bold {z}]/ctr open[/bold {z}] — open the runs folder in the file manager\n\n"
                f"[dim]Example: [bold {z}]/ctr[/bold {z}] then [bold {z}]/ctr list[/bold {z}] and [bold {z}]/ctr use 1[/bold {z}][/dim]",
                title=_quick_guide_subpanel_title("CTR Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_api(self, _: Optional[List[str]] = None) -> bool:
        """Show help for /api (ALIAS_API_KEY in .env)."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "ALIAS_API_KEY for Alias-hosted models (CAI PRO). "
                    "Show reads .env first, then the process environment if unset in the file; "
                    "masked display matches the CLI startup hint (first 4 … last 4 when the key is longer than 10 characters)."
                )
                + f"[bold {z}]Commands:[/bold {z}]\n"
                f"• [bold {z}]/api[/bold {z}] — show masked ALIAS_API_KEY\n"
                f"• [bold {z}]/api show[/bold {z}] — same as bare [bold {z}]/api[/bold {z}]\n"
                f"• [bold {z}]/api set <key>[/bold {z}] — write [bold {z}].env[/bold {z}] and update "
                f"[bold {z}]os.environ[/bold {z}] for this process\n"
                f"• [bold {z}]/api <key>[/bold {z}] — shorthand for [bold {z}]set[/bold {z}] when the "
                f"first token is not [bold {z}]show[/bold {z}] or [bold {z}]set[/bold {z}]\n\n"
                f"[bold {z}]Notes:[/bold {z}]\n"
                f"• Alias: [bold {z}]/apikey[/bold {z}] (same handlers as [bold {z}]/api[/bold {z}])\n"
                f"• Other provider keys: [bold {z}]/env[/bold {z}], [bold {z}]/settings[/bold {z}]\n"
                f"• HTTP API server (FastAPI) may use [bold {z}]CAI_API_KEY[/bold {z}] as a fallback "
                f"root key; see [bold {z}]docs/api.md[/bold {z}]\n\n"
                f"[dim]Example: [bold {z}]/api show[/bold {z}] then [bold {z}]/api set[/bold {z}] "
                f"(paste the key as the next token)[/dim]",
                title=_quick_guide_subpanel_title("API Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_metadebug(self, _: Optional[List[str]] = None) -> bool:
        """Show help for meta-agent debugging."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc("Meta-debug: dump meta-reasoning state, routing, and agent-pick diagnostics.")
                + f"[bold {z}]Available Commands:[/bold {z}]\n"
                "• [bold #00ff9d]/metadebug[/bold #00ff9d] - Show debug information\n\n"
                f"[bold {z}]What it shows:[/bold {z}]\n"
                "• Meta-agent reasoning state\n"
                "• Agent selection decisions\n"
                "• Internal routing information\n\n"
                "[dim]Alias: /md[/dim]",
                title=_quick_guide_subpanel_title("Metadebug Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True

    def handle_commands(self, _: Optional[List[str]] = None) -> bool:
        """List all available commands in one panel (same layout style as other /h topics)."""
        z = _CAI_GREEN
        console.print(
            Panel(
                Text.from_markup(commands_reference_panel_markup(), overflow="fold"),
                title=_quick_guide_subpanel_title("Command Reference"),
                title_align="left",
                padding=(1, 1),
                border_style=z,
            )
        )
        return True

    def handle_merge_help(self, _: Optional[List[str]] = None) -> bool:
        """Show help for merge command."""
        z = _CAI_GREEN
        console.print(
            Panel(
                _h_panel_desc(
                    "Merge: fuse parallel workers into shared histories and leave parallel mode when done."
                )
                + f"[bold {z}]Available Commands:[/bold {z}]\n"
                "• [bold #00ff9d]/merge <agents...> [options][/bold #00ff9d] - Merge specified agents\n"
                "• [bold #00ff9d]/merge all [options][/bold #00ff9d] - Merge all agent histories\n\n"
                f"[bold {z}]Default behavior:[/bold {z}]\n"
                "Without --target, all source agents receive the complete\n"
                "merged history (with automatic duplicate control).\n"
                "After a successful merge, CAI exits parallel mode automatically,\n"
                "so you can continue using that merged context in next prompts.\n\n"
                f"[bold {z}]Options:[/bold {z}]\n"
                "• [bold #00ff9d]--strategy <type>[/bold #00ff9d] - Merge strategy\n"
                "  • chronological (default) - Order by timestamp\n"
                "  • by-agent - Group by agent\n"
                "  • interleaved - Preserve conversation flow\n"
                "• [bold #00ff9d]--target <name>[/bold #00ff9d] - Create new agent with merged history\n"
                "• [bold #00ff9d]--remove-sources[/bold #00ff9d] - Remove source agents after merge\n"
                "• [bold #00ff9d]--no-worker-summary[/bold #00ff9d] - Keep full per-worker transcripts (no AI digest)\n"
                "• [bold #00ff9d]--summarize-workers[/bold #00ff9d] - Digest every worker, even short histories\n\n"
                "[dim]Env: CAI_MERGE_SUMMARIZE_PER_WORKER=1 (default) enables per-worker digests "
                "when a worker has ≥ CAI_MERGE_SUMMARIZE_MIN_MESSAGES (default 20).[/dim]\n\n"
                f"[bold {z}]Examples:[/bold {z}]\n"
                "• [bold #00ff9d]/merge P1 P2[/bold #00ff9d]\n"
                "  → P1 gets P2's messages, P2 gets P1's messages\n"
                "• [bold #00ff9d]/merge P1 P2 --target combined[/bold #00ff9d]\n"
                "  → Creates new 'combined' agent, P1 and P2 unchanged\n"
                "• [bold #00ff9d]/merge all[/bold #00ff9d]\n"
                "  → All agents get the complete combined history\n"
                "• [bold #00ff9d]/merge all --target unified --remove-sources[/bold #00ff9d]\n"
                "  → Creates 'unified' agent and removes all others\n\n"
                f"[bold {z}]Notes:[/bold {z}]\n"
                "• /merge = merge contexts + exit parallel mode automatically\n"
                "• /parallel clear = exit parallel mode without merging contexts\n"
                "• Use agent IDs (P1, P2) or full names\n"
                "• Agent names with spaces are auto-detected\n"
                "• Duplicates are automatically filtered\n"
                "• This is an alias for /parallel merge\n\n"
                "[dim]Alias: /mrg[/dim]",
                title=_quick_guide_subpanel_title("Merge Commands"),
                title_align="left",
                padding=(1, 1),
                border_style=_CAI_GREEN,
            )
        )
        return True


# Register the command
register_command(HelpCommand())
