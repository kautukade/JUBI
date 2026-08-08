"""
Graph command for CAI cli.

This module provides commands for visualizing the agent interaction graph.
It allows users to display a simple directed graph of the conversation history,
showing the sequence of user and agent interactions, including tool calls.
"""

import os
from typing import List, Optional
from rich.console import Console  # pylint: disable=import-error
from rich.panel import Panel
from rich.rule import Rule

from cai.repl.commands.base import Command, register_command
from cai.repl.ui.banner import _CAI_GREEN

console = Console()

# Rich markup / borders aligned with REPL (queue, banner)
_Z = _CAI_GREEN
_HINT = "#9aa0a6"


class GraphCommand(Command):
    """
    Command for visualizing the agent interaction graph.

    This command displays a directed graph of the conversation history,
    showing the sequence of user and agent messages, and highlighting
    tool calls made by the agent.
    """

    def __init__(self):
        """Initialize the graph command."""
        super().__init__(
            name="/graph", description="Visualize the agent interaction graph", aliases=["/g"]
        )

        # Add subcommands
        self.add_subcommand("show", "Show graph (same as bare /graph)", self.handle_graph_show)
        self.add_subcommand("all", "Show graphs for all agents", self.handle_all)
        self.add_subcommand(
            "timeline", "Table of messages per agent (ordered by message index)", self.handle_timeline
        )
        self.add_subcommand("stats", "Show detailed statistics", self.handle_stats)
        self.add_subcommand("export", "Export graph data", self.handle_export)

    def handle(self, args: Optional[List[str]] = None) -> bool:
        """
        Handle the /graph command.

        Args:
            args: Optional list of command arguments

        Returns:
            bool: True if the command was handled successfully, False otherwise.
        """
        if not args:
            return self.handle_graph_show()

        # Check if it's a subcommand
        if args and len(args) > 0 and args[0]:
            subcommand = args[0].lower()
            if subcommand in self.subcommands:
                handler = self.subcommands[subcommand]["handler"]
                return handler(args[1:] if len(args) > 1 else [])

            # Check if it's an agent ID (P1, P2, etc.)
            if args[0].upper().startswith("P") and len(args[0]) >= 2 and args[0][1:].isdigit():
                return self.handle_agent_graph(args[0])

            # Otherwise treat as agent name
            agent_name = " ".join(args)
            return self._handle_single_agent_graph(agent_name)
        else:
            # No valid args, show default graph
            return self.handle_graph_show()

    def handle_graph_show(self, _args: Optional[List[str]] = None) -> bool:
        """Handle bare `/graph` and `/graph show` (default graph view)."""
        # Check if we're in parallel mode first
        parallel_count = int(os.getenv("CAI_PARALLEL", "1"))

        # Also check if we have parallel configs even if not in active parallel mode
        from cai.repl.commands.parallel import PARALLEL_CONFIGS

        if parallel_count > 1 or len(PARALLEL_CONFIGS) > 1:
            # Multi-agent mode - show all agents' conversations
            return self._handle_multi_agent_graph()
        else:
            # Single agent mode - check for agent parameter
            return self._handle_single_agent_graph()

    def _handle_single_agent_graph(self, agent_name: Optional[str] = None) -> bool:
        """Handle graph display for a single agent"""
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
        
        history = None
        current_agent = None
        
        # In TUI mode, get agent from the current terminal
        if os.getenv("CAI_TUI_MODE") == "true":
            try:
                from cai.tui.cai_terminal import CAITerminal
                
                # First, try to get terminal number from the command handler context
                terminal_number = None
                
                # Check if we're running from a specific terminal's command handler
                import inspect
                for frame_info in inspect.stack():
                    frame_locals = frame_info.frame.f_locals
                    if 'self' in frame_locals:
                        obj = frame_locals['self']
                        # Check if this is a CommandHandler with terminal_number
                        if hasattr(obj, 'terminal_number') and hasattr(obj, 'handle_command'):
                            terminal_number = obj.terminal_number
                            break
                
                # Get the app instance - try multiple attributes
                app = getattr(CAITerminal, '_current_app', None)
                if not app:
                    app = getattr(CAITerminal, '_instance', None)
                
                # As last resort, try to import and use get_app from textual
                if not app:
                    try:
                        from textual.app import App
                        app = App.get_running()
                        if app and not isinstance(app, CAITerminal):
                            app = None
                    except:
                        pass
                
                # If we didn't get terminal number from command handler, get the focused terminal
                if terminal_number is None and app:
                    if hasattr(app, 'terminal_grid') and hasattr(app.terminal_grid, 'get_focused_terminal'):
                        focused_terminal = app.terminal_grid.get_focused_terminal()
                        if focused_terminal and hasattr(focused_terminal, 'terminal_number'):
                            terminal_number = focused_terminal.terminal_number
                
                # Get the runner from session manager
                if app and terminal_number is not None:
                    if hasattr(app, 'session_manager') and terminal_number in app.session_manager.terminal_runners:
                        runner = app.session_manager.terminal_runners[terminal_number]
                        if runner and runner.agent:
                            current_agent = runner.agent
                            agent_name = getattr(current_agent, "name", runner.config.agent_name)
                            
                            # Get history from the agent's model
                            if hasattr(current_agent, 'model') and hasattr(current_agent.model, 'message_history'):
                                history = current_agent.model.message_history
            except Exception:
                # Fall back to AGENT_MANAGER
                pass
        
        # Fall back to AGENT_MANAGER if not in TUI or couldn't get from terminal
        if not history:
            # If no agent specified, use the current active agent
            if not agent_name:
                # Try to get the current active agent
                current_agent = AGENT_MANAGER.get_active_agent()
                if current_agent:
                    agent_name = getattr(current_agent, "name", None)
                    if not agent_name:
                        # Fallback to getting from active agents dict
                        active_agents = AGENT_MANAGER.get_active_agents()
                        if active_agents:
                            agent_name = list(active_agents.keys())[0]
                else:
                    # No current agent, try active agents
                    active_agents = AGENT_MANAGER.get_active_agents()
                    if not active_agents:
                        console.print("[yellow]No active agent found.[/yellow]")
                        return True
                    # Get the first active agent
                    agent_name = list(active_agents.keys())[0]

            # Get history for this specific agent
            history = AGENT_MANAGER.get_message_history(agent_name)

        if not history:
            console.print(f"[yellow]No conversation history for agent '{agent_name}'.[/yellow]")
            return True

        try:
            import networkx as nx

            G = nx.DiGraph()
            prev_node_idx = None
            node_counter = 0  # Use a separate counter for node IDs
            current_turn = (
                0  # Track current turn number (will be incremented on first assistant message)
            )
            last_role = None  # Track last role to detect turn changes

            for idx, msg in enumerate(history):
                role = msg.get("role", "unknown")

                # Skip system messages in graph
                if role == "system":
                    continue

                # Increment turn counter for each assistant message
                if role == "assistant":
                    current_turn += 1

                # User messages don't get a turn number
                # Tool messages inherit the current turn number from the last assistant

                label = role
                extra_info = ""

                if role == "assistant":
                    label = agent_name
                    if msg.get("tool_calls"):
                        tool_calls = msg["tool_calls"]
                        tool_info = []
                        for tc in tool_calls[:3]:  # Show first 3 tool calls
                            if tc.get("function"):
                                func_name = tc["function"].get("name", "")
                                tool_info.append(func_name)
                        if tool_info:
                            extra_info = f"\n[bold {_Z}]Tools:[/bold {_Z}] {', '.join(tool_info)}"
                        if len(tool_calls) > 3:
                            extra_info += f" (+{len(tool_calls)-3} more)"
                elif role == "user":
                    user_content = msg.get("content", "")
                    if user_content:
                        # Truncate long content
                        if len(user_content) > 100:
                            user_content = user_content[:97] + "..."
                        extra_info = f"\n{user_content}"
                elif role == "tool":
                    # For tool responses, try to get the tool name
                    tool_call_id = msg.get("tool_call_id", "")
                    tool_name = "Tool Result"
                    # Look back for the tool call
                    for prev_msg in history[:idx]:
                        if prev_msg.get("role") == "assistant" and prev_msg.get("tool_calls"):
                            for tc in prev_msg["tool_calls"]:
                                if tc.get("id") == tool_call_id:
                                    tool_name = tc.get("function", {}).get("name", "Tool")
                                    break
                    label = f"Tool: {tool_name}"
                    content = msg.get("content", "")
                    if len(content) > 80:
                        content = content[:77] + "..."
                    extra_info = f"\n[dim]{content}[/dim]"

                # User messages don't get turn numbers
                if role == "user":
                    G.add_node(node_counter, role=label, extra_info=extra_info, turn_number=0)
                else:
                    G.add_node(
                        node_counter, role=label, extra_info=extra_info, turn_number=current_turn
                    )
                if prev_node_idx is not None:
                    G.add_edge(prev_node_idx, node_counter)
                prev_node_idx = node_counter
                node_counter += 1

                # Update last_role for turn tracking
                last_role = role

            def render_graph(G):
                """Render the conversation graph as panels with arrows"""
                lines = []
                node_list = list(G.nodes(data=True))
                for i, (idx, data) in enumerate(node_list):
                    role = data.get("role", "unknown")
                    extra_info = data.get("extra_info", "")
                    turn_number = data.get("turn_number", 0)

                    # Color based on role type (CAI REPL palette)
                    if "Tool:" in role:
                        role_fmt = f"[bold {_HINT}]{role}[/bold {_HINT}]"
                        border_style = _HINT
                    elif role == "user":
                        role_fmt = f"[bold white]{role.title()}[/bold white]"
                        border_style = "dim"
                    else:
                        role_fmt = f"[bold {_Z}]{role}[/bold {_Z}]"
                        border_style = _CAI_GREEN

                    # Add turn number to the beginning (except for user messages which have turn_number=0)
                    if turn_number == 0 or role == "user" or role.lower() == "user":
                        panel_content = role_fmt
                    else:
                        panel_content = f"[dim]{turn_number}[/dim] {role_fmt}"
                    if extra_info:
                        panel_content += extra_info

                    panel = Panel(panel_content, expand=False, border_style=border_style)
                    lines.append(panel)
                    if i < len(node_list) - 1:
                        lines.append("[dim]   │\n   │\n   ▼[/dim]")
                return lines

            console.print(f"\n[bold]Conversation Graph for {agent_name}:[/bold]")
            console.print("-" * (20 + len(agent_name)))

            if len(G.nodes) == 0:
                console.print("[yellow]No messages to display in graph.[/yellow]")
            else:
                for item in render_graph(G):
                    console.print(item)
            console.print()
            return True

        except Exception as e:
            console.print(f"[red]Error displaying graph: {e}[/red]")
            return False

    def _handle_multi_agent_graph(self) -> bool:
        """Handle graph display for multiple agents in parallel mode"""
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
        from cai.repl.commands.parallel import PARALLEL_CONFIGS
        from cai.sdk.agents.parallel_isolation import PARALLEL_ISOLATION
        from rich.columns import Columns
        from cai.agents import get_available_agents

        # In TUI mode, get histories from all terminal runners
        if os.getenv("CAI_TUI_MODE") == "true":
            try:
                from cai.tui.cai_terminal import CAITerminal
                app = getattr(CAITerminal, '_instance', None)
                
                if app and hasattr(app, 'session_manager') and app.session_manager.terminal_runners:
                    all_histories = {}
                    
                    for term_num in sorted(app.session_manager.terminal_runners.keys()):
                        runner = app.session_manager.terminal_runners[term_num]
                        if runner and runner.agent:
                            agent = runner.agent
                            agent_name = getattr(agent, "name", runner.config.agent_name)
                            
                            # Add terminal number to distinguish between same agents
                            display_name = f"{agent_name} (T{term_num})"
                            
                            # Get agent ID if available
                            if hasattr(agent, 'model') and hasattr(agent.model, 'agent_id'):
                                agent_id = agent.model.agent_id
                                display_name = f"{agent_name} [{agent_id}] (T{term_num})"
                            
                            # Get history from the agent's model
                            if hasattr(agent, 'model') and hasattr(agent.model, 'message_history'):
                                all_histories[display_name] = agent.model.message_history
                    
                    if all_histories:
                        console.print(f"\n[bold {_Z}]Terminal Agents Conversation Graphs[/bold {_Z}]")
                        console.print(Rule(style=_CAI_GREEN))
                        
                        # Create graphs for each terminal
                        agents_to_show = list(all_histories.items())
                        graphs = []
                        
                        for display_name, history in agents_to_show:
                            if not history:
                                # Empty history
                                graphs.append(
                                    Panel(
                                        "[dim]No messages yet[/dim]",
                                        title=f"[bold {_Z}]{display_name}[/bold {_Z}]",
                                        border_style="dim",
                                        padding=(0, 1),
                                        expand=False,
                                    )
                                )
                                continue
                            
                            # Build compact graph representation (reuse existing code)
                            graph_lines = self._build_compact_graph(history)
                            
                            # Create panel for this agent's graph
                            agent_graph = Panel(
                                "\n".join(graph_lines),
                                title=f"[bold {_Z}]{display_name}[/bold {_Z}]",
                                subtitle=f"[dim]{len(history)} msgs[/dim]" if len(history) > 0 else None,
                                border_style=_CAI_GREEN,
                                padding=(0, 1),
                                expand=False,
                            )
                            graphs.append(agent_graph)
                        
                        # Display graphs
                        if len(graphs) > 1:
                            console.print(Columns(graphs, equal=False, expand=False, padding=(1, 2)))
                        elif graphs:
                            console.print(graphs[0])
                        
                        # Summary
                        total_messages = sum(len(hist) for hist in all_histories.values())
                        console.print(f"\n[bold {_Z}]Summary:[/bold {_Z}]")
                        console.print(
                            f"[white]• Total terminals:[/white] [bold {_Z}]{len(all_histories)}[/bold {_Z}]"
                        )
                        console.print(
                            f"[white]• Total messages:[/white] [bold {_Z}]{total_messages}[/bold {_Z}]"
                        )
                        n_t = len(all_histories) if all_histories else 0
                        avg_t = total_messages / n_t if n_t else 0.0
                        console.print(
                            f"[white]• Average messages per terminal:[/white] [bold {_Z}]{avg_t:.1f}[/bold {_Z}]"
                        )
                        
                        return True
            except Exception:
                # Fall back to standard multi-agent graph
                pass

        # First check if we have isolated histories in parallel mode
        if PARALLEL_ISOLATION.has_isolated_histories():
            # Sync isolated histories with AGENT_MANAGER
            PARALLEL_ISOLATION.sync_with_agent_manager()

        # Get all histories including from parallel isolation
        all_histories = {}

        # Add histories from AGENT_MANAGER
        manager_histories = AGENT_MANAGER.get_all_histories()
        for name, hist in manager_histories.items():
            all_histories[name] = hist

        # Also check isolated histories if we have them (even if not explicitly in parallel mode)
        if PARALLEL_CONFIGS and (
            PARALLEL_ISOLATION.is_parallel_mode() or PARALLEL_ISOLATION.has_isolated_histories()
        ):
            available_agents = get_available_agents()
            for idx, config in enumerate(PARALLEL_CONFIGS, 1):
                agent_id = config.id or f"P{idx}"
                isolated_history = PARALLEL_ISOLATION.get_isolated_history(agent_id)
                if isolated_history:
                    # Build proper display name
                    if config.agent_name in available_agents:
                        agent = available_agents[config.agent_name]
                        display_name = getattr(agent, "name", config.agent_name)
                    else:
                        display_name = config.agent_name

                    # Add instance number if needed
                    agent_counts = {}
                    for c in PARALLEL_CONFIGS:
                        agent_counts[c.agent_name] = agent_counts.get(c.agent_name, 0) + 1

                    if agent_counts[config.agent_name] > 1:
                        instance_num = 0
                        for c in PARALLEL_CONFIGS[:idx]:
                            if c.agent_name == config.agent_name:
                                instance_num += 1
                        instance_num += 1
                        display_name = f"{display_name} #{instance_num}"

                    full_name = f"{display_name} [{agent_id}]"
                    all_histories[full_name] = isolated_history

        if not all_histories and not PARALLEL_CONFIGS:
            console.print(
                "[yellow]No agents configured or no conversation history available.[/yellow]"
            )
            return True

        console.print(f"\n[bold {_Z}]Multi-Agent Conversation Graphs[/bold {_Z}]")
        console.print(Rule(style=_CAI_GREEN))

        # Build list of agents to show
        agents_to_show = []

        # If we have parallel configs, show them in order
        if PARALLEL_CONFIGS:
            available_agents = get_available_agents()

            # Count instances of each agent type for proper naming
            agent_counts = {}
            for c in PARALLEL_CONFIGS:
                agent_counts[c.agent_name] = agent_counts.get(c.agent_name, 0) + 1

            # Track current instance for numbering
            agent_instances = {}

            for idx, config in enumerate(PARALLEL_CONFIGS, 1):
                agent_id = config.id or f"P{idx}"

                # Check if config.agent_name is a pattern name
                if config.agent_name.endswith("_pattern"):
                    # Try to get the pattern
                    from cai.agents.patterns import get_pattern

                    pattern = get_pattern(config.agent_name)
                    if pattern and hasattr(pattern, "entry_agent"):
                        # For swarm patterns, use the entry agent
                        base_agent = pattern.entry_agent
                        base_display_name = getattr(base_agent, "name", config.agent_name)
                    else:
                        # Skip if pattern not found
                        continue
                else:
                    # Get the agent instance to get its actual name
                    base_agent = available_agents.get(config.agent_name)
                    if not base_agent:
                        base_agent = available_agents.get(config.agent_name.lower())

                    if base_agent:
                        # Get the display name from the agent object
                        base_display_name = getattr(base_agent, "name", config.agent_name)
                    else:
                        # Agent not found
                        agents_to_show.append((f"{config.agent_name} [{agent_id}]", []))
                        continue

                # Determine instance number if there are duplicates
                if agent_counts[config.agent_name] > 1:
                    if config.agent_name not in agent_instances:
                        agent_instances[config.agent_name] = 0
                    agent_instances[config.agent_name] += 1
                    instance_num = agent_instances[config.agent_name]
                else:
                    instance_num = 1

                # Construct the display name
                if agent_counts[config.agent_name] > 1:
                    display_name = f"{base_display_name} #{instance_num}"
                else:
                    display_name = base_display_name

                full_name = f"{display_name} [{agent_id}]"

                # Look for this agent in all_histories
                if full_name in all_histories:
                    agents_to_show.append((full_name, all_histories[full_name]))
                else:
                    # Try without the ID suffix
                    found = False
                    for hist_name, history in all_histories.items():
                        if display_name in hist_name or f"[{agent_id}]" in hist_name:
                            agents_to_show.append((hist_name, history))
                            found = True
                            break

                    if not found:
                        # No history yet
                        agents_to_show.append((full_name, []))
        else:
            # No parallel configs, just show all histories
            agents_to_show = list(all_histories.items())

        # Create graphs for each agent
        graphs = []
        for display_name, history in agents_to_show:
            if not history:
                # Empty history
                graphs.append(
                    Panel(
                        "[dim]No messages yet[/dim]",
                        title=f"[bold {_Z}]{display_name}[/bold {_Z}]",
                        border_style="dim",
                        padding=(0, 1),
                        expand=False,
                    )
                )
                continue

            try:
                import networkx as nx

                G = nx.DiGraph()
                prev_node_idx = None
                node_counter = 0
                message_count = 0
                turn_counter = 0  # Will be incremented on first assistant message
                last_role = None

                # Build graph for this agent's history
                for idx, msg in enumerate(history):
                    role = msg.get("role", "unknown")

                    # Skip system messages
                    if role == "system":
                        continue

                    message_count += 1

                    # Increment turn counter only for assistant messages
                    if role == "assistant":
                        turn_counter += 1

                    # Create node label
                    if role == "assistant":
                        label = "Assistant"
                        if msg.get("tool_calls"):
                            label += f" ({len(msg['tool_calls'])} tools)"
                    elif role == "user":
                        label = "User"
                    elif role == "tool":
                        label = "Tool"
                    else:
                        label = role.title()

                    # User messages don't get turn numbers
                    if role == "user":
                        G.add_node(node_counter, role=label, turn_number=0)
                    else:
                        G.add_node(node_counter, role=label, turn_number=turn_counter)
                    if prev_node_idx is not None:
                        G.add_edge(prev_node_idx, node_counter)
                    prev_node_idx = node_counter
                    node_counter += 1

                    # Update last_role for turn tracking
                    last_role = role

                # Create simplified graph representation
                graph_lines = []
                nodes = list(G.nodes(data=True))

                if nodes:
                    # Create a more compact representation
                    for i, (idx, data) in enumerate(nodes):
                        role = data.get("role", "unknown")
                        turn_number = data.get("turn_number", 0)

                        # Compact box representation with turn number (except for user)
                        if turn_number == 0 or role == "User":
                            # No turn number for user messages
                            graph_lines.append(f"[bold white]● User[/bold white]")
                        else:
                            turn_prefix = f"[dim]{turn_number}[/dim] "

                            if "Tool" in role:
                                # Shorten tool representation
                                if "(" in role:
                                    # Extract just the tool name
                                    role_short = "Tool"
                                else:
                                    role_short = role
                                graph_lines.append(
                                    f"{turn_prefix}[bold {_HINT}]◆ {role_short}[/bold {_HINT}]"
                                )
                            elif "Assistant" in role:
                                # Check if it has tool calls
                                if "tools)" in role:
                                    graph_lines.append(
                                        f"{turn_prefix}[bold {_Z}]▶ Agent (tools)[/bold {_Z}]"
                                    )
                                else:
                                    graph_lines.append(f"{turn_prefix}[bold {_Z}]▶ Agent[/bold {_Z}]")
                            else:
                                graph_lines.append(f"{turn_prefix}[bold {_Z}]▶ {role}[/bold {_Z}]")

                        if i < len(nodes) - 1:
                            graph_lines.append("    ↓")
                else:
                    # No non-system messages
                    graph_lines.append("[dim]No messages yet[/dim]")

                # Create panel for this agent's graph
                agent_graph = Panel(
                    "\n".join(graph_lines),
                    title=f"[bold {_Z}]{display_name}[/bold {_Z}]",
                    subtitle=f"[dim]{message_count} msgs[/dim]" if message_count > 0 else None,
                    border_style=_CAI_GREEN,
                    padding=(0, 1),
                    expand=False,
                )
                graphs.append(agent_graph)

            except Exception as e:
                graphs.append(
                    Panel(
                        f"[red]Error: {str(e)}[/red]",
                        title=f"[bold {_Z}]{display_name}[/bold {_Z}]",
                        border_style="red",
                    )
                )

        # Display graphs in columns if multiple agents
        if len(graphs) > 1:
            # Create columns layout with appropriate width
            # Use equal=False to let panels size naturally
            console.print(Columns(graphs, equal=False, expand=False, padding=(1, 2)))
        elif graphs:
            # Single graph
            console.print(graphs[0])

        # Summary statistics
        console.print(f"\n[bold {_Z}]Summary:[/bold {_Z}]")
        # Count only actual messages (skip system messages)
        total_messages = 0
        for _, hist in agents_to_show:
            for msg in hist:
                if msg.get("role") != "system":
                    total_messages += 1

        n_ag = len(agents_to_show)
        avg_a = total_messages / n_ag if n_ag else 0.0
        console.print(
            f"[white]• Total agents:[/white] [bold {_Z}]{n_ag}[/bold {_Z}]"
        )
        console.print(
            f"[white]• Total messages:[/white] [bold {_Z}]{total_messages}[/bold {_Z}]"
        )
        console.print(
            f"[white]• Average messages per agent:[/white] [bold {_Z}]{avg_a:.1f}[/bold {_Z}]"
        )

        return True

    def handle_agent_graph(self, agent_id: str) -> bool:
        """Handle graph display for a specific agent by ID."""
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
        from cai.sdk.agents.parallel_isolation import PARALLEL_ISOLATION
        from cai.repl.commands.parallel import PARALLEL_CONFIGS
        from cai.agents import get_available_agents

        # Normalize agent ID
        agent_id = agent_id.upper()

        # First check if we're in parallel mode with isolation
        if PARALLEL_ISOLATION.is_parallel_mode():
            # Look for agent in PARALLEL_CONFIGS
            for idx, config in enumerate(PARALLEL_CONFIGS, 1):
                if f"P{idx}" == agent_id:
                    # Found the config, get the agent display name
                    available_agents = get_available_agents()

                    # Check if config.agent_name is a pattern
                    if config.agent_name.endswith("_pattern"):
                        from cai.agents.patterns import get_pattern

                        pattern = get_pattern(config.agent_name)
                        if pattern and hasattr(pattern, "entry_agent"):
                            agent = pattern.entry_agent
                            agent_display_name = getattr(agent, "name", config.agent_name)
                        else:
                            console.print(
                                f"[yellow]Pattern '{config.agent_name}' not found[/yellow]"
                            )
                            return True
                    elif config.agent_name in available_agents:
                        agent = available_agents[config.agent_name]
                        agent_display_name = getattr(agent, "name", config.agent_name)
                    else:
                        console.print(f"[yellow]Agent '{config.agent_name}' not found[/yellow]")
                        return True

                    # Count instances for proper naming
                    agent_counts = {}
                    for c in PARALLEL_CONFIGS:
                        agent_counts[c.agent_name] = agent_counts.get(c.agent_name, 0) + 1

                    # Determine instance number
                    instance_num = 0
                    for c in PARALLEL_CONFIGS[:idx]:
                        if c.agent_name == config.agent_name:
                            instance_num += 1
                    instance_num += 1

                    # Build the agent name with instance number if needed
                    if agent_counts[config.agent_name] > 1:
                        full_agent_name = f"{agent_display_name} #{instance_num}"
                    else:
                        full_agent_name = agent_display_name

                    # Get the isolated history for this agent
                    history = PARALLEL_ISOLATION.get_isolated_history(agent_id)
                    if history is None:
                        # Try syncing first
                        PARALLEL_ISOLATION.sync_with_agent_manager()
                        history = PARALLEL_ISOLATION.get_isolated_history(agent_id)

                    if history:
                        # Build a temporary graph for this specific agent
                        console.print(
                            f"[bold {_Z}]Showing graph for {full_agent_name} [{agent_id}][/bold {_Z}]"
                        )
                        # Manually build the graph using the isolated history
                        try:
                            import networkx as nx

                            G = nx.DiGraph()
                            prev_node_idx = None
                            node_counter = 0
                            current_turn = 0  # Will be incremented to 1 on first user message
                            last_role = None

                            for idx, msg in enumerate(history):
                                role = msg.get("role", "unknown")

                                # Skip system messages in graph
                                if role == "system":
                                    continue

                                # Increment turn counter only for assistant messages
                                # This groups assistant + tools in same turn
                                if role == "assistant":
                                    current_turn += 1

                                label = role
                                extra_info = ""

                                if role == "assistant":
                                    label = full_agent_name
                                    if msg.get("tool_calls"):
                                        tool_calls = msg["tool_calls"]
                                        tool_info = []
                                        for tc in tool_calls[:3]:  # Show first 3 tool calls
                                            if tc.get("function"):
                                                func_name = tc["function"].get("name", "")
                                                tool_info.append(func_name)
                                        if tool_info:
                                            extra_info = (
                                                f"\n[bold {_Z}]Tools:[/bold {_Z}] {', '.join(tool_info)}"
                                            )
                                        if len(tool_calls) > 3:
                                            extra_info += f" (+{len(tool_calls)-3} more)"
                                elif role == "user":
                                    user_content = msg.get("content", "")
                                    if user_content:
                                        # Truncate long content
                                        if len(user_content) > 100:
                                            user_content = user_content[:97] + "..."
                                        extra_info = f"\n{user_content}"
                                elif role == "tool":
                                    # For tool responses, try to get the tool name
                                    tool_call_id = msg.get("tool_call_id", "")
                                    tool_name = "Tool Result"
                                    # Look back for the tool call
                                    for prev_msg in history[:idx]:
                                        if prev_msg.get("role") == "assistant" and prev_msg.get(
                                            "tool_calls"
                                        ):
                                            for tc in prev_msg["tool_calls"]:
                                                if tc.get("id") == tool_call_id:
                                                    tool_name = tc.get("function", {}).get(
                                                        "name", "Tool"
                                                    )
                                                    break
                                    label = f"Tool: {tool_name}"
                                    content = msg.get("content", "")
                                    if len(content) > 80:
                                        content = content[:77] + "..."
                                    extra_info = f"\n[dim]{content}[/dim]"

                                # User messages don't get turn numbers
                                if role == "user":
                                    G.add_node(
                                        node_counter,
                                        role=label,
                                        extra_info=extra_info,
                                        turn_number=0,
                                    )
                                else:
                                    G.add_node(
                                        node_counter,
                                        role=label,
                                        extra_info=extra_info,
                                        turn_number=current_turn,
                                    )
                                if prev_node_idx is not None:
                                    G.add_edge(prev_node_idx, node_counter)
                                prev_node_idx = node_counter
                                node_counter += 1

                                # Update last_role for turn tracking
                                last_role = role

                            def render_graph(G):
                                """Render the conversation graph as panels with arrows"""
                                lines = []
                                node_list = list(G.nodes(data=True))
                                for i, (idx, data) in enumerate(node_list):
                                    role = data.get("role", "unknown")
                                    extra_info = data.get("extra_info", "")
                                    turn_number = data.get("turn_number", 0)
                                    if turn_number == 0:
                                        turn_number = 1  # Default to 1 if not set

                                    # Color based on role type
                                    if "Tool:" in role:
                                        role_fmt = f"[bold {_HINT}]{role}[/bold {_HINT}]"
                                        border_style = _HINT
                                    elif role == "user":
                                        role_fmt = f"[bold white]{role.title()}[/bold white]"
                                        border_style = "dim"
                                    else:
                                        role_fmt = f"[bold {_Z}]{role}[/bold {_Z}]"
                                        border_style = _CAI_GREEN

                                    # Add turn number to the beginning (except for user messages)
                                    if role == "user" or role.lower() == "user":
                                        panel_content = role_fmt
                                    else:
                                        panel_content = (
                                            f"[dim]{turn_number}[/dim] {role_fmt}"
                                        )
                                    if extra_info:
                                        panel_content += extra_info

                                    from rich.panel import Panel

                                    panel = Panel(
                                        panel_content, expand=False, border_style=border_style
                                    )
                                    lines.append(panel)
                                    if i < len(node_list) - 1:
                                        lines.append("[dim]   │\n   │\n   ▼[/dim]")
                                return lines

                            console.print(
                                f"\n[bold]Conversation Graph for {full_agent_name}:[/bold]"
                            )
                            console.print("-" * (20 + len(full_agent_name)))

                            if len(G.nodes) == 0:
                                console.print("[yellow]No messages to display in graph.[/yellow]")
                            else:
                                for item in render_graph(G):
                                    console.print(item)
                            console.print()
                            return True

                        except Exception as e:
                            console.print(f"[red]Error displaying graph: {e}[/red]")
                            return False
                    else:
                        console.print(
                            f"[yellow]No history found for {full_agent_name} [{agent_id}][/yellow]"
                        )
                        return True

        # Fall back to regular AGENT_MANAGER lookup
        agent_name = AGENT_MANAGER.get_agent_by_id(agent_id)
        if not agent_name:
            console.print(f"[yellow]No agent found with ID '{agent_id}'[/yellow]")
            console.print("[dim]Use '/history' to see available agents with IDs[/dim]")
            return True

        console.print(f"[bold {_Z}]Showing graph for {agent_name} [{agent_id}][/bold {_Z}]")
        return self._handle_single_agent_graph(agent_name)

    def handle_all(self, args: Optional[List[str]] = None) -> bool:
        """Show graphs for all agents with history."""
        return self._handle_multi_agent_graph()

    def handle_timeline(self, args: Optional[List[str]] = None) -> bool:
        """Show a table of messages per agent (ordered by message index within each agent)."""
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
        from rich.table import Table

        all_histories = AGENT_MANAGER.get_all_histories()

        if not all_histories:
            console.print(f"[bold {_Z}]No agents have conversation history[/bold {_Z}]")
            return True

        # Collect all messages with timestamps and agent info
        timeline_events = []
        for display_name, history in all_histories.items():
            for idx, msg in enumerate(history):
                # Extract agent ID from display name
                agent_id = None
                if "[" in display_name and "]" in display_name:
                    agent_id = display_name[display_name.rindex("[") + 1 : display_name.rindex("]")]
                    agent_base_name = display_name[: display_name.rindex("[")].strip()
                else:
                    agent_base_name = display_name

                timeline_events.append(
                    {
                        "agent": agent_base_name,
                        "agent_id": agent_id or "?",
                        "index": idx,
                        "role": msg.get("role", "unknown"),
                        "content": msg.get("content", ""),
                        "tool_calls": msg.get("tool_calls", []),
                        "timestamp": idx,  # Using index as pseudo-timestamp
                    }
                )

        # Sort by pseudo-timestamp (in real implementation, would use actual timestamps)
        timeline_events.sort(key=lambda x: x["timestamp"])

        # Create timeline table
        table = Table(
            title=f"[bold {_Z}]Agent messages (by index)[/bold {_Z}]",
            show_header=True,
            header_style="bold white",
            border_style=_CAI_GREEN,
            title_style=f"bold {_Z}",
            box=None,
            pad_edge=True,
        )
        table.add_column("Time", style=_HINT, width=6)
        table.add_column("Agent", style=f"bold {_Z}", width=25)
        table.add_column("Role", style="white", width=10)
        table.add_column("Action", style="white")

        for event in timeline_events:
            # Format time (using index as pseudo-time)
            time_str = f"T+{event['timestamp']:03d}"

            # Format agent with ID
            agent_str = f"{event['agent']} [{event['agent_id']}]"

            # Format action based on role
            if event["role"] == "user":
                action = (
                    f"User: {event['content'][:80]}..."
                    if len(event["content"]) > 80
                    else f"User: {event['content']}"
                )
            elif event["role"] == "assistant":
                if event["tool_calls"]:
                    tools = [
                        tc.get("function", {}).get("name", "?") for tc in event["tool_calls"][:3]
                    ]
                    action = f"Called tools: {', '.join(tools)}"
                    if len(event["tool_calls"]) > 3:
                        action += f" (+{len(event['tool_calls'])-3} more)"
                else:
                    action = (
                        f"Response: {event['content'][:60]}..."
                        if len(event["content"]) > 60
                        else f"Response: {event['content']}"
                    )
            elif event["role"] == "tool":
                action = (
                    f"Tool result: {event['content'][:60]}..."
                    if len(event["content"]) > 60
                    else f"Tool result: {event['content']}"
                )
            else:
                action = (
                    f"{event['role']}: {event['content'][:60]}..."
                    if len(event["content"]) > 60
                    else f"{event['role']}: {event['content']}"
                )

            # Color role (CAI palette)
            r = event["role"]
            if r == "user":
                role_cell = f"[white]{r}[/white]"
            elif r == "assistant":
                role_cell = f"[bold {_Z}]{r}[/bold {_Z}]"
            elif r == "tool":
                role_cell = f"[bold {_HINT}]{r}[/bold {_HINT}]"
            elif r == "system":
                role_cell = f"[dim]{r}[/dim]"
            else:
                role_cell = f"[white]{r}[/white]"

            table.add_row(time_str, agent_str, role_cell, action)

        console.print(table)
        console.print(f"\n[bold {_Z}]Total events: {len(timeline_events)}[/bold {_Z}]")
        return True

    def handle_stats(self, args: Optional[List[str]] = None) -> bool:
        """Show detailed statistics about agent conversations."""
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
        from rich.table import Table
        from collections import Counter

        all_histories = AGENT_MANAGER.get_all_histories()

        if not all_histories:
            console.print(f"[bold {_Z}]No agents have conversation history[/bold {_Z}]")
            return True

        # Create statistics table
        stats_table = Table(
            title=f"[bold {_Z}]Agent Conversation Statistics[/bold {_Z}]",
            show_header=True,
            header_style="bold white",
            border_style=_CAI_GREEN,
            title_style=f"bold {_Z}",
            box=None,
            pad_edge=True,
        )
        stats_table.add_column("Agent", style=f"bold {_Z}")
        stats_table.add_column("Messages", style="white", justify="right")
        stats_table.add_column("User", style="white", justify="right")
        stats_table.add_column("Assistant", style=f"bold {_Z}", justify="right")
        stats_table.add_column("Tools", style=_HINT, justify="right")
        stats_table.add_column("Tool Calls", style=_HINT, justify="right")
        stats_table.add_column("Avg Length", style="white", justify="right")

        total_stats = Counter()

        for display_name, history in sorted(all_histories.items()):
            if not history:
                continue

            # Count message types
            role_counts = Counter(msg.get("role", "unknown") for msg in history)

            # Count total tool calls
            total_tool_calls = sum(
                len(msg.get("tool_calls", [])) for msg in history if msg.get("role") == "assistant"
            )

            # Calculate average message length
            content_lengths = [
                len(str(msg.get("content", ""))) for msg in history if msg.get("content")
            ]
            avg_length = sum(content_lengths) / len(content_lengths) if content_lengths else 0

            # Add to totals
            total_stats.update(role_counts)
            total_stats["total_tool_calls"] += total_tool_calls
            total_stats["total_messages"] += len(history)

            stats_table.add_row(
                display_name,
                str(len(history)),
                str(role_counts.get("user", 0)),
                str(role_counts.get("assistant", 0)),
                str(role_counts.get("tool", 0)),
                str(total_tool_calls),
                f"{avg_length:.0f}",
            )

        # Add totals row
        stats_table.add_section()
        stats_table.add_row(
            f"[bold white]TOTAL[/bold white]",
            f"[bold {_Z}]{total_stats['total_messages']}[/bold {_Z}]",
            f"[bold {_Z}]{total_stats.get('user', 0)}[/bold {_Z}]",
            f"[bold {_Z}]{total_stats.get('assistant', 0)}[/bold {_Z}]",
            f"[bold {_Z}]{total_stats.get('tool', 0)}[/bold {_Z}]",
            f"[bold {_Z}]{total_stats.get('total_tool_calls', 0)}[/bold {_Z}]",
            "",
        )

        console.print(stats_table)

        # Additional insights
        console.print(f"\n[bold {_Z}]Insights:[/bold {_Z}]")
        if total_stats["total_messages"] > 0:
            user_ratio = total_stats.get("user", 0) / total_stats["total_messages"] * 100
            assistant_ratio = total_stats.get("assistant", 0) / total_stats["total_messages"] * 100
            tool_ratio = total_stats.get("tool", 0) / total_stats["total_messages"] * 100

            console.print(
                f"[white]• Message distribution: User {user_ratio:.1f}%, Assistant "
                f"{assistant_ratio:.1f}%, Tools {tool_ratio:.1f}%[/white]"
            )

            if total_stats.get("assistant", 0) > 0:
                tools_per_assistant = total_stats.get("total_tool_calls", 0) / total_stats.get(
                    "assistant", 0
                )
                console.print(
                    f"[white]• Average tool calls per assistant message: [/white]"
                    f"[bold {_Z}]{tools_per_assistant:.2f}[/bold {_Z}]"
                )

        console.print(
            f"[white]• Active agents:[/white] [bold {_Z}]{len(all_histories)}[/bold {_Z}]"
        )
        console.print(
            f"[white]• Total conversations:[/white] [bold {_Z}]"
            f"{sum(1 for h in all_histories.values() if h)}[/bold {_Z}]"
        )

        return True

    def handle_export(self, args: Optional[List[str]] = None) -> bool:
        """Export graph data to various formats."""
        if not args:
            console.print(f"[bold {_Z}]Export format required[/bold {_Z}]")
            console.print(
                f"[white]Usage:[/white] [bold {_Z}]/graph export <format> [filename][/bold {_Z}]"
            )
            console.print(
                f"[white]Formats:[/white] [bold {_Z}]json[/bold {_Z}], [bold {_Z}]dot[/bold {_Z}], "
                f"[bold {_Z}]mermaid[/bold {_Z}]"
            )
            return True

        format_type = args[0].lower()
        filename = args[1] if len(args) > 1 else None

        if format_type not in ["json", "dot", "mermaid"]:
            console.print(f"[red]Unknown export format: {format_type}[/red]")
            console.print("Supported formats: json, dot, mermaid")
            return False

        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
        import json
        import datetime

        all_histories = AGENT_MANAGER.get_all_histories()

        if not all_histories:
            console.print(f"[bold {_Z}]No conversation history to export[/bold {_Z}]")
            return True

        # Generate default filename if not provided
        if not filename:
            timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
            filename = f"cai_graph_{timestamp}.{format_type}"

        try:
            if format_type == "json":
                # Export as JSON
                export_data = {"timestamp": datetime.datetime.now().isoformat(), "agents": {}}

                for agent_name, history in all_histories.items():
                    export_data["agents"][agent_name] = {
                        "message_count": len(history),
                        "messages": history,
                    }

                with open(filename, "w", encoding="utf-8") as f:
                    json.dump(export_data, f, indent=2)

            elif format_type == "dot":
                # Export as Graphviz DOT format
                dot_content = ["digraph CAI_Conversations {"]
                dot_content.append("  rankdir=TB;")
                dot_content.append("  node [shape=box];")

                node_id = 0
                for agent_name, history in all_histories.items():
                    dot_content.append(f'\n  subgraph "cluster_{agent_name.replace(" ", "_")}" {{')
                    dot_content.append(f'    label="{agent_name}";')

                    prev_node = None
                    for msg in history:
                        if msg.get("role") == "system":
                            continue

                        role = msg.get("role", "unknown")
                        node_name = f"node_{node_id}"

                        if role == "user":
                            dot_content.append(f'    {node_name} [label="{role}", color=blue];')
                        elif role == "assistant":
                            dot_content.append(f'    {node_name} [label="{role}", color=green];')
                        elif role == "tool":
                            dot_content.append(f'    {node_name} [label="{role}", color=red];')

                        if prev_node:
                            dot_content.append(f"    {prev_node} -> {node_name};")

                        prev_node = node_name
                        node_id += 1

                    dot_content.append("  }")

                dot_content.append("}")

                with open(filename, "w", encoding="utf-8") as f:
                    f.write("\n".join(dot_content))

            elif format_type == "mermaid":
                # Export as Mermaid diagram
                mermaid_content = ["graph TD"]

                node_id = 0
                for agent_name, history in all_histories.items():
                    agent_safe_name = agent_name.replace(" ", "_").replace("[", "").replace("]", "")

                    prev_node = None
                    for msg in history:
                        if msg.get("role") == "system":
                            continue

                        role = msg.get("role", "unknown")
                        node_name = f"{agent_safe_name}_{node_id}"

                        if role == "user":
                            mermaid_content.append(f'    {node_name}["{role}"]:::user')
                        elif role == "assistant":
                            tools = len(msg.get("tool_calls", []))
                            label = f"{role} ({tools} tools)" if tools > 0 else role
                            mermaid_content.append(f'    {node_name}["{label}"]:::assistant')
                        elif role == "tool":
                            mermaid_content.append(f'    {node_name}["{role}"]:::tool')

                        if prev_node:
                            mermaid_content.append(f"    {prev_node} --> {node_name}")

                        prev_node = node_name
                        node_id += 1

                # Add styling
                mermaid_content.extend(
                    [
                        "",
                        f"classDef user fill:{_HINT},stroke:#2c3e50,color:#fff",
                        f"classDef assistant fill:{_CAI_GREEN},stroke:#0d3320,color:#000",
                        "classDef tool fill:#2c3e50,stroke:#1a1a1a,color:#fff",
                    ]
                )

                with open(filename, "w", encoding="utf-8") as f:
                    f.write("\n".join(mermaid_content))

            console.print(f"[bold {_Z}]Successfully exported to {filename}[/bold {_Z}]")

            # Show usage hints based on format
            if format_type == "dot":
                console.print("[dim]To render: dot -Tpng {filename} -o output.png[/dim]")
            elif format_type == "mermaid":
                console.print("[dim]Use with Mermaid Live Editor: https://mermaid.live[/dim]")

        except Exception as e:
            console.print(f"[red]Error exporting graph: {e}[/red]")
            return False

        return True
    
    def _build_compact_graph(self, history: list) -> list:
        """Build a compact graph representation from message history"""
        graph_lines = []
        turn_counter = 0
        
        for idx, msg in enumerate(history):
            role = msg.get("role", "unknown")
            
            # Skip system messages
            if role == "system":
                continue
            
            # Increment turn counter for assistant messages
            if role == "assistant":
                turn_counter += 1
            
            # Format based on role
            if role == "user":
                graph_lines.append(f"[bold white]● User[/bold white]")
            elif role == "assistant":
                # Check if it has tool calls
                if msg.get("tool_calls"):
                    graph_lines.append(
                        f"[dim]{turn_counter}[/dim] [bold {_Z}]▶ Agent (tools)[/bold {_Z}]"
                    )
                else:
                    graph_lines.append(f"[dim]{turn_counter}[/dim] [bold {_Z}]▶ Agent[/bold {_Z}]")
            elif role == "tool":
                # Tool messages inherit the current turn number
                graph_lines.append(
                    f"[dim]{turn_counter}[/dim] [bold {_HINT}]◆ Tool[/bold {_HINT}]"
                )
            else:
                graph_lines.append(f"[bold {_Z}]▶ {role}[/bold {_Z}]")
            
            # Add arrow between messages
            if idx < len(history) - 1 and history[idx + 1].get("role") != "system":
                graph_lines.append("    ↓")
        
        return graph_lines if graph_lines else ["[dim]No messages yet[/dim]"]


# Register the command
register_command(GraphCommand())
