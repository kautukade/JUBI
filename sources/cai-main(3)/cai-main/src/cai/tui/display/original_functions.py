"""
Store original function references before display integration

This module stores references to the original util functions before they are
integrated with the TUI display routing. This allows safe_util to call the
original implementations without provocar recursión.
"""

from typing import Any, Dict, Optional

# Storage for original function references
_original_functions: Dict[str, Any] = {}


def store_original_functions():
    """Store references to original functions before integration routing"""
    try:
        import cai.util
        
        # Store all the functions we're going to patch
        functions_to_store = [
            'cli_print_tool_output',
            'cli_print_agent_messages',
            'start_tool_streaming',
            'update_tool_streaming',
            'finish_tool_streaming',
            'create_agent_streaming_context',
            'update_agent_streaming_content',
            'finish_agent_streaming',
            'start_claude_thinking_if_applicable',
            'update_claude_thinking_content',
            'finish_claude_thinking_display',
        ]
        
        for func_name in functions_to_store:
            if hasattr(cai.util, func_name):
                _original_functions[func_name] = getattr(cai.util, func_name)
                
    except ImportError:
        # cai.util not available yet, will be stored later
        pass


def get_original_function(func_name: str):
    """Get the original function reference"""
    return _original_functions.get(func_name)


def has_original_function(func_name: str) -> bool:
    """Check if we have the original function stored"""
    return func_name in _original_functions
