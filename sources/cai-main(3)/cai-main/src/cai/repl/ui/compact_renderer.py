"""Compact REPL renderer — single-line per agent, multi-row Live block.

Subscribes to :class:`cai.output.OutputManager` and renders a transient Rich
``Live`` area. Each task in the current turn occupies one row::

    ● Red Teamer [P1] ─ nmap -sV 10.0.0.1   ⏱ 1.2s   ⋯ · ·  RUNNING
    ● Blue Teamer [P2] ─ tail -f /var/log    ⏱ 0.4s   · ⋯ ·  RUNNING
      ↳ → Web Pentester
    ● Bug Bounter ─ thinking…                ⏱ 0.8s   · · ⋯  THINKING
    ● Red Teamer ─ curl -I http://target     ⏱ 2.1s   ✓ COMPLETED

The ``[Pn]`` agent-id pill is only shown when ``CAI_PARALLEL > 1``; in
single-agent sessions it is omitted as redundant noise.

Lifecycle
---------
* The block appears at the first :class:`TaskStartEvent` of a turn.
* While the turn is alive the block grows: running rows animate (indicator
  pulses orange bold↔dim, right-pill ``⋯`` chases across three slots at 8 Hz)
  and finished rows freeze in place as static ``✓ COMPLETED`` / ``✗ ERROR``
  pills, so the user reads a live checklist of what the agents are doing.
* At end-of-turn (or :meth:`flush`) the block is **dismissed entirely** —
  Rich Live's ``transient`` mode erases the area and the only thing the user
  keeps in scrollback is the model's markdown conclusion. Task rows are
  ephemeral by design (they live on in :data:`TASK_REGISTRY` for the Ctrl+O
  expand popup and for orchestration consumers).

Design notes
------------
* Reuses :data:`CAI_GREEN`, :data:`COMPLETED_PILL`, :data:`ERROR_PILL` from
  :mod:`cai.util.cli_palette` — single brand-color policy (q29=b). Active
  state uses ``orange1``; thinking uses ``yellow``; error uses ``red``.
* Animation tables (``_PULSE_*_STYLES``, ``_DOT_FRAMES``) live at module level
  and are indexed by ``tick % N`` — O(1) memory, no per-row state.
* TUI mode is excluded: the handler short-circuits when ``CAI_TUI_MODE=true``.
* ``pause()`` / ``resume()`` allow interactive prompts (sudo / sensitive
  guard) to take over the terminal cleanly. Coordination with
  :mod:`cai.util.wait_hints` is handled in :meth:`_start_live`.
"""

from __future__ import annotations

import os
import threading
import time

from rich.console import Console, Group, RenderableType
from rich.live import Live
from rich.table import Table
from rich.text import Text

from cai.output import (
    AgentHandoffEvent,
    OUTPUT,
    OutputEvent,
    TASK_REGISTRY,
    TaskCompleteEvent,
    TaskErrorEvent,
    TaskRecord,
    TaskStartEvent,
    TaskUpdateEvent,
    TurnStartEvent,
    TurnSummaryEvent,
)
from cai.util.cli_palette import (
    CAI_GREEN,
    COMPLETED_PILL,
    ERROR_PILL,
    GREY_HINT,
    GREY_TEXT,
)
from cai.util.session import is_parallel_session


def format_secs(s: float) -> str:
    """Compact duration formatter used by the live row and the expand popup."""
    if s < 10.0:
        return f"{s:.1f}s"
    if s < 60.0:
        return f"{s:.0f}s"
    m, sec = divmod(int(s), 60)
    if m < 60:
        return f"{m}m{sec:02d}s"
    h, m = divmod(m, 60)
    return f"{h}h{m:02d}m"

THINKING_TOOL_NAME = "__thinking__"

# Orchestration tools that delegate work to a worker sub-agent. Their live row
# shows just ``● Agent ⏱ time STATUS`` — the tool name and its args (e.g.
# ``agent_type=…, task=…``) would only expose internal mechanics that aren't
# useful to the user while the worker is doing the actual work, which is shown
# on a separate ``↳`` sub-row anyway.
_BARE_ORCH_TOOL_NAMES = frozenset({"run_specialist", "run_parallel_specialists"})

_REFRESH_PER_SEC = 8
_REFRESH_PER_SEC_IDLE = 2
_MAX_VISIBLE_TASK_ROWS = 14
_HANDOFF_TTL_S = 4.0

# Animation tables (status → indicator). Reused by every row each frame so the
# whole animation lives in O(1) memory, no per-row state.
#
#   * Left circle pulses bold↔dim while running so the user perceives "alive"
#     without losing the brand-color contract (orange = active, green = done).
#   * Right pill rotates the ⋯ across three slots with the status text fixed,
#     mirroring the chase pattern the user picked.
_PULSE_RUN_STYLES = ("bold orange1", "orange1")  # tick % 2
_PULSE_THINK_STYLES = ("bold yellow", "yellow")  # subtle distinction
_DOT_FRAMES = ("⋯ · ·", "· ⋯ ·", "· · ⋯")          # tick % 3

# Right pill style per status: (icon-style, text-style).
_RIGHT_PILL_RUN = ("bold orange1", "bold orange1")
_RIGHT_PILL_THINK = ("bold yellow", "bold yellow")


def _is_tui_mode() -> bool:
    return os.getenv("CAI_TUI_MODE", "").strip().lower() in ("1", "true", "yes")


# Spaces used per nesting level for sub-row indentation. Switching from
# ``\t`` to a fixed-width space prefix removes the cell-count uncertainty
# (Rich counts tabs as 1 cell but terminals expand them to a tab-stop multiple,
# which depends on terminal config), making ``max_width``-based truncation
# math exact.
_DEPTH_INDENT_CELLS = 8


def _row_for_record(
    record: TaskRecord,
    *,
    now: float,
    tick: int = 0,
    max_width: int | None = None,
) -> Text:
    """Build one live row: ``● Agent ─ label   ⏱ time   icon STATUS``.

    The optional ``[Pn]`` pill is rendered after the agent name only when more
    than one agent is running in parallel (``CAI_PARALLEL > 1``); the static
    ``AGENT`` sufix has been retired as redundant noise.

    ``tick`` drives all animation: left-circle pulse and right-pill ⋯ chase.
    Pass ``tick=0`` to render a static "frozen" frame (used at scrollback freeze).

    ``max_width`` is the terminal column count (sourced from the Rich
    ``ConsoleOptions``). When provided, the label is shortened with an ellipsis
    so the whole row fits on a single visible line — the timer + status pill
    are always preserved because they carry the live state the user is watching.
    """
    is_thinking = record.tool_name == THINKING_TOOL_NAME
    is_running = record.status == "running"
    is_error = record.status == "error"

    # Left circle: orange pulse while running/thinking, green when completed,
    # red on error. Color is the entire status signal (q-color-by-state).
    if is_running:
        pulse = _PULSE_THINK_STYLES if is_thinking else _PULSE_RUN_STYLES
        left_style = pulse[tick % 2]
    elif is_error:
        left_style = "bold red"
    else:
        left_style = f"bold {CAI_GREEN}"

    # Build the three pieces independently so we can size the body to fit.
    prefix = Text()
    depth = max(0, min(record.depth, 6))
    if depth > 0:
        # Fixed-width indent (spaces, not ``\t``) so the visible cell count
        # equals ``Text.cell_len`` exactly — required for accurate truncation.
        prefix.append(" " * (depth * _DEPTH_INDENT_CELLS), style="")
        prefix.append("↳ ", style=GREY_HINT)
    prefix.append("● ", style=left_style)
    prefix.append(record.agent_name or "Agent", style=f"bold {CAI_GREEN}")
    if (
        is_parallel_session()
        and record.agent_id
        and f"[{record.agent_id}]" not in (record.agent_name or "")
    ):
        prefix.append(f" [{record.agent_id}]", style=f"bold {GREY_HINT}")

    suffix = Text()
    elapsed = max(0.0, now - record.started_at) if is_running else record.duration_seconds
    suffix.append("   ⏱ ", style="dim")
    suffix.append(format_secs(elapsed), style=GREY_TEXT)
    suffix.append("   ", style="")
    if is_running:
        icon_style, text_style = _RIGHT_PILL_THINK if is_thinking else _RIGHT_PILL_RUN
        suffix.append(_DOT_FRAMES[tick % 3], style=icon_style)
        if record.depth == 0:
            suffix.append(" ", style=icon_style)
            suffix.append("THINKING" if is_thinking else "RUNNING", style=text_style)
    elif is_error:
        suffix.append("✗ ", style=ERROR_PILL)
        suffix.append("ERROR", style=ERROR_PILL)
    else:
        suffix.append("✓ ", style=COMPLETED_PILL)
        suffix.append("COMPLETED", style=COMPLETED_PILL)

    line = Text()
    line.append_text(prefix)

    # Orchestration "delegate-to-worker" tools render as a bare row: no
    # ``─ <tool args>`` body, just the agent name + timer + status pill. The
    # actual worker activity has its own ``↳`` sub-row beneath, so duplicating
    # ``run_specialist(agent_type=…, task=…)`` here was redundant noise.
    bare = record.tool_name in _BARE_ORCH_TOOL_NAMES

    if not bare:
        sep = " ─ "
        label = record.label or record.tool_name or "task"
        label_style = (
            f"italic dim {GREY_TEXT}" if is_thinking
            else "bold white" if is_error
            else "white"
        )
        # Truncate the label so the row stays on a single visible line. With
        # space-based indentation ``prefix.cell_len`` matches the rendered
        # cell count exactly, so the budget calculation is exact. The
        # ``Table.grid(no_wrap=True, overflow="crop")`` safety net at the
        # composition layer absorbs any off-by-one before Rich wraps.
        if max_width is not None and max_width > 0:
            available = max_width - prefix.cell_len - suffix.cell_len - len(sep)
            if available <= 0:
                # Not enough room for any label at all; skip the body entirely.
                sep = ""
                label = ""
            elif len(label) > available:
                label = label[: max(1, available - 1)] + "…" if available > 1 else "…"
        if sep:
            line.append(sep, style="dim white")
        if label:
            line.append(label, style=label_style)

    line.append_text(suffix)
    return line


def _visible_task_records(
    records: list[TaskRecord],
    *,
    max_rows: int = _MAX_VISIBLE_TASK_ROWS,
) -> tuple[list[TaskRecord], int]:
    """Return a bounded slice for the Live block plus a hidden-task count.

  Always includes every ``running`` row when possible; fills remaining budget
  with the most recent completed/errored rows. Prevents 40+ line repaints at
  8 Hz (the main source of terminal flicker during autonomous tool bursts).
    """
    if len(records) <= max_rows:
        return records, 0

    running = sorted(
        (r for r in records if r.status == "running"),
        key=lambda r: r.started_at,
    )
    done = sorted(
        (r for r in records if r.status != "running"),
        key=lambda r: r.started_at,
    )

    if len(running) >= max_rows:
        return running[-max_rows:], len(records) - max_rows

    visible = list(running)
    remaining = max_rows - len(visible)
    if remaining > 0 and done:
        visible = done[-remaining:] + visible
    hidden = len(records) - len(visible)
    return visible, hidden


def _overflow_row(hidden: int) -> Text:
    line = Text()
    line.append(f"  … +{hidden} more tasks in this turn", style=f"dim {GREY_HINT}")
    return line


def _handoff_subline(to_agent: str) -> Text:
    line = Text()
    line.append("  ↳ ", style=GREY_HINT)
    line.append(f"→ {to_agent}", style="bold yellow")
    return line


def _wait_hint_row(*, tick: int = 0) -> Text | None:
    """Render the current wait-hint body (model + tool) as a startup-aligned row.

    Uses the same braille ``dots`` spinner frames and grey ``CAI`` pill as
    :class:`cai.repl.ui.startup_hints.StartupHints`, but drawn inside the compact
    ``Live`` block (no second ``Status`` on stderr).

    Returns ``None`` when no wait hint is active, so the live block can drop
    the row entirely instead of leaving an empty line.
    """
    try:
        from cai.util.wait_hints import get_current_wait_hint_body
        from cai.util.hint_renderables import (
            STARTUP_HINT_SPINNER_HZ,
            build_compact_live_wait_hint_row,
        )

        body = get_current_wait_hint_body()
    except Exception:
        body = None
    if not body:
        return None
    frame_tick = int(time.monotonic() * STARTUP_HINT_SPINNER_HZ)
    line = Text()
    line.append("  ", style="")
    line.append_text(build_compact_live_wait_hint_row(body, frame_tick=frame_tick))
    return line


class _LiveBody:
    """Rich callable so :class:`Live` can re-render whenever it auto-refreshes.

    Owns the monotonic ``tick`` counter that drives every animation frame; the
    handler stays stateless w.r.t. animation so freeze-to-scrollback can render
    a deterministic static frame just by passing ``tick=0``.
    """

    def __init__(self, handler: "CompactCLIHandler") -> None:
        self._handler = handler
        self._tick = 0

    def __rich_console__(self, console, options):  # noqa: D401 - Rich protocol
        self._tick += 1
        # Use the largest sensible width: ``console.width`` is the actual
        # terminal column count (auto-detected from TTY size and reactive to
        # SIGWINCH), while ``options.max_width`` may be reduced by Rich for
        # internal padding/measure passes — using only the latter would leave
        # noticeable empty space to the right of long task rows. Falling back
        # to ``options.max_width`` when ``console.width`` is unavailable.
        try:
            full_width = console.width or options.max_width
        except Exception:
            full_width = options.max_width
        yield self._handler._build_renderable(
            tick=self._tick,
            max_width=full_width,
        )


class CompactCLIHandler:
    """Output handler that renders a multi-row Live block in the headless CLI."""

    def __init__(self, console: Console | None = None) -> None:
        self._console = console or Console()
        self._lock = threading.RLock()
        self._live: Live | None = None
        self._paused = False
        # task_id -> (from_agent, to_agent, expires_at)
        self._handoffs: dict[str, tuple[str, str, float]] = {}

    # ----------------------- OutputHandler protocol -----------------------

    def handle(self, event: OutputEvent) -> None:  # noqa: C901 - dispatch table
        if _is_tui_mode():
            return
        if isinstance(event, TaskStartEvent):
            self._on_task_start(event)
        elif isinstance(event, TaskUpdateEvent):
            self._on_task_update(event)
        elif isinstance(event, TaskCompleteEvent):
            self._on_task_complete(event)
        elif isinstance(event, TaskErrorEvent):
            self._on_task_error(event)
        elif isinstance(event, AgentHandoffEvent):
            self._on_handoff(event)
        elif isinstance(event, TurnStartEvent):
            self._on_turn_start(event)
        elif isinstance(event, TurnSummaryEvent):
            self._on_turn_summary(event)

    def flush(self) -> None:
        self._dismiss_live()

    # ------------------------------ Public --------------------------------

    def pause(self) -> None:
        """Suspend the Live block (e.g. before an interactive prompt).

        Keeps the compact-live ownership flag so wait-hint loops stay paused
        for the duration of the prompt; they would otherwise re-spawn their
        Rich Status and overwrite the questionary banner.
        """
        stop = False
        with self._lock:
            stop = self._live is not None and not self._paused
            self._paused = True
        if stop:
            self._stop_live(release_ownership=False)

    def resume(self) -> None:
        """Resume the Live block. ``_start_live`` is idempotent and tolerates an
        empty turn, so the wait-hint row reappears even when no task has started
        yet (mirrors the symmetry introduced in :meth:`_on_turn_start`)."""
        with self._lock:
            self._paused = False
        self._start_live()

    # ---------------------------- Event handlers --------------------------

    def _on_turn_start(self, event: TurnStartEvent) -> None:
        with self._lock:
            self._handoffs.clear()
        # Start Live now: ``model_wait_hints`` publishes its body before the
        # first ``TaskStartEvent``, and compact's exclusivity blocks the stderr
        # Status fallback — without an active Live the body has no renderer.
        self._start_live()

    def _on_task_start(self, event: TaskStartEvent) -> None:
        record = TaskRecord(
            task_id=event.task_id,
            turn_id=TASK_REGISTRY.current_turn_id or "",
            agent_name=event.agent_name,
            agent_id=event.agent_id,
            tool_name=event.tool_name,
            label=event.label,
            started_at=event.timestamp or time.time(),
            call_id=event.call_id,
            parent_task_id=event.parent_task_id,
            depth=max(0, event.depth),
        )
        TASK_REGISTRY.add(record)
        with self._lock:
            self._maybe_clear_handoff_for(event.agent_name)
        # Never call Live.start/stop while holding _lock: Rich's refresh thread
        # invokes _build_renderable on another thread and would deadlock.
        self._start_live()

    def _on_task_update(self, event: TaskUpdateEvent) -> None:
        TASK_REGISTRY.update(event.task_id, chunk=event.chunk or None, label=event.label or None)

    def _on_task_complete(self, event: TaskCompleteEvent) -> None:
        # Registry-only: the Live keeps rendering so completed rows stay
        # visible (as ``✓ COMPLETED`` static pills) alongside any remaining
        # running task. Freeze to scrollback happens at end-of-turn / flush.
        TASK_REGISTRY.complete(
            event.task_id,
            output=event.output or None,
            duration_seconds=event.duration_seconds or None,
            cost=event.cost,
            tokens_input=event.tokens_input,
            tokens_output=event.tokens_output,
        )

    def _on_task_error(self, event: TaskErrorEvent) -> None:
        TASK_REGISTRY.fail(
            event.task_id,
            output=event.output or None,
            error=event.error,
            error_type=event.error_type,
            duration_seconds=event.duration_seconds or None,
        )

    def _on_handoff(self, event: AgentHandoffEvent) -> None:
        if not event.from_agent or not event.to_agent:
            return
        with self._lock:
            self._handoffs[event.from_agent] = (
                event.from_agent, event.to_agent, time.time() + _HANDOFF_TTL_S,
            )

    def _on_turn_summary(self, event: TurnSummaryEvent) -> None:
        # No summary table / footer (per user request). Dismiss the transient
        # live block entirely so the only thing left in scrollback is the
        # model's markdown conclusion — task rows are ephemeral by design.
        self._dismiss_live()

    # ----------------------------- Live block -----------------------------

    def _start_live(self) -> None:
        with self._lock:
            if self._paused or self._live is not None:
                return
            # Empty turn is OK: the wait-hint row appears as soon as
            # ``model_wait_hints`` publishes the body, before any task starts.
            try:
                from cai.util.wait_hints import set_compact_live_owner

                set_compact_live_owner(True)
            except Exception:
                pass
            live = Live(
                _LiveBody(self),
                console=self._console,
                refresh_per_second=_REFRESH_PER_SEC,
                transient=True,
                auto_refresh=True,
                redirect_stdout=False,
                redirect_stderr=False,
            )
            self._live = live

        try:
            live.start()
        except Exception:
            with self._lock:
                if self._live is live:
                    self._live = None
            try:
                from cai.util.wait_hints import set_compact_live_owner

                set_compact_live_owner(False)
            except Exception:
                pass
            return

    def _stop_live(self, *, release_ownership: bool = True) -> bool:
        """Stop the Rich ``Live`` (transient erases the block) and optionally
        release ownership so wait-hint loops can re-spawn their Rich Status.

        ``release_ownership=False`` is used by :meth:`pause` so wait hints
        stay paused while an interactive prompt owns the screen.
        """
        with self._lock:
            live = self._live
            if live is None:
                return False
            self._live = None
        try:
            live.stop()
        except Exception:
            pass
        if release_ownership:
            try:
                from cai.util.wait_hints import set_compact_live_owner

                set_compact_live_owner(False)
            except Exception:
                pass
        return True

    def _dismiss_live(self) -> None:
        """Tear down the transient live block at end-of-turn / flush.

        The block was kept alive throughout the turn so the user could see the
        animated running rows and the ``✓ COMPLETED`` checklist grow in real
        time. At end-of-turn we discard it entirely (Rich Live ``transient``
        erases the area) so the only thing the user keeps in scrollback is
        the model's markdown conclusion — task rows are ephemeral by design.
        """
        stopped = self._stop_live()
        if stopped:
            # Keep the final markdown panel from starting on the old wait-hint/live row.
            try:
                self._console.print("")
            except Exception:
                pass

    # ---------------------------- Renderable ------------------------------

    def _build_renderable(
        self,
        *,
        tick: int = 0,
        max_width: int | None = None,
    ) -> RenderableType:
        """Compose the multi-row block for the current frame.

        Renders running + completed + errored rows of the current turn (the
        "growing checklist" UX) plus any active handoff sublines and the
        wait-hint row last. The block is dismissed entirely at end-of-turn,
        so there is no separate "static frame" mode.

        ``max_width`` (Rich's ``ConsoleOptions.max_width``) is propagated so
        long task labels are shortened with an ellipsis instead of wrapping
        onto a second line — wrapping breaks the single-line-per-task contract.
        """
        now = time.time()
        rows: list[RenderableType] = []

        with self._lock:
            self._handoffs = {k: v for k, v in self._handoffs.items() if v[2] > now}
            handoffs_snapshot = list(self._handoffs.values())

        turn_records = sorted(TASK_REGISTRY.for_turn() or [], key=lambda r: r.started_at)
        visible_records, hidden_count = _visible_task_records(turn_records)
        running_count = sum(1 for r in turn_records if r.status == "running")

        if self._live is not None:
            try:
                self._live.refresh_per_second = (
                    _REFRESH_PER_SEC if running_count else _REFRESH_PER_SEC_IDLE
                )
            except Exception:
                pass

        for record in visible_records:
            rows.append(_row_for_record(record, now=now, tick=tick, max_width=max_width))

        if hidden_count > 0:
            rows.append(_overflow_row(hidden_count))

        for _from_agent, to_agent, _ttl in handoffs_snapshot:
            rows.append(_handoff_subline(to_agent))

        wait_row = _wait_hint_row(tick=tick)
        if wait_row is not None:
            rows.append(wait_row)

        if not rows:
            return Text("")
        # ``no_wrap=True`` + ``overflow="crop"`` is the safety net: rows are
        # already pre-truncated by ``_row_for_record`` against ``max_width``,
        # but if anything slips through (e.g. an unusually wide handoff/wait
        # row), Rich crops the tail instead of wrapping onto a second line.
        table = Table.grid(padding=(0, 0))
        table.add_column(no_wrap=True, overflow="crop")
        for r in rows:
            table.add_row(r)
        return Group(table)

    # --------------------------- Helpers ---------------------------------

    def _maybe_clear_handoff_for(self, agent_name: str) -> None:
        """If this new task is from the destination of a pending handoff, drop the hint."""
        if not agent_name:
            return
        for from_agent, (_, to_agent, _) in list(self._handoffs.items()):
            if to_agent == agent_name:
                self._handoffs.pop(from_agent, None)


# ---------------------------------------------------------------------------
# Module-level helpers (single instance for the headless CLI session)
# ---------------------------------------------------------------------------

_handler: CompactCLIHandler | None = None
_handler_lock = threading.Lock()


def install_compact_handler(console: Console | None = None) -> CompactCLIHandler:
    """Install the singleton handler on :data:`OUTPUT`.

    Idempotent — repeated calls return the same instance. TUI mode short-circuits.
    """
    global _handler
    with _handler_lock:
        if _handler is not None:
            return _handler
        if _is_tui_mode():
            _handler = CompactCLIHandler(console=console)
            return _handler
        _handler = CompactCLIHandler(console=console)
        OUTPUT.subscribe(_handler)
        return _handler


def get_compact_handler() -> CompactCLIHandler | None:
    return _handler


def pause_compact_live() -> None:
    """Convenience: pause the live area (used by the sensitive guard)."""
    h = get_compact_handler()
    if h is not None:
        h.pause()


def resume_compact_live() -> None:
    h = get_compact_handler()
    if h is not None:
        h.resume()


__all__ = [
    "CompactCLIHandler",
    "THINKING_TOOL_NAME",
    "get_compact_handler",
    "install_compact_handler",
    "pause_compact_live",
    "resume_compact_live",
]
