"""
Execution context management for TUI terminals

Thin wrappers delegating to routing.output_router to ensure a single
source of truth for terminal context.
"""

from typing import Optional
from cai.tui.routing.output_router import (
    set_terminal_context as _set_ctx,
    get_current_terminal_id as _get_tid,
    clear_current_terminal_context as _clear_ctx,
)


def set_terminal_id_context(terminal_id: str):
    """Set terminal id using routing's context. Returns None (compat)."""
    _set_ctx(terminal_id)
    return None


def get_terminal_id_context() -> Optional[str]:
    """Get terminal id from routing context."""
    return _get_tid()


def reset_terminal_id_context(token) -> None:  # token ignored for compat
    """Clear terminal context (compat signature)."""
    _clear_ctx()
