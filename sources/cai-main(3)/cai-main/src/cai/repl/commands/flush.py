"""
Flush command for CAI REPL.
This module provides commands for clearing conversation history.
"""

import inspect
import os
import re
from typing import Any, Collection, Dict, List, Optional, Tuple

from rich.console import Console  # pylint: disable=import-error
from rich.panel import Panel  # pylint: disable=import-error

from cai.repl.commands.base import Command, register_command

console = Console()


def merge_flush_histories() -> Dict[str, List[Any]]:
    """Merge AGENT_MANAGER and parallel-isolation histories (same keys as flush help)."""
    try:
        from cai.sdk.agents.models.openai_chatcompletions import get_all_agent_histories
    except ImportError:
        return {}

    all_histories = get_all_agent_histories()
    from cai.sdk.agents.parallel_isolation import PARALLEL_ISOLATION

    parallel_histories: Dict[str, List[Any]] = {}
    if PARALLEL_ISOLATION.is_parallel_mode():
        from cai.repl.commands.parallel import PARALLEL_CONFIGS
        from cai.agents import get_available_agents

        available = get_available_agents()
        for agent_id, history in PARALLEL_ISOLATION._isolated_histories.items():
            if not history:
                continue
            agent_name = f"Unknown Agent {agent_id}"
            for idx, config in enumerate(PARALLEL_CONFIGS, 1):
                slot_id = config.id or f"P{idx}"
                if slot_id != agent_id:
                    continue
                reg_key = config.agent_name
                if reg_key not in available and reg_key.lower() in available:
                    reg_key = reg_key.lower()
                if reg_key in available:
                    agent_obj = available[reg_key]
                    display_name = getattr(agent_obj, "name", config.agent_name)
                    instance_num = 0
                    for j, c in enumerate(PARALLEL_CONFIGS, 1):
                        if c.agent_name != config.agent_name:
                            continue
                        instance_num += 1
                        if (c.id or f"P{j}") == agent_id:
                            break
                    if (
                        sum(1 for c in PARALLEL_CONFIGS if c.agent_name == config.agent_name)
                        > 1
                    ):
                        agent_name = f"{display_name} #{instance_num}"
                    else:
                        agent_name = display_name
                break
            parallel_histories[f"{agent_name} [{agent_id}]"] = history

    combined: Dict[str, List[Any]] = dict(all_histories)
    combined.update(parallel_histories)
    return combined


def _parallel_flush_label_for_slot(
    agent_id: str, PARALLEL_CONFIGS: List[Any], available: Dict[str, Any]
) -> str:
    """Canonical ``Name [Pn]`` label for a parallel slot (must match merge_flush_histories)."""
    agent_name = f"Unknown Agent {agent_id}"
    for idx, config in enumerate(PARALLEL_CONFIGS, 1):
        slot_id = config.id or f"P{idx}"
        if slot_id != agent_id:
            continue
        reg_key = config.agent_name
        if reg_key not in available and reg_key.lower() in available:
            reg_key = reg_key.lower()
        if reg_key in available:
            agent_obj = available[reg_key]
            display_name = getattr(agent_obj, "name", config.agent_name)
            instance_num = 0
            for j, c in enumerate(PARALLEL_CONFIGS, 1):
                if c.agent_name != config.agent_name:
                    continue
                instance_num += 1
                cid = c.id or f"P{j}"
                if cid == agent_id:
                    break
            if sum(1 for c in PARALLEL_CONFIGS if c.agent_name == config.agent_name) > 1:
                agent_name = f"{display_name} #{instance_num}"
            else:
                agent_name = display_name
        break
    return f"{agent_name} [{agent_id}]"


def _active_agent_flush_label(candidates: set[str]) -> Optional[str]:
    """Pick the flush key for the current active agent if it is in ``candidates``."""
    try:
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
    except ImportError:
        return None

    agent = AGENT_MANAGER.get_active_agent()
    if not agent:
        return None
    model = getattr(agent, "model", None)
    if model and hasattr(model, "get_full_display_name"):
        gfd = model.get_full_display_name()
        if gfd in candidates:
            return gfd
    aid = getattr(model, "agent_id", None) if model else None
    if not aid:
        aid = AGENT_MANAGER.get_agent_id()
    name = getattr(agent, "name", None) or AGENT_MANAGER._active_agent_name
    if name and aid:
        cand = f"{name} [{aid}]"
        if cand in candidates:
            return cand
    suffix = f"[{aid}]"
    for k in candidates:
        if k.endswith(suffix):
            return k
    return None


def ordered_nonempty_flush_agent_labels_repl() -> List[str]:
    """REPL-only: non-empty histories, ``Nombre [Pn]`` only, active first then parallel order.

    Queue targets share the same AGENT_MANAGER histories once run; no separate queue store.
    """
    if os.getenv("CAI_TUI_MODE") == "true":
        return []

    combined = merge_flush_histories()
    nonempty: Dict[str, List[Any]] = {
        k: v for k, v in combined.items() if v and len(v) > 0
    }
    if not nonempty:
        return []

    candidates = set(nonempty.keys())
    active_lbl = _active_agent_flush_label(candidates)

    parallel_ordered: List[str] = []
    try:
        from cai.sdk.agents.parallel_isolation import PARALLEL_ISOLATION
        from cai.repl.commands.parallel import PARALLEL_CONFIGS
        from cai.agents import get_available_agents

        if PARALLEL_ISOLATION.is_parallel_mode() and PARALLEL_CONFIGS:
            av = get_available_agents()
            seen: set[str] = set()
            for idx, config in enumerate(PARALLEL_CONFIGS, 1):
                agent_id = config.id or f"P{idx}"
                if not PARALLEL_ISOLATION._isolated_histories.get(agent_id):
                    continue
                lbl = _parallel_flush_label_for_slot(agent_id, PARALLEL_CONFIGS, av)
                if lbl in nonempty and lbl not in seen:
                    parallel_ordered.append(lbl)
                    seen.add(lbl)
    except (ImportError, AttributeError):
        pass

    out: List[str] = []
    pool = set(nonempty.keys())
    base_order = list(nonempty.keys())

    if active_lbl and active_lbl in pool:
        out.append(active_lbl)
        pool.discard(active_lbl)

    for lbl in parallel_ordered:
        if lbl in pool:
            out.append(lbl)
            pool.discard(lbl)

    for lbl in base_order:
        if lbl in pool:
            out.append(lbl)
            pool.discard(lbl)

    return out


def _repl_should_validate_flush_targets() -> bool:
    """REPL-only: TUI uses different ID semantics (terminal P-IDs)."""
    return os.getenv("CAI_TUI_MODE") != "true"


def _repl_add_parallel_slot_name_variants(allowed: set[str]) -> None:
    """Lowercase names that /flush accepts for parallel slots (matches _clear_agent)."""
    try:
        from cai.repl.commands.parallel import PARALLEL_CONFIGS
        from cai.agents import get_available_agents
    except ImportError:
        return
    if not PARALLEL_CONFIGS:
        return
    available = get_available_agents()
    for idx, config in enumerate(PARALLEL_CONFIGS, 1):
        agent_id = config.id or f"P{idx}"
        allowed.add(str(agent_id).strip().lower())
        reg_key = config.agent_name
        if reg_key not in available and reg_key.lower() in available:
            reg_key = reg_key.lower()
        if reg_key not in available:
            continue
        agent_obj = available[reg_key]
        display_name = getattr(agent_obj, "name", config.agent_name)
        instance_num = 0
        for c in PARALLEL_CONFIGS[:idx]:
            if c.agent_name == config.agent_name:
                instance_num += 1
        instance_num += 1
        if sum(1 for c in PARALLEL_CONFIGS if c.agent_name == config.agent_name) > 1:
            instance_name = f"{display_name} #{instance_num}"
        else:
            instance_name = display_name
        allowed.add(display_name.strip().lower())
        allowed.add(instance_name.strip().lower())
        allowed.add(f"{display_name} [{agent_id}]".strip().lower())
        allowed.add(f"{instance_name} [{agent_id}]".strip().lower())


def repl_collect_allowed_flush_queries_lower() -> set[str]:
    """Lowercase set of agent strings accepted for /flush <name> and /flush agent <name>."""
    allowed: set[str] = set()
    try:
        combined = merge_flush_histories()
    except Exception:
        combined = {}
    for k in combined:
        s = k.strip()
        if not s:
            continue
        allowed.add(s.lower())
        if "[P" in s and s.endswith("]"):
            allowed.add(s.rsplit("[", 1)[0].strip().lower())
    try:
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER

        for reg_name, reg_id in AGENT_MANAGER.get_registered_agents().items():
            if reg_name.strip():
                allowed.add(reg_name.strip().lower())
            if reg_id and str(reg_id).strip():
                allowed.add(str(reg_id).strip().lower())
    except ImportError:
        pass
    _repl_add_parallel_slot_name_variants(allowed)
    return allowed


def repl_flush_target_query_is_valid(query: str) -> bool:
    """True if ``query`` matches a known REPL flush target (same pool as tab completion)."""
    q = query.strip()
    if not q:
        return False
    allowed = repl_collect_allowed_flush_queries_lower()
    if not allowed:
        return True
    return q.lower() in allowed


def repl_flush_pid_is_valid(agent_id: str) -> bool:
    """True if ``agent_id`` is a configured P-slot or registered session id (e.g. P0, P1)."""
    raw = agent_id.strip()
    if not raw.upper().startswith("P") or not raw[1:].isdigit():
        return False
    aid = raw.upper()
    try:
        from cai.repl.commands.parallel import PARALLEL_CONFIGS

        for idx, config in enumerate(PARALLEL_CONFIGS, 1):
            slot = (config.id or f"P{idx}").upper()
            if slot == aid:
                return True
    except ImportError:
        pass
    try:
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER

        for reg_id in AGENT_MANAGER.get_registered_agents().values():
            if reg_id and str(reg_id).upper() == aid:
                return True
    except ImportError:
        pass
    try:
        for k in merge_flush_histories():
            ks = k.strip()
            if ks.upper().endswith(f"[{aid}]"):
                return True
    except Exception:
        pass
    return False


def repl_display_label_for_pid(agent_id: str) -> Optional[str]:
    """Resolve ``Name [Pn]`` for flush panels (session ``P0`` and parallel ``P1``…).

    ``handle_specific_agent`` used only ``config.id`` (skipping default ``P{idx}``) and
    never resolved the primary session id ``P0``, yielding generic ``Agent P0``.
    """
    raw = agent_id.strip()
    if not raw.upper().startswith("P") or not raw[1:].isdigit():
        return None
    aid = raw.upper()

    try:
        from cai.agents import get_available_agents

        av = get_available_agents()
    except ImportError:
        av = {}

    try:
        for k in merge_flush_histories().keys():
            ks = k.strip()
            if not ks.upper().endswith(f"[{aid}]"):
                continue
            base = ks.rsplit("[", 1)[0].strip()
            if base in av:
                display = getattr(av[base], "name", base)
                return f"{display} [{aid}]"
            if base:
                return ks
    except Exception:
        pass

    try:
        from cai.repl.commands.parallel import PARALLEL_CONFIGS

        for idx, config in enumerate(PARALLEL_CONFIGS, 1):
            slot = config.id or f"P{idx}"
            if slot.upper() != aid:
                continue
            return _parallel_flush_label_for_slot(slot, PARALLEL_CONFIGS, av)
    except ImportError:
        pass

    try:
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER

        reg = AGENT_MANAGER.get_agent_by_id(aid)
        if not reg:
            return None
        display = getattr(av[reg], "name", reg) if reg in av else reg
        if reg not in av:
            active = AGENT_MANAGER.get_active_agent()
            if active and str(AGENT_MANAGER.get_agent_id()).upper() == aid:
                display = getattr(active, "name", display)
        return f"{display} [{aid}]"
    except ImportError:
        return None


def repl_ordered_nonempty_labels_for_keys(history_keys: Collection[str]) -> List[str]:
    """Labels in the same order as /flush agent TAB, intersected with ``history_keys``.

    Used by /merge error output so listed agents match tab completion.
    """
    key_set = frozenset(history_keys)
    ordered = ordered_nonempty_flush_agent_labels_repl()
    if not ordered:
        ordered = sorted(merge_flush_histories().keys())
    filtered = [lbl for lbl in ordered if lbl in key_set]
    if filtered:
        return filtered
    return sorted(key_set)


def repl_print_unknown_flush_target(user_query: str) -> None:
    """Tell the user the target is unknown and show completion-style examples."""
    list_labels = ordered_nonempty_flush_agent_labels_repl()
    if not list_labels:
        list_labels = sorted(merge_flush_histories().keys())
    console.print("[red]Error: Unknown agent or history target.[/red]")
    console.print(
        f"[dim]No matching agent for[/dim] [bold]{user_query}[/bold] "
        f"[dim](use tab completion after /flush, or run /flush with no arguments).[/dim]"
    )
    if list_labels:
        preview = list_labels[:8]
        extra = len(list_labels) - len(preview)
        lines = "\n".join(f"  • {lbl}" for lbl in preview)
        console.print(f"[dim]Examples:[/dim]\n{lines}")
        if extra > 0:
            console.print(f"[dim]  … and {extra} more[/dim]")


class FlushCommand(Command):
    """Command to flush the conversation history."""

    def __init__(self):
        """Initialize the flush command."""
        super().__init__(
            name="/flush",
            description="Clear conversation history (all agents by default, or specific agent)",
            aliases=["/clear"],
        )

        # Add subcommands
        self.add_subcommand("all", "Clear history for all agents", self.handle_all)
        self.add_subcommand("agent", "Clear history for a specific agent", self.handle_agent)

    def handle(
        self, args: Optional[List[str]] = None, messages: Optional[List[Dict]] = None
    ) -> bool:
        """Handle the flush command.

        Args:
            args: Command arguments - can be agent name or subcommand
            messages: Optional list of conversation messages (legacy, ignored)

        Returns:
            True if the command was handled successfully
        """
        tui_context = None
        if os.getenv("CAI_TUI_MODE") == "true":
            tui_context = self._get_tui_context()
            app, terminal_number, runner = tui_context

            if runner and self._is_runner_busy(runner):
                self._notify_runner_busy(runner)
                return True

        # In TUI mode without args, flush only the current terminal's agent
        if not args and os.getenv("CAI_TUI_MODE") == "true":
            return self.handle_current_terminal(context=tui_context)
        
        if not args:
            # No arguments in CLI mode - show help
            return self.show_flush_help()

        # Check if first arg is "all" (special case)
        if args[0].lower() == "all":
            return self.handle_all(args[1:] if len(args) > 1 else [])

        # Check if first arg is "agent" subcommand
        if args[0].lower() == "agent":
            return self.handle_agent(args[1:] if len(args) > 1 else [])

        # Otherwise treat it as an agent name
        return self.handle_specific_agent(args)

    def handle_current_terminal(self, context: Optional[tuple] = None) -> bool:
        """Clear history for the current terminal's agent in TUI mode."""
        try:
            app, terminal_number, runner = context or self._get_tui_context()

            if not app:
                console.print("[red]Error: TUI not properly initialized[/red]")
                return False

            if terminal_number is None or runner is None:
                console.print("[red]Error: Could not determine current terminal[/red]")
                return False

            if self._is_runner_busy(runner):
                self._notify_runner_busy(runner)
                return True

            if not runner.agent:
                console.print(f"[red]Error: No agent in terminal {terminal_number}[/red]")
                return False

            agent = runner.agent
            agent_name = getattr(agent, "name", runner.config.agent_name)
            
            # Get history length before clearing
            initial_length = 0
            if hasattr(agent, 'model') and hasattr(agent.model, 'message_history'):
                initial_length = len(agent.model.message_history)
                # Clear the history
                agent.model.message_history.clear()
            
            # Display information
            if initial_length > 0:
                content = [
                    f"Conversation history cleared for {agent_name} in Terminal {terminal_number}.",
                    f"Removed {initial_length} messages.",
                ]
                
                console.print(
                    Panel(
                        "\n".join(content),
                        title=f"[bold cyan]Context Flushed - T{terminal_number}[/bold cyan]",
                        border_style="blue",
                        padding=(1, 2),
                    )
                )
            else:
                console.print(
                    Panel(
                        f"No conversation history to clear for {agent_name} in Terminal {terminal_number}.",
                        title=f"[bold cyan]Context Flushed - T{terminal_number}[/bold cyan]",
                        border_style="blue",
                        padding=(1, 2),
                    )
                )
            
            return True
            
        except Exception as e:
            console.print(f"[red]Error clearing terminal history: {str(e)}[/red]")
            return False
    
    def handle_current_agent(self) -> bool:
        """Clear history for the current agent."""
        # Try to get current agent name from environment or default
        current_agent = os.getenv("CAI_CURRENT_AGENT", "Current Agent")

        try:
            from cai.sdk.agents.models.openai_chatcompletions import (
                clear_agent_history,
                get_agent_message_history,
            )
        except ImportError:
            console.print("[red]Error: Could not access conversation history[/red]")
            return False

        # Get initial length before clearing
        history = get_agent_message_history(current_agent)
        initial_length = len(history)

        # Clear the history
        clear_agent_history(current_agent)

        # Display information about the cleared messages
        if initial_length > 0:
            content = [
                f"Conversation history cleared for {current_agent}.",
                f"Removed {initial_length} messages.",
            ]

            console.print(
                Panel(
                    "\n".join(content),
                    title=f"[bold cyan]Context Flushed - {current_agent}[/bold cyan]",
                    border_style="blue",
                    padding=(1, 2),
                )
            )
        else:
            console.print(
                Panel(
                    f"No conversation history to clear for {current_agent}.",
                    title=f"[bold cyan]Context Flushed - {current_agent}[/bold cyan]",
                    border_style="blue",
                    padding=(1, 2),
                )
            )

        return True

    def handle_all(self, args: Optional[List[str]] = None) -> bool:
        """Clear history for all agents."""
        # In TUI mode, clear histories from all terminal runners
        if os.getenv("CAI_TUI_MODE") == "true":
            try:
                app = self._get_tui_app()

                if app and hasattr(app, 'session_manager') and app.session_manager.terminal_runners:
                    agent_count = 0
                    total_messages = 0
                    
                    for term_num, runner in app.session_manager.terminal_runners.items():
                        if runner and runner.agent:
                            agent = runner.agent
                            if hasattr(agent, 'model') and hasattr(agent.model, 'message_history'):
                                history_len = len(agent.model.message_history)
                                if history_len > 0:
                                    agent_count += 1
                                    total_messages += history_len
                                    # Clear the history
                                    agent.model.message_history.clear()

                    # Clear cached sudo password and allowed-commands guard cache
                    from cai.util.user_prompts import (
                        clear_allowed_commands,
                        clear_cached_password,
                    )
                    clear_cached_password()
                    clear_allowed_commands()

                    # Display information
                    if agent_count > 0:
                        content = [
                            f"Cleared history for all {agent_count} terminal agents.",
                            f"Total messages removed: {total_messages}",
                            "Sudo credential cache cleared.",
                            "Sensitive command allowlist cleared.",
                        ]
                        
                        console.print(
                            Panel(
                                "\n".join(content),
                                title="[bold cyan]All Terminal Contexts Flushed[/bold cyan]",
                                border_style="blue",
                                padding=(1, 2),
                            )
                        )
                    else:
                        console.print(
                            Panel(
                                "No terminal agent histories to clear.",
                                title="[bold cyan]All Terminal Contexts Flushed[/bold cyan]",
                                border_style="blue",
                                padding=(1, 2),
                            )
                        )
                    
                    return True
            except Exception as e:
                console.print(f"[red]Error clearing TUI histories: {str(e)}[/red]")
                # Fall back to standard method
                pass
        
        # Standard CLI mode or fallback
        try:
            from cai.sdk.agents.models.openai_chatcompletions import (
                clear_all_histories,
                get_all_agent_histories,
                ACTIVE_MODEL_INSTANCES,
            )
        except ImportError:
            console.print("[red]Error: Could not access conversation history[/red]")
            return False

        # Get agent count and total messages before clearing
        all_histories = get_all_agent_histories()
        agent_count = len(all_histories)
        total_messages = sum(len(history) for history in all_histories.values())

        # Also count parallel isolation histories
        from cai.sdk.agents.parallel_isolation import PARALLEL_ISOLATION

        if PARALLEL_ISOLATION.is_parallel_mode():
            for agent_id, history in PARALLEL_ISOLATION._isolated_histories.items():
                if history:
                    agent_count += 1
                    total_messages += len(history)

        # Clear all histories from AGENT_MANAGER
        clear_all_histories()

        # Clear parallel isolation histories
        PARALLEL_ISOLATION.clear_all_histories()

        # Clear histories from all active model instances
        for key, model_ref in list(ACTIVE_MODEL_INSTANCES.items()):
            model = model_ref() if callable(model_ref) else model_ref
            if model and hasattr(model, "message_history"):
                model.message_history.clear()

        # Clear cached sudo password and allowed-commands guard cache
        from cai.util.user_prompts import (
            clear_allowed_commands,
            clear_cached_password,
        )
        clear_cached_password()
        clear_allowed_commands()

        # Display information
        if agent_count > 0:
            content = [
                f"Cleared history for all {agent_count} agents.",
                f"Total messages removed: {total_messages}",
                "Sudo credential cache cleared.",
                "Sensitive command allowlist cleared.",
            ]

            console.print(
                Panel(
                    "\n".join(content),
                    title="[bold cyan]All Contexts Flushed[/bold cyan]",
                    border_style="blue",
                    padding=(1, 2),
                )
            )
        else:
            console.print(
                Panel(
                    "No agent histories to clear.",
                    title="[bold cyan]All Contexts Flushed[/bold cyan]",
                    border_style="blue",
                    padding=(1, 2),
                )
            )

        return True

    def handle_agent(self, args: Optional[List[str]] = None) -> bool:
        """Clear history for a specific agent using 'agent' subcommand."""
        if not args:
            console.print("[red]Error: Agent name required[/red]")
            console.print("Usage: /flush agent <agent_name>")
            return False

        joined = " ".join(args).strip()
        # Pn is only for direct /flush Pn — not after ``agent`` (avoids broken lookups).
        if re.fullmatch(r"(?i)P\d+", joined):
            console.print(
                "[yellow]Para borrar por id de slot usa el atajo sin subcomando:[/yellow] "
                "[bold]/flush Pn[/bold] o [bold]/clear Pn[/bold] "
                "(ej. [bold]/clear P2[/bold])."
            )
            console.print(
                "[dim]Tras [bold]/flush agent[/bold] indica el nombre completo "
                "tal como en el autocompletado (p. ej. [bold]Red Team Agent [P2][/bold]).[/dim]"
            )
            return False

        # Join all args to handle agent names with spaces
        agent_name = " ".join(args)
        return self._clear_agent(agent_name)

    def handle_specific_agent(self, args: List[str]) -> bool:
        """Clear history for a specific agent (direct syntax)."""
        # Check if first arg is an ID
        identifier = args[0]

        if identifier.upper().startswith("P") and len(identifier) >= 2 and identifier[1:].isdigit():
            # Clear by ID directly for parallel agents
            from cai.sdk.agents.parallel_isolation import PARALLEL_ISOLATION
            from cai.sdk.agents.models.openai_chatcompletions import ACTIVE_MODEL_INSTANCES

            agent_id = identifier.upper()

            if _repl_should_validate_flush_targets() and not repl_flush_pid_is_valid(agent_id):
                repl_print_unknown_flush_target(agent_id)
                return False

            # Get the history length before clearing
            initial_length = 0
            isolated_history = PARALLEL_ISOLATION.get_isolated_history(agent_id)
            if isolated_history:
                initial_length = len(isolated_history)

            # Clear from parallel isolation
            PARALLEL_ISOLATION.clear_agent_history(agent_id)

            # Clear from any active model instances with this agent_id
            for key, model_ref in list(ACTIVE_MODEL_INSTANCES.items()):
                if key[1] == agent_id:  # key is (agent_name, agent_id)
                    model = model_ref() if callable(model_ref) else model_ref
                    if model and hasattr(model, "message_history"):
                        model.message_history.clear()

            # Display name: parallel slots (implicit P{idx}), session P0, merge/AGENT_MANAGER keys
            agent_name = repl_display_label_for_pid(agent_id) or f"Agent {agent_id}"

            # Display information
            if initial_length > 0:
                content = [
                    f"Conversation history cleared for {agent_name}.",
                    f"Removed {initial_length} messages.",
                ]

                console.print(
                    Panel(
                        "\n".join(content),
                        title=f"[bold cyan]Context Flushed - {agent_name}[/bold cyan]",
                        border_style="blue",
                        padding=(1, 2),
                    )
                )
            else:
                console.print(
                    Panel(
                        f"No conversation history to clear for {agent_name}.",
                        title=f"[bold cyan]Context Flushed - {agent_name}[/bold cyan]",
                        border_style="blue",
                        padding=(1, 2),
                    )
                )

            return True
        else:
            # Join all args to handle agent names with spaces
            agent_name = " ".join(args)
            return self._clear_agent(agent_name)

    def _clear_agent(self, agent_name: str) -> bool:
        """Common method to clear a specific agent's history."""
        if not agent_name.strip():
            console.print("[red]Error: Agent name required[/red]")
            return False

        if _repl_should_validate_flush_targets() and not repl_flush_target_query_is_valid(agent_name):
            repl_print_unknown_flush_target(agent_name)
            return False

        try:
            from cai.sdk.agents.models.openai_chatcompletions import (
                clear_agent_history,
                get_agent_message_history,
                ACTIVE_MODEL_INSTANCES,
            )
        except ImportError:
            console.print("[red]Error: Could not access conversation history[/red]")
            return False

        # Get initial length before clearing
        history = get_agent_message_history(agent_name)
        initial_length = len(history)

        # Clear the history from AGENT_MANAGER
        clear_agent_history(agent_name)

        # Also clear from parallel isolation if present
        from cai.sdk.agents.parallel_isolation import PARALLEL_ISOLATION
        from cai.repl.commands.parallel import PARALLEL_CONFIGS

        # Find if this agent is in parallel configs and clear by ID
        cleared_from_parallel = False
        for idx, config in enumerate(PARALLEL_CONFIGS, 1):
            agent_id = config.id or f"P{idx}"
            # Check if the agent name matches
            from cai.agents import get_available_agents

            available = get_available_agents()
            if config.agent_name in available:
                agent_obj = available[config.agent_name]
                display_name = getattr(agent_obj, "name", config.agent_name)

                # Count instances to get correct numbering
                instance_num = 0
                for c in PARALLEL_CONFIGS[:idx]:
                    if c.agent_name == config.agent_name:
                        instance_num += 1
                instance_num += 1  # Current instance

                # Build the instance name
                if sum(1 for c in PARALLEL_CONFIGS if c.agent_name == config.agent_name) > 1:
                    instance_name = f"{display_name} #{instance_num}"
                else:
                    instance_name = display_name

                if agent_name == display_name or agent_name == instance_name:
                    # Clear from parallel isolation
                    isolated_history = PARALLEL_ISOLATION.get_isolated_history(agent_id)
                    if isolated_history:
                        initial_length = max(initial_length, len(isolated_history))
                    PARALLEL_ISOLATION.clear_agent_history(agent_id)
                    cleared_from_parallel = True

                    # Also clear from any active model instances with this agent_id
                    for key, model_ref in list(ACTIVE_MODEL_INSTANCES.items()):
                        if key[1] == agent_id:  # key is (agent_name, agent_id)
                            model = model_ref() if callable(model_ref) else model_ref
                            if model and hasattr(model, "message_history"):
                                model.message_history.clear()
                    break

        # If not cleared from parallel, check if it's a parallel agent by ID in agent name
        if not cleared_from_parallel and "[P" in agent_name and agent_name.endswith("]"):
            # Extract ID from agent name like "Agent Name [P1]"
            agent_id = agent_name.split("[P")[-1].rstrip("]")
            agent_id = f"P{agent_id}"
            isolated_history = PARALLEL_ISOLATION.get_isolated_history(agent_id)
            if isolated_history:
                initial_length = max(initial_length, len(isolated_history))
                PARALLEL_ISOLATION.clear_agent_history(agent_id)

        # Display information
        if initial_length > 0:
            content = [
                f"Conversation history cleared for {agent_name}.",
                f"Removed {initial_length} messages.",
            ]

            console.print(
                Panel(
                    "\n".join(content),
                    title=f"[bold cyan]Context Flushed - {agent_name}[/bold cyan]",
                    border_style="blue",
                    padding=(1, 2),
                )
            )
        else:
            console.print(
                Panel(
                    f"No conversation history to clear for {agent_name}.",
                    title=f"[bold cyan]Context Flushed - {agent_name}[/bold cyan]",
                    border_style="blue",
                    padding=(1, 2),
                )
            )

        return True

    def _is_runner_busy(self, runner: Any) -> bool:
        """Return True if the TUI runner is currently executing a task."""
        if not runner:
            return False

        if getattr(runner, "is_running", False):
            return True

        current_task = getattr(runner, "current_task", None)
        return bool(current_task and not current_task.done())

    def _notify_runner_busy(self, runner: Any) -> None:
        """Notify the user that the targeted terminal is busy."""
        message = (
            "[yellow]Agent is busy. Wait for the current task to finish before flushing.[/yellow]"
        )

        terminal = getattr(runner, "terminal", None)
        if (
            terminal
            and hasattr(terminal, "write")
            and os.getenv("CAI_BROADCAST_MODE") != "true"
        ):
            terminal.write(message)
        else:
            console.print(message)

    def _detect_terminal_number_from_stack(self) -> Optional[int]:
        """Best-effort detection of the command handler's terminal number."""
        try:
            for frame_info in inspect.stack():
                owner = frame_info.frame.f_locals.get("self")
                if (
                    owner
                    and hasattr(owner, "terminal_number")
                    and hasattr(owner, "handle_command")
                ):
                    return int(owner.terminal_number)
        except Exception:
            return None

        return None

    def _get_tui_app(self) -> Optional[Any]:
        """Return the active CAI Terminal application if available."""
        try:
            from cai.tui.cai_terminal import CAITerminal

            app = getattr(CAITerminal, "_current_app", None) or getattr(
                CAITerminal, "_instance", None
            )
            if app:
                return app

            try:
                from textual.app import App

                running = App.get_running()
                if running and isinstance(running, CAITerminal):
                    return running
            except Exception:
                return None
        except Exception:
            return None

        return None

    def _get_tui_context(self) -> Tuple[Optional[Any], Optional[int], Optional[Any]]:
        """Gather the TUI app, terminal number, and runner for the current command."""
        app = self._get_tui_app()
        terminal_number = self._detect_terminal_number_from_stack()

        if app and terminal_number is None:
            try:
                terminal_grid = getattr(app, "terminal_grid", None)
                if terminal_grid and hasattr(terminal_grid, "get_focused_terminal"):
                    focused = terminal_grid.get_focused_terminal()
                    if focused and hasattr(focused, "terminal_number"):
                        terminal_number = int(focused.terminal_number)
            except Exception:
                terminal_number = None

        runner = None
        if app and terminal_number is not None:
            try:
                session_manager = getattr(app, "session_manager", None)
                if session_manager and getattr(session_manager, "terminal_runners", None):
                    runner = session_manager.terminal_runners.get(int(terminal_number))
            except Exception:
                runner = None

        return app, terminal_number, runner

    def show_flush_help(self) -> bool:
        """Show help menu with available agents to flush."""
        try:
            from cai.sdk.agents.models.openai_chatcompletions import get_all_agent_histories
        except ImportError:
            console.print("[red]Error: Could not access conversation history[/red]")
            return False

        all_histories = get_all_agent_histories()
        combined_histories = merge_flush_histories()

        if not combined_histories:
            console.print("[yellow]No agents have conversation history to clear[/yellow]")
            console.print("\n[dim]Usage:[/dim]")
            console.print("[dim]  /flush <agent_name>  - Clear specific agent's history[/dim]")
            console.print("[dim]  /flush all           - Clear all agents' histories[/dim]")
            return True

        # Get IDs for agents if available
        from cai.repl.commands.parallel import PARALLEL_CONFIGS
        from cai.agents import get_available_agents

        agent_ids = {}
        if PARALLEL_CONFIGS:
            available_agents = get_available_agents()
            for config in PARALLEL_CONFIGS:
                if config.agent_name in available_agents:
                    agent = available_agents[config.agent_name]
                    display_name = getattr(agent, "name", config.agent_name)

                    # Count instances to get the right name
                    total_count = sum(
                        1 for c in PARALLEL_CONFIGS if c.agent_name == config.agent_name
                    )
                    instance_num = 0
                    for c in PARALLEL_CONFIGS:
                        if c.agent_name == config.agent_name:
                            instance_num += 1
                            if c.id == config.id:
                                break

                    # Add instance number if there are duplicates
                    if total_count > 1:
                        full_name = f"{display_name} #{instance_num}"
                    else:
                        full_name = display_name

                    agent_ids[full_name] = config.id

        # Create a panel showing available agents
        from rich.tree import Tree

        tree = Tree("[bold #00ff9d]Flush Command - Available Agents[/bold #00ff9d]")

        total_messages = 0
        for agent_name, history in sorted(combined_histories.items()):
            msg_count = len(history)
            total_messages += msg_count

            # Get ID for this agent (if it's not already in the name)
            if "[P" in agent_name and agent_name.endswith("]"):
                id_str = ""  # ID already in name
            else:
                id_str = f" [{agent_ids.get(agent_name, '')}]" if agent_name in agent_ids else ""

            # Add agent to tree
            if msg_count > 0:
                tree.add(
                    f"[bold #00ff9d]{agent_name}{id_str}[/bold #00ff9d] "
                    f"[white]({msg_count} messages)[/white]"
                )
            else:
                tree.add(f"[#9aa0a6]{agent_name}{id_str}[/#9aa0a6] [dim](no messages)[/dim]")

        console.print(tree)
        console.print(
            f"\n[#9aa0a6][CAI] Total messages across all agents:[/] "
            f"[bold #00ff9d]{total_messages}[/bold #00ff9d]"
        )

        console.print("\n[#9aa0a6][CAI] Usage:[/]")
        console.print(
            "  [#9aa0a6]• [/][bold #00ff9d]/flush <agent_name>[/bold #00ff9d]"
            "[#9aa0a6] - Clear specific agent's history[/]"
        )
        console.print(
            "  [#9aa0a6]• [/][bold #00ff9d]/flush <ID>[/bold #00ff9d]"
            "[#9aa0a6] - Clear agent by ID (e.g., /flush P2)[/]"
        )
        console.print(
            "  [#9aa0a6]• [/][bold #00ff9d]/flush all[/bold #00ff9d]"
            "[#9aa0a6] - Clear all agents' histories[/]"
        )
        console.print(
            "  [#9aa0a6]• [/][bold #00ff9d]/flush agent <name>[/bold #00ff9d]"
            "[#9aa0a6] - Clear specific agent (explicit syntax)[/]"
        )

        # Show example for agents with spaces
        agents_with_spaces = [name for name in all_histories.keys() if " " in name]
        if agents_with_spaces:
            console.print("\n[#9aa0a6][CAI] Examples for agents with spaces:[/]")
            for agent in agents_with_spaces[:2]:  # Show max 2 examples
                id_str = f" (or /flush {agent_ids[agent]})" if agent in agent_ids else ""
                console.print(
                    f"[#9aa0a6]  • [/][bold #00ff9d]/flush {agent}{id_str}[/bold #00ff9d]"
                )

        return True

    def handle_no_args(self, messages: Optional[List[Dict]] = None) -> bool:
        """Legacy method for backward compatibility."""
        return self.handle_current_agent()

    def _get_client(self):
        """Get the CAI client from the global namespace.

        This function avoids circular imports by accessing the client
        at runtime instead of import time.

        Returns:
            The global CAI client instance or None if not available
        """
        try:
            # Import here to avoid circular import
            from cai.repl.repl import (
                client as global_client,  # pylint: disable=import-outside-toplevel # noqa: E501
            )

            return global_client
        except (ImportError, AttributeError):
            return None


# Register the /flush command
register_command(FlushCommand())
