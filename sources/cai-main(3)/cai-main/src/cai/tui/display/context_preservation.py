"""
Context preservation utilities for TUI display system

This module ensures that terminal ID and display context are properly
propagated through async execution chains, especially for parallel agents.
"""

import contextvars
import functools
from typing import Any, Callable, Optional, TypeVar, Union
import asyncio
import inspect

from cai.tui.core.execution_context import (
    set_terminal_id_context,
    get_terminal_id_context,
    reset_terminal_id_context
)
from cai.tui.core.terminal_tracking import (
    set_current_terminal_id,
    get_current_terminal_id,
    clear_current_terminal_id
)

# Type variable for generic function signatures
F = TypeVar('F', bound=Callable[..., Any])


def preserve_terminal_context(func: F) -> F:
    """
    Decorator that preserves terminal ID context across async boundaries.
    
    This decorator captures the current terminal ID from both thread-local
    storage and context variables, then ensures it's available in the
    decorated function's execution context.
    """
    @functools.wraps(func)
    async def async_wrapper(*args, **kwargs):
        # Capture current terminal ID from both sources
        terminal_id = get_current_terminal_id() or get_terminal_id_context()
        
        if terminal_id:
            # Set terminal ID in both thread-local and context var
            set_current_terminal_id(terminal_id)
            token = set_terminal_id_context(terminal_id)
            try:
                return await func(*args, **kwargs)
            finally:
                # Clean up
                clear_current_terminal_id()
                reset_terminal_id_context(token)
        else:
            # No terminal ID to preserve
            return await func(*args, **kwargs)
    
    @functools.wraps(func)
    def sync_wrapper(*args, **kwargs):
        # For sync functions, just preserve thread-local storage
        terminal_id = get_current_terminal_id()
        if terminal_id:
            # Terminal ID is already in thread-local storage
            return func(*args, **kwargs)
        else:
            # Try to get from context var
            terminal_id = get_terminal_id_context()
            if terminal_id:
                set_current_terminal_id(terminal_id)
                try:
                    return func(*args, **kwargs)
                finally:
                    clear_current_terminal_id()
            else:
                return func(*args, **kwargs)
    
    if inspect.iscoroutinefunction(func):
        return async_wrapper
    else:
        return sync_wrapper


# Store the original asyncio.create_task before any patching
_original_create_task = asyncio.create_task
_task_context_enabled = False


def create_task_with_context(coro, *, name=None):
    """
    Create an asyncio task that preserves the current terminal context.
    
    This is a drop-in replacement for asyncio.create_task that ensures
    the terminal ID context is available in the created task.
    """
    # Capture current context from both sources
    terminal_id = get_current_terminal_id() or get_terminal_id_context()
    
    # Important: We must set the terminal_id in the context var BEFORE
    # creating the task, so it's included in the context copy
    if terminal_id and not get_terminal_id_context():
        set_terminal_id_context(terminal_id)
    
    async def wrapped_coro():
        if terminal_id:
            # Ensure both thread-local and context var are set
            # This is important because some code checks thread-local first
            set_current_terminal_id(terminal_id)
            # Context var should already be set from parent context
            if not get_terminal_id_context():
                set_terminal_id_context(terminal_id)
            try:
                return await coro
            finally:
                # Clear thread-local to avoid contaminating other tasks
                clear_current_terminal_id()
        else:
            return await coro
    
    # IMPORTANT: Use the original create_task to avoid recursion
    return _original_create_task(wrapped_coro(), name=name)


def enable_task_context_propagation() -> None:
    """Enable global propagation of terminal context for asyncio tasks.

    Replaces asyncio.create_task with a context-aware variant that preserves
    terminal context across task boundaries. Safe to call multiple times.
    """
    global _task_context_enabled
    if _task_context_enabled:
        return
    # Replace create_task with context-preserving version
    asyncio.create_task = create_task_with_context  # type: ignore[assignment]
    _task_context_enabled = True


def run_in_executor_with_context(executor, func, *args):
    """
    Run a function in an executor while preserving terminal context.
    
    This is useful for running synchronous functions that need access
    to the terminal ID context.
    """
    # Capture current context
    terminal_id = get_current_terminal_id() or get_terminal_id_context()
    
    def wrapped_func():
        if terminal_id:
            set_current_terminal_id(terminal_id)
            try:
                return func(*args)
            finally:
                clear_current_terminal_id()
        else:
            return func(*args)
    
    loop = asyncio.get_event_loop()
    return loop.run_in_executor(executor, wrapped_func)


class ContextPreservingRunner:
    """
    A context manager that ensures terminal context is preserved
    for all async operations within its scope.
    """
    
    def __init__(self, terminal_id: Optional[str] = None):
        self.terminal_id = terminal_id
        self.token = None
        self.had_thread_local = False
        
    def __enter__(self):
        # Use provided terminal ID or try to get current one
        if not self.terminal_id:
            self.terminal_id = get_current_terminal_id() or get_terminal_id_context()
        
        if self.terminal_id:
            # Check if we already have it in thread-local
            self.had_thread_local = get_current_terminal_id() is not None
            
            # Set in both places
            if not self.had_thread_local:
                set_current_terminal_id(self.terminal_id)
            self.token = set_terminal_id_context(self.terminal_id)
        
        return self
    
    def __exit__(self, exc_type, exc_val, exc_tb):
        if self.terminal_id:
            # Clean up what we set
            if not self.had_thread_local:
                clear_current_terminal_id()
            if self.token:
                reset_terminal_id_context(self.token)
    
    async def __aenter__(self):
        return self.__enter__()
    
    async def __aexit__(self, exc_type, exc_val, exc_tb):
        return self.__exit__(exc_type, exc_val, exc_tb)


def get_terminal_id_from_context() -> Optional[str]:
    """
    Get terminal ID from any available source.
    
    Checks both thread-local storage and context variables.
    """
    return get_current_terminal_id() or get_terminal_id_context()


def ensure_terminal_context(terminal_id: str) -> Callable:
    """
    Decorator factory that ensures a specific terminal ID is set
    for the decorated function.
    """
    def decorator(func: F) -> F:
        @functools.wraps(func)
        async def async_wrapper(*args, **kwargs):
            set_current_terminal_id(terminal_id)
            token = set_terminal_id_context(terminal_id)
            try:
                return await func(*args, **kwargs)
            finally:
                clear_current_terminal_id()
                reset_terminal_id_context(token)
        
        @functools.wraps(func)
        def sync_wrapper(*args, **kwargs):
            set_current_terminal_id(terminal_id)
            try:
                return func(*args, **kwargs)
            finally:
                clear_current_terminal_id()
        
        if inspect.iscoroutinefunction(func):
            return async_wrapper
        else:
            return sync_wrapper
    
    return decorator
