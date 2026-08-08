"""
Interaction counter and limit enforcement for CAI sessions.
"""

import asyncio
import os
import sys
import time

from cai.util.timing import stop_active_timer, start_idle_timer

# ======================== GLOBAL INTERACTION COUNTER ========================
_interaction_counter = 0


def reset_interaction_counter():
    global _interaction_counter
    _interaction_counter = 0


def increment_interaction_counter():
    global _interaction_counter
    _interaction_counter += 1
    return _interaction_counter


def get_interaction_counter():
    return _interaction_counter


class MaxInteractionsExceeded(Exception):
    def __init__(self, current, limit):
        super().__init__(f"Maximum interaction limit ({limit}) reached: {current}")
        self.current = current
        self.limit = limit


def check_interaction_limit(force_until_flag=False):
    import os
    limit_env = os.getenv("CAI_MAX_INTERACTIONS")
    try:
        max_interactions = float(limit_env) if limit_env is not None else float("inf")
    except ValueError:
        max_interactions = float("inf")
    current = get_interaction_counter()
    if max_interactions != float("inf") and current >= max_interactions:
        raise MaxInteractionsExceeded(current, max_interactions)


# Track consecutive Ctrl+C presses for force exit
_interrupt_count = 0
_last_interrupt_time = 0

# Set by signal_handler (MainThread) to ask any blocking user prompt running
# in an executor thread (e.g. sudo password getpass in asyncio_0) to abort
# at the next opportunity. Read/cleared by cai.util.user_prompts.
_PROMPT_ABORT_REQUESTED = False


def is_prompt_abort_requested() -> bool:
    """Return True if SIGINT was raised since the last reset."""
    return _PROMPT_ABORT_REQUESTED


def clear_prompt_abort_request() -> None:
    """Reset the abort flag (called when a new prompt block starts)."""
    global _PROMPT_ABORT_REQUESTED
    _PROMPT_ABORT_REQUESTED = False


def signal_handler(signum, frame):
    """
    Handle interrupt signals (CTRL+C) gracefully.
    First Ctrl+C: Clean interrupt with KeyboardInterrupt
    Second Ctrl+C (within 2 seconds): Force exit
    """
    global _interrupt_count, _last_interrupt_time, _PROMPT_ABORT_REQUESTED
    # Signal any blocking user prompt running in an executor thread (e.g.
    # the sudo password reader in asyncio_0) to abort at its next poll.
    _PROMPT_ABORT_REQUESTED = True
    # Import here to avoid circular import at module level
    from cai.util.streaming import (
        cleanup_all_streaming_resources,
        _force_stop_all_panels,
    )

    current_time = time.time()

    # Reset counter if more than 2 seconds since last interrupt
    if current_time - _last_interrupt_time > 2.0:
        _interrupt_count = 0

    _interrupt_count += 1
    _last_interrupt_time = current_time

    # On second Ctrl+C, force exit immediately
    if _interrupt_count >= 2:
        # Force restore cursor and clear any Rich rendering state
        try:
            print("\033[?25h", end="", file=sys.stderr)  # Show cursor
            sys.stderr.flush()
        except Exception:
            pass
        print("\n\nForce exiting...")
        # Force stop all panels immediately
        _force_stop_all_panels()
        # Cancel all pending asyncio tasks before exiting
        try:
            loop = asyncio.get_event_loop()
            if loop and not loop.is_closed():
                pending = asyncio.all_tasks(loop) if hasattr(asyncio, 'all_tasks') else asyncio.Task.all_tasks(loop)
                for task in pending:
                    task.cancel()
        except Exception:
            pass
        sys.exit(0)

    # Print newline to break out of any inline output
    try:
        print("", file=sys.stderr)
        sys.stderr.flush()
    except Exception:
        pass

    # Stop any active timers
    try:
        stop_active_timer()
        start_idle_timer()
    except Exception:
        pass

    # Cancel any pending asyncio tasks on first Ctrl+C
    try:
        loop = asyncio.get_event_loop()
        if loop and not loop.is_closed():
            pending = asyncio.all_tasks(loop) if hasattr(asyncio, 'all_tasks') else asyncio.Task.all_tasks(loop)
            for task in pending:
                if not task.done():
                    task.cancel()
    except Exception:
        pass

    # Stop Live panels without stacking extra newlines (cleanup also runs from the REPL handler).
    cleanup_all_streaming_resources(leave_alternate_screen=False)

    # Re-raise KeyboardInterrupt to allow normal interrupt handling
    raise KeyboardInterrupt()
