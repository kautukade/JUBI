"""
Integration shim for TUI display routing.

Ensures OpenAI ChatCompletions uses the TUI display integration layer.
"""

import sys


def integrate_openai_chatcompletions_display():
    """Route display functions to the TUI integration layer (idempotent)."""

    # Import the module
    import cai.sdk.agents.models.openai_chatcompletions as openai_module

    # Import the display wrapper
    from cai.tui.display.wrapper import DISPLAY
    
    # Ensure original functions are stored by the display layer if needed
    try:
        from cai.tui.display.original_functions import store_original_functions
        store_original_functions()
    except Exception:
        pass

    # Replace the imported functions in the module with wrapper methods
    openai_module.cli_print_tool_output = DISPLAY.print_tool_output
    openai_module.cli_print_agent_messages = DISPLAY.print_agent_messages
    openai_module.start_tool_streaming = DISPLAY.start_tool_streaming
    openai_module.update_tool_streaming = DISPLAY.update_tool_streaming
    openai_module.finish_tool_streaming = DISPLAY.finish_tool_streaming
    openai_module.create_agent_streaming_context = DISPLAY.create_agent_streaming_context
    openai_module.update_agent_streaming_content = DISPLAY.update_agent_streaming_content
    openai_module.finish_agent_streaming = DISPLAY.finish_agent_streaming
    openai_module.start_claude_thinking_if_applicable = DISPLAY.start_thinking_if_applicable
    openai_module.update_claude_thinking_content = DISPLAY.update_thinking_content
    openai_module.finish_claude_thinking_display = DISPLAY.finish_thinking_display

    # Also patch in sys.modules to ensure all imports get the patched version
    if "cai.util" in sys.modules:
        util_module = sys.modules["cai.util"]
        util_module.cli_print_tool_output = DISPLAY.print_tool_output
        util_module.cli_print_agent_messages = DISPLAY.print_agent_messages
        util_module.start_tool_streaming = DISPLAY.start_tool_streaming
        util_module.update_tool_streaming = DISPLAY.update_tool_streaming
        util_module.finish_tool_streaming = DISPLAY.finish_tool_streaming
        util_module.create_agent_streaming_context = DISPLAY.create_agent_streaming_context
        util_module.update_agent_streaming_content = DISPLAY.update_agent_streaming_content
        util_module.finish_agent_streaming = DISPLAY.finish_agent_streaming
        util_module.start_claude_thinking_if_applicable = DISPLAY.start_thinking_if_applicable
        util_module.update_claude_thinking_content = DISPLAY.update_thinking_content
        util_module.finish_claude_thinking_display = DISPLAY.finish_thinking_display
        
        # Route the global console object to TUI terminals
        # This intercepts all console.print(panel) calls
        # Optional console interceptor; if absent, continue silently
        try:
            from cai.tui.display.console_interceptor import TUIConsoleInterceptor
            util_module.console = TUIConsoleInterceptor()
        except Exception:
            pass
