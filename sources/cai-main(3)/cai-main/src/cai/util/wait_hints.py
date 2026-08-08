"""Terminal wait hints: Rich status on stderr (model) or footer renderable (tools).

Fixed schedules and animations (no CAI_WAIT_HINT_* tuning). Disabled when stderr is not
a TTY or in TUI mode. Retry backoffs overlay the model hint or use a standalone line.
"""

from __future__ import annotations

import asyncio
import json
import os
import random
import sys
import threading
import time
from collections.abc import Iterable
from contextlib import asynccontextmanager
from typing import Any, List

from rich.console import Console, RenderableType
from rich.status import Status

from cai.util.hint_renderables import build_wait_hint_renderable, pipe_frame

# --- Message pools (model long-wait; shared with retry trailer) -----------------

MODEL_DELAY_MESSAGES: tuple[str, ...] = (
    "The service looks busy right now—thanks for waiting.",
    "Still no reply; large context or tools often slow things down.",
    "Taking longer than usual—the provider may be under load.",
    "Hang tight; we're still waiting on the model.",
    "Slower than expected—cold starts and big prompts add delay.",
    "Nothing wrong on your side yet; the request is still in flight.",
    "Queue or capacity upstream may be causing the delay.",
    "Patience appreciated—some runs need more than a minute.",
    "If this repeats often, check provider status or your plan limits.",
    "We haven't timed out—we'll keep waiting until we get an answer.",
)

TOOL_DELAY_MESSAGES: tuple[str, ...] = (
    "The tool is still running—large output or slow I/O can take a while.",
    "Waiting on the subprocess; disk or network may be the bottleneck.",
    "No tool result yet—commands can queue behind heavier work on the host.",
    "Still executing; downloads or streaming output extend this phase.",
    "The machine may be busy; the tool hasn't returned yet.",
    "Long shell jobs stay here until they exit cleanly.",
    "Heavy filesystem work can keep this visible for quite some time.",
    "Remote calls inside the tool may be waiting on another service.",
    "If this hangs, the command might be blocked on input (not supported).",
    "We're still collecting stdout/stderr from the tool process.",
)

RETRY_TRAILER = (
    "Heavy context or API load may be slowing the response. "
    "If this persists, contact support and consider upgrading your plan."
)

_PIPE_SPIN_INTERVAL = 0.25

# Tool waits: Rich renderable in pricing footer (stdout/Live).
_TOOL_FOOTER_LOCK = threading.Lock()
_tool_wait_footer_renderable: RenderableType | None = None
# Plain tool-wait message (same string as in the footer line body) for in-panel Layout 1.
_tool_wait_body_plain: str | None = None
_last_footer_ui_refresh = 0.0

# Retry overlay while model stderr hint is active
_retry_overlay_message: str | None = None
_active_stderr_wait_loops = 0

# Active wait-hint loops (for cooperative pause from interactive prompts).
_active_loops_lock = threading.Lock()
_active_loops: list["_WaitHintLoop"] = []

# Per-mode "primary writer" — only the first loop of each mode owns the shared
# ``_current_*_body`` globals (and the Rich Status, for ``mode='model'``).
# Concurrent loops (e.g. orchestration spawns several specialist Runners that
# each open their own ``model_wait_hints`` / ``tool_batch_wait_hints``) become
# **passive**: they exist for pause/resume coordination but never write to the
# globals nor spin a Status. This avoids the flicker the compact renderer
# otherwise shows when several loops with different ``elapsed`` values race on
# the same globals at 4 Hz while the renderer reads at 8 Hz.
_PRIMARY_LOCK = threading.Lock()
_primary_model_loop: "_WaitHintLoop | None" = None
_primary_tool_loop: "_WaitHintLoop | None" = None

# Latest wait-hint body strings (mirror of what would be rendered into
# Rich Status / footer). Read by the compact REPL renderer to draw the wait
# hint as the LAST row of the live block when it owns the screen.
_BODY_LOCK = threading.Lock()
_current_model_body: str | None = None
_current_tool_body: str | None = None
# True when the compact REPL live block is rendering. While set:
#   * mode="model" loops do NOT spawn a Rich Status (compact draws the body)
#   * mode="tool" loops publish only the plain body; legacy footer refreshes
#     are suppressed so the compact renderer is the only terminal surface.
_compact_live_owner = False


def _set_model_wait_body(body: str | None) -> None:
    global _current_model_body
    with _BODY_LOCK:
        _current_model_body = body


def _set_tool_wait_body(body: str | None) -> None:
    global _current_tool_body
    with _BODY_LOCK:
        _current_tool_body = body


def get_current_wait_hint_body() -> str | None:
    """Return the combined wait-hint body the compact renderer should draw.

    Returns ``None`` when nothing is being awaited. When both the model and a
    tool batch are active, both lines are concatenated with a separator.
    """
    with _BODY_LOCK:
        parts = [p for p in (_current_model_body, _current_tool_body) if p]
    if not parts:
        return None
    return "  ·  ".join(parts)


def set_compact_live_owner(active: bool) -> None:
    """Inform the wait-hint subsystem that the compact REPL live block is active.

    While ``active=True``:
      * existing model-mode Rich Status objects are torn down (force_pause)
        so the compact renderer is the only Rich Live on the console;
      * model-mode loops will skip Status creation on subsequent ticks.

    On ``active=False``, paused loops are resumed; the next tick re-spawns
    their Rich Status.
    """
    global _compact_live_owner
    _compact_live_owner = active
    with _active_loops_lock:
        loops = list(_active_loops)
    for loop in loops:
        try:
            if active:
                loop.force_pause()
            else:
                loop.force_resume()
        except Exception:
            pass


def get_tool_wait_footer_renderable() -> RenderableType | None:
    with _TOOL_FOOTER_LOCK:
        return _tool_wait_footer_renderable


def pause_all_wait_hints() -> None:
    """Synchronously pause every active wait hint loop.

    Called by interactive prompts (e.g. :func:`prompt_user_for_sensitive_command`)
    to yield the terminal. Idempotent. Pair with :func:`resume_all_wait_hints`.
    """
    with _active_loops_lock:
        loops = list(_active_loops)
    for loop in loops:
        try:
            loop.force_pause()
        except Exception:
            pass


def resume_all_wait_hints() -> None:
    """Resume every loop previously paused by :func:`pause_all_wait_hints`."""
    with _active_loops_lock:
        loops = list(_active_loops)
    for loop in loops:
        try:
            loop.force_resume()
        except Exception:
            pass


def get_tool_wait_body_plain() -> str | None:
    """Human tool-wait line (no CAI badge), for Result/captured rail in Layout 1."""
    with _TOOL_FOOTER_LOCK:
        return _tool_wait_body_plain


def _set_tool_wait_footer_renderable(
    r: RenderableType | None, *, body_plain: str | None = None
) -> None:
    global _tool_wait_footer_renderable, _tool_wait_body_plain
    with _TOOL_FOOTER_LOCK:
        _tool_wait_footer_renderable = r
        if r is None:
            _tool_wait_body_plain = None
        else:
            _tool_wait_body_plain = body_plain


def _request_footer_ui_refresh() -> None:
    global _last_footer_ui_refresh
    if _compact_live_owner:
        return
    now = time.monotonic()
    if now - _last_footer_ui_refresh < 0.12:
        return
    _last_footer_ui_refresh = now
    try:
        from cai.util.streaming import refresh_tool_wait_displays

        refresh_tool_wait_displays()
    except Exception:
        pass


def _compact_cli_owns_wait_hints() -> bool:
    """True when the compact REPL is the sole terminal surface for wait hints.

    Delegates to the single source of truth :func:`is_compact_enabled` (cached
    at startup) so the wait-hint gate and the subscriber wiring stay in sync.
    """
    from cai.repl.ui.compact_wiring import is_compact_enabled

    return is_compact_enabled()


def wait_hints_enabled() -> bool:
    if os.getenv("CAI_TUI_MODE", "").strip().lower() in ("true", "1", "yes"):
        return False
    try:
        return sys.stderr.isatty()
    except Exception:
        return False


def tool_wait_hints_enabled() -> bool:
    """Whether to run tool-batch wait hints (updates Result-rail plain text + optional footer).

    Broader than ``wait_hints_enabled()`` (stderr-only): many environments attach a TTY
    only to stdout (e.g. some IDE terminals), so we accept either stream.
    """
    if os.getenv("CAI_TUI_MODE", "").strip().lower() in ("true", "1", "yes"):
        return False
    if os.getenv("CAI_DISABLE_TOOL_WAIT_HINTS", "").strip().lower() in (
        "1",
        "true",
        "yes",
    ):
        return False
    try:
        return sys.stdout.isatty() or sys.stderr.isatty()
    except Exception:
        return False


def summarize_tool_arguments(raw: str | None, max_len: int = 100) -> str:
    if raw is not None and not isinstance(raw, str):
        raw = str(raw)
    raw = (raw or "").strip()
    try:
        data = json.loads(raw) if raw else {}
        if isinstance(data, dict):
            for key in ("command", "cmd", "query", "path", "url"):
                val = data.get(key)
                if val is not None and str(val).strip():
                    s = str(val).replace("\n", " ").strip()
                    if len(s) > max_len:
                        return s[: max_len - 1] + "…"
                    return s
    except Exception:
        pass
    s = raw.replace("\n", " ").strip()
    if len(s) > max_len:
        return s[: max_len - 1] + "…"
    return s or "(args)"


def _tool_batch_label_and_summary(
    names: List[str], summaries: List[str]
) -> tuple[str, str]:
    if not names:
        return "tools", "…"
    if len(names) == 1:
        return names[0], summaries[0] if summaries else "…"
    joined = ", ".join(names[:3])
    if len(names) > 3:
        joined += f" (+{len(names) - 3} more)"
    summary = summaries[0] if summaries else joined
    return joined, summary


def _model_body(elapsed: float, state: dict[str, Any]) -> str:
    overlay = _retry_overlay_message
    if overlay:
        return overlay
    if elapsed >= 90.0:
        phase = int((elapsed - 90.0) // 30.0)
        if state.get("m_phase") != phase:
            state["m_phase"] = phase
            state["m_pick"] = random.choice(MODEL_DELAY_MESSAGES)
        return state["m_pick"]
    if elapsed >= 60.0:
        return "Planning the next move…"
    if elapsed >= 30.0:
        return "Analyzing and optimizing information…"
    if elapsed >= 10.0:
        return "Gathering context and reviewing sources…"
    if elapsed >= 5.0:
        return "Thinking…"
    return "Preparing context and calling the model"


def _tool_body(
    elapsed: float, tool_label: str, exec_summary: str, state: dict[str, Any]
) -> str:
    if elapsed >= 120.0:
        phase = int((elapsed - 120.0) // 30.0)
        if state.get("t_phase") != phase:
            state["t_phase"] = phase
            state["t_pick"] = random.choice(TOOL_DELAY_MESSAGES)
        return state["t_pick"]
    if elapsed >= 90.0:
        return "Waiting for the tool; the model will process this right after."
    if elapsed >= 60.0:
        return "Waiting for tool output before sending it to the model."
    if elapsed >= 30.0:
        return "This command is heavy; it may need more time…"
    if elapsed >= 10.0:
        return "Processing the command—please wait."
    if elapsed >= 3.0:
        return f"Executing: {exec_summary}"
    return f"Preparing tool {tool_label} for execution"


class _WaitHintLoop:
    """Rich Status on stderr (model) or footer renderable updates (tools)."""

    def __init__(self, *, mode: str, tool_label: str = "", exec_summary: str = "") -> None:
        self._mode = mode  # "model" | "tool"
        self._tool_label = tool_label
        self._exec_summary = exec_summary
        self._task: asyncio.Task[None] | None = None
        self._stopped = asyncio.Event()
        self._disposed = False
        self._state: dict[str, Any] = {}
        self._status: Status | None = None
        self._console: Console | None = None
        # External (synchronous) pause: when set, the run loop yields the
        # terminal until the pause flag is cleared. Used by interactive prompts
        # (e.g. the sensitive command questionary) so nothing fights the
        # questionary for stderr/stdout.
        self._externally_paused = False
        # When ``True`` this loop won't write to the shared body globals nor
        # spawn a Rich Status. Set by ``start()`` if another loop of the same
        # mode is already the primary writer (see ``_primary_*_loop`` above).
        self._is_passive = False

    def _claim_primary(self) -> bool:
        """Try to register as the primary writer for this mode.

        Returns ``True`` if this loop is now primary, ``False`` if another
        loop already holds the slot and this one must run passively.
        """
        global _primary_model_loop, _primary_tool_loop
        with _PRIMARY_LOCK:
            if self._mode == "model":
                if _primary_model_loop is None:
                    _primary_model_loop = self
                    return True
                return False
            if self._mode == "tool":
                if _primary_tool_loop is None:
                    _primary_tool_loop = self
                    return True
                return False
        return False

    def _release_primary(self) -> None:
        """Release the per-mode primary slot if this loop owned it."""
        global _primary_model_loop, _primary_tool_loop
        with _PRIMARY_LOCK:
            if _primary_model_loop is self:
                _primary_model_loop = None
            if _primary_tool_loop is self:
                _primary_tool_loop = None

    async def start(self) -> None:
        global _active_stderr_wait_loops
        if self._disposed:
            return
        if self._mode == "tool":
            if not tool_wait_hints_enabled():
                return
        elif not wait_hints_enabled():
            return

        # Concurrency-safe per-mode primary registration: only one loop per
        # mode writes to the shared globals at any time. Secondary loops still
        # join ``_active_loops`` so pause/resume from interactive prompts can
        # reach them, but they do not touch ``_current_*_body`` nor spawn a
        # Rich ``Status``.
        primary = self._claim_primary()
        self._is_passive = not primary

        with _active_loops_lock:
            if self not in _active_loops:
                _active_loops.append(self)

        if self._is_passive:
            return

        if self._mode == "model":
            _active_stderr_wait_loops += 1
            try:
                self._console = Console(stderr=True, soft_wrap=False)
                # Always publish the initial body so the compact renderer can
                # show it immediately even before the first tick.
                _set_model_wait_body(_model_body(0.0, self._state))
                # Only spawn a Rich Status when the compact REPL is NOT active.
                # Before the first TaskStartEvent, ``_compact_live_owner`` is still
                # false but we must not paint a second Live on stderr.
                if not (_compact_live_owner or _compact_cli_owns_wait_hints()):
                    initial = build_wait_hint_renderable(
                        _model_body(0.0, self._state),
                        pipe_frame(0),
                        include_suffix=True,
                    )
                    self._status = Status(
                        initial,
                        console=self._console,
                        spinner="dots",
                        spinner_style="dim",
                        refresh_per_second=8,
                    )
                    self._status.start()
                else:
                    self._externally_paused = True
            except Exception:
                _active_stderr_wait_loops -= 1
                raise
        elif self._mode == "tool":
            body0 = _tool_body(0.0, self._tool_label, self._exec_summary, self._state)
            _set_tool_wait_body(body0)
            if _compact_live_owner or _compact_cli_owns_wait_hints():
                self._externally_paused = True
            else:
                r0 = build_wait_hint_renderable(
                    body0, pipe_frame(0), include_suffix=True
                )
                _set_tool_wait_footer_renderable(r0, body_plain=body0)
                _request_footer_ui_refresh()

        async def _run() -> None:
            # ``-=`` in ``finally`` requires ``global`` or Python treats the name as local
            # and raises UnboundLocalError when the task is cancelled (e.g. Ctrl+C).
            global _active_stderr_wait_loops
            start = time.monotonic()
            tick = 0
            try:
                while not self._stopped.is_set():
                    elapsed = time.monotonic() - start
                    if self._mode == "model":
                        body = _model_body(elapsed, self._state)
                        # Always publish the body so the compact REPL can draw
                        # it even while we are externally paused.
                        _set_model_wait_body(body)
                        if not self._externally_paused and self._status is not None:
                            self._status.update(
                                build_wait_hint_renderable(
                                    body, pipe_frame(tick), include_suffix=True
                                )
                            )
                    else:
                        body = _tool_body(
                            elapsed, self._tool_label, self._exec_summary, self._state
                        )
                        _set_tool_wait_body(body)
                        if not self._externally_paused and not (
                            _compact_live_owner or _compact_cli_owns_wait_hints()
                        ):
                            r = build_wait_hint_renderable(
                                body, pipe_frame(tick), include_suffix=True
                            )
                            _set_tool_wait_footer_renderable(r, body_plain=body)
                            _request_footer_ui_refresh()
                    tick += 1
                    try:
                        await asyncio.wait_for(
                            self._stopped.wait(), timeout=_PIPE_SPIN_INTERVAL
                        )
                        break
                    except asyncio.TimeoutError:
                        pass
            except asyncio.CancelledError:
                pass
            finally:
                if self._mode == "model":
                    if self._status is not None:
                        try:
                            self._status.stop()
                        except Exception:
                            pass
                        self._status = None
                    _active_stderr_wait_loops -= 1
                    _set_model_wait_body(None)
                else:
                    _set_tool_wait_footer_renderable(None)
                    _set_tool_wait_body(None)
                    _request_footer_ui_refresh()
                with _active_loops_lock:
                    try:
                        _active_loops.remove(self)
                    except ValueError:
                        pass

        self._task = asyncio.create_task(_run(), name="cai_wait_hints")

    async def stop(self) -> None:
        if self._disposed:
            return
        self._disposed = True
        self._stopped.set()
        if self._task is not None:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
            except Exception:
                pass
            self._task = None
        # Passive loops never enter ``_run``; remove them from ``_active_loops``
        # and release any primary slot they may hold. For primary loops the
        # ``_run`` finally block already pops them from ``_active_loops``; the
        # primary release is idempotent and safe to repeat here.
        if self._is_passive:
            with _active_loops_lock:
                try:
                    _active_loops.remove(self)
                except ValueError:
                    pass
        self._release_primary()

    def force_pause(self) -> None:
        """Synchronously yield the terminal until :meth:`force_resume`.

        Designed for interactive prompts (e.g. the sensitive command guard
        questionary) that need exclusive control of stderr/stdout. Tears
        down the rich state without cancelling the asyncio task; the run
        loop spins idle on ``_externally_paused`` until cleared.

        No-op for passive loops (they don't own the terminal).
        """
        if self._is_passive:
            return
        if self._externally_paused:
            return
        self._externally_paused = True
        if self._mode == "model":
            if self._status is not None:
                try:
                    self._status.stop()
                except Exception:
                    pass
                self._status = None
        else:
            _set_tool_wait_footer_renderable(None)
            _request_footer_ui_refresh()

    def force_resume(self) -> None:
        """Restart the rich rendering torn down by :meth:`force_pause`.

        Skipped when the compact REPL is still owning the screen — the
        compact renderer continues to draw the wait-hint body as its last
        row, so re-spawning a Rich Status would create a duplicate Live.

        No-op for passive loops (they don't own the terminal).
        """
        if self._is_passive:
            return
        if not self._externally_paused:
            return
        if _compact_live_owner or _compact_cli_owns_wait_hints():
            # Compact still owns the screen; stay paused and let the live
            # block keep drawing the body via ``get_current_wait_hint_body``.
            return
        if self._mode == "model" and self._console is not None:
            try:
                initial = build_wait_hint_renderable(
                    _model_body(0.0, self._state),
                    pipe_frame(0),
                    include_suffix=True,
                )
                self._status = Status(
                    initial,
                    console=self._console,
                    spinner="dots",
                    spinner_style="dim",
                    refresh_per_second=8,
                )
                self._status.start()
            except Exception:
                self._status = None
        # Clearing the pause flag last lets the running ``_run`` loop pick
        # the new ``self._status`` up on its next tick and resume updating
        # both the model body and the tool footer renderable.
        self._externally_paused = False


def clear_wait_hints() -> None:
    """Clear published wait-hint renderables before another UI surface prints."""
    _set_model_wait_body(None)
    _set_tool_wait_body(None)
    _set_tool_wait_footer_renderable(None)
    _request_footer_ui_refresh()


@asynccontextmanager
async def model_wait_hints():
    loop = _WaitHintLoop(mode="model")
    await loop.start()
    try:
        yield
    finally:
        await loop.stop()


class ModelStreamWaitHints:
    """Hints while waiting for the first streamed chunk."""

    def __init__(self) -> None:
        self._loop = _WaitHintLoop(mode="model")
        self._stopped_once = False

    async def start(self) -> None:
        await self._loop.start()

    async def stop(self) -> None:
        if self._stopped_once:
            return
        self._stopped_once = True
        await self._loop.stop()


@asynccontextmanager
async def tool_batch_wait_hints(names: Iterable[str], argument_strings: Iterable[str]):
    name_list = list(names)
    arg_list = list(argument_strings)
    summaries = [
        summarize_tool_arguments(arg_list[i] if i < len(arg_list) else "")
        for i in range(len(name_list))
    ]
    label, exec_summary = _tool_batch_label_and_summary(name_list, summaries)
    loop = _WaitHintLoop(mode="tool", tool_label=label, exec_summary=exec_summary)
    await loop.start()
    try:
        yield
    finally:
        await loop.stop()


async def _standalone_retry_hint_sleep(body: str, duration: float) -> None:
    if duration <= 0:
        return
    console = Console(stderr=True, soft_wrap=False)
    tick = 0
    status = Status(
        build_wait_hint_renderable(body, pipe_frame(0), include_suffix=True),
        console=console,
        spinner="dots",
        spinner_style="dim",
        refresh_per_second=8,
    )
    status.start()
    end = time.monotonic() + duration
    try:
        while time.monotonic() < end:
            status.update(
                build_wait_hint_renderable(body, pipe_frame(tick), include_suffix=True)
            )
            tick += 1
            remaining = end - time.monotonic()
            if remaining <= 0:
                break
            await asyncio.sleep(min(_PIPE_SPIN_INTERVAL, remaining))
    finally:
        try:
            status.stop()
        except Exception:
            pass


def set_model_wait_retry_overlay(message: str | None) -> None:
    """Override the model wait-hint body while empty-completion retries or
    gateway-rate pacing are in flight. ``_model_body()`` reads this first."""
    global _retry_overlay_message
    _retry_overlay_message = message


async def sleep_with_retry_backoff_hint(duration: float) -> None:
    """Sleep during HTTP/model retries: random delay line + trailer, with live hint."""
    if duration <= 0:
        return
    global _retry_overlay_message
    body = f"{random.choice(MODEL_DELAY_MESSAGES)} {RETRY_TRAILER}"
    if not wait_hints_enabled():
        await asyncio.sleep(duration)
        return
    if _active_stderr_wait_loops > 0:
        _retry_overlay_message = body
        try:
            await asyncio.sleep(duration)
        finally:
            _retry_overlay_message = None
        return
    await _standalone_retry_hint_sleep(body, duration)
