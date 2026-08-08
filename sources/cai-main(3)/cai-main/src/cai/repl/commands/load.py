"""
Load command for CAI REPL.

This module provides commands for loading a jsonl into
the context of the current session.
"""

import os
from typing import List, Optional

from rich.console import Console  # pylint: disable=import-error
from rich.table import Table  # pylint: disable=import-error

from cai.repl.commands.base import Command, register_command
from cai.repl.commands.parallel import PARALLEL_CONFIGS
from cai.sdk.agents.models.openai_chatcompletions import (
    get_agent_message_history,
    get_all_agent_histories,
)
from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
from cai.sdk.agents.run_to_jsonl import load_history_from_jsonl
from cai.repl.session_resume import (
    fast_load_messages,
    normalize_messages_for_agent,
    load_session_into_agent,
    restore_session_stats,
)

console = Console()


class LoadCommand(Command):
    """Command for loading a jsonl into the context of the current session."""

    def __init__(self):
        """Initialize the load command."""
        super().__init__(
            name="/load",
            description="Merge a jsonl file into agent histories with duplicate control (uses logs/last if no file specified)",
            aliases=["/l"],
        )

        # Add subcommands
        self.add_subcommand("agent", "Load history into a specific agent", self.handle_agent)
        self.add_subcommand("all", "Show all available agents", self.handle_all)
        self.add_subcommand(
            "parallel", "Load JSONL matching configured parallel agents", self.handle_parallel
        )
        self.add_subcommand(
            "load-all",
            "Load JSONL into all parallel agents with same messages",
            self.handle_load_all,
        )

    def handle(self, args: Optional[List[str]] = None) -> bool:
        """Handle the load command.

        Args:
            args: Optional list of command arguments

        Returns:
            True if the command was handled successfully, False otherwise
        """
        if not args:
            # No arguments - load into default agent
            # In TUI mode, load to current terminal's agent
            if os.getenv("CAI_TUI_MODE") == "true":
                from cai.tui.core.terminal_tracking import get_current_terminal_id
                terminal_id = get_current_terminal_id()

                if terminal_id:
                    # Terminal ID format is like: "19f22766-2de8-45e1-914d-5052514b1489"
                    # We need to extract the terminal number which is embedded in the ID
                    # In command_handler.py, it's generated as: f"terminal-{os.getpid()}-{self.terminal_number}"

                    # Check for the pattern "terminal-PID-NUMBER"
                    if terminal_id.startswith("terminal-") and terminal_id.count("-") >= 2:
                        # Extract everything after "terminal-"
                        parts = terminal_id.split("-", 2)  # Split into at most 3 parts
                        if len(parts) >= 3 and parts[2].isdigit():
                            terminal_num = parts[2]
                            console.print(f"[cyan]Loading to Terminal {terminal_num}[/cyan]")
                            args = [f"P{terminal_num}"]
                            return self.handle(args)

                    # Otherwise look for the actual terminal number from CommandHandler
                    # From debug output, we know terminal_number = 2 when terminal_id = "19f22766-2de8-45e1-914d-5052514b1489"
                    # Since we can't parse it from the ID, we need to get it from the CommandHandler context
                    # Let's check if we can get it from the thread-local context
                    from cai.tui.core import terminal_tracking
                    if hasattr(terminal_tracking._thread_local, 'terminal_number'):
                        terminal_num = terminal_tracking._thread_local.terminal_number
                        console.print(f"[cyan]Loading to Terminal {terminal_num}[/cyan]")
                        args = [f"P{terminal_num}"]
                        return self.handle(args)

            # Load into default agent (P1)
            return self.handle_load_default()

        # Check if first arg is "all" (special case for showing all agents)
        if args[0].lower() == "all":
            return self.handle_all(args[1:] if len(args) > 1 else [])

        # Check if first arg is "agent" subcommand
        if args[0].lower() == "agent":
            return self.handle_agent(args[1:] if len(args) > 1 else [])

        # Check if first arg is "parallel" subcommand
        if args[0].lower() == "parallel":
            return self.handle_parallel(args[1:] if len(args) > 1 else [])

        # Check if first arg is "load-all" subcommand
        if args[0].lower() == "load-all":
            return self.handle_load_all(args[1:] if len(args) > 1 else [])

        # Check if first arg is a parallel pattern
        # Try to load it as a pattern first (more generic approach)
        from cai.agents.patterns import get_pattern
        from cai.repl.commands.parallel import PARALLEL_CONFIGS

        pattern = get_pattern(args[0])
        if pattern and hasattr(pattern, "configs"):
            # Clear existing configs
            PARALLEL_CONFIGS.clear()

            # Load pattern configs
            for idx, config in enumerate(pattern.configs, 1):
                config.id = f"P{idx}"
                PARALLEL_CONFIGS.append(config)

            # Enable parallel mode
            if len(PARALLEL_CONFIGS) >= 2:
                os.environ["CAI_PARALLEL"] = str(len(PARALLEL_CONFIGS))
                agent_names = [config.agent_name for config in PARALLEL_CONFIGS]
                os.environ["CAI_PARALLEL_AGENTS"] = ",".join(agent_names)

            console.print(f"[green]Loaded parallel pattern: {pattern.description}[/green]")
            console.print(f"[cyan]{len(PARALLEL_CONFIGS)} agents configured[/cyan]")

            # Show configured agents with IDs
            for idx, config in enumerate(PARALLEL_CONFIGS, 1):
                model_info = f" [{config.model}]" if config.model else " [default]"
                console.print(f"  [P{idx}] {config.agent_name}{model_info}")

            # Load history file if provided, or default to logs/last
            jsonl_file = args[1] if len(args) > 1 else "logs/last"

            # Try to load and match agent histories
            loaded = self.handle_load_pattern_from_jsonl(jsonl_file)
            if not loaded:
                console.print(f"[yellow]No history loaded from {jsonl_file}[/yellow]")

            return True

        # Check if it's a file path (contains / or . or ends with .jsonl)
        if "/" in args[0] or "." in args[0] or args[0].endswith(".jsonl"):
            # In TUI mode, load to current terminal
            if os.getenv("CAI_TUI_MODE") == "true":
                from cai.tui.core.terminal_tracking import get_current_terminal_id
                terminal_id = get_current_terminal_id()
                
                
                # Try to get terminal number from thread-local storage
                from cai.tui.core import terminal_tracking
                if hasattr(terminal_tracking._thread_local, 'terminal_number'):
                    terminal_num = terminal_tracking._thread_local.terminal_number
                    console.print(f"[cyan]Loading file to Terminal {terminal_num}[/cyan]")
                    return self.handle_load_to_agent([f"P{terminal_num}", args[0]])
                
                # Fallback to P1
                console.print(f"[yellow]DEBUG: Fallback to P1 for file load, terminal_id={terminal_id}[/yellow]")
                return self.handle_load_to_agent(["P1", args[0]])
            else:
                # Not in TUI mode, load into default session agent (P0)
                return self.handle_load_default(args[0])

        # Check if first arg is a numeric ID (like "14")
        if args[0].isdigit():
            # Convert to P format
            args[0] = f"P{args[0]}"

        # Check if first arg is an ID (P1, P2, etc)
        if args[0].upper().startswith("P"):
            # Try to resolve ID to agent name
            from cai.repl.commands.parallel import PARALLEL_CONFIGS
            from cai.agents import get_available_agents

            identifier = args[0].upper()  # Normalize to uppercase
            agent_name = None
            available_agents = get_available_agents()

            # Import AGENT_MANAGER for single agent mode handling
            from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER

            # Check if we're in TUI mode
            if os.getenv("CAI_TUI_MODE") == "true":
                # In TUI mode, P1 means Terminal 1, P2 means Terminal 2, etc
                terminal_number = identifier[1:]  # Extract number from P1, P2, etc
                if terminal_number.isdigit():
                    terminal_num = int(terminal_number)
                    
                    # Get current terminal ID to check if we're loading to current terminal
                    from cai.tui.core.terminal_tracking import get_current_terminal_id
                    current_terminal_id = get_current_terminal_id()
                    current_terminal_num = None
                    
                    if current_terminal_id and "-" in current_terminal_id:
                        parts = current_terminal_id.split("-")
                        if len(parts) >= 2 and parts[-1].isdigit():
                            current_terminal_num = int(parts[-1])
                    
                    # In TUI, messages are stored with P-ID (P1, P2, etc)
                    p_id = f"P{terminal_num}"
                    
                    # Get the agent name from P-ID mapping
                    agent_name = AGENT_MANAGER._p_id_to_agent_name.get(p_id, None)
                    
                    # If not found, try to get from SessionManager
                    if not agent_name:
                        try:
                            from cai.tui.core.session_manager import SessionManager
                            # SessionManager is not a singleton, so we can't easily get it
                            # Instead, use the registry to find the agent
                            for key, registered_p_id in AGENT_MANAGER._agent_registry.items():
                                if registered_p_id == p_id:
                                    if "_" in key:
                                        agent_type = key.split("_", 1)[1]
                                        # Map agent types to display names
                                        agent_name_map = {
                                            "bug_bounter": "Bug Bounter",
                                            "red_teamer": "Red Teamer", 
                                            "blue_teamer": "Blue Teamer",
                                            "one_tool_agent": "CTF Agent",
                                            "one_tool": "CTF Agent",
                                        }
                                        agent_name = agent_name_map.get(agent_type, agent_type.replace("_", " ").title())
                                        break
                        except:
                            pass
                    
                    if agent_name:
                        console.print(f"[cyan]Loading to Terminal {terminal_num} agent: {agent_name}[/cyan]")
                    else:
                        # In TUI mode, we can still load using P-ID directly
                        console.print(f"[yellow]Loading to Terminal {terminal_num} (P{terminal_num})[/yellow]")
                        # Use P-ID as the agent identifier for loading
                        agent_name = p_id
            # Check if there are no parallel configs
            elif not PARALLEL_CONFIGS:
                from cai.sdk.agents.simple_agent_manager import DEFAULT_SESSION_AGENT_ID

                if identifier in (DEFAULT_SESSION_AGENT_ID, "P1"):
                    # Session primary (P0) or legacy P1 — load to the current active agent
                    current_agent = AGENT_MANAGER.get_active_agent()
                    current_agent_name = AGENT_MANAGER._active_agent_name
                    if current_agent and current_agent_name:
                        agent_name = current_agent_name
                        console.print(f"[cyan]Loading to current agent: {agent_name}[/cyan]")
                    else:
                        console.print(f"[red]Error: No active agent found[/red]")
                        return False
                else:
                    # Any other ID in single agent mode is invalid
                    console.print(f"[red]Error: No agent found with ID '{identifier}'[/red]")
                    console.print("[yellow]In single agent mode, only P1 is valid[/yellow]")
                    console.print("[dim]Use '/parallel' to configure multiple agents[/dim]")
                    return False
            else:
                # Look for matching ID in parallel configs
                for config in PARALLEL_CONFIGS:
                    if config.id and config.id.upper() == identifier:
                        if config.agent_name in available_agents:
                            agent = available_agents[config.agent_name]
                            display_name = getattr(agent, "name", config.agent_name)

                            # Count how many instances of this agent type exist
                            total_count = sum(
                                1 for c in PARALLEL_CONFIGS if c.agent_name == config.agent_name
                            )

                            # Count instances to find the right one
                            instance_num = 0
                            for c in PARALLEL_CONFIGS:
                                if c.agent_name == config.agent_name:
                                    instance_num += 1
                                    if c.id == config.id:
                                        break

                            # Add instance number if there are duplicates
                            if total_count > 1:
                                agent_name = f"{display_name} #{instance_num}"
                            else:
                                agent_name = display_name
                            break

            if agent_name:
                # Replace ID with resolved agent name and process
                args[0] = agent_name
                return self.handle_load_to_agent(args)
            else:
                console.print(f"[red]Error: No agent found with ID '{identifier}'[/red]")
                console.print("[dim]Use '/parallel' to see configured agents with IDs[/dim]")
                return False

        # Otherwise, treat first arg as agent name and rest as file path
        return self.handle_load_to_agent(args)

    def handle_load_pattern_from_jsonl(self, jsonl_file: Optional[str] = None) -> bool:
        """Load a JSONL file and match agent messages to configured parallel agents.

        Args:
            jsonl_file: Optional jsonl file path, defaults to "logs/last"

        Returns:
            bool: True if successful
        """
        from cai.repl.commands.parallel import PARALLEL_CONFIGS
        import json
        

        # In TUI mode, don't use parallel loading - each terminal is independent
        if os.getenv("CAI_TUI_MODE") == "true":
            # Get current terminal and load only to that terminal
            from cai.tui.core.terminal_tracking import get_current_terminal_id
            terminal_id = get_current_terminal_id()
            
            
            # Try to get terminal number from thread-local storage
            from cai.tui.core import terminal_tracking
            if hasattr(terminal_tracking._thread_local, 'terminal_number'):
                terminal_num = terminal_tracking._thread_local.terminal_number
                p_id = f"P{terminal_num}"
                console.print(f"[cyan]Loading to Terminal {terminal_num}[/cyan]")
                
                # Load to specific terminal using handle_load_to_agent
                return self.handle_load_to_agent([p_id, jsonl_file] if jsonl_file else [p_id])
            
            # Fallback to P1 if terminal not detected
            console.print(f"[yellow]DEBUG: Fallback to P1, terminal_id={terminal_id}[/yellow]")
            return self.handle_load_to_agent(["P1", jsonl_file] if jsonl_file else ["P1"])

        if not PARALLEL_CONFIGS:
            # No parallel configs, fallback to default behavior
            return self.handle_load_default(jsonl_file)

        if not jsonl_file:
            jsonl_file = "logs/last"

        try:
            # First, try to parse agent names from JSONL if file exists
            agent_conversations = {}

            try:
                jsonl_file = os.path.normpath(os.path.expanduser(str(jsonl_file).strip()))
                with open(jsonl_file, "r", encoding="utf-8") as f:
                    current_agent = None
                    current_messages = []

                    for line in f:
                        line = line.strip()
                        if not line:
                            continue
                        try:
                            record = json.loads(line)

                            # Check if this is a completion record with agent_name
                            if "agent_name" in record and record.get("object") == "chat.completion":
                                # Save previous agent's messages if any
                                if current_agent and current_messages:
                                    if current_agent not in agent_conversations:
                                        agent_conversations[current_agent] = []
                                    agent_conversations[current_agent].extend(current_messages)

                                # Start tracking new agent
                                current_agent = record["agent_name"]
                                current_messages = []

                            # Check if this is a request record with messages
                            elif (
                                "model" in record
                                and "messages" in record
                                and isinstance(record["messages"], list)
                            ):
                                # These messages belong to the current agent
                                for msg in record["messages"]:
                                    if msg.get("role") != "system":  # Skip system messages
                                        current_messages.append(msg)

                        except json.JSONDecodeError:
                            continue

                # Save last agent's messages
                if current_agent and current_messages:
                    if current_agent not in agent_conversations:
                        agent_conversations[current_agent] = []
                    agent_conversations[current_agent].extend(current_messages)
            except FileNotFoundError:
                # File doesn't exist, will use traditional parsing below
                pass

            # Also load traditional messages for backward compatibility
            messages = load_history_from_jsonl(jsonl_file)
            console.print(f"[green]Loaded {len(messages)} messages from {jsonl_file}[/green]")

            # Debug: Show what agent names were found
            if agent_conversations:
                console.print("[dim]Found agent conversations:[/dim]")
                for agent_name, msgs in agent_conversations.items():
                    console.print(f"[dim]  - {agent_name}: {len(msgs)} messages[/dim]")

            # If we didn't find agent names in completion records, try traditional parsing
            if not agent_conversations:
                agent_messages = {}
                current_agent = None

                for msg in messages:
                    # Check multiple ways agents can be identified
                    # 1. Direct "name" field in assistant messages
                    if msg.get("role") == "assistant" and "name" in msg:
                        current_agent = msg["name"]
                    # 2. "sender" field (used in multi-agent logs)
                    elif "sender" in msg:
                        current_agent = msg["sender"]
                    # 3. Look in nested message structure for agent_name
                    elif isinstance(msg, dict) and "agent_name" in msg:
                        current_agent = msg["agent_name"]

                    # Initialize agent message list if needed
                    if current_agent and current_agent not in agent_messages:
                        agent_messages[current_agent] = []

                    # Add message to current agent's list
                    if current_agent:
                        agent_messages[current_agent].append(msg)

                # Use traditional parsing result
                agent_conversations = agent_messages

            # Match configured agents with loaded messages
            loaded_count = 0
            from cai.agents import get_available_agents

            agents = get_available_agents()

            # Count instances of each agent type
            agent_counts = {}
            for config in PARALLEL_CONFIGS:
                agent_counts[config.agent_name] = agent_counts.get(config.agent_name, 0) + 1

            # Track current instance for numbering
            agent_instances = {}

            for idx, config in enumerate(PARALLEL_CONFIGS, 1):
                # Check if config.agent_name is a pattern name
                if config.agent_name.endswith("_pattern"):
                    # Try to get the pattern
                    from cai.agents.patterns import get_pattern

                    pattern = get_pattern(config.agent_name)
                    if pattern and hasattr(pattern, "entry_agent"):
                        # For swarm patterns, use the entry agent
                        agent = pattern.entry_agent
                        agent_display_name = getattr(agent, "name", config.agent_name)
                    else:
                        # Skip if pattern not found
                        console.print(
                            f"[yellow]Warning: Pattern '{config.agent_name}' not found[/yellow]"
                        )
                        continue
                elif config.agent_name in agents:
                    agent = agents[config.agent_name]
                    agent_display_name = getattr(agent, "name", config.agent_name)
                else:
                    # Skip if agent not found
                    console.print(
                        f"[yellow]Warning: Agent '{config.agent_name}' not found[/yellow]"
                    )
                    continue

                # Determine the instance name
                if agent_counts[config.agent_name] > 1:
                    if config.agent_name not in agent_instances:
                        agent_instances[config.agent_name] = 0
                    agent_instances[config.agent_name] += 1
                    instance_name = f"{agent_display_name} #{agent_instances[config.agent_name]}"
                else:
                    instance_name = agent_display_name

                    # Look for matching messages in various formats
                    possible_names = [
                        instance_name,
                        agent_display_name,
                        f"{agent_display_name} #1",
                        f"{agent_display_name} #2",
                        f"{agent_display_name} #3",
                        config.agent_name,
                        # Also check without spaces
                        agent_display_name.replace(" ", ""),
                        config.agent_name.replace("_agent", ""),
                        config.agent_name.replace("_", " ").title(),
                        # Add pattern-specific names
                        "Red team manager",
                        "Bug bounty Triage Agent",
                        "ThoughtAgent",
                        "Retester Agent",
                    ]

                    # Find the longest matching history
                    best_match = None
                    best_count = 0

                    for name in possible_names:
                        if (
                            name in agent_conversations
                            and len(agent_conversations[name]) > best_count
                        ):
                            best_match = name
                            best_count = len(agent_conversations[name])

                    if best_match:
                        # Load these messages into the agent's history with the correct instance name
                        # CRITICAL: We need to get the actual model instance to add messages properly
                        # Using get_agent_message_history() and appending won't work as it returns a copy
                        from cai.sdk.agents.models.openai_chatcompletions import (
                            ACTIVE_MODEL_INSTANCES,
                        )

                        # Find the matching model instance
                        model_instance = None
                        for (name, inst_id), model_ref in ACTIVE_MODEL_INSTANCES.items():
                            if name == instance_name:
                                model = model_ref() if model_ref else None
                                if model:
                                    model_instance = model
                                    break

                        # Check if we're in parallel mode with isolation
                        from cai.sdk.agents.parallel_isolation import PARALLEL_ISOLATION

                        # Check if we should be in parallel mode based on configs
                        if len(PARALLEL_CONFIGS) >= 2:
                            # Ensure parallel mode is enabled
                            PARALLEL_ISOLATION._parallel_mode = True

                        if PARALLEL_ISOLATION.is_parallel_mode():
                            # Update the isolated history instead of the main history
                            agent_id = config.id or f"P{idx}"
                            # Replace the entire isolated history with the loaded messages
                            PARALLEL_ISOLATION.replace_isolated_history(
                                agent_id, agent_conversations[best_match]
                            )

                            # Verify it was stored
                            test_history = PARALLEL_ISOLATION.get_isolated_history(agent_id)

                            # Also sync with AGENT_MANAGER for consistency
                            # Don't use set_message_history or any method that might register the agent
                            AGENT_MANAGER._message_history[instance_name] = list(
                                agent_conversations[best_match]
                            )

                            # Force sync the isolated histories back to AGENT_MANAGER for display
                            # This ensures /history and /graph see the loaded data
                            PARALLEL_ISOLATION.sync_with_agent_manager()
                        else:
                            # Normal mode - update as before
                            if model_instance:
                                # Add messages directly to the model's message history
                                # Use skip_deduplication=True to preserve order from JSONL
                                for msg in agent_conversations[best_match]:
                                    model_instance.add_to_message_history(msg, skip_deduplication=True)
                            else:
                                # No active instance, store in persistent history
                                from cai.sdk.agents.models.openai_chatcompletions import (
                                    PERSISTENT_MESSAGE_HISTORIES,
                                )

                                PERSISTENT_MESSAGE_HISTORIES[instance_name] = list(
                                    agent_conversations[best_match]
                                )

                                # CRITICAL: Also update AGENT_MANAGER to ensure consistency
                                # This ensures the history is available when the agent is created
                                # Don't use set_message_history or any method that might register the agent
                                AGENT_MANAGER._message_history[instance_name] = list(
                                    agent_conversations[best_match]
                                )

                        console.print(
                            f"[green]Loaded {best_count} messages into '{instance_name}' [P{idx}][/green]"
                        )
                        loaded_count += 1

            if loaded_count > 0:
                console.print(
                    f"[bold green]Successfully loaded history for {loaded_count} agents[/bold green]"
                )

                # Final sync to ensure all histories are visible
                if PARALLEL_ISOLATION.is_parallel_mode():
                    console.print("[dim]Syncing loaded histories...[/dim]")
                    PARALLEL_ISOLATION.sync_with_agent_manager()
            else:
                console.print("[yellow]No matching agent histories found in JSONL[/yellow]")

                # If no agents were found, provide helpful information
                if not agent_conversations:
                    console.print(
                        "[dim]The JSONL file appears to be empty or does not contain agent messages[/dim]"
                    )
                    console.print(
                        "[dim]Agent names should be in 'name', 'sender', or 'agent_name' fields[/dim]"
                    )
                    return False
                else:
                    console.print(f"\n[dim]Found agents in JSONL:[/dim]")
                    for agent, messages in sorted(
                        agent_conversations.items(), key=lambda x: len(x[1]), reverse=True
                    )[:5]:
                        console.print(f"  • {agent} ({len(messages)} messages)")
                    if len(agent_conversations) > 5:
                        console.print(f"  ... and {len(agent_conversations) - 5} more")

                    console.print(f"\n[dim]Configured agents expecting history:[/dim]")
                    for idx, config in enumerate(PARALLEL_CONFIGS, 1):
                        if config.agent_name in agents:
                            agent = agents[config.agent_name]
                            display_name = getattr(agent, "name", config.agent_name)
                            console.print(f"  • [P{idx}] {display_name}")

                    console.print(
                        "\n[dim]Tip: Agent names in JSONL must match the configured agent names[/dim]"
                    )

            return True

        except Exception as e:
            console.print(f"[red]Error loading pattern from JSONL: {str(e)}[/red]")
            return False

    def handle_load_default(self, jsonl_file: Optional[str] = None) -> bool:
        """Load a jsonl into the current active agent (same as /resume).

        Args:
            jsonl_file: Optional jsonl file path, defaults to "logs/last"

        Returns:
            bool: True if the jsonl was loaded successfully
        """
        if not jsonl_file:
            jsonl_file = "logs/last"

        try:
            # Load the jsonl file using fast_load_messages (same as /resume)
            try:
                messages = fast_load_messages(jsonl_file)
                console.print(f"[green]Loaded {len(messages)} messages from {jsonl_file}[/green]")
            except FileNotFoundError:
                console.print(f"[red]Error: File '{jsonl_file}' not found[/red]")
                return False
            except Exception as e:
                console.print(f"[red]Error loading history from {jsonl_file}: {e}[/red]")
                return False

            # Check if there are any messages to load
            if not messages:
                console.print(f"[yellow]No messages found in {jsonl_file}[/yellow]")
                return True

            # Normalize messages (same as /resume)
            normalized_messages = normalize_messages_for_agent(messages)

            # Get the current active agent from AGENT_MANAGER
            from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER

            current_agent = AGENT_MANAGER.get_active_agent()

            # In TUI mode, try to find the agent for the current terminal
            if os.getenv("CAI_TUI_MODE") == "true":
                from cai.tui.core.terminal_tracking import get_current_terminal_id

                terminal_id = get_current_terminal_id()

                if terminal_id:
                    # Extract terminal number from ID
                    terminal_num = None
                    if "terminal-" in terminal_id:
                        parts = terminal_id.split("-")
                        if len(parts) >= 3 and parts[-1].isdigit():
                            terminal_num = parts[-1]

                    if terminal_num:
                        p_id = f"P{terminal_num}"
                        console.print(f"[cyan]Loading to Terminal {terminal_num} ({p_id})[/cyan]")
                        return self._load_to_agent(p_id, jsonl_file)

            if not current_agent:
                console.print("[red]Error: No active agent found[/red]")
                console.print("[yellow]Please select an agent first with '/agent <name>'[/yellow]")
                return False

            # Use the same loading logic as /resume (without replay display)
            success = load_session_into_agent(current_agent, normalized_messages, jsonl_file)

            if success:
                console.print(
                    "[green]Session loaded. You can continue the conversation.[/green]"
                )

            return success

        except Exception as e:  # pylint: disable=broad-exception-caught
            console.print(f"[red]Error loading jsonl file: {str(e)}[/red]")
            return False

    def handle_load_to_agent(self, args: List[str]) -> bool:
        """Load a jsonl file into a specific agent by parsing agent name from args.

        Args:
            args: List where first elements form agent name, last is optional file

        Returns:
            bool: True if successful
        """
        if len(args) == 1:
            # Only agent name provided
            agent_name = args[0]
            jsonl_file = "logs/last"
        else:
            # Find where the file path starts
            file_idx = -1
            for i, arg in enumerate(args[1:], 1):  # Start from second arg
                if "/" in arg or "." in arg or arg.endswith(".jsonl"):
                    file_idx = i
                    break

            if file_idx == -1:
                # No clear file path indicator, treat last arg as file if exactly 2 args
                if len(args) == 2:
                    agent_name = args[0]
                    jsonl_file = args[1]
                else:
                    # Multiple args, all form agent name
                    agent_name = " ".join(args)
                    jsonl_file = "logs/last"
            else:
                # Everything before file path is agent name
                agent_name = " ".join(args[:file_idx])
                jsonl_file = args[file_idx]

        return self._load_to_agent(agent_name, jsonl_file)

    def handle_agent(self, args: Optional[List[str]] = None) -> bool:
        """Load a jsonl file into a specific agent's history using 'agent' subcommand.

        Args:
            args: List containing agent name and optional jsonl file path

        Returns:
            bool: True if successful
        """
        if not args:
            console.print("[red]Error: Agent name required[/red]")
            console.print("Usage: /load agent <agent_name> [jsonl_file]")
            console.print("Example: /load agent red_teamer")
            console.print('Example: /load agent "Bug Bounter #1" logs/last')
            return False

        # Parse using same logic as handle_load_to_agent
        return self.handle_load_to_agent(args)

    def _load_to_agent(self, agent_name: str, jsonl_file: str) -> bool:
        """Common method to load a jsonl file into a specific agent's history.

        Uses the same loading logic as /resume for consistency.

        Args:
            agent_name: Name of the agent
            jsonl_file: Path to jsonl file

        Returns:
            bool: True if successful
        """
        try:
            # Load the jsonl file using fast_load_messages (same as /resume)
            try:
                messages = fast_load_messages(jsonl_file)
                console.print(f"[green]Loaded {len(messages)} messages from {jsonl_file}[/green]")
            except FileNotFoundError:
                console.print(f"[red]Error: File '{jsonl_file}' not found[/red]")
                return False
            except Exception as e:
                console.print(f"[red]Error loading history from {jsonl_file}: {e}[/red]")
                return False

            # Check if there are any messages to load
            if not messages:
                console.print(f"[yellow]No messages found in {jsonl_file}[/yellow]")
                console.print("[dim]The file may be empty or contain only session events[/dim]")
                return True

            # Normalize messages (same as /resume)
            normalized_messages = normalize_messages_for_agent(messages)

            # If agent_name is an ID (P1, P2, etc), resolve it to actual agent name
            from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER

            resolved_agent_name = agent_name

            if (
                agent_name.upper().startswith("P")
                and len(agent_name) >= 2
                and agent_name[1:].isdigit()
            ):
                # This is an ID
                agent_id = agent_name.upper()

                # In TUI mode, P-IDs are used directly for message storage
                if os.getenv("CAI_TUI_MODE") == "true":
                    resolved_agent_name = agent_id
                    console.print(f"[cyan]Loading to Terminal {agent_id[1:]} ({agent_id})[/cyan]")

                    if agent_id not in AGENT_MANAGER._message_history:
                        AGENT_MANAGER._message_history[agent_id] = []
                else:
                    # Non-TUI mode - try to resolve to agent name
                    resolved_name = AGENT_MANAGER.get_agent_by_id(agent_id)
                    if resolved_name:
                        resolved_agent_name = resolved_name
                        console.print(f"[cyan]Resolved {agent_id} to {resolved_agent_name}[/cyan]")
                    else:
                        console.print(f"[red]Error: No agent found with ID '{agent_id}'[/red]")
                        console.print("[yellow]Available agents:[/yellow]")
                        all_histories = get_all_agent_histories()
                        for agent in sorted(all_histories.keys()):
                            console.print(f"  - {agent}")
                        return False

            # Try to get the current active agent to use load_session_into_agent
            current_agent = AGENT_MANAGER.get_active_agent()

            # Check if we're loading into the active agent
            current_agent_name = AGENT_MANAGER._active_agent_name
            from cai.sdk.agents.simple_agent_manager import DEFAULT_SESSION_AGENT_ID

            is_active_agent = (
                current_agent
                and (
                    current_agent_name == resolved_agent_name
                    or resolved_agent_name in (DEFAULT_SESSION_AGENT_ID, "P1")
                    or (
                        hasattr(current_agent, "name")
                        and current_agent.name == resolved_agent_name
                    )
                )
            )

            if is_active_agent and current_agent:
                # Use the same loading logic as /resume (without replay display)
                success = load_session_into_agent(current_agent, normalized_messages, jsonl_file)

                if success:
                    console.print(
                        f"[green]Session loaded into '{resolved_agent_name}'. You can continue the conversation.[/green]"
                    )
                return success
            else:
                # Loading into a non-active agent or no active agent
                # Use direct history manipulation
                from cai.sdk.agents.models.openai_chatcompletions import (
                    ACTIVE_MODEL_INSTANCES,
                    PERSISTENT_MESSAGE_HISTORIES,
                )

                # Find the matching model instance
                model_instance = None

                if os.getenv("CAI_TUI_MODE") == "true" and resolved_agent_name.startswith("P"):
                    for (name, inst_id), model_ref in ACTIVE_MODEL_INSTANCES.items():
                        model = model_ref() if model_ref else None
                        if model and hasattr(model, 'agent_id'):
                            if model.agent_id == resolved_agent_name:
                                model_instance = model
                                break
                else:
                    for (name, inst_id), model_ref in ACTIVE_MODEL_INSTANCES.items():
                        if name == resolved_agent_name:
                            model = model_ref() if model_ref else None
                            if model:
                                model_instance = model
                                break

                if model_instance:
                    # Clear and load into model instance
                    model_instance.message_history.clear()
                    os.environ["CAI_CONTEXT_USAGE"] = "0.0"
                    for msg in normalized_messages:
                        model_instance.add_to_message_history(msg, skip_deduplication=True)
                    console.print(f"[green]✓ Updated model instance history[/green]")
                else:
                    # Store in persistent history
                    PERSISTENT_MESSAGE_HISTORIES[resolved_agent_name] = list(normalized_messages)

                # Update AGENT_MANAGER
                AGENT_MANAGER._message_history[resolved_agent_name] = list(normalized_messages)

                # Restore session stats
                restore_session_stats(jsonl_file)

                console.print(
                    f"[green]Loaded {len(normalized_messages)} messages into agent '{resolved_agent_name}'[/green]"
                )

            return True

        except Exception as e:  # pylint: disable=broad-exception-caught
            console.print(f"[red]Error loading jsonl file: {str(e)}[/red]")
            return False

    def handle_parallel(self, args: Optional[List[str]] = None) -> bool:
        """Load a JSONL file matching messages to configured parallel agents.

        Args:
            args: Optional list containing jsonl file path

        Returns:
            bool: True if successful
        """
        # Get jsonl file from args or use default
        jsonl_file = args[0] if args else "logs/last"

        # Call the pattern loading method
        return self.handle_load_pattern_from_jsonl(jsonl_file)

    def handle_all(self, args: Optional[List[str]] = None) -> bool:
        """Show all available agents that can have history loaded.

        Returns:
            bool: True if successful
        """
        all_histories = get_all_agent_histories()

        # Also include agents from PARALLEL_CONFIGS that might not have history yet
        from cai.repl.commands.parallel import PARALLEL_CONFIGS
        from cai.agents import get_available_agents

        configured_agents = set()
        if PARALLEL_CONFIGS:
            available_agents = get_available_agents()
            for idx, config in enumerate(PARALLEL_CONFIGS, 1):
                if config.agent_name in available_agents:
                    agent = available_agents[config.agent_name]
                    display_name = getattr(agent, "name", config.agent_name)

                    # Count instances to get the right name
                    instance_count = sum(
                        1 for c in PARALLEL_CONFIGS[:idx] if c.agent_name == config.agent_name
                    )
                    if instance_count > 1:
                        display_name = f"{display_name} #{instance_count}"

                    configured_agents.add(display_name)

        # Combine histories and configured agents
        all_agents = set(all_histories.keys()) | configured_agents

        if not all_agents:
            console.print("[yellow]No agents have been initialized or configured yet[/yellow]")
            console.print(
                "[dim]Agents are created when they are first used in a conversation[/dim]"
            )
            console.print("[dim]Or configured using '/parallel add <agent>'[/dim]")
            return True

        # Get agent IDs mapping from AGENT_MANAGER
        agent_ids = {}
        for agent_name, history in all_histories.items():
            # Extract ID from display format "Agent Name [ID]"
            if "[" in agent_name and "]" in agent_name:
                id_part = agent_name[agent_name.rindex("[") + 1 : agent_name.rindex("]")]
                name_part = agent_name[: agent_name.rindex("[")].strip()
                agent_ids[name_part] = id_part
            # Also check for TUI format like "Agent Name (T1)"
            elif "(T" in agent_name and ")" in agent_name:
                # Extract terminal number from (T1) format
                start = agent_name.rindex("(T") + 2
                end = agent_name.rindex(")")
                terminal_num = agent_name[start:end]
                if terminal_num.isdigit():
                    agent_ids[agent_name] = f"P{terminal_num}"

        # Also add configured but inactive agents from PARALLEL_CONFIGS
        if PARALLEL_CONFIGS:
            available_agents = get_available_agents()
            for config in PARALLEL_CONFIGS:
                if config.id:
                    agent_ids[config.agent_name] = config.id

        # Create a table showing all agents
        table = Table(
            title="Available Agents for Loading History",
            show_header=True,
            header_style="bold yellow",
        )
        table.add_column("ID", style="magenta", width=4)
        table.add_column("Agent Name", style="cyan")
        table.add_column("Current Messages", style="green", justify="right")
        table.add_column("Message Types", style="magenta")
        table.add_column("Status", style="yellow")

        for agent_name in sorted(all_agents):
            history = all_histories.get(agent_name, [])
            msg_count = len(history)

            # Count message types if history exists
            if history:
                role_counts = {}
                for msg in history:
                    role = msg.get("role", "unknown")
                    role_counts[role] = role_counts.get(role, 0) + 1

                # Format role counts
                role_str = ", ".join(
                    [f"{role}: {count}" for role, count in sorted(role_counts.items())]
                )
                status = "Active"
            else:
                role_str = "No messages"
                status = "Configured" if agent_name in configured_agents else "Empty"

            # Get ID for this agent
            id_str = agent_ids.get(agent_name, "-")

            table.add_row(id_str, agent_name, str(msg_count), role_str, status)

        console.print(table)
        console.print("\n[dim]Usage: /load agent <agent_name> [jsonl_file][/dim]")
        console.print("[dim]       /load <ID> [jsonl_file][/dim]")
        console.print(
            "[dim]       /load load-all [jsonl_file] - Load same messages to all parallel agents[/dim]"
        )
        console.print("[dim]Example: /load agent red_teamer logs/session_20240101.jsonl[/dim]")
        console.print('[dim]Example: /load agent "Bug Bounter #1"[/dim]')
        console.print("[dim]Example: /load P2 logs/last[/dim]")
        console.print("[dim]Example: /load load-all logs/session.jsonl[/dim]")

        # IDs are now shown in the table above

        return True

    def handle_load_all(self, args: Optional[List[str]] = None) -> bool:
        """Load the same JSONL messages into all configured parallel agents.

        Args:
            args: Optional list containing jsonl file path

        Returns:
            bool: True if successful
        """
        # Get jsonl file from args or use default
        jsonl_file = args[0] if args else "logs/last"

        # Check if there are parallel configs
        if not PARALLEL_CONFIGS:
            console.print("[yellow]No parallel agents configured[/yellow]")
            console.print("[dim]Use '/parallel add <agent>' to configure agents first[/dim]")
            return False

        try:
            # Load messages from JSONL file
            try:
                messages = load_history_from_jsonl(jsonl_file)
                console.print(f"[green]Loaded {len(messages)} messages from {jsonl_file}[/green]")
            except FileNotFoundError:
                console.print(f"[red]Error: File '{jsonl_file}' not found[/red]")
                return False
            except Exception as e:
                console.print(f"[red]Error loading history from {jsonl_file}: {e}[/red]")
                return False

            if not messages:
                console.print(f"[yellow]No messages found in {jsonl_file}[/yellow]")
                return True

            # Load the same messages into each parallel agent
            from cai.agents import get_available_agents
            from cai.sdk.agents.models.openai_chatcompletions import (
                ACTIVE_MODEL_INSTANCES,
                PERSISTENT_MESSAGE_HISTORIES,
            )
            from cai.sdk.agents.parallel_isolation import PARALLEL_ISOLATION

            available_agents = get_available_agents()
            loaded_agents = []

            # Count instances of each agent type for proper naming
            agent_counts = {}
            for config in PARALLEL_CONFIGS:
                agent_counts[config.agent_name] = agent_counts.get(config.agent_name, 0) + 1

            agent_instances = {}

            for idx, config in enumerate(PARALLEL_CONFIGS, 1):
                if config.agent_name in available_agents:
                    agent = available_agents[config.agent_name]
                    display_name = getattr(agent, "name", config.agent_name)

                    # Add instance number if there are duplicates
                    if agent_counts[config.agent_name] > 1:
                        if config.agent_name not in agent_instances:
                            agent_instances[config.agent_name] = 0
                        agent_instances[config.agent_name] += 1
                        instance_name = f"{display_name} #{agent_instances[config.agent_name]}"
                    else:
                        instance_name = display_name

                    agent_id = config.id or f"P{idx}"

                    # Check if we're in parallel mode with isolation
                    if PARALLEL_ISOLATION.is_parallel_mode():
                        # Replace the isolated history with the loaded messages
                        PARALLEL_ISOLATION.replace_isolated_history(agent_id, messages[:])

                        # Also sync with AGENT_MANAGER for consistency
                        AGENT_MANAGER._message_history[instance_name] = messages[:]
                    else:
                        # Find the matching model instance
                        model_instance = None
                        for (name, inst_id), model_ref in ACTIVE_MODEL_INSTANCES.items():
                            if name == instance_name:
                                model = model_ref() if model_ref else None
                                if model:
                                    model_instance = model
                                    break

                        if model_instance:
                            # Clear existing messages and add new ones
                            model_instance.message_history.clear()
                            os.environ["CAI_CONTEXT_USAGE"] = "0.0"
                            # Use skip_deduplication=True to preserve order from JSONL
                            for message in messages:
                                model_instance.add_to_message_history(message, skip_deduplication=True)
                        else:
                            # No active instance, store in persistent history
                            PERSISTENT_MESSAGE_HISTORIES[instance_name] = messages[:]
                            # Also update AGENT_MANAGER
                            AGENT_MANAGER._message_history[instance_name] = messages[:]

                    loaded_agents.append(f"{instance_name} [{agent_id}]")
                    console.print(f"[green]✓ Loaded into {instance_name} [{agent_id}][/green]")

            console.print(
                f"\n[bold green]Successfully loaded {len(messages)} messages into {len(loaded_agents)} agents[/bold green]"
            )

            return True

        except Exception as e:
            console.print(f"[red]Error loading jsonl file: {str(e)}[/red]")
            return False


# Register the command
register_command(LoadCommand())
