"""
Integration layer between CAI core and TUI display system
"""

import os
from typing import Any, Dict, List, Optional, Union
from cai.tui.display import DisplayManager, DisplayMode


def get_display_mode() -> DisplayMode:
    """Determine current display mode"""
    # Check if we're in TUI mode
    if os.getenv("CAI_TUI_MODE") == "true":
        return DisplayMode.TUI
    return DisplayMode.CLI


def get_terminal_id() -> Optional[str]:
    """Get current terminal ID from context"""
    # In async contexts, prefer context variables over thread-local
    # This is important for parallel execution where multiple tasks
    # share the same thread but have different context vars
    from cai.tui.core.execution_context import get_terminal_id_context
    
    terminal_id = get_terminal_id_context()
    
    if not terminal_id:
        # Fall back to thread-local storage
        from cai.tui.core.terminal_tracking import get_current_terminal_id
        terminal_id = get_current_terminal_id()

    return terminal_id


def display_tool_output(
    tool_name: str = "",
    args: Union[Dict, str] = "",
    output: str = "",
    call_id: Optional[str] = None,
    execution_info: Optional[Dict] = None,
    token_info: Optional[Dict] = None,
    streaming: bool = False,
) -> None:
    """Display tool output - routes to appropriate display system"""
    display_manager = DisplayManager()

    if os.getenv("CAI_DEBUG_DISPLAY"):
        print(f"[DEBUG] display_tool_output integration called:")
        print(f"  - display_mode: {display_manager.get_mode()}")
        print(f"  - terminal_id: {get_terminal_id()}")
        print(f"  - tool_name: {tool_name}")

    if display_manager.get_mode() == DisplayMode.TUI:
        terminal_id = get_terminal_id()

        # If no terminal ID from tracking, try display context
        if not terminal_id:
            from cai.tui.display.handoff_context import get_display_context
            display_ctx = get_display_context()
            if display_ctx:
                terminal_id = display_ctx.terminal_id
                if os.getenv("CAI_DEBUG_DISPLAY"):
                    print(f"[DEBUG] Using terminal_id from display context: {terminal_id}")

        if terminal_id:
            display_manager.display_tool_output(
                terminal_id=terminal_id,
                tool_name=tool_name,
                args=args,
                output=output,
                execution_info=execution_info,
                token_info=token_info,
                streaming=streaming,
                call_id=call_id,
            )
        else:
            if os.getenv("CAI_DEBUG_DISPLAY"):
                print(f"[DEBUG] No terminal_id found! Cannot route to TUI display")
                print(f"[DEBUG] Tool: {tool_name}, Output: {output[:100] if output else 'None'}")
            # Don't fall back to CLI when in TUI mode - this would cause recursion
            # Just log the error and return
            import sys
            print(f"[WARNING] TUI mode but no terminal_id for tool output: {tool_name}", file=sys.stderr)
            return
    else:
        # Fall back to CLI display
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


def display_agent_messages(
    messages: List[Dict],
    model: Optional[str] = None,
    max_messages: int = 3,
    agent_name: Optional[str] = None,
    counter: Optional[int] = None,
    token_info: Optional[Dict] = None,
) -> None:
    """Display agent messages - routes to appropriate display system"""
    display_manager = DisplayManager()

    if display_manager.get_mode() == DisplayMode.TUI:
        terminal_id = get_terminal_id()

        # Debug logging
        if os.getenv("CAI_DEBUG") == "2":
            print(f"[DEBUG] display_agent_messages: terminal_id from context = {terminal_id}")
            print(f"[DEBUG] display_agent_messages: agent_name = {agent_name}")

        if terminal_id:
            # Update context with agent info if provided
            context = display_manager.get_context(terminal_id)
            if context and agent_name:
                context.agent_name = agent_name
            if context and counter is not None:
                context.interaction_counter = counter

            # Include token info in the data
            data = {"messages": messages, "model": model, "max_messages": max_messages}
            if token_info:
                data["token_info"] = token_info

            display_manager.display_agent_messages(
                terminal_id=terminal_id,
                messages=messages,
                model=model,
                max_messages=max_messages,
                token_info=token_info,
            )
    else:
        # Fall back to CLI display - this won't be called from wrapper
        pass


def start_tool_streaming(
    tool_name: str,
    args: Union[Dict, str],
    call_id: Optional[str] = None,
    token_info: Optional[Dict] = None,
) -> str:
    """Start tool streaming - routes to appropriate display system"""
    display_manager = DisplayManager()

    if display_manager.get_mode() == DisplayMode.TUI:
        terminal_id = get_terminal_id()
        
        # If no terminal ID from tracking, try display context
        if not terminal_id:
            from cai.tui.display.handoff_context import get_display_context
            display_ctx = get_display_context()
            if display_ctx:
                terminal_id = display_ctx.terminal_id
        
        if terminal_id:
            return display_manager.start_tool_streaming(
                terminal_id=terminal_id,
                tool_name=tool_name,
                args=args,
                call_id=call_id,
                token_info=token_info,
            )
    else:
        # Fall back to CLI display
        from cai.tui.display.safe_util import start_tool_streaming

        return start_tool_streaming(tool_name, args, call_id, token_info)

    return call_id or ""


def update_tool_streaming(
    tool_name: str,
    args: Union[Dict, str],
    output: str,
    call_id: str,
    token_info: Optional[Dict] = None,
) -> None:
    """Update tool streaming - routes to appropriate display system"""
    display_manager = DisplayManager()

    if display_manager.get_mode() == DisplayMode.TUI:
        terminal_id = get_terminal_id()
        
        # If no terminal ID from tracking, try display context
        if not terminal_id:
            from cai.tui.display.handoff_context import get_display_context
            display_ctx = get_display_context()
            if display_ctx:
                terminal_id = display_ctx.terminal_id
        
        if terminal_id:
            display_manager.update_tool_streaming(
                terminal_id=terminal_id,
                tool_name=tool_name,
                args=args,
                output=output,
                call_id=call_id,
                token_info=token_info,
            )
    else:
        # Fall back to CLI display
        from cai.tui.display.safe_util import update_tool_streaming

        update_tool_streaming(tool_name, args, output, call_id, token_info)


def finish_tool_streaming(
    tool_name: str,
    args: Union[Dict, str],
    output: str,
    call_id: str,
    execution_info: Optional[Dict] = None,
    token_info: Optional[Dict] = None,
) -> None:
    """Finish tool streaming - routes to appropriate display system"""
    display_manager = DisplayManager()

    if display_manager.get_mode() == DisplayMode.TUI:
        terminal_id = get_terminal_id()
        
        # If no terminal ID from tracking, try display context
        if not terminal_id:
            from cai.tui.display.handoff_context import get_display_context
            display_ctx = get_display_context()
            if display_ctx:
                terminal_id = display_ctx.terminal_id
        
        if terminal_id:
            display_manager.finish_tool_streaming(
                terminal_id=terminal_id,
                tool_name=tool_name,
                args=args,
                output=output,
                call_id=call_id,
                execution_info=execution_info,
                token_info=token_info,
            )
    else:
        # Fall back to CLI display
        from cai.tui.display.safe_util import finish_tool_streaming

        finish_tool_streaming(tool_name, args, output, call_id, execution_info, token_info)


def create_agent_streaming_context(
    agent_name: str, counter: int, model: str
) -> Optional[Dict[str, Any]]:
    """Create agent streaming context - routes to appropriate display system"""
    display_manager = DisplayManager()

    if display_manager.get_mode() == DisplayMode.TUI:
        terminal_id = get_terminal_id()
        if terminal_id:
            return display_manager.create_agent_streaming_context(
                terminal_id=terminal_id, agent_name=agent_name, counter=counter, model=model
            )
    
    # In non-TUI mode, return None - let util.py handle it
    return None


def update_agent_streaming_content(
    context: Dict[str, Any], text_delta: str, token_stats: Optional[Dict] = None
) -> bool:
    """Update agent streaming content - routes to appropriate display system"""
    if not context:
        return False

    display_manager = DisplayManager()

    if display_manager.get_mode() == DisplayMode.TUI or context.get("is_tui"):
        return display_manager.update_agent_streaming_content(
            streaming_context=context, text_delta=text_delta, token_stats=token_stats
        )
    else:
        # Fall back to CLI display
        from cai.tui.display.safe_util import update_agent_streaming_content

        return update_agent_streaming_content(context, text_delta, token_stats)


def finish_agent_streaming(context: Dict[str, Any], final_stats: Optional[Dict] = None) -> bool:
    """Finish agent streaming - routes to appropriate display system"""
    if not context:
        return False

    display_manager = DisplayManager()

    if display_manager.get_mode() == DisplayMode.TUI or context.get("is_tui"):
        return display_manager.finish_agent_streaming(
            streaming_context=context, final_stats=final_stats
        )
    else:
        # Fall back to CLI display
        from cai.tui.display.safe_util import finish_agent_streaming

        return finish_agent_streaming(context, final_stats)


def start_thinking_display_if_applicable(
    model_name: str, agent_name: str, counter: int
) -> Optional[Dict[str, Any]]:
    """Start thinking display if applicable - routes to appropriate display system"""
    display_manager = DisplayManager()

    # Check if thinking is supported
    if not display_manager.should_show_thinking(model_name):
        return None

    if display_manager.get_mode() == DisplayMode.TUI:
        terminal_id = get_terminal_id()
        if terminal_id:
            return display_manager.start_thinking_display(
                terminal_id=terminal_id, agent_name=agent_name, counter=counter, model=model_name
            )
    else:
        # Fall back to CLI display
        from cai.tui.display.safe_util import start_claude_thinking_if_applicable

        return start_claude_thinking_if_applicable(model_name, agent_name, counter)

    return None


def update_thinking_content(context: Dict[str, Any], thinking_delta: str) -> bool:
    """Update thinking content - routes to appropriate display system"""
    if not context:
        return False

    display_manager = DisplayManager()

    if display_manager.get_mode() == DisplayMode.TUI or context.get("is_tui"):
        return display_manager.update_thinking_content(
            thinking_context=context, thinking_delta=thinking_delta
        )
    else:
        # Fall back to CLI display
        from cai.tui.display.safe_util import update_claude_thinking_content

        return update_claude_thinking_content(context, thinking_delta)


def finish_thinking_display(context: Dict[str, Any]) -> bool:
    """Finish thinking display - routes to appropriate display system"""
    if not context:
        return False

    display_manager = DisplayManager()

    if display_manager.get_mode() == DisplayMode.TUI or context.get("is_tui"):
        return display_manager.finish_thinking_display(thinking_context=context)
    else:
        # Fall back to CLI display
        from cai.tui.display.safe_util import finish_claude_thinking_display

        return finish_claude_thinking_display(context)
