"""
Utility to update info status bars from external sources like openai_chatcompletions
"""

import os
from typing import Any, Dict
from weakref import WeakSet

# Global registry of active info status bars
_ACTIVE_INFO_BARS: WeakSet = WeakSet()


def register_info_bar(info_bar):
    """Register an info status bar for updates"""
    _ACTIVE_INFO_BARS.add(info_bar)


def unregister_info_bar(info_bar):
    """Unregister an info status bar"""
    _ACTIVE_INFO_BARS.discard(info_bar)


def update_all_info_bars(usage_data: Dict[str, Any]) -> None:
    """
    Update all active info status bars with usage data.
    This is called from openai_chatcompletions when token usage changes.
    
    Args:
        usage_data: Dictionary containing:
            - input_tokens: int
            - output_tokens: int
            - reasoning_tokens: int (optional)
            - total_cost: float
            - interaction_cost: float
            - context_usage: float (0.0 to 1.0)
            - model_name: str (optional)
    """
    # Check if we're in TUI mode
    if os.environ.get("CAI_TUI_MODE") != "true":
        return

    # Update all registered info bars
    for info_bar in list(_ACTIVE_INFO_BARS):
        try:
            # Use call_soon_threadsafe if we're in a different thread
            if hasattr(info_bar.app, 'call_from_thread'):
                info_bar.app.call_from_thread(info_bar.update_from_usage, usage_data)
            else:
                # Direct update if in same thread
                info_bar.update_from_usage(usage_data)
        except Exception:
            # Info bar might have been destroyed
            _ACTIVE_INFO_BARS.discard(info_bar)


def update_context_usage(context_usage: float) -> None:
    """
    Update only the context usage in all info bars.
    
    Args:
        context_usage: Context usage percentage (0.0 to 1.0)
    """
    update_all_info_bars({"context_usage": context_usage})


def update_token_usage(input_tokens: int, output_tokens: int, reasoning_tokens: int = 0) -> None:
    """
    Update token counts in all info bars.
    
    Args:
        input_tokens: Number of input tokens
        output_tokens: Number of output tokens
        reasoning_tokens: Number of reasoning tokens (optional)
    """
    update_all_info_bars({
        "input_tokens": input_tokens,
        "output_tokens": output_tokens,
        "reasoning_tokens": reasoning_tokens
    })


def update_cost_info(total_cost: float, interaction_cost: float = 0.0) -> None:
    """
    Update cost information in all info bars.
    
    Args:
        total_cost: Total session cost
        interaction_cost: Current interaction cost
    """
    update_all_info_bars({
        "total_cost": total_cost,
        "interaction_cost": interaction_cost
    })


def force_refresh_all_info_bars() -> None:
    """
    Force an immediate refresh of all info bars.
    This is useful when state changes occur (like switching between active/idle).
    """
    # Check if we're in TUI mode
    if os.environ.get("CAI_TUI_MODE") != "true":
        return

    # Force update all registered info bars
    for info_bar in list(_ACTIVE_INFO_BARS):
        try:
            # Use call_soon_threadsafe if we're in a different thread
            if hasattr(info_bar.app, 'call_from_thread'):
                info_bar.app.call_from_thread(info_bar._update_info)
            else:
                # Direct update if in same thread
                info_bar._update_info()
        except Exception:
            # Info bar might have been destroyed
            _ACTIVE_INFO_BARS.discard(info_bar)

