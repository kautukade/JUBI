"""
Terminal ID tracking for TUI with async context support
"""

import contextvars
from typing import Optional

# Context variable for terminal ID that propagates through async calls
_terminal_id_context: contextvars.ContextVar[Optional[str]] = contextvars.ContextVar(
    'terminal_id',
    default=None
)


def set_current_terminal_id_async(terminal_id: str) -> contextvars.Token:
    """Set the current terminal ID for this async context"""
    return _terminal_id_context.set(terminal_id)


def get_current_terminal_id_async() -> Optional[str]:
    """Get the current terminal ID for this async context"""
    return _terminal_id_context.get()


def reset_current_terminal_id_async(token: contextvars.Token) -> None:
    """Reset the terminal ID context to previous value"""
    _terminal_id_context.reset(token)


def clear_current_terminal_id_async() -> None:
    """Clear the current terminal ID for this async context"""
    _terminal_id_context.set(None)