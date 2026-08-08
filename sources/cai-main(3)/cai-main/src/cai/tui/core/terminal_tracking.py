"""
Terminal ID tracking for TUI (compat wrappers).

This module provides a stable interface for getting/setting the current
terminal routing context. Historically, some call sites accessed a
module-level ``_thread_local`` object directly. The routing layer has
since moved to ContextVars with a hybrid strategy, so we expose a
backwards-compatible proxy that mirrors the old attributes while
delegating to the single source of truth in ``routing.output_router``.
"""

from typing import Optional

from cai.tui.routing.output_router import (
    set_terminal_context as _set_ctx,
    get_current_terminal_id as _get_tid,
    clear_current_terminal_context as _clear_ctx,
    current_terminal_id as _ctx_tid,
    current_terminal_number as _ctx_tnum,
)


def set_current_terminal_id(terminal_id: str) -> None:
    """Set the current terminal id, leaving number unchanged if present."""
    _set_ctx(terminal_id)


def get_current_terminal_id() -> Optional[str]:
    """Return the current terminal id if set, else None."""
    return _get_tid()


def get_current_terminal_number() -> Optional[int]:
    """Return the current terminal number if set, else None."""
    try:
        return _ctx_tnum.get()
    except Exception:
        return None


def clear_current_terminal_id() -> None:
    """Clear the current terminal context from context variables."""
    _clear_ctx()


class _CompatThreadLocalProxy:
    """Compatibility proxy exposing ``terminal_id`` and ``terminal_number``.

    Old code accessed ``terminal_tracking._thread_local.terminal_number`` to
    discover the active terminal. We now store this in ContextVars; this
    proxy forwards attribute reads to those ContextVars so legacy code
    continues to work without modification.
    """

    def __getattr__(self, name):
        if name == "terminal_id":
            value = _ctx_tid.get()
            if value is None:
                raise AttributeError("terminal_id is not set")
            return value
        if name == "terminal_number":
            value = _ctx_tnum.get()
            if value is None:
                raise AttributeError("terminal_number is not set")
            return value
        raise AttributeError(f"Unknown attribute '{name}' on _CompatThreadLocalProxy")


# Backwards-compat attributes: both with and without underscore
_thread_local = _CompatThreadLocalProxy()
thread_local = _thread_local
