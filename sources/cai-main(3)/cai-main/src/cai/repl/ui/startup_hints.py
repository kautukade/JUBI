"""Optional startup status lines for the headless CLI (Rich spinner + dim text).

Phases follow real work (license, updates, agent wiring) instead of fixed timers.
Disable with CAI_NO_STARTUP_HINTS=1 for CI or scripts.

Minimum time each message stays visible (so fast phases are readable):
``CAI_STARTUP_HINT_MIN_SEC`` (default ``0.55``). Set ``0`` to disable pacing.
"""

from __future__ import annotations

import os
import time
from typing import Optional

from rich.console import Console
from rich.status import Status

from cai.util.hint_renderables import build_startup_hint_renderable


def startup_hints_disabled() -> bool:
    v = os.environ.get("CAI_NO_STARTUP_HINTS", "").strip().lower()
    return v in ("1", "true", "yes", "on")


def mask_key_for_hint(raw: str) -> str:
    raw = (raw or "").strip()
    if not raw:
        return "not set"
    if len(raw) <= 10:
        return "***"
    return f"{raw[:4]}…{raw[-4:]}"


class StartupHints:
    """Idempotent Rich status line: ``[CAI] | message`` with dim spinner."""

    def __init__(self, console: Optional[Console] = None):
        self._console = console or Console()
        self._cm: Optional[Status] = None
        self._phase_started: Optional[float] = None

    def _min_visible_phase(self) -> float:
        if startup_hints_disabled():
            return 0.0
        try:
            return float(os.environ.get("CAI_STARTUP_HINT_MIN_SEC", "0.55"))
        except ValueError:
            return 0.55

    def _sleep_phase_minimum(self) -> None:
        if self._cm is None or self._phase_started is None:
            return
        min_s = self._min_visible_phase()
        if min_s <= 0:
            return
        elapsed = time.monotonic() - self._phase_started
        remaining = min_s - elapsed
        if remaining > 0:
            time.sleep(remaining)

    def _mark_phase(self) -> None:
        self._phase_started = time.monotonic()

    def _render(self, message: str):
        return build_startup_hint_renderable(message)

    def start(self, message: str = "Starting framework...", *, leading_blank: bool = True) -> None:
        if startup_hints_disabled() or self._cm is not None:
            return
        if leading_blank:
            self._console.print()
        self._cm = self._console.status(
            self._render(message),
            spinner="dots",
            spinner_style="dim",
            refresh_per_second=8,
        )
        self._cm.__enter__()
        self._mark_phase()

    def update(self, message: str) -> None:
        if self._cm is None:
            return
        self._sleep_phase_minimum()
        self._cm.update(self._render(message))
        self._mark_phase()

    def set_message(
        self, message: str, *, leading_blank_if_start: bool = True
    ) -> None:
        """Show ``message``, starting the status line again if it was stopped."""
        if startup_hints_disabled():
            return
        if self._cm is not None:
            self.update(message)
        else:
            self.start(message, leading_blank=leading_blank_if_start)

    def stop(self, trailing_blank: bool = False) -> None:
        if self._cm is None:
            return
        self._sleep_phase_minimum()
        try:
            self._cm.__exit__(None, None, None)
        finally:
            self._cm = None
            self._phase_started = None
        if trailing_blank:
            self._console.print()
