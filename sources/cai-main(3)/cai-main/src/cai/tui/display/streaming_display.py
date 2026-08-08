"""
Streaming content display for TUI
"""

import time
import uuid
from datetime import datetime
from typing import Any

from rich.box import ROUNDED
from rich.console import Console, Group
from rich.live import Live
from rich.panel import Panel
from rich.text import Text

from .base import BaseDisplay, DisplayContext, OutputType
from .error_handler import (
    CONCURRENCY_MANAGER,
    ERROR_HANDLER,
    MEMORY_GUARD,
    RATE_LIMITER,
    ContentValidator,
    handle_streaming_errors,
)
from .panel_formatter import PanelFormatter

# Import COST_TRACKER for session cost
try:
    from cai.util import COST_TRACKER
except ImportError:
    COST_TRACKER = None


def _get_terminal_richlog(terminal_id: str):
    """Get the RichLog widget from a terminal, handling UniversalTerminal wrapper"""
    from cai.tui.core.terminal_console import get_terminal_output
    terminal_output = get_terminal_output(terminal_id)
    # If we got a UniversalTerminal, get its output (RichLog)
    if terminal_output and hasattr(terminal_output, 'output') and terminal_output.output:
        return terminal_output.output
    return terminal_output


def _get_terminal_widget(terminal_id: str):
    """Get the full UniversalTerminal widget (needed for action_bar access)"""
    from cai.tui.core.terminal_console import get_terminal_output
    return get_terminal_output(terminal_id)


def _safe_write_panel(terminal_output, panel):
    """Safely write a panel to terminal output, checking for expand parameter support"""
    if hasattr(terminal_output, "write"):
        # Try with expand first, fall back without it
        try:
            terminal_output.write(panel, expand=True)
        except TypeError as e:
            if "expand" in str(e):
                terminal_output.write(panel)
            else:
                raise
    else:
        # Fallback - try to print
        if hasattr(terminal_output, "print"):
            terminal_output.print(panel)


class StreamingDisplay(BaseDisplay):
    """Handles streaming content display for TUI"""

    def __init__(self):
        """Initialize streaming display"""
        super().__init__()
        self._streaming_contexts: dict[str, dict[str, Any]] = {}
        self._thinking_contexts: dict[str, dict[str, Any]] = {}
        # Track panel IDs to prevent duplicates
        self._active_panels: dict[str, str] = {}  # panel_id -> last_content_hash

    def display(self, context: DisplayContext, data: dict[str, Any]) -> None:
        """Display non-streaming content (not typically used for streaming)"""
        # This is mainly for displaying final streaming results
        content = data.get("content", "")
        content_type = data.get("content_type", "text")

        if not content:
            return

        console = self.get_console() or Console()

        # Create appropriate display based on content type
        if content_type == "thinking":
            self._display_thinking_content(console, context, content, data)
        else:
            self._display_stream_content(console, context, content, data)

    @handle_streaming_errors
    def start_streaming(
        self, context: DisplayContext, stream_id: str, data: dict[str, Any]
    ) -> None:
        """Start a streaming session with error handling"""
        if not stream_id:
            stream_id = f"stream_{str(uuid.uuid4())[:8]}"

        # Validate data
        data = ContentValidator.validate_stream_data(data)
        content_type = data.get("content_type", "text")

        # Check concurrency limits
        if CONCURRENCY_MANAGER.is_at_capacity():
            raise RuntimeError("Maximum concurrent streams reached")

        with CONCURRENCY_MANAGER.acquire_stream(stream_id):
            if content_type == "thinking":
                self._start_thinking_stream(context, stream_id, data)
            else:
                self._start_content_stream(context, stream_id, data)

    @handle_streaming_errors
    def update_streaming(self, stream_id: str, data: dict[str, Any]) -> None:
        """Update a streaming session with error handling"""
        # Check if interrupted
        if ERROR_HANDLER.is_interrupted:
            return

        # Validate data
        data = ContentValidator.validate_stream_data(data)

        # Check thinking contexts first
        if stream_id in self._thinking_contexts:
            self._update_thinking_stream(stream_id, data)
        elif stream_id in self._streaming_contexts:
            # ALWAYS update the content to accumulate it
            # Rate limiting is handled inside _update_content_stream for visual updates only
            self._update_content_stream(stream_id, data)

    @handle_streaming_errors
    def finish_streaming(self, stream_id: str, data: dict[str, Any]) -> None:
        """Finish a streaming session with cleanup"""
        try:
            # Validate data
            data = ContentValidator.validate_stream_data(data)

            # Check thinking contexts first
            if stream_id in self._thinking_contexts:
                self._finish_thinking_stream(stream_id, data)
            elif stream_id in self._streaming_contexts:
                self._finish_content_stream(stream_id, data)
        finally:
            # Always clean up resources
            ERROR_HANDLER.clear_context(stream_id)
            RATE_LIMITER.clear(stream_id)
            MEMORY_GUARD.clear_buffer(stream_id)

    def _start_content_stream(
        self, context: DisplayContext, stream_id: str, data: dict[str, Any]
    ) -> None:
        """Start a regular content stream"""
        timestamp = datetime.now().strftime("%H:%M:%S")
        agent_name = context.agent_name or f"Agent {context.terminal_number}"
        model = data.get("model", context.metadata.get("model", ""))

        # Get terminal output for this context
        terminal_output = _get_terminal_richlog(context.terminal_id)
        terminal_widget = _get_terminal_widget(context.terminal_id)

        if terminal_output and hasattr(terminal_output, "write"):
            # For TUI mode, we'll show a streaming indicator and update periodically
            # Store context for streaming


            self._streaming_contexts[stream_id] = {
                "context": context,
                "terminal_output": terminal_output,
                "terminal_widget": terminal_widget,
                "timestamp": timestamp,
                "model": model,
                "agent_name": agent_name,
                "is_started": True,
                "accumulated_content": "",
                "tui_mode": True,
                "interaction_counter": context.interaction_counter,
                "token_stats": {},
                "initial_panel_shown": False,
                "panel_id": f"stream_{context.terminal_id}_{context.interaction_counter}",
                "panel_written": False,
                "last_written_content": "",
                "last_written_header": "",
                "last_update_time": 0,
                "last_update_length": 0,
                "last_content_length": 0,
                "stream_start_time": time.time(),
                "is_agent_message": True,  # Programmatic flag for agent messages
            }

            # Don't show initial panel - wait for actual content
            # This prevents the duplicate panels issue

            with self._lock:
                self._active_streams[stream_id] = self._streaming_contexts[stream_id]
            return

        # Create Live display for non-TUI mode
        console = self.get_console() or Console()

        # Create initial panel components
        header = Text()
        header.append(f"[{context.interaction_counter}]", style="bold cyan")
        header.append(f" {agent_name}", style="bold blue")
        header.append(" >> ", style="yellow")
        header.append("⚡ Streaming...", style="yellow")
        if model:
            header.append(f" ({model})", style="bold magenta")

        content = Text("")
        footer = Text(f"\n[{timestamp}]", style="dim")

        panel = Panel(
            Group(header, content, footer),
            border_style="yellow",
            box=ROUNDED,
            padding=(0, 1),
            title="Stream",
            title_align="left",
            expand=True,
        )

        try:
            # Use higher refresh rate for smooth updates
            live = Live(
                panel,
                refresh_per_second=10,  # 10 updates per second
                console=console,
                auto_refresh=True,
                vertical_overflow="visible",
                transient=True,  # Don't clear the screen
            )

            # Print the initial panel statically to avoid screen clearing
            console.print(panel)

            # Now start the live display with the panel already shown
            live.start(refresh=False)  # Don't refresh immediately

            # Store context
            self._streaming_contexts[stream_id] = {
                "context": context,
                "live": live,
                "panel": panel,
                "header": header,
                "content": content,
                "footer": footer,
                "timestamp": timestamp,
                "model": model,
                "agent_name": agent_name,
                "is_started": True,
                "accumulated_content": "",
            }
        except Exception:
            # Fallback to static display
            # Use terminal output directly to avoid console routing issues
            terminal_output = _get_terminal_richlog(context.terminal_id)
            if terminal_output:
                _safe_write_panel(terminal_output, panel)
            else:
                console.print(panel)
            self._streaming_contexts[stream_id] = {
                "context": context,
                "console": console,
                "static": True,
                "accumulated_content": "",
            }

        with self._lock:
            self._active_streams[stream_id] = self._streaming_contexts[stream_id]

    def _update_content_stream(self, stream_id: str, data: dict[str, Any]) -> None:
        """Update a content stream with memory protection"""
        stream_ctx = self._streaming_contexts.get(stream_id)
        if not stream_ctx:
            return

        # Update content with memory check
        content_delta = data.get("content", "")

        # Check memory limits
        if not MEMORY_GUARD.check_buffer(stream_id, content_delta):
            # Buffer too large, truncate or skip
            self._handle_buffer_overflow(stream_id, stream_ctx)
            return

        # ALWAYS accumulate content, regardless of rate limiting
        # This ensures no content is lost
        stream_ctx["accumulated_content"] += content_delta

        # Update token stats if provided
        token_stats = data.get("token_stats")
        if token_stats:
            # Ensure we have session total cost and context usage percentage
            if (
                "session_total_cost" not in token_stats
                and COST_TRACKER
                and hasattr(COST_TRACKER, "session_total_cost")
            ):
                token_stats["session_total_cost"] = COST_TRACKER.session_total_cost

            # Calculate context usage percentage if not provided (use current interaction input)
            if "context_usage_pct" not in token_stats and ("interaction_input_tokens" in token_stats or "input_tokens" in token_stats):
                try:
                    from cai.util import get_model_input_tokens

                    model_name = stream_ctx.get("model", "")
                    if model_name:
                        max_tokens = get_model_input_tokens(model_name)
                        if max_tokens > 0:
                            base = token_stats.get("interaction_input_tokens", token_stats.get("input_tokens", 0))
                            token_stats["context_usage_pct"] = (base / max_tokens) * 100
                except Exception:
                    pass

            stream_ctx["token_stats"] = token_stats

        if stream_ctx.get("static"):
            # Static display mode - don't update
            return

        if stream_ctx.get("tui_mode"):
            # For TUI mode, schedule periodic updates
            terminal_output = stream_ctx.get("terminal_output")

            if terminal_output and hasattr(terminal_output, "write"):
                # Use rate limiter for visual updates only
                if RATE_LIMITER.should_update(stream_id):
                    # Schedule update in main thread
                    try:
                        from textual.app import App
                        app = App.get_running_app()

                        # Check if we're already in main thread to avoid recursion
                        import threading
                        if threading.current_thread() == threading.main_thread():
                            # Already in main thread, update directly
                            self._perform_tui_update(stream_id)
                        else:
                            # Use call_from_thread to update UI safely
                            app.call_from_thread(self._perform_tui_update, stream_id)
                    except Exception:
                        # If app not available, try to update directly if in main thread
                        import threading
                        if threading.current_thread() == threading.main_thread():
                            self._perform_tui_update(stream_id)
        else:
            # For non-TUI mode, update with panels
            # Update content
            stream_ctx["content"].plain = stream_ctx["accumulated_content"]

            # Update footer with token stats
            if token_stats:
                self._update_stream_footer(stream_ctx, token_stats)

            # Update panel
            updated_panel = Panel(
                Group(stream_ctx["header"], stream_ctx["content"], stream_ctx["footer"]),
                border_style="blue",
                box=ROUNDED,
                padding=(0, 1),
                title="Stream",
                title_align="left",
                expand=True,
            )

            # For non-TUI mode, use Live
            try:
                stream_ctx["live"].update(updated_panel)
                stream_ctx["panel"] = updated_panel
                stream_ctx["live"].refresh()
            except Exception:
                pass

    def _perform_tui_update(self, stream_id: str) -> None:
        """Perform TUI update in main thread with error handling"""
        try:
            stream_ctx = self._streaming_contexts.get(stream_id)
            if not stream_ctx or not stream_ctx.get("tui_mode"):
                return

            terminal_output = stream_ctx.get("terminal_output")
            terminal_widget = stream_ctx.get("terminal_widget")
            if not terminal_output or not hasattr(terminal_output, "write"):
                return

            # Get accumulated content
            content = stream_ctx.get("accumulated_content", "")
            if not content:
                return
            
            # Sanitize content
            content = ContentValidator.sanitize_content(content)

            # For TUI mode, show progressive content in the terminal
            if not stream_ctx.get("initial_panel_shown"):
                stream_ctx["initial_panel_shown"] = True
                stream_ctx["header_prefix"] = f"[bold cyan][{stream_ctx['interaction_counter']}][/bold cyan] [bold green]{stream_ctx['agent_name']}[/bold green] >> "
                if stream_ctx.get('model'):
                    stream_ctx["header_prefix"] += f"[dim]({stream_ctx['model']})[/dim] "

                # Create a streaming line ID for this stream
                stream_ctx["stream_line_id"] = f"stream_{stream_id}"
                stream_ctx["last_displayed_length"] = 0
                stream_ctx["update_counter"] = 0
                stream_ctx["last_written_content"] = ""

                # Start streaming in the terminal (which updates action bar)
                if terminal_widget and hasattr(terminal_widget, 'start_streaming_line'):
                    # Use UniversalTerminal's streaming methods which handle action bar
                    terminal_widget.start_streaming_line(
                        stream_ctx["stream_line_id"],
                        header=stream_ctx["header_prefix"]
                    )
                
                # Write the header for streaming without indicator
                terminal_output.write(stream_ctx["header_prefix"], end="")
                stream_ctx["content_start_line"] = terminal_output.line_count

            else:
                
                # Progressive update - write only the new content
                # Update action bar if available
                if terminal_widget and hasattr(terminal_widget, 'update_streaming_line'):
                    terminal_widget.update_streaming_line(
                        stream_ctx["stream_line_id"],
                        content  # Pass full content with newlines preserved
                    )
                
                # Calculate what's new since last update
                last_len = len(stream_ctx.get("last_written_content", ""))
                if len(content) > last_len:
                    # Get only the new tokens
                    new_content = content[last_len:]
                    
                    # Write only the new content incrementally with Markdown rendering
                    if new_content:
                        # Use Markdown rendering for streaming content
                        from rich.markdown import Markdown
                        
                        # DON'T use Markdown during streaming - it causes token loss
                        # Just write plain text incrementally
                        
                        # Don't clear and rewrite - just append new content directly
                        # This prevents losing tokens
                        
                        # Write only the new content without clearing
                        if new_content:
                            # Don't add cursor here - it causes token loss
                            # The cursor should only be in the action bar
                            terminal_output.write(new_content, end="")
                        
                        stream_ctx["last_written_content"] = content
        except Exception as e:
            ERROR_HANDLER.record_error(stream_id, e, {
                "method": "_perform_tui_update",
                "content_length": len(content) if 'content' in locals() else 0
            })


    def _finish_content_stream(self, stream_id: str, data: dict[str, Any]) -> None:
        """Finish a content stream"""
        stream_ctx = self._streaming_contexts.pop(stream_id, None)
        if not stream_ctx:
            return

        # Use final_content if provided, otherwise use accumulated content
        if "final_content" in data and data["final_content"]:
            stream_ctx["accumulated_content"] = data["final_content"]
        
        final_stats = data.get("final_stats", stream_ctx.get("token_stats", {}))
        
        # Ensure final_stats is not None
        if final_stats is None:
            final_stats = {}

        # Ensure we have session total cost and context usage percentage in final stats
        if (
            final_stats is not None
            and "session_total_cost" not in final_stats
            and COST_TRACKER
            and hasattr(COST_TRACKER, "session_total_cost")
        ):
            final_stats["session_total_cost"] = COST_TRACKER.session_total_cost

        # Calculate context usage percentage if not provided (use current interaction input)
        if "context_usage_pct" not in final_stats and ("interaction_input_tokens" in final_stats or "input_tokens" in final_stats):
            try:
                from cai.util import get_model_input_tokens

                model_name = stream_ctx.get("model", "")
                if model_name:
                    max_tokens = get_model_input_tokens(model_name)
                    if max_tokens > 0:
                        base = final_stats.get("interaction_input_tokens", final_stats.get("input_tokens", 0))
                        final_stats["context_usage_pct"] = (base / max_tokens) * 100
            except Exception:
                pass

        if stream_ctx.get("tui_mode"):
            # For TUI mode, schedule final update in main thread
            # Enrich final stats with token information if needed
            if final_stats and COST_TRACKER:
                # Ensure session_total_cost is included
                if final_stats is not None and "session_total_cost" not in final_stats and hasattr(COST_TRACKER, "session_total_cost"):
                    final_stats["session_total_cost"] = COST_TRACKER.session_total_cost

                # Calculate context usage percentage if not provided
                if final_stats is not None and "context_usage_pct" not in final_stats and ("interaction_input_tokens" in final_stats or "input_tokens" in final_stats):
                    try:
                        from cai.util import get_model_input_tokens
                        model_name = stream_ctx.get("model", "")
                        if model_name:
                            max_tokens = get_model_input_tokens(model_name)
                            if max_tokens > 0:
                                base = final_stats.get("interaction_input_tokens", final_stats.get("input_tokens", 0))
                                final_stats["context_usage_pct"] = (base / max_tokens) * 100
                    except Exception:
                        pass

            # Store final stats in context for the update
            stream_ctx["final_stats"] = final_stats
            stream_ctx["is_final"] = True

            try:
                from textual.app import App
                app = App.get_running_app()

                # Use call_from_thread for final update
                app.call_from_thread(self._perform_final_tui_update, stream_id, stream_ctx)
            except Exception:
                # If app not available, try to update directly if in main thread
                import threading
                if threading.current_thread() == threading.main_thread():
                    self._perform_final_tui_update(stream_id, stream_ctx)
                else:
                    # Can't update UI from background thread without app
                    # Just write the final panel
                    self._write_final_panel(stream_ctx, final_stats)
        else:
            # For non-TUI mode, update with final panel
            final_stats_to_use = final_stats or {}
            if final_stats_to_use:
                self._update_stream_footer(stream_ctx, final_stats_to_use, final=True)

            # Change border to green
            final_panel = Panel(
                Group(stream_ctx["header"], stream_ctx["content"], stream_ctx["footer"]),
                border_style="green",
                box=ROUNDED,
                padding=(0, 1),
                title="Stream Complete",
                title_align="left",
                expand=True,
            )

            # Stop live display if exists
            if stream_ctx.get("live") and not stream_ctx.get("static"):
                try:
                    stream_ctx["live"].update(final_panel)
                    time.sleep(0.1)  # Brief pause to show final state
                    stream_ctx["live"].stop()
                except Exception:
                    pass

        # Add to context history
        context = stream_ctx.get("context")
        if context:
            context.add_output(
                {
                    "type": OutputType.STREAMING,
                    "content": stream_ctx["accumulated_content"],
                    "final_stats": data.get("final_stats"),
                }
            )

        with self._lock:
            self._active_streams.pop(stream_id, None)

    def _start_thinking_stream(
        self, context: DisplayContext, stream_id: str, data: dict[str, Any]
    ) -> None:
        """Start a thinking/reasoning stream"""
        thinking_id = (
            f"thinking_{context.agent_name}_{context.interaction_counter}_{str(uuid.uuid4())[:8]}"
        )

        # Check if already exists
        if thinking_id in self._thinking_contexts:
            return

        model = data.get("model", context.metadata.get("model", ""))
        agent_name = context.agent_name or f"Agent {context.terminal_number}"

        # Get terminal output for this context
        terminal_output = _get_terminal_richlog(context.terminal_id)
        terminal_widget = _get_terminal_widget(context.terminal_id)

        if terminal_output and hasattr(terminal_output, "write"):
            # For TUI mode, store context but don't show panel until content arrives
            self._thinking_contexts[thinking_id] = {
                "thinking_id": thinking_id,
                "stream_id": stream_id,
                "context": context,
                "terminal_output": terminal_output,
                "terminal_widget": terminal_widget,
                "model": model,
                "agent_name": agent_name,
                "accumulated_thinking": "",
                "is_started": True,
                "tui_mode": True,
                "panel_shown": False,
                "interaction_counter": context.interaction_counter,
                "timestamp": datetime.now().strftime("%H:%M:%S"),
            }
        else:
            # Non-TUI mode
            console = self.get_console() or Console()

            # Create initial panel
            panel = PanelFormatter.create_thinking_panel(
                agent_name=agent_name, thinking_content="", model_name=model, finished=False
            )

            try:
                # Use higher refresh rate
                live = Live(panel, refresh_per_second=8, console=console, auto_refresh=True,
                           transient=True)  # Don't clear the screen

                # Print the initial panel statically to avoid screen clearing
                console.print(panel)

                # Now start the live display with the panel already shown
                live.start(refresh=False)  # Don't refresh immediately

                self._thinking_contexts[thinking_id] = {
                    "thinking_id": thinking_id,
                    "stream_id": stream_id,
                    "context": context,
                    "live": live,
                    "panel": panel,
                    "model": model,
                    "agent_name": agent_name,
                    "accumulated_thinking": "",
                    "is_started": True,
                }
            except:
                # Fallback to static display
                # Use terminal output directly to avoid console routing issues
                terminal_output = _get_terminal_richlog(context.terminal_id)
                if terminal_output:
                    _safe_write_panel(terminal_output, panel)
                else:
                    console.print(panel)
                self._thinking_contexts[thinking_id] = {
                    "thinking_id": thinking_id,
                    "stream_id": stream_id,
                    "context": context,
                    "console": console,
                    "static": True,
                    "accumulated_thinking": "",
                }

        # Map stream_id to thinking_id
        self._thinking_contexts[stream_id] = self._thinking_contexts[thinking_id]

    def _update_thinking_stream(self, stream_id: str, data: dict[str, Any]) -> None:
        """Update a thinking stream"""
        thinking_ctx = self._thinking_contexts.get(stream_id)
        if not thinking_ctx:
            return

        # Update content
        thinking_delta = data.get("content", "")
        thinking_ctx["accumulated_thinking"] += thinking_delta

        if thinking_ctx.get("static"):
            return

        if thinking_ctx.get("tui_mode"):
            # For TUI mode, only show panel when we have actual, meaningful content
            terminal_output = thinking_ctx.get("terminal_output")
            content = thinking_ctx["accumulated_thinking"].strip()

            # Only show panel if we have substantial content (not just whitespace or very short)
            if terminal_output and hasattr(terminal_output, "write") and len(content) > 10:
                # Show panel if not shown yet
                if not thinking_ctx.get("panel_shown"):
                    # Show initial thinking panel
                    initial_panel = PanelFormatter.create_thinking_panel(
                        agent_name=thinking_ctx["agent_name"],
                        thinking_content=thinking_ctx["accumulated_thinking"],
                        model_name=thinking_ctx["model"],
                        finished=False,
                    )
                    terminal_output.write(initial_panel)
                    thinking_ctx["panel_shown"] = True
                    thinking_ctx["last_content_length"] = len(thinking_ctx["accumulated_thinking"])
                else:
                    # For updates, don't clear - thinking is usually short enough to just show final
                    # We'll update in the finish method
                    pass
        else:
            # Update panel for non-TUI mode
            updated_panel = PanelFormatter.create_thinking_panel(
                agent_name=thinking_ctx["agent_name"],
                thinking_content=thinking_ctx["accumulated_thinking"],
                model_name=thinking_ctx["model"],
                finished=False,
            )

            try:
                thinking_ctx["live"].update(updated_panel)
                thinking_ctx["panel"] = updated_panel
                thinking_ctx["live"].refresh()
            except Exception:
                pass

    def _finish_thinking_stream(self, stream_id: str, data: dict[str, Any]) -> None:
        """Finish a thinking stream"""
        thinking_ctx = self._thinking_contexts.pop(stream_id, None)
        if not thinking_ctx:
            return

        # Also remove by thinking_id
        thinking_id = thinking_ctx.get("thinking_id")
        if thinking_id and thinking_id != stream_id:
            self._thinking_contexts.pop(thinking_id, None)

        # Only show final panel if we actually showed content AND have meaningful content
        content = thinking_ctx.get("accumulated_thinking", "").strip()
        if thinking_ctx.get("panel_shown") and thinking_ctx.get("tui_mode") and len(content) > 10:
            terminal_output = thinking_ctx.get("terminal_output")
            if terminal_output:
                # Show final thinking panel without clearing
                final_panel = PanelFormatter.create_thinking_panel(
                    agent_name=thinking_ctx["agent_name"],
                    thinking_content=thinking_ctx["accumulated_thinking"],
                    model_name=thinking_ctx["model"],
                    finished=True,
                )
                _safe_write_panel(terminal_output, final_panel)
                terminal_output.write("")  # Add spacing

        # Show final panel for non-TUI mode
        if thinking_ctx.get("live") and not thinking_ctx.get("static"):
            try:
                final_panel = PanelFormatter.create_thinking_panel(
                    agent_name=thinking_ctx["agent_name"],
                    thinking_content=thinking_ctx["accumulated_thinking"],
                    model_name=thinking_ctx["model"],
                    finished=True,
                )

                thinking_ctx["live"].update(final_panel)
                time.sleep(0.3)  # Show completion briefly
                thinking_ctx["live"].stop()
            except Exception:
                pass

        # Add to context history
        context = thinking_ctx.get("context")
        if context:
            context.add_output(
                {
                    "type": OutputType.THINKING,
                    "content": thinking_ctx["accumulated_thinking"],
                    "model": thinking_ctx["model"],
                }
            )

    def _update_stream_footer(
        self, stream_ctx: dict, token_stats: dict, final: bool = False
    ) -> None:
        """Update stream footer with token statistics"""
        footer = Text()

        # Add timestamp and model
        footer.append(f"\n[{stream_ctx.get('timestamp', '')}]", style="dim")
        if stream_ctx.get("model"):
            footer.append(f" ({stream_ctx['model']})", style="bold magenta")
        footer.append("]", style="dim")

        # Add token stats
        input_tokens = token_stats.get("input_tokens", 0)
        output_tokens = token_stats.get("output_tokens", 0)
        reasoning_tokens = token_stats.get("reasoning_tokens", 0)
        total_input_tokens = token_stats.get("total_input_tokens", 0)
        total_output_tokens = token_stats.get("total_output_tokens", 0)
        total_reasoning_tokens = token_stats.get("total_reasoning_tokens", 0)
        interaction_cost = token_stats.get("interaction_cost", 0.0)
        total_cost = token_stats.get("total_cost", 0.0)

        if input_tokens > 0 or output_tokens > 0:
            footer.append(" | ", style="dim")

            # Show interaction tokens
            token_parts = []
            if input_tokens > 0:
                token_parts.append(f"I:{input_tokens}")
            if output_tokens > 0:
                token_parts.append(f"O:{output_tokens}")
            if reasoning_tokens > 0:
                token_parts.append(f"R:{reasoning_tokens}")
            footer.append(" ".join(token_parts), style="green")

            # Show totals if different from interaction
            if total_input_tokens > input_tokens or total_output_tokens > output_tokens:
                footer.append(" | Total: ", style="dim")
                total_parts = []
                if total_input_tokens > 0:
                    total_parts.append(f"I:{total_input_tokens}")
                if total_output_tokens > 0:
                    total_parts.append(f"O:{total_output_tokens}")
                if total_reasoning_tokens > 0:
                    total_parts.append(f"R:{total_reasoning_tokens}")
                footer.append(" ".join(total_parts), style="cyan")

            # Context usage indicator
            context_usage_pct = token_stats.get("context_usage_pct", 0)
            if context_usage_pct > 0:
                if context_usage_pct < 50:
                    indicator = "🟩"
                    color = "green"
                elif context_usage_pct < 80:
                    indicator = "🟨"
                    color = "yellow"
                else:
                    indicator = "🟥"
                    color = "red"

                footer.append(f" | {indicator} {context_usage_pct:.1f}%", style=f"bold {color}")

            # Add cost information if available
            if interaction_cost > 0 or total_cost > 0:
                footer.append(" | 💰 ", style="dim")
                if interaction_cost > 0:
                    footer.append(f"${interaction_cost:.4f}", style="yellow")
                if total_cost > 0 and total_cost != interaction_cost:
                    footer.append(f" (Total: ${total_cost:.4f})", style="bold yellow")

        stream_ctx["footer"] = footer

    def _perform_final_tui_update(self, stream_id: str, stream_ctx: dict[str, Any]) -> None:
        """Perform final TUI update in main thread"""
        terminal_output = stream_ctx.get("terminal_output")
        terminal_widget = stream_ctx.get("terminal_widget")

        # If terminal_output is a UniversalTerminal, get its RichLog output
        if terminal_output and hasattr(terminal_output, 'output') and terminal_output.output:
            terminal_output = terminal_output.output

        if not terminal_output or not hasattr(terminal_output, "write"):
            return

        final_stats = stream_ctx.get("final_stats", {})
        final_content = stream_ctx.get("accumulated_content", "")
        
        # Clear the streaming line and write newline
        terminal_output.write("")  # New line after streaming
        
        # Prepare metadata for panel
        metadata = {
            'timestamp': stream_ctx.get('timestamp', datetime.now().strftime("%H:%M:%S")),
            'model': stream_ctx.get('model', ''),
            'interaction': stream_ctx.get('interaction_counter', 1)
        }
        
        # Use PanelFormatter.create_agent_panel for comprehensive cost display
        final_panel = PanelFormatter.create_agent_panel(
            agent_name=stream_ctx['agent_name'],
            message=final_content,
            metadata=metadata,
            streaming=False,
            token_info=final_stats  # Pass final_stats as token_info for full cost display
        )
        
        # Write the final panel
        terminal_output.write(final_panel)
        
        # Complete streaming in the terminal (which updates action bar)
        if terminal_widget and hasattr(terminal_widget, 'finish_streaming_line'):
            # If we received an error in final_stats, mark it on the action bar before finishing
            if isinstance(final_stats, dict) and final_stats.get("is_error"):
                if hasattr(terminal_widget, 'action_bar') and terminal_widget.action_bar:
                    if hasattr(terminal_widget.action_bar, 'mark_stream_error'):
                        try:
                            terminal_widget.action_bar.mark_stream_error(str(final_stats.get("error_message", "Error")))
                        except Exception:
                            pass
            terminal_widget.finish_streaming_line(
                stream_ctx.get("stream_line_id", ""),
                final_content,
                final_stats
            )
        return

    def _write_final_panel(self, stream_ctx: dict[str, Any], final_stats: dict[str, Any]) -> None:
        """Write final panel directly (fallback)"""
        # DEAD CODE REMOVED - Map from SDK naming convention to display naming
        terminal_output = stream_ctx.get("terminal_output")
        if not terminal_output:
            return

        # Similar to _perform_final_tui_update but called directly
        header = f"[bold cyan][{stream_ctx['interaction_counter']}][/bold cyan] [bold green]{stream_ctx['agent_name']}[/bold green] >> [green]Response[/green] [dim][{stream_ctx['timestamp']} ({stream_ctx['model']})[/dim]"

        # Add stats...
        final_panel = Panel(
            stream_ctx["accumulated_content"],
            title=header,
            title_align="left",
            border_style="green",
            box=ROUNDED,
            padding=(0, 1),
        )

        _safe_write_panel(terminal_output, final_panel)
        terminal_output.write("")

    def _display_stream_content(
        self, console: Console, context: DisplayContext, content: str, data: dict[str, Any]
    ) -> None:
        """Display stream content statically"""
        agent_name = context.agent_name or f"Agent {context.terminal_number}"

        panel = PanelFormatter.create_agent_panel(
            agent_name=agent_name,
            message=content,
            metadata=data.get("metadata", {}),
            streaming=False,
            token_info=data.get("token_info", {}),  # Pass token info for costs
        )

        # Use terminal output directly in TUI mode
        terminal_output = _get_terminal_richlog(context.terminal_id)
        if terminal_output:
            _safe_write_panel(terminal_output, panel)
        else:
            console.print(panel)

    def _display_thinking_content(
        self, console: Console, context: DisplayContext, content: str, data: dict[str, Any]
    ) -> None:
        """Display thinking content statically"""
        model = data.get("model", context.metadata.get("model", ""))
        agent_name = context.agent_name or f"Agent {context.terminal_number}"

        panel = PanelFormatter.create_thinking_panel(
            agent_name=agent_name, thinking_content=content, model_name=model, finished=True
        )

        # Use terminal output directly in TUI mode
        terminal_output = _get_terminal_richlog(context.terminal_id)
        if terminal_output:
            _safe_write_panel(terminal_output, panel)
        else:
            console.print(panel)


    def _handle_buffer_overflow(self, stream_id: str, stream_ctx: dict[str, Any]) -> None:
        """Handle buffer overflow by truncating content"""
        # Keep last 80% of content
        content = stream_ctx.get("accumulated_content", "")
        if len(content) > 1000:
            keep_size = int(len(content) * 0.8)
            stream_ctx["accumulated_content"] = "... [truncated] " + content[-keep_size:]

            # Log the truncation
            ERROR_HANDLER.record_error(stream_id,
                MemoryError(f"Buffer overflow, truncated from {len(content)} to {keep_size}"),
                {"method": "_handle_buffer_overflow"}
            )
