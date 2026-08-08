"""
Compact command for CAI REPL.
Compacts current conversation and manages model/prompt settings.
"""

from typing import List, Optional
import asyncio
import os
import datetime
import threading
from rich.console import Console
from rich.table import Table
from rich.panel import Panel

from cai.repl.commands.base import Command, register_command
from cai.sdk.agents.models.openai_chatcompletions import get_current_active_model
from cai.repl.commands.model import (
    get_all_predefined_models,
    load_all_available_models,
)

console = Console()


class TuiCompactionMonitor:
    """Tracks compaction state for each TUI terminal."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._active: set[int] = set()

    def start(self, terminal_number: int) -> None:
        with self._lock:
            self._active.add(int(terminal_number))

    def end(self, terminal_number: int) -> None:
        with self._lock:
            self._active.discard(int(terminal_number))

    def is_active(self, terminal_number: Optional[int] = None) -> bool:
        with self._lock:
            if terminal_number is None:
                return bool(self._active)
            return int(terminal_number) in self._active


TUI_COMPACTION_MONITOR = TuiCompactionMonitor()


class CompactCommand(Command):
    """Command for compacting conversations with optional model and prompt settings."""

    def __init__(self):
        """Initialize the compact command."""
        super().__init__(
            name="/compact",
            description="Compact current conversation into a memory summary",
            aliases=["/cmp"],
        )

        # Add subcommands
        self.add_subcommand("model", "Set model for compaction", self.handle_model)
        self.add_subcommand("prompt", "Set custom summarization prompt", self.handle_prompt)
        self.add_subcommand("status", "Show compaction settings", self.handle_status)

        # Default model for compaction (None means use current model)
        self.compact_model = None

        # Custom summarization prompt (None means use default)
        self.custom_prompt = None

        # Cache for model numbers
        self.cached_model_numbers = {}

    def handle(self, args: Optional[List[str]] = None) -> bool:
        """Handle the compact command."""
        # Parse arguments for --model and --prompt flags
        model_override = None
        prompt_override = None

        if args:
            i = 0
            while i < len(args):
                if args[i] == "--model":
                    if i + 1 < len(args):
                        model_override = args[i + 1]
                        i += 2
                    else:
                        return self.handle_model()
                elif args[i] == "--prompt" and i + 1 < len(args):
                    prompt_override = " ".join(args[i + 1 :])
                    break
                else:
                    subcommand = args[i].lower()
                    if subcommand in self.subcommands:
                        handler = self.subcommands[subcommand]["handler"]
                        return handler(args[i + 1 :] if len(args) > i + 1 else [])
                    else:
                        console.print(f"[yellow]Unknown argument: {args[i]}[/yellow]")
                        console.print(
                            "[dim]Usage: /compact [--model <model>] [--prompt <prompt>][/dim]"
                        )
                        return True
        else:
            # No arguments provided - check if in parallel mode
            from cai.repl.commands.parallel import PARALLEL_CONFIGS

            # In TUI mode, always use single agent compaction
            if os.getenv("CAI_TUI_MODE") == "true":
                # TUI mode - each terminal has its own agent
                self._show_help_menu()
                return self._ask_and_perform_compaction()
            elif PARALLEL_CONFIGS:
                # In parallel mode - automatically compact all agents
                return self._perform_parallel_compaction()
            else:
                # Single agent mode - show help menu and ask
                self._show_help_menu()
                return self._ask_and_perform_compaction()

        # If arguments provided, perform compaction with overrides
        return self._perform_compaction(model_override, prompt_override)

    def handle_model(self, args: Optional[List[str]] = None) -> bool:
        """Set model for compaction."""
        if not args:
            console.print(
                Panel(
                    f"Current compact model: [bold green]"
                    f"{self.compact_model or 'Using current model'}[/bold green]",
                    border_style="green",
                    title="Compact Model Setting",
                )
            )

            all_predefined = get_all_predefined_models()
            all_model_names, ollama_models_data = load_all_available_models()

            predefined_names = [m["name"] for m in all_predefined]
            litellm_names = [
                n for n in all_model_names[len(predefined_names):]
                if n not in [d.get("name") for d in ollama_models_data]
            ]

            model_table = Table(
                title="Available Models for Compaction",
                show_header=True,
                header_style="bold yellow",
            )
            model_table.add_column("#", style="bold white", justify="right")
            model_table.add_column("Model", style="cyan")
            model_table.add_column("Provider", style="magenta")
            model_table.add_column("Category", style="blue")
            model_table.add_column("Input Cost ($/M)", style="green", justify="right")
            model_table.add_column("Output Cost ($/M)", style="red", justify="right")
            model_table.add_column("Description", style="white")

            for i, model in enumerate(all_predefined, 1):
                input_cost_str = (
                    f"${model['input_cost']:.2f}"
                    if model["input_cost"] is not None else "Unknown"
                )
                output_cost_str = (
                    f"${model['output_cost']:.2f}"
                    if model["output_cost"] is not None else "Unknown"
                )
                model_table.add_row(
                    str(i), model["name"], model["provider"], model["category"],
                    input_cost_str, output_cost_str, model["description"],
                )

            if ollama_models_data:
                start_index = len(predefined_names) + len(litellm_names) + 1
                for i, model in enumerate(ollama_models_data, start_index):
                    model_name = model.get("name", "")
                    model_size = model.get("size", 0)
                    size_str = ""
                    if model_size:
                        size_mb = model_size / (1024 * 1024)
                        if model_size < 1024 * 1024 * 1024:
                            size_str = f"{size_mb:.1f} MB"
                        else:
                            size_gb = size_mb / 1024
                            size_str = f"{size_gb:.1f} GB"
                    model_description = "Local model"
                    if size_str:
                        model_description += f" ({size_str})"
                    model_table.add_row(
                        str(i), model_name, "Ollama", "Local",
                        "Free", "Free", model_description,
                    )

            console.print(model_table)

            console.print("\n[cyan]Usage:[/cyan]")
            console.print(
                "  [bold]/compact model <model_name>[/bold] - Set model by name"
            )
            console.print(
                "  [bold]/compact model <number>[/bold]     - Set model by number from table"
            )
            console.print(
                "  [bold]/compact model default[/bold]      - Use current agent model"
            )

            self.cached_model_numbers = {
                str(i): name for i, name in enumerate(all_model_names, 1)
            }

            return True

        model_arg = args[0]

        # Check if it's a number for model selection
        if model_arg.isdigit() and hasattr(self, "cached_model_numbers"):
            if model_arg in self.cached_model_numbers:
                model_name = self.cached_model_numbers[model_arg]
            else:
                console.print(f"[red]Invalid model number: {model_arg}[/red]")
                return True
        else:
            model_name = model_arg

        if model_name.lower() == "default":
            self.compact_model = None
            console.print("[green]Will use current model for compaction[/green]")
        else:
            self.compact_model = model_name
            console.print(f"[green]Set compact model to: {model_name}[/green]")

        return True

    def handle_prompt(self, args: Optional[List[str]] = None) -> bool:
        """Set custom summarization prompt."""
        if not args:
            if self.custom_prompt:
                console.print("[cyan]Current custom prompt:[/cyan]")
                console.print(self.custom_prompt)
            else:
                console.print("[yellow]No custom prompt set. Using default prompt.[/yellow]")

            console.print("\nUsage: /compact prompt <prompt_text>")
            console.print("       /compact prompt reset    - Reset to default prompt")
            console.print(
                "\nExample: /compact prompt Focus on security findings and vulnerabilities"
            )
            return True

        if args[0].lower() == "reset":
            self.custom_prompt = None
            console.print("[green]Reset to default summarization prompt[/green]")
        else:
            # Join all args as the prompt
            self.custom_prompt = " ".join(args)
            console.print(f"[green]Set custom prompt: {self.custom_prompt}[/green]")

        return True

    def handle_status(self, args: Optional[List[str]] = None) -> bool:
        """Show compaction settings."""
        current_model = get_current_active_model()

        console.print("[bold cyan]Compaction Settings[/bold cyan]\n")

        # Show model info
        console.print(f"Compact Model: {self.compact_model or 'Using current model'}")
        if current_model:
            console.print(f"Current Model: {current_model.model}")

        # Show prompt info
        if self.custom_prompt:
            console.print(f"\nCustom Prompt: {self.custom_prompt}")
        else:
            console.print("\nCustom Prompt: Not set (using default)")

        # Show default prompt
        console.print("\n[dim]Default summarization prompt:[/dim]")
        console.print(
            "[dim]You are a conversation summarizer. Your task is to create a concise summary that captures:[/dim]"
        )
        console.print("[dim]1. The main objectives and goals discussed[/dim]")
        console.print("[dim]2. Key findings and important information discovered[/dim]")
        console.print("[dim]3. Critical tool outputs and results[/dim]")
        console.print("[dim]4. Current status and next steps[/dim]")
        console.print("[dim]5. Any flags, credentials, or important data found[/dim]")

        console.print("\n[yellow]Note: For memory management, use the /memory command[/yellow]")

        return True

    def _show_help_menu(self):
        """Show help menu for the compact command."""
        from rich.panel import Panel

        # Show current status
        current_model = get_current_active_model()
        model_info = self.compact_model or (current_model.model if current_model else "default")

        console.print(
            Panel(
                "[bold #00ff9d]Compact Command - Memory Summarization[/bold #00ff9d]\n\n"
                f"[#9aa0a6]Current model:[/] [bold white]{model_info}[/bold white]\n"
                f"[#9aa0a6]Custom prompt:[/] [bold white]{'Set' if self.custom_prompt else 'Using default'}[/bold white]",
                title="[bold #00ff9d]Compact Settings[/bold #00ff9d]",
                border_style="#00ff9d",
            )
        )

        console.print("\n[#9aa0a6][CAI] Available commands:[/]")
        console.print(
            "  [#9aa0a6]• [/][bold #00ff9d]/compact[/bold #00ff9d]"
            "[#9aa0a6] - Summarize current conversation[/]"
        )
        console.print(
            "  [#9aa0a6]• [/][bold #00ff9d]/compact model[/bold #00ff9d]"
            "[#9aa0a6] - Configure model for compaction[/]"
        )
        console.print(
            "  [#9aa0a6]• [/][bold #00ff9d]/compact prompt[/bold #00ff9d]"
            "[#9aa0a6] - Set custom summarization prompt[/]"
        )
        console.print(
            "  [#9aa0a6]• [/][bold #00ff9d]/compact status[/bold #00ff9d]"
            "[#9aa0a6] - Show current settings[/]"
        )
        console.print("\n[#9aa0a6][CAI] Quick usage:[/]")
        console.print(
            "  [#9aa0a6]• [/][bold #00ff9d]/compact --model o3-mini[/bold #00ff9d]"
            "[#9aa0a6] - Compact with specific model[/]"
        )
        console.print(
            '  [#9aa0a6]• [/][bold #00ff9d]/compact --prompt "Focus on..."[/bold #00ff9d]'
            "[#9aa0a6] - Compact with custom prompt[/]"
        )
        console.print(
            "\n[#9aa0a6][CAI] Note: compacted conversations are saved to [/]"
            "[bold #00ff9d]/memory[/bold #00ff9d][#9aa0a6] for later use.[/]"
        )

    def _ask_and_perform_compaction(self) -> bool:
        """Ask user if they want to compact and perform if confirmed."""
        from cai.sdk.agents.models.openai_chatcompletions import (
            get_agent_message_history,
            get_all_agent_histories,
        )
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER

        # Try to find an agent with messages
        agent_name = None
        current_agent = None

        # Check if we're in TUI mode
        if os.getenv("CAI_TUI_MODE") == "true":
            # In TUI mode, use a completely different approach
            console.print("\n[cyan]Starting compaction in TUI mode...[/cyan]")
            
            # Get the current terminal ID to identify which terminal we're in
            from cai.tui.core.terminal_tracking import get_current_terminal_id
            terminal_id = get_current_terminal_id()
            terminal_num = 1  # Default
            
            # Try to get terminal number from thread-local storage
            from cai.tui.core import terminal_tracking
            if hasattr(terminal_tracking._thread_local, 'terminal_number'):
                terminal_num = terminal_tracking._thread_local.terminal_number
                if os.getenv("CAI_DEBUG"):
                    console.print(f"[cyan]DEBUG compact: Got terminal_num {terminal_num} from thread-local[/cyan]")
            
            # Find the P-ID for this terminal
            p_id = f"P{terminal_num}"
            
            # Debug: Show available P-IDs and mappings
            if os.getenv("CAI_DEBUG") == "1":
                console.print(f"\n[dim]Debug: Looking for P-ID: {p_id}[/dim]")
                console.print(f"[dim]Available message histories: {list(AGENT_MANAGER._message_history.keys())}[/dim]")
                console.print(f"[dim]P-ID to agent name mappings: {AGENT_MANAGER._p_id_to_agent_name}[/dim]")
            
            # Check if we have history for this P-ID
            if p_id not in AGENT_MANAGER._message_history:
                console.print(f"[yellow]No conversation history found for Terminal {terminal_num}[/yellow]")
                return True
            
            history = AGENT_MANAGER._message_history[p_id]
            msg_count = len(history)
            
            if msg_count == 0:
                console.print("[yellow]No conversation history to compact[/yellow]")
                return True
            
            # Get the agent name from P-ID mapping
            agent_name = AGENT_MANAGER._p_id_to_agent_name.get(p_id, None)
            
            # If not found in P-ID mapping, try to get from the actual terminal runner
            if not agent_name or agent_name == "Unknown Agent":
                try:
                    # Try to get from SessionManager's terminal runners
                    from cai.tui.core.session_manager import SessionManager
                    
                    # Get singleton instance using a safe method
                    session_manager = None
                    if hasattr(SessionManager, 'get_instance'):
                        session_manager = SessionManager.get_instance()
                    elif hasattr(SessionManager, '_instance'):
                        session_manager = SessionManager._instance
                    
                    if session_manager and hasattr(session_manager, 'terminal_runners'):
                        runner = session_manager.terminal_runners.get(terminal_num)
                        if runner and runner.agent:
                            agent_name = runner.agent.name
                            # Remove terminal suffix if present
                            if " (T" in agent_name and ")" in agent_name:
                                agent_name = agent_name.split(" (T")[0]
                    
                    # If still not found, check the agent registry
                    if not agent_name or agent_name == "Unknown Agent":
                        for key, registered_p_id in AGENT_MANAGER._agent_registry.items():
                            if registered_p_id == p_id:
                                # Extract agent name from key like "T1_bug_bounter"
                                if "_" in key:
                                    parts = key.split("_", 1)
                                    if len(parts) > 1:
                                        # Get the raw agent type
                                        agent_type = parts[1]
                                        
                                        # Map agent types to display names
                                        agent_name_map = {
                                            "bug_bounter": "Bug Bounter",
                                            "red_teamer": "Red Teamer", 
                                            "blue_teamer": "Blue Teamer",
                                            "one_tool_agent": "CTF Agent",
                                            "one_tool": "CTF Agent",
                                            "retester": "Retester",
                                            "reporter": "Reporter",
                                            "dfir": "DFIR",
                                            "network_traffic_analyzer": "Network Traffic Analyzer",
                                            "reverse_engineering_agent": "Reverse Engineering Agent",
                                            "memory_analysis_agent": "Memory Analysis Agent"
                                        }
                                        
                                        # Use mapping or convert to title case
                                        agent_name = agent_name_map.get(agent_type, agent_type.replace("_", " ").title())
                                        break
                    
                    # Final fallback - use a generic name based on terminal
                    if not agent_name or agent_name == "Unknown Agent":
                        # Try to infer from environment or use default
                        env_agent = os.getenv("CAI_AGENT_TYPE", "one_tool_agent")
                        if env_agent == "bug_bounter":
                            agent_name = "Bug Bounter"
                        elif env_agent == "red_teamer":
                            agent_name = "Red Teamer"
                        elif env_agent == "blue_teamer":
                            agent_name = "Blue Teamer"
                        else:
                            agent_name = "CTF Agent"  # Default name
                            
                except Exception as e:
                    if os.getenv("CAI_DEBUG") == "1":
                        console.print(f"[dim]Error getting agent name: {e}[/dim]")
                    agent_name = "CTF Agent"
            
            # Store the resolved agent name in the P-ID mapping for future use
            if agent_name and agent_name != "Unknown Agent":
                AGENT_MANAGER._p_id_to_agent_name[p_id] = agent_name
            
            console.print(f"[cyan]Found {msg_count} messages for agent '{agent_name}' in Terminal {terminal_num}[/cyan]")
            
            # In TUI, proceed directly without asking
            console.print(f"\n[cyan]Compacting conversation ({msg_count} messages)...[/cyan]")
            
            # Execute compaction in background to avoid blocking TUI
            import threading
            
            runner = self._get_tui_runner(terminal_num)

            TUI_COMPACTION_MONITOR.start(terminal_num)
            self._set_terminal_lock_state(runner, locked=True)

            def run_compaction_async():
                try:
                    # Copy history to agent name key for memory command compatibility
                    AGENT_MANAGER._message_history[agent_name] = history.copy()
                    
                    # Add terminal suffix to agent name for proper tracking
                    agent_name_with_terminal = f"{agent_name} (T{terminal_num})"
                    
                    # Perform compaction
                    result = self._perform_compaction(None, None, agent_name=agent_name_with_terminal)

                    def handle_success():
                        if result:
                            console.print("\n[green]✓ Compaction completed successfully[/green]")
                            console.print("[dim]Memory saved and applied to the active agent[/dim]")

                            if p_id in AGENT_MANAGER._message_history:
                                AGENT_MANAGER._message_history[p_id].clear()
                                console.print("[green]✓ Terminal history cleared[/green]")

                            os.environ["CAI_COMPACTED_MEMORY"] = "true"
                            self._apply_memory_to_tui_agent(terminal_num, agent_name)
                        else:
                            console.print("\n[red]✗ Error during compaction[/red]")

                    self._dispatch_to_ui(handle_success)

                except Exception as e:
                    def handle_error():
                        console.print(f"\n[red]✗ Error: {str(e)}[/red]")
                        if os.getenv("CAI_DEBUG") == "1":
                            import traceback
                            console.print(f"[dim]{traceback.format_exc()}[/dim]")

                    self._dispatch_to_ui(handle_error)
                
                finally:
                    def handle_cleanup():
                        TUI_COMPACTION_MONITOR.end(terminal_num)
                        self._set_terminal_lock_state(self._get_tui_runner(terminal_num), locked=False)

                    self._dispatch_to_ui(handle_cleanup)

            # Start compaction in background
            console.print("[dim]Processing in background. The terminal will stay locked until completion...[/dim]")
            compaction_thread = threading.Thread(target=run_compaction_async, daemon=True)
            compaction_thread.start()

            return True
            
        # Rest of the original code for non-TUI mode
        if not agent_name:
            # First check if there's an active agent
            current_agent = AGENT_MANAGER.get_active_agent()
            if current_agent:
                agent_name = getattr(current_agent, "name", None)

            # If no active agent or no name, check all histories for one with messages
            if not agent_name:
                all_histories = get_all_agent_histories()
                for name, history in all_histories.items():
                    if history and len(history) > 0:
                        agent_name = name
                        break

            # If still no agent, try to get from registered agents
            if not agent_name:
                registered = AGENT_MANAGER.get_registered_agents()
                if registered:
                    # Get the first registered agent
                    agent_name = list(registered.keys())[0]

            # If still no agent, try to get from environment
            if not agent_name:
                agent_type = os.getenv("CAI_AGENT_TYPE", "one_tool_agent")
                from cai.agents import get_available_agents

                agents = get_available_agents()
                if agent_type in agents:
                    agent = agents[agent_type]
                    agent_name = getattr(agent, "name", agent_type)

        # Get message count for non-TUI mode
        history = get_agent_message_history(agent_name) if agent_name else []
        msg_count = len(history)

        if msg_count == 0:
            console.print("\n[yellow]No conversation history to compact[/yellow]")
            
            return True

        # In non-TUI mode, ask for confirmation
        console.print(
            f"\n[#9aa0a6][CAI] Compact current conversation? [/]"
            f"[bold white]({msg_count} messages)[/bold white]"
        )
        confirm = console.input(
            "[#9aa0a6][CAI] Compact conversation? [/][bold #00ff9d](y/N): [/]"
        )

        if confirm.lower() == "y":
            # Pass the detected agent name to _perform_compaction
            return self._perform_compaction(None, None, agent_name=agent_name)
        else:
            console.print("[dim]Compaction cancelled[/dim]")
            return True

    def _perform_parallel_compaction(self) -> bool:
        """Perform compaction for all parallel agents."""
        from cai.repl.commands.parallel import PARALLEL_CONFIGS
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
        from cai.sdk.agents.models.openai_chatcompletions import get_agent_message_history
        from cai.sdk.agents.parallel_isolation import PARALLEL_ISOLATION
        from cai.agents import get_available_agents
        from cai.agents.patterns import get_pattern

        if not PARALLEL_CONFIGS:
            console.print("[yellow]No parallel agents configured[/yellow]")
            return True

        console.print("[bold cyan]Compacting all parallel agents automatically...[/bold cyan]\n")

        success_count = 0
        total_count = 0

        # Process each parallel agent
        for idx, config in enumerate(PARALLEL_CONFIGS, 1):
            total_count += 1
            agent_id = config.id or f"P{idx}"

            # Get isolated history for this agent
            history = PARALLEL_ISOLATION.get_isolated_history(agent_id)
            if not history or len(history) == 0:
                # Also check AGENT_MANAGER for the history
                # Resolve the agent name from the config
                agent_name = None

                if config.agent_name.endswith("_pattern"):
                    # This is a pattern, get the entry agent name
                    pattern = get_pattern(config.agent_name)
                    if pattern and hasattr(pattern, "entry_agent"):
                        agent_name = getattr(pattern.entry_agent, "name", None)
                else:
                    # Regular agent
                    available_agents = get_available_agents()
                    if config.agent_name in available_agents:
                        agent = available_agents[config.agent_name]
                        agent_name = getattr(agent, "name", config.agent_name)

                if agent_name:
                    # Try to get history from AGENT_MANAGER
                    history = get_agent_message_history(agent_name)

                if not history or len(history) == 0:
                    console.print(
                        f"[yellow]{config.agent_name} [{agent_id}]: No messages to compact[/yellow]"
                    )
                    continue

            # Resolve the agent name for display
            display_name = config.agent_name
            if config.agent_name.endswith("_pattern"):
                pattern = get_pattern(config.agent_name)
                if pattern and hasattr(pattern, "entry_agent"):
                    display_name = getattr(pattern.entry_agent, "name", config.agent_name)
            else:
                available_agents = get_available_agents()
                if config.agent_name in available_agents:
                    agent = available_agents[config.agent_name]
                    display_name = getattr(agent, "name", config.agent_name)

            console.print(
                f"[cyan]Compacting {display_name} [{agent_id}] ({len(history)} messages)...[/cyan]"
            )

            # Create a temporary agent instance for this compaction
            # This is necessary because _perform_compaction expects an active agent
            from cai.agents import get_agent_by_name

            try:
                # Get the correct agent type name
                agent_type = config.agent_name

                # Create a temporary agent instance
                temp_agent = get_agent_by_name(
                    agent_type, custom_name=display_name, agent_id=agent_id
                )

                # Set it as active temporarily
                old_active = AGENT_MANAGER.get_active_agent()
                old_active_name = AGENT_MANAGER._active_agent_name

                AGENT_MANAGER.set_active_agent(temp_agent, display_name)

                # Set the isolated history to the agent's model
                if hasattr(temp_agent, "model") and hasattr(temp_agent.model, "message_history"):
                    temp_agent.model.message_history.clear()
                    temp_agent.model.message_history.extend(history)

                # Perform compaction for this agent
                if self._perform_compaction(agent_name=display_name):
                    success_count += 1
                    console.print(
                        f"[green]✓ {display_name} [{agent_id}] compacted successfully[/green]\n"
                    )

                    # Clear the isolated history after successful compaction
                    PARALLEL_ISOLATION.replace_isolated_history(agent_id, [])
                else:
                    console.print(f"[red]✗ Failed to compact {display_name} [{agent_id}][/red]\n")

                # Restore the previous active agent
                if old_active:
                    AGENT_MANAGER.set_active_agent(old_active, old_active_name)
                else:
                    AGENT_MANAGER._active_agent = None
                    AGENT_MANAGER._active_agent_name = None

            except Exception as e:
                console.print(f"[red]Error compacting {display_name}: {str(e)}[/red]\n")
                if os.getenv("CAI_DEBUG", "1") == "2":
                    import traceback

                    traceback.print_exc()

        # Summary
        console.print(
            f"\n[bold]Parallel compaction complete: {success_count}/{total_count} agents processed[/bold]"
        )

        if success_count > 0:
            console.print("[dim]Use '/memory list' to see all saved memories[/dim]")
            console.print("[dim]All agent histories have been cleared after compaction[/dim]")

        return True

    def _perform_compaction(
        self,
        model_override: Optional[str] = None,
        prompt_override: Optional[str] = None,
        agent_name: Optional[str] = None,
        *args,
        **kwargs,
    ) -> bool:
        """Perform immediate compaction of the current conversation.

        Args:
            model_override: Optional model to use for this compaction
            prompt_override: Optional prompt to use for this compaction
            *args: Additional positional arguments (ignored)
            **kwargs: Additional keyword arguments (ignored)

        Returns:
            True if successful
        """
        from cai.repl.commands.memory import MEMORY_COMMAND_INSTANCE
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
        from cai.sdk.agents.models.openai_chatcompletions import (
            ACTIVE_MODEL_INSTANCES,
            PERSISTENT_MESSAGE_HISTORIES,
            get_all_agent_histories,
        )

        # If agent_name wasn't passed, try to detect it
        if not agent_name:
            # Get current agent
            current_agent = AGENT_MANAGER.get_active_agent()
            if current_agent:
                agent_name = getattr(current_agent, "name", None)

            # If still no agent, check all histories for one with messages
            if not agent_name:
                all_histories = get_all_agent_histories()
                for name, history in all_histories.items():
                    if history and len(history) > 0:
                        agent_name = name
                        break

            # If still no agent, try to get from registered agents
            if not agent_name:
                registered = AGENT_MANAGER.get_registered_agents()
                if registered:
                    # Get the first registered agent
                    agent_name = list(registered.keys())[0]

            # If still no agent, try to get from environment
            if not agent_name:
                agent_type = os.getenv("CAI_AGENT_TYPE", "one_tool_agent")
                from cai.agents import get_available_agents

                agents = get_available_agents()
                if agent_type in agents:
                    agent = agents[agent_type]
                    agent_name = getattr(agent, "name", agent_type)

            if not agent_name:
                console.print("[red]Could not determine agent name[/red]")
                return False

        # Try to get the actual agent object if we don't have it
        current_agent = AGENT_MANAGER.get_active_agent()
        
        # In TUI mode, we might need to handle the agent name specially
        if os.getenv("CAI_TUI_MODE") == "true" and agent_name:
            # Check if agent_name has terminal format like "Agent Name (T1)"
            if "(T" in agent_name and ")" in agent_name:
                # This is a TUI agent, use it as-is
                console.print(f"[dim]Using TUI agent: {agent_name}[/dim]")
            else:
                # Check if we need to find the TUI-formatted name
                from cai.sdk.agents.models.openai_chatcompletions import ACTIVE_MODEL_INSTANCES
                from cai.tui.core.terminal_tracking import get_current_terminal_id
                
                terminal_id = get_current_terminal_id()
                
                # Try to get terminal number from thread-local storage
                from cai.tui.core import terminal_tracking
                if hasattr(terminal_tracking._thread_local, 'terminal_number'):
                    terminal_num = terminal_tracking._thread_local.terminal_number
                    
                    # Look for the agent with this terminal number
                    for (name, inst_id), model_ref in ACTIVE_MODEL_INSTANCES.items():
                        if f"(T{terminal_num})" in name:
                            agent_name = name
                            console.print(f"[dim]Found TUI agent name: {agent_name}[/dim]")
                            break
        
        # Only try to set active agent if it's different
        if not current_agent or getattr(current_agent, "name", None) != agent_name:
            # The detected agent might not be the active one
            # In TUI mode, don't try to create a new agent
            if os.getenv("CAI_TUI_MODE") != "true":
                # Set it as active if possible
                from cai.agents import get_agent_by_name

                try:
                    current_agent = get_agent_by_name(agent_name.lower().replace(" ", "_"))
                    if current_agent:
                        AGENT_MANAGER.set_active_agent(current_agent, agent_name)
                except:
                    # If we can't create the agent, continue anyway
                    # The history might still be accessible
                    pass

        # Temporarily set model/prompt if overrides provided
        original_compact_model = self.compact_model
        original_env_model = os.environ.get("CAI_MODEL", "alias1")
        original_prompt = self.custom_prompt

        if model_override:
            self.compact_model = model_override
            console.print(f"[dim]Using model override: {model_override}[/dim]")

        if prompt_override:
            self.custom_prompt = prompt_override
            console.print(f"[dim]Using custom prompt: {prompt_override[:50]}...[/dim]")

        try:
            # Generate memory name
            timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
            # Clean agent name for file - remove terminal indicators like (T1)
            clean_agent_name = agent_name.replace(' ', '_').replace('#', '').replace('(', '').replace(')', '')
            memory_name = f"compact_{clean_agent_name}_{timestamp}"

            console.print(f"\n[cyan]Compacting conversation for {agent_name}...[/cyan]")
            
            if os.getenv("CAI_DEBUG") == "1":
                console.print(f"[dim]Debug: Passing agent_name to handle_save: {agent_name}[/dim]")
                console.print(f"[dim]Debug: Memory name: {memory_name}[/dim]")

            # Use memory command's save functionality
            # Pass the compact model if set
            if self.compact_model:
                # Temporarily override the model for this operation
                os.environ["CAI_MODEL"] = self.compact_model
                try:
                    result = MEMORY_COMMAND_INSTANCE.handle_save(
                        [memory_name, agent_name], preserve_history=False
                    )
                finally:
                    os.environ["CAI_MODEL"] = original_env_model
            else:
                result = MEMORY_COMMAND_INSTANCE.handle_save([memory_name, agent_name], preserve_history=False)

            if result:
                console.print(f"\n[green]✓ Conversation compacted successfully![/green]")
                console.print("[dim]The memory has been saved and applied to the agent[/dim]")
                console.print("[dim]Use '/memory list' to see all saved memories[/dim]")

                # IMPORTANT: Explicitly clear the history after compaction
                # The handle_save with preserve_history=False doesn't always clear properly
                console.print("\n[cyan]Clearing conversation history...[/cyan]")

                # In TUI mode, we need to clear the terminal runner's history
                if os.getenv("CAI_TUI_MODE") == "true" and agent_name:
                    # Extract terminal number from agent name like "Agent Name (T1)"
                    terminal_num = None
                    if "(T" in agent_name and ")" in agent_name:
                        start = agent_name.rfind("(T") + 2
                        end = agent_name.find(")", start)
                        if end > start:
                            terminal_num = agent_name[start:end]
                    
                    if terminal_num and terminal_num.isdigit():
                        # Clear the P-ID history first
                        p_id = f"P{terminal_num}"
                        if p_id in AGENT_MANAGER._message_history:
                            AGENT_MANAGER._message_history[p_id].clear()
                            console.print(f"[dim]Cleared history for {p_id}[/dim]")
                        
                        # Also try to clear the terminal runner's agent history
                        try:
                            from cai.tui.core.session_manager import SessionManager
                            session_manager = SessionManager.get_instance()
                            
                            if session_manager:
                                terminal_runner = session_manager.terminal_runners.get(int(terminal_num))
                                if terminal_runner and terminal_runner.agent:
                                    if hasattr(terminal_runner.agent, 'model') and hasattr(terminal_runner.agent.model, 'message_history'):
                                        terminal_runner.agent.model.message_history.clear()
                                        console.print(f"[dim]Also cleared terminal runner history[/dim]")
                        except Exception as e:
                            if os.getenv("CAI_DEBUG") == "1":
                                console.print(f"[dim]Error clearing terminal history: {e}[/dim]")
                    else:
                        # Fallback to standard clear
                        AGENT_MANAGER.clear_history(agent_name)
                else:
                    # Clear using AGENT_MANAGER (this uses .clear() to maintain reference)
                    AGENT_MANAGER.clear_history(agent_name)
                    # Also clear any history keyed by agent_id (parallel/registry cases)
                    try:
                        agent_id = AGENT_MANAGER.get_id_by_name(agent_name)
                        if agent_id:
                            AGENT_MANAGER.clear_history(agent_id)
                    except Exception:
                        pass

                # Also clear persistent history
                if agent_name in PERSISTENT_MESSAGE_HISTORIES:
                    PERSISTENT_MESSAGE_HISTORIES[agent_name].clear()

                # Get the current active agent and clear its model history too
                current_agent = AGENT_MANAGER.get_active_agent()
                if (
                    current_agent
                    and hasattr(current_agent, "model")
                    and hasattr(current_agent.model, "message_history")
                ):
                    current_agent.model.message_history.clear()

                # Reset context usage since we cleared the history
                os.environ["CAI_CONTEXT_USAGE"] = "0.0"
                console.print("[green]✓ Conversation history cleared[/green]")

                # Debug: Verify histories are actually cleared
                if os.getenv("CAI_DEBUG", "1") == "2":
                    # Check AGENT_MANAGER
                    manager_history = AGENT_MANAGER.get_message_history(agent_name)
                    console.print(
                        f"[dim]Debug: AGENT_MANAGER history length: {len(manager_history)}[/dim]"
                    )

                    # Check active agent (re-fetch to ensure we have the current one)
                    current_active_agent = AGENT_MANAGER.get_active_agent()
                    if (
                        current_active_agent
                        and hasattr(current_active_agent, "model")
                        and hasattr(current_active_agent.model, "message_history")
                    ):
                        console.print(
                            f"[dim]Debug: Active agent model history length: {len(current_active_agent.model.message_history)}[/dim]"
                        )

            else:
                console.print(f"[red]Failed to compact conversation[/red]")

            return result

        finally:
            # Restore original settings
            self.compact_model = original_compact_model
            self.custom_prompt = original_prompt

    def _dispatch_to_ui(self, callback) -> None:
        """Run callback inside the TUI event loop when possible."""
        # Prefer scheduling on the active TUI application's event loop
        tui_app = None
        try:
            from textual.app import App

            tui_app = App.get_running_app()
        except Exception:
            try:
                from cai.tui.cai_terminal import CAITerminal

                tui_app = getattr(CAITerminal, "_instance", None) or getattr(
                    CAITerminal, "_current_app", None
                )
            except Exception:
                tui_app = None

        if tui_app and hasattr(tui_app, "call_from_thread"):
            def invoke() -> None:
                result = callback()
                if asyncio.iscoroutine(result):
                    asyncio.create_task(result)

            try:
                tui_app.call_from_thread(invoke)
                return
            except Exception:
                # Fall back to running inline if scheduling fails
                pass

        # No active TUI loop detected - run inline as a last resort (CLI mode)
        result = callback()
        if asyncio.iscoroutine(result):
            try:
                loop = asyncio.get_event_loop()
                loop.create_task(result)
            except RuntimeError:
                asyncio.run(result)

    def _get_tui_runner(self, terminal_number: Optional[int]):
        """Fetch the TerminalRunner associated with a terminal number."""
        if terminal_number is None:
            return None

        session_manager = None
        try:
            from cai.tui.core.session_manager import SessionManager

            if hasattr(SessionManager, "get_instance"):
                session_manager = SessionManager.get_instance()
            elif hasattr(SessionManager, "_instance"):
                session_manager = SessionManager._instance
        except Exception:
            session_manager = None

        if not session_manager:
            try:
                from cai.tui.cai_terminal import CAITerminal

                app = getattr(CAITerminal, "_instance", None)
                if app and hasattr(app, "session_manager"):
                    session_manager = app.session_manager
            except Exception:
                session_manager = None

        if session_manager and getattr(session_manager, "terminal_runners", None):
            return session_manager.terminal_runners.get(int(terminal_number))

        return None

    def _set_terminal_lock_state(self, runner, locked: bool) -> None:
        """Toggle visual feedback and running flag for a terminal during compaction."""
        if not runner:
            return

        try:
            if locked:
                runner.is_running = True
            else:
                runner.is_running = False
            terminal = getattr(runner, "terminal", None)
            if not terminal:
                return

            if hasattr(terminal, "set_running"):
                terminal.set_running(locked)

            if os.getenv("CAI_BROADCAST_MODE") == "true":
                return

            if locked:
                terminal.write(
                    "[yellow]Compacting conversation. This terminal is temporarily locked.[/yellow]"
                )
            else:
                terminal.write("[dim]Compaction finished. Terminal unlocked.[/dim]")
        except Exception:
            pass

    def _apply_memory_to_tui_agent(self, terminal_number: int, base_agent_name: str) -> None:
        """Reload the terminal agent so compacted memory is reflected immediately."""
        runner = self._get_tui_runner(terminal_number)
        if not runner or not getattr(runner, "agent", None):
            console.print(
                f"[dim]No active agent found for Terminal {terminal_number} to receive memory[/dim]"
            )
            return

        async def refresh_agent() -> None:
            try:
                await runner.cancel_current_task()
            except Exception:
                pass

            try:
                target_agent = runner.config.agent_name
                await runner.switch_agent(target_agent)
                if runner.terminal and hasattr(runner.terminal, "_update_header"):
                    runner.terminal._update_header()
                console.print(
                    f"[green]✓ Reloaded {base_agent_name} with compacted memory in Terminal {terminal_number}[/green]"
                )
            except Exception as exc:
                console.print(
                    f"[yellow]Warning: Unable to refresh agent with compacted memory ({exc})[/yellow]"
                )

        self._dispatch_to_ui(refresh_agent)


# Global instance for access from other modules
COMPACT_COMMAND_INSTANCE = CompactCommand()

# Register the command
register_command(COMPACT_COMMAND_INSTANCE)


def get_compact_model() -> Optional[str]:
    """Get the configured compaction model.

    Returns:
        Model name if set, None to use current model
    """
    return COMPACT_COMMAND_INSTANCE.compact_model


def get_custom_prompt() -> Optional[str]:
    """Get the custom summarization prompt.

    Returns:
        Custom prompt if set, None to use default
    """
    return COMPACT_COMMAND_INSTANCE.custom_prompt
