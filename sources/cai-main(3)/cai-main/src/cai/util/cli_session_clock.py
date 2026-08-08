"""Wall-clock anchor for CLI session timing (tool panels, summaries).

``cli_headless`` must not be imported only to read ``START_TIME`` — that module
used to call ``set_tracing_disabled(True)`` at import time, which broke tracing
in unrelated code (e.g. unit tests that format tool output).

Call :func:`reset_session_clock` from headless (and TUI) entry points.
"""

from __future__ import annotations

import time

START_TIME: float | None = None


def reset_session_clock() -> float:
    """Start a new session wall anchor; returns the new anchor (epoch seconds)."""
    global START_TIME
    START_TIME = time.time()
    return START_TIME
