"""Persistent error log for the compact REPL.

Subscribes to the :data:`OUTPUT` bus and writes every :class:`TaskErrorEvent`
to a JSONL file at::

    ~/.cai/sessions/<session_id>/errors.jsonl

The file is created lazily on the first error so successful sessions leave no
artifacts behind. Reuses :class:`FileOutputHandler` for the actual JSON
serialization, keeping a single source of truth.

Decisions applied (see plan):
* q4=c — full tool output is shown only on errors and persisted here for
  post-mortem.
* q24=b — RAM during the turn (``TaskRegistry``) plus this JSONL for errors.
"""

from __future__ import annotations

import threading
import time
import uuid
from pathlib import Path

from cai.output import (
    OUTPUT,
    FileOutputHandler,
    OutputEvent,
    TaskErrorEvent,
)
from cai.util.config_utils import get_config_dir


def _new_session_id() -> str:
    return time.strftime("%Y%m%d-%H%M%S") + "_" + uuid.uuid4().hex[:6]


class ErrorJSONLSink:
    """Persist :class:`TaskErrorEvent` events to a per-session JSONL file."""

    def __init__(self, session_id: str | None = None) -> None:
        self._session_id = session_id or _new_session_id()
        self._handler: FileOutputHandler | None = None
        self._lock = threading.Lock()
        self._path: Path | None = None

    @property
    def session_id(self) -> str:
        return self._session_id

    @property
    def path(self) -> Path:
        if self._path is None:
            self._path = get_config_dir() / "sessions" / self._session_id / "errors.jsonl"
        return self._path

    def handle(self, event: OutputEvent) -> None:
        if not isinstance(event, TaskErrorEvent):
            return
        try:
            self._ensure_open()
            assert self._handler is not None
            self._handler.handle(event)
        except Exception:
            # Never let logging crash the runtime
            pass

    def flush(self) -> None:
        with self._lock:
            if self._handler is not None:
                try:
                    self._handler.flush()
                except Exception:
                    pass

    def close(self) -> None:
        with self._lock:
            if self._handler is not None:
                try:
                    self._handler.close()
                finally:
                    self._handler = None

    def _ensure_open(self) -> None:
        with self._lock:
            if self._handler is not None:
                return
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self._handler = FileOutputHandler(self.path)


# ---------------------------------------------------------------------------
# Module-level singleton wiring
# ---------------------------------------------------------------------------

_sink: ErrorJSONLSink | None = None
_sink_lock = threading.Lock()


def install_error_sink(session_id: str | None = None) -> ErrorJSONLSink:
    """Subscribe a singleton :class:`ErrorJSONLSink` on :data:`OUTPUT`.

    Idempotent: subsequent calls return the same instance and ignore
    ``session_id``.
    """
    global _sink
    with _sink_lock:
        if _sink is not None:
            return _sink
        _sink = ErrorJSONLSink(session_id=session_id)
        OUTPUT.subscribe(_sink)
        return _sink


__all__ = [
    "ErrorJSONLSink",
    "install_error_sink",
]
