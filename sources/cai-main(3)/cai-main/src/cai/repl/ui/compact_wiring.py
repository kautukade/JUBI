"""Compact REPL wiring: subscribers + turn lifecycle helpers.

Glue layer that the CLI entrypoint uses to bring up the compact UI without
duplicating subscription logic across modules.

Responsibilities
----------------
* :func:`install_compact_ui` — subscribe :class:`CompactCLIHandler` and
  :class:`ErrorJSONLSink` on the global :data:`OUTPUT` bus. Idempotent.
* :func:`is_compact_enabled` — single source of truth for the compact toggle
  (``CAI_COMPACT_REPL`` env var, default ``true`` per q3=b).
* :func:`emit_turn_start` / :func:`emit_turn_summary` — publish
  ``Turn*Event``s and rotate :data:`TASK_REGISTRY`. Designed to wrap a single
  agent turn from the REPL loop.
* :func:`turn_lifecycle` — context manager combining the two helpers; ensures
  the summary is emitted even when the agent run raises.

Excluded: TUI mode (``CAI_TUI_MODE=1``) is detected by
:class:`CompactCLIHandler` itself, so no extra check is required here.
"""

from __future__ import annotations

import contextlib
import os
import threading
import uuid
from typing import Iterator

from cai.output import (
    OUTPUT,
    TASK_REGISTRY,
    TurnStartEvent,
    TurnSummaryEvent,
)


_install_lock = threading.Lock()
_installed = False

# Cached decision: the compact flag locks at the first call (typically the
# ``install_compact_ui()`` check in ``cli.py``). Runtime env mutations (e.g.
# ``/env CAI_COMPACT_REPL=...``) are intentionally ignored — the wiring of
# ``OUTPUT`` subscribers is fixed at startup, so the gate must match. Matches
# the documented behavior: "Restart CAI for the change to take effect."
_compact_cache_lock = threading.Lock()
_compact_enabled_cached: bool | None = None


def _reset_compact_enabled_cache_for_tests() -> None:
    """Clear the cached decision; pytest fixtures only."""
    global _compact_enabled_cached
    with _compact_cache_lock:
        _compact_enabled_cached = None


def is_compact_enabled() -> bool:
    """Return ``True`` when the compact REPL UI should be active.

    Honours ``CAI_COMPACT_REPL`` (``1``/``true``/``yes``/``on`` to enable,
    anything else to disable) read **once** at first invocation. Defaults to
    ``True``. Always ``False`` when ``CAI_TUI_MODE`` is set — the TUI owns its
    own rendering pipeline. The value is cached because the subscriber wiring
    on :data:`OUTPUT` is fixed at startup; flipping the env var later would
    desync the runtime gate from the subscription state and produce either a
    ghost compact ``Live`` block (true→false) or a silent black hole
    (false→true) — see ``/env`` warning in the env catalog.
    """
    global _compact_enabled_cached
    if _compact_enabled_cached is not None:
        return _compact_enabled_cached
    with _compact_cache_lock:
        if _compact_enabled_cached is not None:
            return _compact_enabled_cached
        if os.getenv("CAI_TUI_MODE", "").strip().lower() in ("1", "true", "yes"):
            _compact_enabled_cached = False
        else:
            raw = os.getenv("CAI_COMPACT_REPL", "1").strip().lower()
            _compact_enabled_cached = raw in ("1", "true", "yes", "on")
        return _compact_enabled_cached


def install_compact_ui() -> None:
    """Subscribe compact handlers on :data:`OUTPUT`. Idempotent.

    Safe to call multiple times; the second call is a no-op. Skipped entirely
    when :func:`is_compact_enabled` is ``False``.
    """
    global _installed
    if not is_compact_enabled():
        return
    with _install_lock:
        if _installed:
            return
        # Imported lazily so the wiring module can be referenced from non-CLI
        # code paths without pulling Rich/prompt_toolkit transitively.
        from cai.repl.ui.compact_renderer import install_compact_handler
        from cai.repl.ui.error_sink import install_error_sink

        install_compact_handler()
        install_error_sink()
        _installed = True


def emit_turn_start(user_input: str = "") -> str:
    """Begin a new compact turn and emit :class:`TurnStartEvent`.

    Returns the freshly minted ``turn_id``. The caller is expected to pair
    this with :func:`emit_turn_summary` (or use :func:`turn_lifecycle`).
    """
    turn_id = uuid.uuid4().hex[:12]
    TASK_REGISTRY.begin_turn(turn_id)
    if is_compact_enabled():
        OUTPUT.emit(TurnStartEvent(turn_id=turn_id, user_input=user_input or ""))
    return turn_id


def emit_turn_summary(turn_id: str) -> None:
    """Emit :class:`TurnSummaryEvent` for ``turn_id``.

    No-op when compact mode is disabled. Safely handles unknown turn ids by
    emitting an event with an empty task list.
    """
    if not is_compact_enabled() or not turn_id:
        return
    tasks = [r.as_dict() for r in TASK_REGISTRY.for_turn(turn_id)]
    OUTPUT.emit(TurnSummaryEvent(turn_id=turn_id, tasks=tasks))


@contextlib.contextmanager
def turn_lifecycle(user_input: str = "") -> Iterator[str]:
    """Context manager wrapping a turn with start/summary events.

    Always emits :class:`TurnSummaryEvent` on exit, even on exception, so the
    transient :class:`Live` block collapses cleanly between turns. (No summary
    table or session footer is rendered — see ``compact_renderer._on_turn_summary``.)
    """
    turn_id = emit_turn_start(user_input)
    try:
        yield turn_id
    finally:
        emit_turn_summary(turn_id)


__all__ = [
    "emit_turn_start",
    "emit_turn_summary",
    "install_compact_ui",
    "is_compact_enabled",
    "turn_lifecycle",
]
