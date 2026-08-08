"""
Tool execution display for TUI
"""

import os
import threading
import time
import uuid
from typing import Any

_CAI_DEBUG_DIR = os.path.join(os.path.expanduser("~"), ".cai", "debug")

from rich.console import Console

from .base import BaseDisplay, DisplayContext, OutputType
from .panel_formatter import PanelFormatter
from .tool_streaming_handler import get_tool_streaming_handler
from .live_output_capture import get_live_output_capture


# Recursion guard for display operations
class DisplayRecursionGuard:
    def __init__(self, max_depth=3):
        self._lock = threading.Lock()
        self._call_stack = threading.local()
        self._max_depth = max_depth

    def can_proceed(self, operation_id: str) -> bool:
        """Check if we can proceed with display operation"""
        with self._lock:
            if not hasattr(self._call_stack, 'depth'):
                self._call_stack.depth = {}

            current_depth = self._call_stack.depth.get(operation_id, 0)
            if current_depth >= self._max_depth:
                return False

            self._call_stack.depth[operation_id] = current_depth + 1
            return True

    def release(self, operation_id: str) -> None:
        """Release the guard for an operation"""
        with self._lock:
            if hasattr(self._call_stack, 'depth') and operation_id in self._call_stack.depth:
                self._call_stack.depth[operation_id] = max(0, self._call_stack.depth[operation_id] - 1)


_recursion_guard = DisplayRecursionGuard()


class ToolDisplay(BaseDisplay):
    """Handles tool execution display for TUI"""

    def __init__(self):
        """Initialize tool display"""
        super().__init__()
        self._streaming_sessions: dict[str, dict[str, Any]] = {}
        self._command_display_times: dict[str, float] = {}
        self._streaming_handler = get_tool_streaming_handler()
        self._live_capture = get_live_output_capture()

    def _print_to_console(self, console: Any, content: Any) -> None:
        """Print to console or terminal output"""

        if hasattr(console, "print"):
            console.print(content)
        else:
            # Direct terminal output - convert content to text
            from rich.console import Console

            temp_console = Console()
            with temp_console.capture() as capture:
                temp_console.print(content)
            console.write(capture.get())

    def display(self, context: DisplayContext, data: dict[str, Any]) -> None:
        """Display a non-streaming tool output"""

        # IMPORTANT: Skip display if tool_name is empty
        # This prevents issues with empty tool displays
        if not data.get('tool_name'):
            return

        # Check recursion guard after validating tool_name
        operation_id = f"tool_display_{context.terminal_id}_{data.get('tool_name', '')}_{data.get('call_id', '')}"
        if not _recursion_guard.can_proceed(operation_id):
            # Recursion detected, bail out silently
            return

        try:
            # Extract data
            tool_name = data.get("tool_name", "")
            args = data.get("args", {})
            output = data.get("output", "")
            execution_info = data.get("execution_info")
            token_info = data.get("token_info", {})
            call_id = data.get("call_id", "")

            # Skip internal tools
            if tool_name.startswith("_internal_"):
                return

            # Generate deduplication key (using call_id for reliable deduplication)
            item_key = self._generate_tool_key(context, tool_name, args, call_id)

            # Check for duplicates
            if self._is_duplicate(item_key):
                # Check if enough time has passed for re-display
                last_display = self._command_display_times.get(item_key, 0)
                if time.time() - last_display < 0.5:
                    return

            # Update display time
            self._command_display_times[item_key] = time.time()

            # Get terminal output for this context
            terminal_output = self.get_terminal_output(context.terminal_id)

            import os
            if os.getenv("CAI_DEBUG_DISPLAY"):
                print("[DEBUG] tool_display.display:")
                print(f"  - terminal_id: {context.terminal_id}")
                print(f"  - terminal_output: {terminal_output}")
                print(
                    f"  - has write method: {hasattr(terminal_output, 'write') if terminal_output else False}"
                )

            if not terminal_output:
                # Try to get from terminal console directly
                from cai.tui.core.terminal_console import get_terminal_output

                terminal_output = get_terminal_output(context.terminal_id)

                # Keep the UniversalTerminal reference to access action_bar
                universal_terminal = terminal_output
                # Get the RichLog for text output
                if terminal_output and hasattr(terminal_output, 'output') and terminal_output.output:
                    terminal_output = terminal_output.output
                else:
                    universal_terminal = None

            if not terminal_output:
                # Fall back to console
                console = self.get_console()
                if not console:
                    console = Console()
                terminal_output = console

            # Register terminal with both handlers (only once)
            # Try to register the UniversalTerminal widget instead of just the RichLog
            from cai.tui.core.terminal_widget_registry import get_terminal_widget
            terminal_widget = get_terminal_widget(context.terminal_id)
            if terminal_widget:
                self._streaming_handler.register_terminal(context.terminal_id, terminal_widget)
                self._live_capture.register_terminal(context.terminal_id, terminal_widget)
            
            # Show tool execution in action bar
            from cai.tui.core.terminal_widget_registry import get_terminal_widget, get_terminal_widget_by_agent_id
            
            # Debug logging for terminal routing
            try:
                with open(f"{_CAI_DEBUG_DIR}/cai_action_bar_routing.log", "a") as f:
                    import datetime
                    f.write(f"\n[{datetime.datetime.now().isoformat()}] Looking for terminal widget:\n")
                    f.write(f"  context.terminal_id: {context.terminal_id}\n")
                    f.write(f"  context.agent_id: {getattr(context, 'agent_id', 'None')}\n")
                    f.write(f"  context.terminal_number: {getattr(context, 'terminal_number', 'None')}\n")
            except:
                pass
            
            # Try multiple ways to find the terminal widget
            terminal_widget = get_terminal_widget(context.terminal_id)
            
            # If not found by terminal_id, try by agent_id
            if not terminal_widget and hasattr(context, 'agent_id') and context.agent_id:
                terminal_widget = get_terminal_widget_by_agent_id(context.agent_id)
            
            # If still not found, try predictable ID
            if not terminal_widget and hasattr(context, 'terminal_number') and context.terminal_number:
                predictable_id = f"terminal-{context.terminal_number}"
                terminal_widget = get_terminal_widget(predictable_id)
            
            # Debug log result
            try:
                with open(f"{_CAI_DEBUG_DIR}/cai_action_bar_routing.log", "a") as f:
                    f.write(f"  Found widget: {terminal_widget is not None}\n")
                    if terminal_widget:
                        f.write(f"  Widget type: {type(terminal_widget).__name__}\n")
                        f.write(f"  Has show_tool_execution: {hasattr(terminal_widget, 'show_tool_execution')}\n")
            except:
                pass
            
            if terminal_widget and hasattr(terminal_widget, 'show_tool_execution'):
                try:
                    terminal_widget.show_tool_execution(tool_name, args)
                    # IMPORTANT: No quick flash to action bar here to avoid duplicate status lines.
                    # Streaming/Live capture paths will update the action bar as needed.
                except Exception as e:
                    # Log the error
                    try:
                        with open(f"{_CAI_DEBUG_DIR}/cai_action_bar_routing.log", "a") as f:
                            f.write(f"  ERROR showing tool execution: {str(e)}\n")
                    except:
                        pass
            
            # Always show completed tool output in main terminal
            panel = PanelFormatter.create_tool_panel(
                tool_name=tool_name,
                args=args,
                output=output,
                execution_info=execution_info,
                token_info=token_info,
                streaming=False,
            )

            # Write directly to terminal output if it's a RichLog widget
            if hasattr(terminal_output, "write"):
                # RichLog doesn't support expand parameter
                terminal_output.write(panel)
            else:
                self._print_to_console(terminal_output, panel)

            # Finalize the tool call indicator line on the action bar without touching tool output
            try:
                from cai.tui.core.terminal_widget_registry import get_terminal_widget, get_terminal_widget_by_agent_id
                tw = get_terminal_widget(context.terminal_id)
                if not tw and hasattr(context, 'agent_id') and context.agent_id:
                    tw = get_terminal_widget_by_agent_id(context.agent_id)
                status = (execution_info or {}).get('status', 'completed')
                if tw and hasattr(tw, 'action_bar') and hasattr(tw.action_bar, 'finish_tool_execution_indicator'):
                    tw.action_bar.finish_tool_execution_indicator(status)
            except Exception:
                pass

            # Add to context history
            try:
                context.add_output(
                    {"type": OutputType.TOOL_OUTPUT, "tool_name": tool_name, "args": args, "output": output}
                )
            except Exception:
                raise
        finally:
            # Always release the recursion guard
            _recursion_guard.release(operation_id)

    def start_streaming(
        self, context: DisplayContext, stream_id: str, data: dict[str, Any]
    ) -> None:
        """Start a streaming tool execution"""
        tool_name = data.get("tool_name", "")
        args = data.get("args", {})
        token_info = data.get("token_info", {})

        # Skip internal tools
        if tool_name.startswith("_internal_"):
            return

        # Generate stream ID if not provided
        if not stream_id:
            stream_id = f"tool_{tool_name}_{str(uuid.uuid4())[:8]}"

        # BUGFIX: Check if already streaming for this tool/command combination
        # to prevent duplicate indicators for the same command
        tool_key = f"{context.terminal_id}:{tool_name}:{str(args)}"

        # Check if we already have an active stream for this exact tool/args combination
        for existing_stream_id, session in self._streaming_sessions.items():
            if (session.get("context") and
                session["context"].terminal_id == context.terminal_id and
                session.get("tool_name") == tool_name and
                str(session.get("args", "")) == str(args)):
                # Already streaming this exact command, skip duplicate
                return

        # Check if stream_id already exists
        if stream_id in self._streaming_sessions:
            return

        # Get terminal output for this context
        terminal_output = self.get_terminal_output(context.terminal_id)
        if not terminal_output:
            # Fall back to console
            terminal_output = self.get_console() or Console()

        # Create streaming session
        session = {
            "tool_name": tool_name,
            "args": args,
            "buffer": "",
            "start_time": time.time(),
            "last_update": time.time(),
            "context": context,
            "token_info": token_info,
            "is_complete": False,
            "console": terminal_output,
        }

        self._streaming_sessions[stream_id] = session
        
        # Ensure the terminal is registered with the streaming handler BEFORE starting
        try:
            from cai.tui.core.terminal_widget_registry import get_terminal_widget
            terminal_widget = get_terminal_widget(context.terminal_id)
            if terminal_widget:
                self._streaming_handler.register_terminal(context.terminal_id, terminal_widget)
        except Exception:
            pass

        # Use the new streaming handler
        self._streaming_handler.start_streaming(context.terminal_id, stream_id, tool_name, args)
        
        # Show tool execution in action bar
        from cai.tui.core.terminal_widget_registry import get_terminal_widget, get_terminal_widget_by_agent_id
        
        # Debug logging for streaming terminal routing
        try:
            with open(f"{_CAI_DEBUG_DIR}/cai_action_bar_routing.log", "a") as f:
                import datetime
                f.write(f"\n[{datetime.datetime.now().isoformat()}] [STREAMING] Looking for terminal widget:\n")
                f.write(f"  context.terminal_id: {context.terminal_id}\n")
                f.write(f"  context.agent_id: {getattr(context, 'agent_id', 'None')}\n")
                f.write(f"  context.terminal_number: {getattr(context, 'terminal_number', 'None')}\n")
                f.write(f"  tool_name: {tool_name}\n")
        except:
            pass
        
        # Try multiple ways to find the terminal widget
        terminal_for_action_bar = get_terminal_widget(context.terminal_id)
        
        # If not found by terminal_id, try by agent_id
        if not terminal_for_action_bar and context.agent_id:
            terminal_for_action_bar = get_terminal_widget_by_agent_id(context.agent_id)
        
        # If still not found, try predictable ID
        if not terminal_for_action_bar and context.terminal_number:
            predictable_id = f"terminal-{context.terminal_number}"
            terminal_for_action_bar = get_terminal_widget(predictable_id)
        
        # Debug log result
        try:
            with open(f"{_CAI_DEBUG_DIR}/cai_action_bar_routing.log", "a") as f:
                f.write(f"  Found widget: {terminal_for_action_bar is not None}\n")
                if terminal_for_action_bar:
                    f.write(f"  Widget type: {type(terminal_for_action_bar).__name__}\n")
        except:
            pass
        
        if terminal_for_action_bar and hasattr(terminal_for_action_bar, 'show_tool_execution'):
            try:
                terminal_for_action_bar.show_tool_execution(tool_name, args)
            except Exception as e:
                # Log the error
                try:
                    with open(f"{_CAI_DEBUG_DIR}/cai_action_bar_routing.log", "a") as f:
                        f.write(f"  ERROR showing tool execution: {str(e)}\n")
                except:
                    pass

        # COMMENTED OUT: Don't show panels during streaming
        # The action bar already shows all streaming progress
        # Only show final panel when streaming completes
        
        # # Special handling for execute_code
        # if tool_name == "execute_code" and isinstance(args, dict) and "code" in args:
        #     self._show_code_panel(session)
        #     session["code_panel_shown"] = True
        # else:
        #     # Show initial panel for tool execution
        #     panel = PanelFormatter.create_tool_panel(
        #         tool_name=tool_name,
        #         args=args,
        #         output="",  # DO NOT show "Starting..." - leave empty
        #         execution_info={"status": "running"},
        #         token_info=token_info,
        #         streaming=False,  # NO streaming
        #     )
        #     
        #     if hasattr(terminal_output, "write"):
        #         # RichLog doesn't support expand parameter
        #         terminal_output.write(panel)
        #         terminal_output.write("")  # Add spacing
        #         session["initial_panel_shown"] = True

        with self._lock:
            self._active_streams[stream_id] = session

    def update_streaming(self, stream_id: str, data: dict[str, Any]) -> None:
        """Update a streaming session"""
        if stream_id not in self._streaming_sessions:
            return

        session = self._streaming_sessions[stream_id]
        output = data.get("output", "")

        # Update buffer
        session["buffer"] = output
        session["last_update"] = time.time()
        
        # Use the new streaming handler for updates
        self._streaming_handler.update_stream(stream_id, output)
        
        # Debug log
        with open(f"{_CAI_DEBUG_DIR}/cai_tool_streaming_debug.log", "a") as f:
            f.write(f"  Called streaming handler update_stream\n")

        # COMMENTED OUT: Don't write panels during streaming updates
        # The action bar already shows the streaming progress
        # Writing panels during streaming can cause rich.write errors
        # Only the final panel should be written in finish_streaming
        
        # # Mostrar el output completo cuando termine en la terminal principal
        # terminal_output = session.get("console")
        # if terminal_output and hasattr(terminal_output, "write") and output:
        #     # Solo actualizar el panel cuando hay output significativo
        #     # NO actualizar constantemente durante el streaming
        #     # Create an updated panel with all current output
        #     current_time = time.time()
        #     panel = PanelFormatter.create_tool_panel(
        #         tool_name=session["tool_name"],
        #         args=session["args"],
        #         output=output,  # Show all available output
        #         execution_info={"status": "completed", "tool_time": current_time - session["start_time"]},
        #         token_info=session.get("token_info"),
        #         streaming=False,  # NO streaming
        #     )
        #     
        #     # Write the updated panel
        #     try:
        #         # Debug logging
        #         with open(f"{_CAI_DEBUG_DIR}/cai_tool_streaming_debug.log", "a") as f:
        #             f.write(f"  About to write panel during streaming\n")
        #             f.write(f"  terminal_output type: {type(terminal_output)}\n")
        #             f.write(f"  panel type: {type(panel)}\n")
        #         
        #         # Try with expand first, fall back without it
        #         try:
        #             terminal_output.write(panel)
        #         except TypeError as e:
        #             if "expand" in str(e):
        #                 terminal_output.write(panel)
        #             else:
        #                 raise
        #         terminal_output.write("")  # Add spacing
        #     except Exception as e:
        #         # Log the error for debugging
        #         with open(f"{_CAI_DEBUG_DIR}/cai_rich_write_error.log", "a") as f:
        #             import traceback
        #             f.write(f"\n[{datetime.datetime.now().isoformat()}] Error writing panel in update_streaming\n")
        #             f.write(f"  Error: {e}\n")
        #             f.write(f"  Error type: {type(e).__name__}\n")
        #             f.write(f"  Traceback: {traceback.format_exc()}\n")

    def finish_streaming(self, stream_id: str, data: dict[str, Any]) -> None:
        """Finish a streaming session"""
        if stream_id not in self._streaming_sessions:
            return

        session = self._streaming_sessions[stream_id]

        # Check recursion guard
        operation_id = f"tool_finish_{stream_id}"
        if not _recursion_guard.can_proceed(operation_id):
            # Recursion detected, clean up session and bail out
            self._streaming_sessions.pop(stream_id, None)
            return

        try:
            output = data.get("output", "")
            execution_info = data.get("execution_info", {})
            execution_info["is_final"] = True
            execution_info["status"] = execution_info.get("status", "completed")

            # Calculate execution time
            if "tool_time" not in execution_info:
                execution_info["tool_time"] = time.time() - session["start_time"]

            # Update final output
            session["buffer"] = output
            session["is_complete"] = True

            # Ensure a final buffer flush to action bar before closing
            try:
                self._streaming_handler.update_stream(stream_id, output or session.get("buffer", ""))
            except Exception:
                pass

            # Use streaming handler to finish and get terminal output
            status = execution_info.get("status", "completed")
            terminal_output = self._streaming_handler.finish_stream(stream_id, output, status)
            
            # Show final output in main terminal
            if terminal_output and hasattr(terminal_output, "write"):
                # Create final panel for main terminal display
                panel = PanelFormatter.create_tool_panel(
                    tool_name=session["tool_name"],
                    args=session["args"],
                    output=output,
                    execution_info=execution_info,
                    token_info=session.get("token_info"),
                    streaming=False,  # Non-streaming format
                )
                terminal_output.write(panel)
                terminal_output.write("")  # Add spacing

            # Clean up
            self._streaming_sessions.pop(stream_id, None)
            with self._lock:
                self._active_streams.pop(stream_id, None)

            # Add to context history
            context = session.get("context")
            if context:
                context.add_output(
                    {
                        "type": OutputType.TOOL_OUTPUT,
                        "tool_name": session["tool_name"],
                        "args": session["args"],
                        "output": output,
                        "execution_info": execution_info,
                    }
                )
        finally:
            # Always release the recursion guard
            _recursion_guard.release(operation_id)

    def _show_streaming_panel(self, session: dict, output: str) -> None:
        """Show a streaming panel"""
        terminal_output = session["console"]

        # Create panel
        panel = PanelFormatter.create_tool_panel(
            tool_name=session["tool_name"],
            args=session["args"],
            output=output,
            execution_info={"status": "running"},
            token_info=session.get("token_info"),
            streaming=True,
        )

        # Write directly to terminal output if it's a RichLog widget
        if hasattr(terminal_output, "write"):
            # RichLog doesn't support expand parameter
            terminal_output.write(panel)
        else:
            self._print_to_console(terminal_output, panel)

    def _update_live_panel(self, session: dict) -> None:
        """Update a live streaming panel"""
        if not session.get("live"):
            return

        panel = PanelFormatter.create_tool_panel(
            tool_name=session["tool_name"],
            args=session["args"],
            output=session["buffer"],
            execution_info={"status": "running"},
            token_info=session.get("token_info"),
            streaming=True,
        )

        try:
            session["live"].update(panel)
        except Exception:
            pass

    def _show_code_panel(self, session: dict) -> None:
        """Show code panel for execute_code tool"""
        from rich.box import ROUNDED
        from rich.panel import Panel
        from rich.syntax import Syntax

        terminal_output = session["console"]
        args = session["args"]

        code = args.get("code", "")
        language = args.get("language", "python")
        args.get("filename", "code")

        # Choose Rich/Pygments theme based on current Textual theme
        try:
            from textual.app import App
            app = App.get_app()
            current_theme = getattr(app, "theme", "textual-dark") if app else "textual-dark"
        except Exception:
            current_theme = "textual-dark"

        pygments_theme_map = {
            "textual-dark": "monokai",
            "tokyo-night": "monokai",
            "nord": "nord-darker",
            "solarized-light": "solarized-light",
            "textual-light": "default",
            "nature": "monokai",
        }
        pygments_theme = pygments_theme_map.get(current_theme, "monokai")

        # Create code syntax
        code_syntax = Syntax(
            code,
            language,
            theme=pygments_theme,
            line_numbers=True,
            word_wrap=True,
        )

        # Get agent name
        agent_name = session.get("token_info", {}).get("agent_name", "Agent")

        # Create panel
        panel = Panel(
            code_syntax,
            title=f"[bold cyan]{agent_name}[/bold cyan] - Code ({language})",
            border_style="cyan",
            title_align="left",
            box=ROUNDED,
            padding=(0, 1),
        )

        # Write directly to terminal output if it's a RichLog widget
        if hasattr(terminal_output, "write"):
            # RichLog doesn't support expand parameter
            terminal_output.write(panel)
        else:
            self._print_to_console(terminal_output, panel)

    def _show_output_panel(self, session: dict, output: str, execution_info: dict) -> None:
        """Show output panel for execute_code tool"""
        from rich.box import ROUNDED
        from rich.panel import Panel
        from rich.syntax import Syntax

        terminal_output = session["console"]

        # Create output syntax (use same pygments theme as above)
        try:
            from textual.app import App
            app = App.get_app()
            current_theme = getattr(app, "theme", "textual-dark") if app else "textual-dark"
        except Exception:
            current_theme = "textual-dark"

        pygments_theme_map = {
            "textual-dark": "monokai",
            "tokyo-night": "monokai",
            "nord": "nord-darker",
            "solarized-light": "solarized-light",
            "textual-light": "default",
            "nature": "monokai",
        }
        pygments_theme = pygments_theme_map.get(current_theme, "monokai")

        output_syntax = Syntax(
            output or "No output",
            "text",
            theme=pygments_theme,
            word_wrap=True,
        )

        # Get agent name and status
        agent_name = session.get("token_info", {}).get("agent_name", "Agent")
        status = execution_info.get("status", "completed")

        # Determine style
        if status == "completed":
            border_style = "green"
            title = f"[bold green]{agent_name}[/bold green] - Output"
        else:
            border_style = "red"
            title = f"[bold red]{agent_name}[/bold red] - Output (Error)"

        # Create panel
        panel = Panel(
            output_syntax,
            title=title,
            border_style=border_style,
            title_align="left",
            box=ROUNDED,
            padding=(0, 1),
        )

        # Write directly to terminal output if it's a RichLog widget
        if hasattr(terminal_output, "write"):
            # RichLog doesn't support expand parameter
            terminal_output.write(panel)
        else:
            self._print_to_console(terminal_output, panel)

    def _generate_tool_key(self, context: DisplayContext, tool_name: str, args: Any, call_id: str = "") -> str:
        """Generate unique key for tool deduplication

        NOTE: We intentionally do NOT include interaction_counter in the key.
        Including it would cause tool calls to be re-displayed when the turn
        changes, leading to duplicates across multi-turn conversations.

        Instead, we use:
        - call_id (if available) - unique per tool call
        - agent context
        - tool name and args hash
        """
        # If we have a call_id, use it as the primary deduplication key
        # This is the most reliable way to deduplicate tool calls
        if call_id:
            return f"call_{call_id}"

        # Extract effective args for hashing
        effective_args = ""
        if isinstance(args, dict):
            if "args" in args:
                effective_args = args.get("args", "")
            elif "command" in args:
                effective_args = args.get("command", "")
            elif "query" in args:
                effective_args = args.get("query", "")
            else:
                effective_args = str(sorted(args.items()))
        else:
            effective_args = str(args)

        # Build key WITHOUT interaction_counter to prevent re-display on turn change
        parts = [
            f"agent_{context.agent_id or context.agent_name or 'default'}",
            tool_name,
            effective_args[:200],  # Limit args length for key stability
        ]

        return ":".join(parts)
