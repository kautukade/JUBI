"""
Safe utility imports for TUI display system

This module provides safe wrappers around util functions to prevent
circular dependencies and recursion when the util functions are routed
to the TUI display integration layer.
"""

import sys
import threading
from typing import Any, Dict, Optional, Callable

import os
_CAI_DEBUG_DIR = os.path.join(os.path.expanduser("~"), ".cai", "debug")


# Thread-local storage for recursion detection
_recursion_guard = threading.local()


def _is_in_recursion(func_name: str) -> bool:
    """Check if we're already in a recursive call for this function"""
    if not hasattr(_recursion_guard, 'call_stack'):
        _recursion_guard.call_stack = set()
    return func_name in _recursion_guard.call_stack


def _enter_function(func_name: str):
    """Mark that we're entering a function"""
    if not hasattr(_recursion_guard, 'call_stack'):
        _recursion_guard.call_stack = set()
    _recursion_guard.call_stack.add(func_name)


def _exit_function(func_name: str):
    """Mark that we're exiting a function"""
    if hasattr(_recursion_guard, 'call_stack'):
        _recursion_guard.call_stack.discard(func_name)


def _call_safe_function(func_name: str, *args, **kwargs) -> Any:
    """
    Safely call a function with recursion protection.
    First tries to use the original stored function, then falls back to module lookup.
    """
    if _is_in_recursion(func_name):
        with open(f"{_CAI_DEBUG_DIR}/cai_recursion_debug.log", "a") as f:
            f.write(f"[WARNING] Recursion detected in {func_name}, skipping call\n")
            # DEBUG: Print stack trace to file
            import traceback
            for frame in traceback.extract_stack()[-10:]:
                f.write(f"  {frame.filename}:{frame.lineno} in {frame.name}\n")
        return None
    
    try:
        _enter_function(func_name)
        
        # First try to get the original function from storage
        from cai.tui.display.original_functions import get_original_function
        original_func = get_original_function(func_name)
        
        if original_func:
            # We have the original function, use it
            return original_func(*args, **kwargs)
        
        # Fallback: Try to get from module (this might be patched)
        util_module = sys.modules.get('cai.util')
        if util_module and hasattr(util_module, func_name):
            func = getattr(util_module, func_name)
            
            # Check if it's been integrated by comparing to our wrapper
            from cai.tui.display.wrapper import DISPLAY
            
            # Map function names to their wrapper equivalents
            wrapper_name_map = {
                'cli_print_tool_output': 'print_tool_output',
                'cli_print_agent_messages': 'print_agent_messages',
                'start_tool_streaming': 'start_tool_streaming',
                'update_tool_streaming': 'update_tool_streaming',
                'finish_tool_streaming': 'finish_tool_streaming',
                'create_agent_streaming_context': 'create_agent_streaming_context',
                'update_agent_streaming_content': 'update_agent_streaming_content',
                'finish_agent_streaming': 'finish_agent_streaming',
                'start_claude_thinking_if_applicable': 'start_thinking_if_applicable',
                'update_claude_thinking_content': 'update_thinking_content',
                'finish_claude_thinking_display': 'finish_thinking_display',
            }
            
            wrapper_method_name = wrapper_name_map.get(func_name)
            wrapper_func = getattr(DISPLAY, wrapper_method_name, None) if wrapper_method_name else None
            
            if wrapper_func and func is wrapper_func:
                print(f"[WARNING] {func_name} is routed to TUI and no original stored, cannot fall back to CLI", file=sys.stderr)
                return None
            
            # Call the function (might be original or patched)
            return func(*args, **kwargs)
        else:
            print(f"[WARNING] Could not find {func_name} in cai.util", file=sys.stderr)
            return None
    finally:
        _exit_function(func_name)


def cli_print_tool_output(**kwargs):
    """Safe wrapper for cli_print_tool_output that prevents recursion"""
    return _call_safe_function('cli_print_tool_output', **kwargs)


def cli_print_agent_messages(*args, **kwargs):
    """Safe wrapper for cli_print_agent_messages that prevents recursion"""
    return _call_safe_function('cli_print_agent_messages', *args, **kwargs)


def start_tool_streaming(tool_name: str, args: Any, call_id: Optional[str] = None, token_info: Optional[Dict] = None):
    """Safe wrapper for start_tool_streaming that prevents recursion"""
    result = _call_safe_function('start_tool_streaming', tool_name, args, call_id, token_info)
    return result if result is not None else (call_id or "")


def update_tool_streaming(tool_name: str, args: Any, output: str, call_id: str, token_info: Optional[Dict] = None):
    """Safe wrapper for update_tool_streaming that prevents recursion"""
    return _call_safe_function('update_tool_streaming', tool_name, args, output, call_id, token_info)


def finish_tool_streaming(tool_name: str, args: Any, output: str, call_id: str, execution_info: Optional[Dict] = None, token_info: Optional[Dict] = None):
    """Safe wrapper for finish_tool_streaming that prevents recursion"""
    return _call_safe_function('finish_tool_streaming', tool_name, args, output, call_id, execution_info, token_info)


def create_agent_streaming_context(agent_name: str, counter: int, model: str):
    """Safe wrapper for create_agent_streaming_context that prevents recursion"""
    return _call_safe_function('create_agent_streaming_context', agent_name, counter, model)


def update_agent_streaming_content(context: Dict[str, Any], text_delta: str, token_stats: Optional[Dict] = None):
    """Safe wrapper for update_agent_streaming_content that prevents recursion"""
    result = _call_safe_function('update_agent_streaming_content', context, text_delta, token_stats)
    return result if result is not None else False


def finish_agent_streaming(context: Dict[str, Any], final_stats: Optional[Dict] = None):
    """Safe wrapper for finish_agent_streaming that prevents recursion"""
    result = _call_safe_function('finish_agent_streaming', context, final_stats)
    return result if result is not None else False


def start_claude_thinking_if_applicable(model_name: str, agent_name: str, counter: int):
    """Safe wrapper for start_claude_thinking_if_applicable that prevents recursion"""
    return _call_safe_function('start_claude_thinking_if_applicable', model_name, agent_name, counter)


def update_claude_thinking_content(context: Dict[str, Any], thinking_delta: str):
    """Safe wrapper for update_claude_thinking_content that prevents recursion"""
    result = _call_safe_function('update_claude_thinking_content', context, thinking_delta)
    return result if result is not None else False


def finish_claude_thinking_display(context: Dict[str, Any]):
    """Safe wrapper for finish_claude_thinking_display that prevents recursion"""
    result = _call_safe_function('finish_claude_thinking_display', context)
    return result if result is not None else False


def detect_claude_thinking_in_stream(model_name: str) -> bool:
    """Safe wrapper for detect_claude_thinking_in_stream"""
    func_name = 'detect_claude_thinking_in_stream'
    
    # This is a simple utility function, no recursion risk
    util_module = sys.modules.get('cai.util')
    if util_module and hasattr(util_module, 'detect_claude_thinking_in_stream'):
        func = getattr(util_module, 'detect_claude_thinking_in_stream')
        return func(model_name)
    else:
        # Default to False if function not found
        return False


def create_claude_thinking_context(agent_name: str, counter: int, model: str):
    """Safe wrapper for create_claude_thinking_context"""
    return _call_safe_function('create_claude_thinking_context', agent_name, counter, model)
