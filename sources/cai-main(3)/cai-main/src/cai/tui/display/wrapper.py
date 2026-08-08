"""
Display wrapper for clean separation between CLI and TUI modes
"""

import os

_CAI_DEBUG_DIR = os.path.join(os.path.expanduser("~"), ".cai", "debug")
import threading
from typing import Any, Optional, Union

# Thread-local storage to track if we're already in a wrapper call
_thread_local = threading.local()


class DisplayWrapper:
    """
    Unified display wrapper that routes to appropriate display system
    based on current mode (CLI or TUI)
    """

    def __init__(self):
        """Initialize display wrapper"""
        self._is_tui = os.getenv("CAI_TUI_MODE") == "true"

    @property
    def is_tui(self) -> bool:
        """Check if we're in TUI mode"""
        return self._is_tui

    def print_tool_output(
        self,
        tool_name: str = "",
        args: Union[dict, str] = "",
        output: str = "",
        call_id: Optional[str] = None,
        execution_info: Optional[dict] = None,
        token_info: Optional[dict] = None,
        streaming: bool = False,
    ) -> None:
        """Print tool output"""
        # DEBUG: Log to file
        import traceback
        with open(f"{_CAI_DEBUG_DIR}/cai_recursion_debug.log", "a") as f:
            stack_size = len(traceback.extract_stack())
            f.write(f"[RECURSION DEBUG] DisplayWrapper.print_tool_output - Tool: '{tool_name}', Stack: {stack_size}\n")
            if stack_size > 30:
                f.write("  Deep stack in print_tool_output! Stack trace:\n")
                for frame in traceback.extract_stack()[-20:]:
                    f.write(f"    {frame.filename}:{frame.lineno} in {frame.name}\n")

        if os.getenv("CAI_DEBUG_DISPLAY"):
            print("[DEBUG] print_tool_output called:")
            print(f"  - is_tui: {self._is_tui}")
            print(f"  - tool_name: {tool_name}")
            print(f"  - args: {args}")
            print(f"  - output: {output[:100] if output else ''}")
            print(f"  - streaming: {streaming}")

        # Use thread-local storage for recursion guard
        if not hasattr(_thread_local, 'in_wrapper_call'):
            _thread_local.in_wrapper_call = False

        if _thread_local.in_wrapper_call:
            # We're already in a wrapper call - this means we're being called recursively
            # This can happen if original_functions.py imported already-patched functions
            # Just print to stderr and return to avoid infinite recursion
            import sys
            with open(f"{_CAI_DEBUG_DIR}/cai_recursion_debug.log", "a") as f:
                f.write(f"[ERROR] Recursion prevented in DisplayWrapper.print_tool_output for tool: {tool_name}\n")
            print(f"[ERROR] Recursion prevented in DisplayWrapper.print_tool_output for tool: {tool_name}", file=sys.stderr)
            return

        try:
            _thread_local.in_wrapper_call = True

            if self._is_tui:
                from cai.tui.display.integration import display_tool_output

                display_tool_output(
                    tool_name=tool_name,
                    args=args,
                    output=output,
                    call_id=call_id,
                    execution_info=execution_info,
                    token_info=token_info,
                    streaming=streaming,
                )
            else:
                # Use safe_util to avoid circular imports
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
        except RecursionError:
            # Recursion detected - log to stderr and continue
            import sys
            print(f"[ERROR] Recursion detected in DisplayWrapper for tool: {tool_name}", file=sys.stderr)
        except Exception as e:
            # Other errors - log to stderr and continue
            import sys
            print(f"[ERROR] Display error in DisplayWrapper for tool {tool_name}: {str(e)}", file=sys.stderr)
        finally:
            _thread_local.in_wrapper_call = False

    def print_agent_messages(self, *args, **kwargs) -> None:
        """Print agent messages - supports both old and new signatures"""
        # DEBUG: Log all calls to file
        with open(f"{_CAI_DEBUG_DIR}/cai_recursion_debug.log", "a") as f:
            f.write("[RECURSION DEBUG] DisplayWrapper.print_agent_messages called\n")
            f.write(f"  args: {args}\n")
            f.write(f"  kwargs keys: {list(kwargs.keys())}\n")
            if "tool_output" in kwargs:
                tool_output = kwargs.get('tool_output')
                if tool_output is not None:
                    f.write(f"  WARNING: tool_output in kwargs! Value: {str(tool_output)[:100]}\n")
                else:
                    f.write("  WARNING: tool_output in kwargs! Value: None\n")

        # Debug: log what we received
        import os

        if os.getenv("CAI_DEBUG_DISPLAY"):
            print(f"[DEBUG] print_agent_messages called with args={args}, kwargs={kwargs}")

        # Check if this is actually a tool output call
        # Only redirect if we have tool_name AND it's not part of agent message parameters
        if "tool_name" in kwargs and "agent_name" not in kwargs and "message" not in kwargs:
            # DEBUG: Log this redirect
            with open(f"{_CAI_DEBUG_DIR}/cai_recursion_debug.log", "a") as f:
                f.write("[RECURSION DEBUG] print_agent_messages redirecting to print_tool_output!\n")
                f.write(f"  kwargs keys: {list(kwargs.keys())}\n")
                f.write(f"  tool_name: {kwargs.get('tool_name', 'NOT SET')}\n")
                f.write("  This redirect should not happen for agent messages!\n")

            # This is actually a tool output, redirect to proper method
            # Extract the relevant parameters
            tool_kwargs = {
                "tool_name": kwargs.get("tool_name", ""),
                "args": kwargs.get("args", ""),
                "output": kwargs.get("output", kwargs.get("tool_output", "")),
                "call_id": kwargs.get("call_id"),
                "execution_info": kwargs.get("execution_info"),
                "token_info": kwargs.get("token_info"),
                "streaming": kwargs.get("streaming", False),
            }
            self.print_tool_output(**tool_kwargs)
            return

        if self._is_tui:
            from cai.tui.display.integration import display_agent_messages

            # Handle positional arguments
            if args:
                # Old-style positional call: (agent_name, message, counter, model, debug, ...)
                agent_name = args[0] if len(args) > 0 else kwargs.get("agent_name")
                message = args[1] if len(args) > 1 else kwargs.get("message")
                counter = args[2] if len(args) > 2 else kwargs.get("counter")
                model = args[3] if len(args) > 3 else kwargs.get("model")
                args[4] if len(args) > 4 else kwargs.get("debug", False)
            else:
                # Keyword arguments
                agent_name = kwargs.get("agent_name")
                message = kwargs.get("message")
                counter = kwargs.get("counter")
                model = kwargs.get("model")
                kwargs.get("debug", False)

            # Extract token information
            interaction_input_tokens = kwargs.get("interaction_input_tokens")
            interaction_output_tokens = kwargs.get("interaction_output_tokens")
            interaction_reasoning_tokens = kwargs.get("interaction_reasoning_tokens")
            total_input_tokens = kwargs.get("total_input_tokens")
            total_output_tokens = kwargs.get("total_output_tokens")
            total_reasoning_tokens = kwargs.get("total_reasoning_tokens")
            interaction_cost = kwargs.get("interaction_cost")
            total_cost = kwargs.get("total_cost")

            # Check if we have a message to display
            if message is not None:
                # Single message format from OpenAI module
                # Convert OpenAI message object to dict
                if hasattr(message, "model_dump"):
                    # It's an OpenAI message object
                    msg_dict = message.model_dump()
                elif isinstance(message, dict):
                    msg_dict = message
                elif hasattr(message, "tool_calls"):
                    # It's a tool call object (like ToolCallStreamDisplay)
                    msg_dict = {
                        "role": "assistant",
                        "content": None,
                        "tool_calls": []
                    }
                    # Extract tool calls
                    if message.tool_calls:
                        for tool_call in message.tool_calls:
                            tc_dict = {
                                "id": getattr(tool_call, "id", ""),
                                "type": getattr(tool_call, "type", "function"),
                                "function": {
                                    "name": "",
                                    "arguments": ""
                                }
                            }
                            if hasattr(tool_call, "function"):
                                tc_dict["function"]["name"] = getattr(tool_call.function, "name", "")
                                tc_dict["function"]["arguments"] = getattr(tool_call.function, "arguments", "")
                            msg_dict["tool_calls"].append(tc_dict)
                else:
                    msg_dict = {"role": "assistant", "content": str(message)}

                messages_list = [msg_dict]

                # Build token info if provided
                token_info = None
                if any(
                    [
                        interaction_input_tokens is not None,
                        interaction_output_tokens is not None,
                        total_input_tokens is not None,
                        total_output_tokens is not None,
                    ]
                ):
                    token_info = {
                        "interaction_input_tokens": interaction_input_tokens or 0,
                        "interaction_output_tokens": interaction_output_tokens or 0,
                        "interaction_reasoning_tokens": interaction_reasoning_tokens or 0,
                        "total_input_tokens": total_input_tokens or 0,
                        "total_output_tokens": total_output_tokens or 0,
                        "total_reasoning_tokens": total_reasoning_tokens or 0,
                        "interaction_cost": interaction_cost,
                        "total_cost": total_cost,
                        "model": model,
                        "agent_name": agent_name,
                    }

                display_agent_messages(
                    messages_list,
                    model,
                    kwargs.get("max_messages", 3),
                    agent_name=agent_name,
                    counter=counter,
                    token_info=token_info,
                )
            elif "messages" in kwargs:
                # List format
                display_agent_messages(
                    kwargs["messages"], kwargs.get("model"), kwargs.get("max_messages", 3)
                )
        else:
            # For CLI, pass through all arguments
            from cai.tui.display import safe_util
            safe_util.cli_print_agent_messages(*args, **kwargs)

    def start_tool_streaming(
        self,
        tool_name: str,
        args: Union[dict, str],
        call_id: Optional[str] = None,
        token_info: Optional[dict] = None,
    ) -> str:
        """Start tool streaming"""
        if self._is_tui:
            from cai.tui.display.integration import start_tool_streaming

            return start_tool_streaming(tool_name, args, call_id, token_info)
        else:
            from cai.tui.display import safe_util
            return safe_util.start_tool_streaming(tool_name, args, call_id, token_info)

    def update_tool_streaming(
        self,
        tool_name: str,
        args: Union[dict, str],
        output: str,
        call_id: str,
        token_info: Optional[dict] = None,
    ) -> None:
        """Update tool streaming"""
        if self._is_tui:
            from cai.tui.display.integration import update_tool_streaming

            update_tool_streaming(tool_name, args, output, call_id, token_info)
        else:
            from cai.tui.display import safe_util
            safe_util.update_tool_streaming(tool_name, args, output, call_id, token_info)

    def finish_tool_streaming(
        self,
        tool_name: str,
        args: Union[dict, str],
        output: str,
        call_id: str,
        execution_info: Optional[dict] = None,
        token_info: Optional[dict] = None,
    ) -> None:
        """Finish tool streaming"""
        if self._is_tui:
            from cai.tui.display.integration import finish_tool_streaming

            finish_tool_streaming(tool_name, args, output, call_id, execution_info, token_info)
        else:
            from cai.tui.display import safe_util
            safe_util.finish_tool_streaming(tool_name, args, output, call_id, execution_info, token_info)

    def create_agent_streaming_context(
        self, agent_name: str, counter: int, model: str
    ) -> Optional[dict[str, Any]]:
        """Create agent streaming context"""
        if self._is_tui:
            from cai.tui.display.integration import create_agent_streaming_context

            return create_agent_streaming_context(agent_name, counter, model)
        else:
            from cai.tui.display import safe_util
            return safe_util.create_agent_streaming_context(agent_name, counter, model)

    def update_agent_streaming_content(
        self, context: dict[str, Any], text_delta: str, token_stats: Optional[dict] = None
    ) -> bool:
        """Update agent streaming content"""
        if self._is_tui:
            from cai.tui.display.integration import update_agent_streaming_content

            return update_agent_streaming_content(context, text_delta, token_stats)
        else:
            from cai.tui.display import safe_util
            return safe_util.update_agent_streaming_content(context, text_delta, token_stats)

    def finish_agent_streaming(
        self, context: dict[str, Any], final_stats: Optional[dict] = None
    ) -> bool:
        """Finish agent streaming"""
        if self._is_tui:
            from cai.tui.display.integration import finish_agent_streaming

            return finish_agent_streaming(context, final_stats)
        else:
            from cai.tui.display import safe_util
            return safe_util.finish_agent_streaming(context, final_stats)

    def start_thinking_if_applicable(
        self, model_name: str, agent_name: str, counter: int
    ) -> Optional[dict[str, Any]]:
        """Start thinking display if applicable"""
        if self._is_tui:
            from cai.tui.display.integration import start_thinking_display_if_applicable

            return start_thinking_display_if_applicable(model_name, agent_name, counter)
        else:
            from cai.tui.display import safe_util
            return safe_util.start_claude_thinking_if_applicable(model_name, agent_name, counter)

    def update_thinking_content(self, context: dict[str, Any], thinking_delta: str) -> bool:
        """Update thinking content"""
        if self._is_tui:
            from cai.tui.display.integration import update_thinking_content

            return update_thinking_content(context, thinking_delta)
        else:
            from cai.tui.display import safe_util
            return safe_util.update_claude_thinking_content(context, thinking_delta)

    def finish_thinking_display(self, context: dict[str, Any]) -> bool:
        """Finish thinking display"""
        if self._is_tui:
            from cai.tui.display.integration import finish_thinking_display

            return finish_thinking_display(context)
        else:
            from cai.tui.display import safe_util
            return safe_util.finish_claude_thinking_display(context)


# Global display wrapper instance
DISPLAY = DisplayWrapper()
