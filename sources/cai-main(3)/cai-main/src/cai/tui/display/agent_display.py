"""
Agent message display for TUI
"""

import os
from datetime import datetime
from typing import Any

from rich.box import ROUNDED
from rich.console import Console
from rich.panel import Panel
from rich.table import Table
from rich.text import Text

from .base import BaseDisplay, DisplayContext, OutputType
from .panel_formatter import PanelFormatter

# Import COST_TRACKER for session cost
try:
    from cai.util import COST_TRACKER
except ImportError:
    COST_TRACKER = None


class AgentDisplay(BaseDisplay):
    """Handles agent message display for TUI"""

    def __init__(self):
        """Initialize agent display"""
        super().__init__()
        self._message_history: dict[str, list[dict]] = {}

    def display(self, context: DisplayContext, data: dict[str, Any]) -> None:
        """Display agent messages"""
        messages = data.get("messages", [])
        if not messages:
            return

        # Get terminal output directly from context
        from cai.tui.core.terminal_console import get_terminal_output
        import os

        terminal_output = get_terminal_output(context.terminal_id)
        
        # Debug logging
        if os.getenv("CAI_DEBUG") == "2":
            print(f"[DEBUG] AgentDisplay: context.terminal_id = {context.terminal_id}")
            print(f"[DEBUG] AgentDisplay: context.terminal_number = {context.terminal_number}")
            print(f"[DEBUG] AgentDisplay: terminal_output = {terminal_output}")

        # If we got a UniversalTerminal, get its output (RichLog)
        if terminal_output and hasattr(terminal_output, 'output') and terminal_output.output:
            terminal_output = terminal_output.output

        if not terminal_output:
            # Fallback to console
            console = self.get_console()
            if not console:
                console = Console()
        else:
            # We'll write directly to terminal output
            console = terminal_output

        # Track message history for this context
        context_key = f"{context.terminal_id}:{context.agent_name}"
        if context_key not in self._message_history:
            self._message_history[context_key] = []

        # Display each message
        for msg in messages:
            # Check if already displayed
            msg_key = self._generate_message_key(context, msg)
            if self._is_duplicate(msg_key):
                continue

            # Create metadata
            metadata = {
                "timestamp": datetime.now().strftime("%H:%M:%S"),
                "model": data.get("model", context.metadata.get("model")),
                "interaction": context.interaction_counter,
            }

            # Get message content
            content = self._extract_message_content(msg)

            # Check if this is a tool call
            if msg.get("tool_calls") and not content:
                # This is a tool call message, display it differently
                self._display_tool_calls(
                    terminal_output if terminal_output else console, msg, metadata, context
                )
                continue

            if not content:
                continue

            # Create and display panel
            role = msg.get("role", "assistant")
            agent_name = context.agent_name or f"Agent {context.terminal_number}"
            
            # Meta agent integration - capture agent output
            if role == "assistant" and os.getenv("CAI_META_AGENT", "false").lower() == "true":
                try:
                    import asyncio
                    from cai.tui.meta_agent_controller import meta_agent_post_output_hook
                    # Run the hook in the background - non-blocking
                    asyncio.create_task(meta_agent_post_output_hook(agent_name, content))
                except (ImportError, RuntimeError):
                    # RuntimeError can occur if no event loop is running
                    pass

            if role == "assistant":
                # Always use the token-aware display method
                token_info = data.get("token_info", {})
                self._display_agent_message_with_tokens(
                    terminal_output if terminal_output else console,
                    agent_name,
                    content,
                    metadata,
                    token_info,
                )

            # Add to history
            self._message_history[context_key].append(
                {
                    "timestamp": metadata["timestamp"],
                    "role": role,
                    "content": content,
                    "metadata": metadata,
                }
            )

            # Add to context output
            context.add_output(
                {
                    "type": OutputType.AGENT_MESSAGE,
                    "role": role,
                    "content": content,
                    "metadata": metadata,
                }
            )

    def start_streaming(
        self, context: DisplayContext, stream_id: str, data: dict[str, Any]
    ) -> None:
        """Start streaming agent message"""
        # Agent messages typically don't use streaming in the same way as tools
        # But we'll support it for consistency
        session = {
            "context": context,
            "agent_name": context.agent_name or f"Agent {context.terminal_number}",
            "buffer": "",
            "metadata": {
                "timestamp": datetime.now().strftime("%H:%M:%S"),
                "model": data.get("model", context.metadata.get("model")),
            },
            "console": self.get_console() or Console(),
        }

        with self._lock:
            self._active_streams[stream_id] = session

    def update_streaming(self, stream_id: str, data: dict[str, Any]) -> None:
        """Update streaming message"""
        with self._lock:
            session = self._active_streams.get(stream_id)
            if not session:
                return

        # Update buffer
        content_delta = data.get("content", "")
        session["buffer"] += content_delta

        # For now, we'll use the streaming display for real-time updates
        # This could be enhanced with Live display similar to tools

    def finish_streaming(self, stream_id: str, data: dict[str, Any]) -> None:
        """Finish streaming message"""
        with self._lock:
            session = self._active_streams.pop(stream_id, None)
            if not session:
                return

        # Display final message
        console = session["console"]
        # Extract token_info from data if available
        token_info = data.get("token_info", {})
        
        panel = PanelFormatter.create_agent_panel(
            agent_name=session["agent_name"],
            message=session["buffer"],
            metadata=session["metadata"],
            streaming=False,
            token_info=token_info,  # Pass token info to show costs
        )
        self._print_to_console(console, panel)

        # Add to context history
        context = session["context"]
        if context:
            context.add_output(
                {
                    "type": OutputType.AGENT_MESSAGE,
                    "role": "assistant",
                    "content": session["buffer"],
                    "metadata": session["metadata"],
                }
            )

    def display_message_history(self, context: DisplayContext) -> None:
        """Display message history for a context"""
        context_key = f"{context.terminal_id}:{context.agent_name}"
        history = self._message_history.get(context_key, [])

        if not history:
            return

        console = self.get_console() or Console()

        # Create a table or formatted display
        table = Table(show_header=True, header_style="bold magenta", expand=True)
        table.add_column("#", style="dim", width=3)
        table.add_column("Time", style="cyan", width=10)
        table.add_column("Role", style="blue", width=10)
        table.add_column("Content", width=80)

        for i, msg in enumerate(history):
            # Truncate content for table display
            content = msg["content"]
            if len(content) > 100:
                content = content[:97] + "..."

            table.add_row(str(i + 1), msg["timestamp"], msg["role"].capitalize(), content)

        panel = Panel(
            table,
            title=f"[bold]Message History - {context.agent_name}[/bold]",
            border_style="magenta",
            box=ROUNDED,
            expand=False,
        )

        self._print_to_console(console, panel)

    def _extract_message_content(self, message: dict) -> str:
        """Extract content from message dict"""
        content = message.get("content")

        if content is None:
            return ""

        if isinstance(content, str):
            return content

        if isinstance(content, list):
            # Handle multi-part content
            text_parts = []
            for part in content:
                if isinstance(part, dict) and part.get("type") == "text":
                    text_parts.append(part.get("text", ""))
                elif isinstance(part, str):
                    text_parts.append(part)
            return "\n".join(text_parts)

        return str(content)

    def _generate_message_key(self, context: DisplayContext, message: dict) -> str:
        """Generate unique key for message deduplication.

        NOTE: For messages with tool_calls, we use the call_ids instead of
        interaction_counter to prevent re-display when the turn changes.
        """
        content = self._extract_message_content(message)
        content_preview = content[:50] if content else ""

        # For tool call messages, use call_ids instead of interaction_counter
        # This prevents re-display when the turn changes
        tool_calls = message.get("tool_calls", [])
        if tool_calls:
            # Extract all call_ids for stable deduplication
            call_ids = []
            for tc in tool_calls:
                if isinstance(tc, dict):
                    call_id = tc.get("id", "")
                    if call_id:
                        call_ids.append(call_id)

            if call_ids:
                # Use call_ids as the key - stable across turns
                return f"msg_with_tools:{context.terminal_id}:{':'.join(sorted(call_ids))}"

        # For regular messages without tool_calls, use content-based key
        parts = [
            context.terminal_id,
            context.agent_name or "",
            message.get("role", "unknown"),
            content_preview,
        ]

        return ":".join(parts)

    def _print_to_console(self, console: Any, content: Any) -> None:
        """Print to console or terminal output"""
        if hasattr(console, "print"):
            console.print(content)
        elif hasattr(console, "write"):
            # It's a RichLog or UniversalTerminal
            # Try with expand first, fall back without it
            try:
                console.write(content, expand=True)
            except TypeError as e:
                if "expand" in str(e):
                    console.write(content)
                else:
                    raise
        else:
            # Fallback: render to string
            temp_console = Console()
            with temp_console.capture() as capture:
                temp_console.print(content)
            # Note: we don't have anywhere to write the captured output here

    def _display_tool_calls(self, console: Any, msg: dict, metadata: dict, context: DisplayContext) -> None:
        """Display tool calls from a message"""
        tool_calls = msg.get("tool_calls", [])
        if not tool_calls:
            return

        # Display each tool call
        for tool_call in tool_calls:
            if isinstance(tool_call, dict):
                function = tool_call.get("function", {})
                tool_name = function.get("name", "unknown")
                args_str = function.get("arguments", "{}")
                call_id = tool_call.get("id", "")

                # Deduplicate using call_id - this prevents showing the same tool call
                # multiple times even when interaction_counter changes
                if call_id:
                    tool_call_key = f"tool_call:{call_id}"
                    if self._is_duplicate(tool_call_key):
                        continue

                # Parse arguments
                try:
                    import json
                    args = json.loads(args_str) if args_str else {}
                except Exception:
                    # If parsing fails, try to extract meaningful data
                    args = {}
                    if args_str:
                        # Don't wrap in "raw" - just use empty dict
                        # The actual args will be shown when the tool executes
                        pass

                # Create a tool call panel for display
                panel = PanelFormatter.create_tool_call_panel(
                    tool_name=tool_name,
                    args=args,
                    call_id=call_id,
                    agent_name=context.agent_name or f"Agent {context.terminal_number}",
                    interaction=metadata.get("interaction", 1)
                )
                
                # Display the panel
                if hasattr(console, "write"):
                    console.write(panel)
                    console.write("")
                else:
                    self._print_to_console(console, panel)
                
                # Also update the action bar if available
                try:
                    from cai.tui.core.terminal_console import get_terminal_output
                    from cai.tui.core.terminal_widget_registry import get_terminal_widget
                    
                    # Try multiple ways to get the terminal widget
                    terminal_widget = get_terminal_output(context.terminal_id)
                    if not terminal_widget:
                        terminal_widget = get_terminal_widget(context.terminal_id)
                    
                    if terminal_widget and hasattr(terminal_widget, "action_bar"):
                        # Show tool execution in action bar
                        terminal_widget.action_bar.show_tool_execution(tool_name, args)
                        
                        # Debug logging
                        import sys
                        print(f"[TOOL CALL DEBUG] Updated action bar for tool: {tool_name} on terminal: {context.terminal_id}", file=sys.stderr)
                    else:
                        import sys
                        print(f"[TOOL CALL DEBUG] No action bar found for terminal: {context.terminal_id}, widget: {terminal_widget}", file=sys.stderr)
                except Exception as e:
                    # Log the error for debugging
                    import sys
                    print(f"[TOOL CALL ERROR] Failed to update action bar: {e}", file=sys.stderr)


    def _display_agent_message_with_tokens(
        self, console: Any, agent_name: str, content: str, metadata: dict, token_info: dict
    ) -> None:
        """Display agent message with token information"""
        # Use PanelFormatter.create_agent_panel for consistent formatting
        # This ensures both streaming and non-streaming use the same format
        # with proper individual costs and arrow formatting
        panel = PanelFormatter.create_agent_panel(
            agent_name=agent_name,
            message=content,
            metadata=metadata,
            streaming=False,
            token_info=token_info
        )

        # Always use _print_to_console for consistent handling
        self._print_to_console(console, panel)
