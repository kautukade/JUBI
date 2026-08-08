"""Session-level helpers shared across CLI, REPL, TUI and SDK layers.

Centralises the few "is the current session running in mode X?" checks that
were previously open-coded against environment variables in many files.
Single source of truth makes the runtime contract explicit and removes the
small but real risk of two sites drifting on whitespace, default values, or
exception handling around ``int(os.getenv(...))``.
"""

from __future__ import annotations

import os


def is_parallel_session() -> bool:
    """Return ``True`` when the session runs more than one agent in parallel.

    Mirrors the convention previously inlined in :mod:`cai.util.streaming`,
    :mod:`cai.repl.ui.compact_renderer`, :mod:`cai.util.terminal`, and
    :mod:`cai.util.pricing`. Honours ``CAI_PARALLEL`` and tolerates a missing
    or non-integer value (treated as single-agent).
    """
    try:
        return int(os.getenv("CAI_PARALLEL", "1")) > 1
    except (TypeError, ValueError):
        return False


__all__ = ["is_parallel_session"]
