"""
Display manager for coordinating all TUI displays
"""

import os
import threading
from typing import Any, Optional, Union

from rich.console import Console

from .agent_display import AgentDisplay
from .base import BaseDisplay, DisplayContext, DisplayMode
from .streaming_display import StreamingDisplay
from .tool_display import ToolDisplay


class DisplayManager:
    """Manages all display operations for TUI"""

    _instance = None
    _lock = threading.Lock()

    def __new__(cls):
        """Singleton pattern for display manager"""
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = super().__new__(cls)
        return cls._instance

    def __init__(self):
        """Initialize display manager"""
        if hasattr(self, "_initialized"):
            return

        self._initialized = True
        self._mode = DisplayMode.TUI
        self._displays: dict[str, BaseDisplay] = {
            "tool": ToolDisplay(),
            "agent": AgentDisplay(),
            "streaming": StreamingDisplay(),
        }
        self._contexts: dict[str, DisplayContext] = {}
        self._console: Optional[Console] = None
        self._lock = threading.Lock()

    @classmethod
    def get_instance(cls):
        """Return the global DisplayManager instance for compatibility.

        Some callers expect a `get_instance()` accessor. Provide it to avoid
        AttributeError and centralize singleton access.
        """
        try:
            return DISPLAY_MANAGER  # type: ignore[name-defined]
        except NameError:
            # Module-level instance not initialized yet; create one.
            return cls()

    def set_mode(self, mode: DisplayMode) -> None:
        """Set display mode (CLI or TUI)"""
        with self._lock:
            self._mode = mode

    def get_mode(self) -> DisplayMode:
        """Get current display mode"""
        with self._lock:
            return self._mode

    def set_console(self, console: Console) -> None:
        """Set console for all displays"""
        with self._lock:
            self._console = console
            for display in self._displays.values():
                display.set_console(console)

    def set_terminal_output(self, terminal_id: str, output: Any) -> None:
        """Set terminal output for a specific terminal"""
        # Pass to all displays
        for display in self._displays.values():
            if hasattr(display, "set_terminal_output"):
                display.set_terminal_output(terminal_id, output)

    def create_context(
        self,
        terminal_id: str,
        terminal_number: int,
        agent_name: Optional[str] = None,
        agent_id: Optional[str] = None,
        **kwargs,
    ) -> DisplayContext:
        """Create a new display context"""
        context = DisplayContext(
            terminal_id=terminal_id,
            terminal_number=terminal_number,
            mode=self._mode,
            agent_name=agent_name,
            agent_id=agent_id,
            **kwargs,
        )

        with self._lock:
            self._contexts[terminal_id] = context

        return context

    def get_context(self, terminal_id: str) -> Optional[DisplayContext]:
        """Get context for a terminal"""
        with self._lock:
            return self._contexts.get(terminal_id)

    def remove_context(self, terminal_id: str) -> None:
        """Remove and cleanup a context"""
        with self._lock:
            context = self._contexts.pop(terminal_id, None)
            if context:
                # Cleanup all displays for this context
                for display in self._displays.values():
                    display.cleanup(context)

    # Tool display methods
    def display_tool_output(
        self,
        terminal_id: str,
        tool_name: str,
        args: Union[dict, str],
        output: str,
        execution_info: Optional[dict] = None,
        token_info: Optional[dict] = None,
        streaming: bool = False,
        call_id: Optional[str] = None,
    ) -> None:
        """Display tool output"""
        if self._mode != DisplayMode.TUI:
            # In CLI mode, use the original util.py functions
            from cai.tui.display import safe_util

            safe_util.cli_print_tool_output(
                tool_name=tool_name,
                args=args,
                output=output,
                call_id=call_id,
                execution_info=execution_info,
                token_info=token_info,
                streaming=streaming,
            )
            return

        context = self.get_context(terminal_id)
        if not context:
            return

        data = {
            "tool_name": tool_name,
            "args": args,
            "output": output,
            "execution_info": execution_info,
            "token_info": token_info,
        }

        if streaming and call_id:
            # Streaming tool execution
            if execution_info and execution_info.get("is_final"):
                self._displays["tool"].finish_streaming(call_id, data)
            elif execution_info and execution_info.get("status") == "running":
                self._displays["tool"].update_streaming(call_id, data)
            else:
                self._displays["tool"].start_streaming(context, call_id, data)
        else:
            # Non-streaming display
            self._displays["tool"].display(context, data)

    def start_tool_streaming(
        self,
        terminal_id: str,
        tool_name: str,
        args: Union[dict, str],
        call_id: Optional[str] = None,
        token_info: Optional[dict] = None,
    ) -> str:
        """Start a tool streaming session"""
        if self._mode != DisplayMode.TUI:
            from cai.tui.display.safe_util import start_tool_streaming

            return start_tool_streaming(tool_name, args, call_id, token_info)

        context = self.get_context(terminal_id)
        if not context:
            return call_id or ""

        data = {"tool_name": tool_name, "args": args, "token_info": token_info}

        self._displays["tool"].start_streaming(context, call_id or "", data)
        return call_id or f"tool_{tool_name}"

    def update_tool_streaming(
        self,
        terminal_id: str,
        tool_name: str,
        args: Union[dict, str],
        output: str,
        call_id: str,
        token_info: Optional[dict] = None,
    ) -> None:
        """Update a tool streaming session"""
        if self._mode != DisplayMode.TUI:
            from cai.tui.display.safe_util import update_tool_streaming

            update_tool_streaming(tool_name, args, output, call_id, token_info)
            return

        data = {"tool_name": tool_name, "args": args, "output": output, "token_info": token_info}

        self._displays["tool"].update_streaming(call_id, data)

    def finish_tool_streaming(
        self,
        terminal_id: str,
        tool_name: str,
        args: Union[dict, str],
        output: str,
        call_id: str,
        execution_info: Optional[dict] = None,
        token_info: Optional[dict] = None,
    ) -> None:
        """Finish a tool streaming session"""
        if self._mode != DisplayMode.TUI:
            from cai.tui.display.safe_util import finish_tool_streaming

            finish_tool_streaming(tool_name, args, output, call_id, execution_info, token_info)
            return

        data = {
            "tool_name": tool_name,
            "args": args,
            "output": output,
            "execution_info": execution_info,
            "token_info": token_info,
        }

        self._displays["tool"].finish_streaming(call_id, data)

    # Agent display methods
    def display_agent_messages(
        self,
        terminal_id: str,
        messages: list,
        model: Optional[str] = None,
        max_messages: int = 3,
        token_info: Optional[dict] = None,
    ) -> None:
        """Display agent messages"""
        if self._mode != DisplayMode.TUI:
            from cai.tui.display.safe_util import cli_print_agent_messages

            cli_print_agent_messages(messages, model, max_messages)
            return

        context = self.get_context(terminal_id)
        if not context:
            return

        data = {"messages": messages, "model": model, "max_messages": max_messages}

        # Add token info if provided
        if token_info:
            data["token_info"] = token_info

        self._displays["agent"].display(context, data)

    # Streaming display methods
    def create_agent_streaming_context(
        self, terminal_id: str, agent_name: str, counter: int, model: str
    ) -> Optional[dict[str, Any]]:
        """Create streaming context for agent"""
        import uuid
        
        # Create a streaming context directly for TUI
        stream_id = f"stream_{uuid.uuid4().hex[:8]}"
        
        # Start streaming in the display
        self._displays["streaming"].start_streaming(stream_id, {
            "agent_name": agent_name,
            "counter": counter,
            "model": model,
            "type": "agent"
        })
        
        # Return the context
        context = {
            "agent_name": agent_name,
            "interaction_counter": counter,
            "model": model,
            "stream_id": stream_id,
            "is_tui": True,
            "terminal_id": terminal_id,
            "content": "",
            "is_started": True,
        }
        
        return context

    def update_agent_streaming_content(
        self, streaming_context: dict[str, Any], text_delta: str, token_stats: Optional[dict] = None
    ) -> bool:
        """Update agent streaming content"""
        stream_id = streaming_context.get("stream_id")
        if not stream_id:
            return False

        # Pass the raw text_delta directly - don't modify it
        # The StreamingDisplay will handle accumulation
        data = {
            "content": text_delta,  # Pass the delta as-is
            "token_stats": token_stats,
            "content_type": "text",
            "terminal_id": streaming_context.get("terminal_id")
        }

        # Start streaming if not already started
        if not hasattr(self._displays["streaming"], "_streaming_contexts") or stream_id not in getattr(self._displays["streaming"], "_streaming_contexts", {}):
            # Initialize streaming
            context = self.get_context(streaming_context.get("terminal_id"))
            if context:
                self._displays["streaming"].start_streaming(context, stream_id, {
                    "content_type": "text",
                    "agent_name": streaming_context.get("agent_name"),
                    "model": streaming_context.get("model"),
                    "interaction_counter": streaming_context.get("interaction_counter")
                })

        # Update streaming
        self._displays["streaming"].update_streaming(stream_id, data)
        return True

    def finish_agent_streaming(
        self, streaming_context: dict[str, Any], final_stats: Optional[dict] = None
    ) -> bool:
        """Finish agent streaming"""
        stream_id = streaming_context.get("stream_id")
        if not stream_id:
            return False

        # Include the accumulated content in the final data
        data = {
            "final_stats": final_stats,
            "final_content": streaming_context.get("content", "")
        }

        # If final_stats indicates an error, try to mark it on the action bar too
        try:
            if final_stats and isinstance(final_stats, dict) and final_stats.get("is_error"):
                # Access the terminal widget to mark the error on its action bar
                context = self.get_context(streaming_context.get("terminal_id"))
                if context and hasattr(context, "terminal_id"):
                    from cai.tui.core.terminal_console import get_terminal_output
                    terminal_widget = get_terminal_output(context.terminal_id)
                    if terminal_widget and hasattr(terminal_widget, 'action_bar') and terminal_widget.action_bar:
                        if hasattr(terminal_widget.action_bar, 'mark_stream_error'):
                            terminal_widget.action_bar.mark_stream_error(
                                str(final_stats.get("error_message", "Error"))
                            )
        except Exception:
            # Non-fatal: continue finishing streaming
            pass

        self._displays["streaming"].finish_streaming(stream_id, data)
        return True

    # Thinking display methods
    def start_thinking_display(
        self, terminal_id: str, agent_name: str, counter: int, model: str
    ) -> Optional[dict[str, Any]]:
        """Start thinking/reasoning display"""
        if self._mode != DisplayMode.TUI:
            from cai.tui.display.safe_util import create_claude_thinking_context

            return create_claude_thinking_context(agent_name, counter, model)

        context = self.get_context(terminal_id)
        if not context:
            return None

        thinking_id = f"thinking_{agent_name}_{counter}"
        data = {"model": model, "content_type": "thinking"}

        self._displays["streaming"].start_streaming(context, thinking_id, data)

        return {"thinking_id": thinking_id, "context": context, "is_tui": True}

    def update_thinking_content(
        self, thinking_context: dict[str, Any], thinking_delta: str
    ) -> bool:
        """Update thinking content"""
        if self._mode != DisplayMode.TUI or not thinking_context.get("is_tui"):
            from cai.tui.display.safe_util import update_claude_thinking_content

            return update_claude_thinking_content(thinking_context, thinking_delta)

        thinking_id = thinking_context.get("thinking_id")
        if not thinking_id:
            return False

        data = {"content": thinking_delta}

        self._displays["streaming"].update_streaming(thinking_id, data)
        return True

    def finish_thinking_display(self, thinking_context: dict[str, Any]) -> bool:
        """Finish thinking display"""
        if self._mode != DisplayMode.TUI or not thinking_context.get("is_tui"):
            from cai.tui.display.safe_util import finish_claude_thinking_display

            return finish_claude_thinking_display(thinking_context)

        thinking_id = thinking_context.get("thinking_id")
        if not thinking_id:
            return False

        self._displays["streaming"].finish_streaming(thinking_id, {})
        return True

    # Utility methods
    def is_thinking_supported(self, model_name: str) -> bool:
        """Check if model supports thinking display"""
        from cai.tui.display.safe_util import detect_claude_thinking_in_stream

        return detect_claude_thinking_in_stream(model_name)

    def should_show_thinking(self, model_name: str) -> bool:
        """Check if thinking should be shown"""
        if self._mode != DisplayMode.TUI:
            # In CLI mode, check streaming setting
            streaming_enabled = os.getenv("CAI_STREAM", "false").lower() == "true"
            return streaming_enabled and self.is_thinking_supported(model_name)
        else:
            # In TUI mode, always show if supported
            return self.is_thinking_supported(model_name)


# Global display manager instance
DISPLAY_MANAGER = DisplayManager()
