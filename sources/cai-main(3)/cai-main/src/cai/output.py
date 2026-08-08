"""CAI Output Manager.

Event-driven output system replacing 13+ mutable globals in util.py.
Inspired by Codex's event channel pattern (tx_event + typed deltas).

Created in Day 0 as shared contract between 3 refactoring streams.
- Stream 1 (Core Engine): emits events from tool execution and LLM calls
- Stream 3 (Interface): implements handlers for TUI, CLI, API display
- Stream 2 (Foundation): removes old globals from util.py once migration complete

Compact REPL extension (orchestration-ready):
- ``Task*`` events represent agent activity at the *task* granularity. A task is
  one tool invocation by an agent (or a logical unit emitted by a future planner).
  They are higher-level than ``Tool*`` events and drive the single-line Live
  renderer + Ctrl+O expand popup.
- ``Turn*`` events bracket a user turn so the renderer can collapse the Live
  area cleanly between turns.
- ``TaskRegistry`` keeps the in-RAM task state (capped FIFO) consumed by the
  Ctrl+O expand popup.
"""

from __future__ import annotations

import json
import sys
import threading
import time
import uuid
from collections import OrderedDict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Protocol, TextIO


# --- Event Types ---


@dataclass
class OutputEvent:
    """Base output event."""

    timestamp: float = field(default_factory=time.time)
    agent_id: str | None = None


@dataclass
class ToolStartEvent(OutputEvent):
    """Tool execution has started."""

    tool_name: str = ""
    call_id: str = ""


@dataclass
class ToolStreamEvent(OutputEvent):
    """Incremental tool output (streaming)."""

    tool_name: str = ""
    call_id: str = ""
    chunk: str = ""


@dataclass
class ToolCompleteEvent(OutputEvent):
    """Tool execution completed."""

    tool_name: str = ""
    call_id: str = ""
    output: str = ""
    exit_code: int = 0
    duration_seconds: float = 0.0


@dataclass
class ToolErrorEvent(OutputEvent):
    """Tool execution failed."""

    tool_name: str = ""
    call_id: str = ""
    error: str = ""
    error_type: str = ""


@dataclass
class LLMStreamEvent(OutputEvent):
    """Incremental LLM response chunk."""

    content: str = ""
    is_reasoning: bool = False


@dataclass
class LLMCompleteEvent(OutputEvent):
    """LLM response completed."""

    content: str = ""
    usage: dict = field(default_factory=dict)
    model: str = ""
    cost: float = 0.0


@dataclass
class StatusEvent(OutputEvent):
    """General status update."""

    message: str = ""
    level: str = "info"


@dataclass
class AgentHandoffEvent(OutputEvent):
    """Agent handoff occurred."""

    from_agent: str = ""
    to_agent: str = ""


# --- Compact / orchestration events ---


@dataclass
class TurnStartEvent(OutputEvent):
    """A user turn has just started."""

    turn_id: str = ""
    user_input: str = ""


@dataclass
class TurnSummaryEvent(OutputEvent):
    """A user turn has just finished. Used by the compact handler to collapse
    the transient Live block between turns. ``tasks`` is preserved so future
    consumers (telemetry, JSON sinks, orchestrator) can attach a snapshot
    without re-deriving it from :data:`TASK_REGISTRY`."""

    turn_id: str = ""
    tasks: list[dict[str, Any]] = field(default_factory=list)


@dataclass
class TaskStartEvent(OutputEvent):
    """An agent task has started.

    ``task_id`` is unique per turn. ``label`` is the human-readable description
    rendered on the live row (inferred deterministically today; emitted by a
    planner agent in the future).
    """

    task_id: str = ""
    agent_name: str = ""
    agent_id: str = ""
    tool_name: str = ""
    label: str = ""
    call_id: str = ""
    parent_task_id: str = ""
    depth: int = 0


@dataclass
class TaskUpdateEvent(OutputEvent):
    """Incremental task progress (output chunk and/or label override)."""

    task_id: str = ""
    chunk: str = ""
    label: str = ""


@dataclass
class TaskCompleteEvent(OutputEvent):
    """A task finished successfully."""

    task_id: str = ""
    output: str = ""
    duration_seconds: float = 0.0
    cost: float = 0.0
    tokens_input: int = 0
    tokens_output: int = 0


@dataclass
class TaskErrorEvent(OutputEvent):
    """A task failed; carries error info for the JSONL sink and the expand popup."""

    task_id: str = ""
    output: str = ""
    error: str = ""
    error_type: str = ""
    duration_seconds: float = 0.0


# --- Output Handler Protocol ---


class OutputHandler(Protocol):
    """Interface for output consumers (TUI, CLI, API, file)."""

    def handle(self, event: OutputEvent) -> None: ...


# --- Output Manager ---


class OutputManager:
    """Central output bus replacing global mutable state.

    Usage:
        # At startup (Stream 3 wires handlers)
        output = OutputManager()
        output.subscribe(TUIOutputHandler(...))

        # During execution (Stream 1 emits)
        output.emit(ToolStartEvent(tool_name="nmap", call_id="abc"))
        output.emit(ToolStreamEvent(tool_name="nmap", call_id="abc", chunk="..."))
        output.emit(ToolCompleteEvent(tool_name="nmap", call_id="abc", output="..."))
    """

    def __init__(self) -> None:
        self._handlers: list[OutputHandler] = []

    def subscribe(self, handler: OutputHandler) -> None:
        self._handlers.append(handler)

    def unsubscribe(self, handler: OutputHandler) -> None:
        self._handlers.remove(handler)

    def emit(self, event: OutputEvent) -> None:
        for handler in self._handlers:
            try:
                handler.handle(event)
            except Exception:
                pass  # Handlers must not crash the pipeline

    def flush(self) -> None:
        for handler in self._handlers:
            if hasattr(handler, "flush"):
                handler.flush()


# --- Concrete Handlers ---


class CLIOutputHandler:
    """Renders output events to Rich console (headless/CLI mode).

    Designed for non-TUI sessions where output goes directly to the terminal.
    Uses Rich formatting when available, falls back to plain text.
    """

    def __init__(self, file: TextIO | None = None) -> None:
        self._file = file or sys.stderr
        try:
            from rich.console import Console

            self._console = Console(file=self._file, highlight=False)
            self._rich = True
        except ImportError:
            self._console = None
            self._rich = False

    def _print(self, text: str) -> None:
        if self._rich and self._console:
            self._console.print(text, highlight=False)
        else:
            print(text, file=self._file, flush=True)

    def handle(self, event: OutputEvent) -> None:  # noqa: C901
        if isinstance(event, ToolStartEvent):
            # Suppressed: tool start/output/complete are rendered by the flat-style
            # display in cli_print_tool_output / _create_tool_panel_content.
            pass
        elif isinstance(event, ToolStreamEvent):
            # Suppressed: streaming chunks handled by Rich Live display
            pass
        elif isinstance(event, ToolCompleteEvent):
            # Suppressed: completion rendered by flat-style display in streaming.py
            pass
        elif isinstance(event, ToolErrorEvent):
            # Errors are still shown to avoid silent failures
            self._print(
                f"[bold red]!! {event.tool_name}: {event.error}[/bold red]"
                if self._rich
                else f"!! {event.tool_name}: {event.error}"
            )
        elif isinstance(event, LLMStreamEvent):
            if self._rich and self._console:
                self._console.print(event.content, end="", highlight=False)
            else:
                print(event.content, end="", file=self._file, flush=True)
        elif isinstance(event, LLMCompleteEvent):
            if event.content:
                self._print(event.content)
        elif isinstance(event, StatusEvent):
            level_style = {
                "info": "cyan",
                "warning": "yellow",
                "error": "bold red",
            }.get(event.level, "dim")
            self._print(
                f"[{level_style}]{event.message}[/{level_style}]"
                if self._rich
                else f"[{event.level.upper()}] {event.message}"
            )
        elif isinstance(event, AgentHandoffEvent):
            self._print(
                f"[bold magenta]>> Handoff: {event.from_agent} -> {event.to_agent}[/bold magenta]"
                if self._rich
                else f">> Handoff: {event.from_agent} -> {event.to_agent}"
            )

    def flush(self) -> None:
        self._file.flush()


class FileOutputHandler:
    """Logs output events to a JSONL file for replay/audit.

    Each line is a JSON object with ``type``, event fields, and a timestamp.
    Non-serializable values are converted via ``str()``.
    """

    def __init__(self, filepath: str | Path) -> None:
        self._path = Path(filepath)
        self._file: TextIO = open(self._path, "a", encoding="utf-8")

    def _serialize(self, obj: Any) -> Any:
        """Make dataclass fields JSON-safe."""
        if isinstance(obj, dict):
            return {k: self._serialize(v) for k, v in obj.items()}
        if isinstance(obj, (list, tuple)):
            return [self._serialize(v) for v in obj]
        # Primitives pass through; everything else becomes str
        if isinstance(obj, (str, int, float, bool, type(None))):
            return obj
        return str(obj)

    def handle(self, event: OutputEvent) -> None:
        data: dict[str, Any] = {"type": type(event).__name__}
        data.update(self._serialize(event.__dict__))
        self._file.write(json.dumps(data, default=str) + "\n")

    def flush(self) -> None:
        self._file.flush()

    def close(self) -> None:
        self._file.flush()
        self._file.close()


# --- Task Registry ---


@dataclass
class TaskRecord:
    """Snapshot of a single task lifecycle. Consumed by the Ctrl+O expand popup
    and any future telemetry / orchestrator subscribers."""

    task_id: str
    turn_id: str
    agent_name: str
    agent_id: str
    tool_name: str
    label: str
    started_at: float
    status: str = "running"  # "running" | "completed" | "error"
    completed_at: float | None = None
    duration_seconds: float = 0.0
    output: str = ""
    error: str = ""
    error_type: str = ""
    cost: float = 0.0
    tokens_input: int = 0
    tokens_output: int = 0
    call_id: str = ""
    parent_task_id: str = ""
    depth: int = 0

    def as_dict(self) -> dict[str, Any]:
        return {
            "task_id": self.task_id,
            "turn_id": self.turn_id,
            "agent_name": self.agent_name,
            "agent_id": self.agent_id,
            "tool_name": self.tool_name,
            "label": self.label,
            "started_at": self.started_at,
            "status": self.status,
            "completed_at": self.completed_at,
            "duration_seconds": self.duration_seconds,
            "output": self.output,
            "error": self.error,
            "error_type": self.error_type,
            "cost": self.cost,
            "tokens_input": self.tokens_input,
            "tokens_output": self.tokens_output,
            "call_id": self.call_id,
            "parent_task_id": self.parent_task_id,
            "depth": self.depth,
        }


class TaskRegistry:
    """In-memory registry of task records, capped FIFO.

    Thread-safe; consumed by the Ctrl+O expand popup and the live renderer.
    The full ``output`` payload is kept here so the LLM context (handled
    separately) and the UI never share buffers.

    Records persist across turns until evicted by FIFO; ``begin_turn`` only
    rotates ``current_turn_id`` so the popup can scope to "this turn" without
    losing prior data when needed.
    """

    def __init__(self, max_size: int = 200) -> None:
        self._tasks: "OrderedDict[str, TaskRecord]" = OrderedDict()
        self._max = max_size
        self._lock = threading.RLock()
        self._current_turn_id: str | None = None

    @property
    def current_turn_id(self) -> str | None:
        return self._current_turn_id

    def begin_turn(self, turn_id: str | None = None) -> str:
        with self._lock:
            self._current_turn_id = turn_id or uuid.uuid4().hex[:12]
            return self._current_turn_id

    def add(self, record: TaskRecord) -> None:
        with self._lock:
            self._tasks[record.task_id] = record
            while len(self._tasks) > self._max:
                self._tasks.popitem(last=False)

    def update(self, task_id: str, *, chunk: str | None = None, label: str | None = None) -> None:
        with self._lock:
            rec = self._tasks.get(task_id)
            if rec is None:
                return
            if chunk:
                rec.output += chunk
            if label:
                rec.label = label

    def complete(
        self,
        task_id: str,
        *,
        output: str | None = None,
        duration_seconds: float | None = None,
        cost: float = 0.0,
        tokens_input: int = 0,
        tokens_output: int = 0,
    ) -> None:
        with self._lock:
            rec = self._tasks.get(task_id)
            if rec is None:
                return
            rec.status = "completed"
            rec.completed_at = time.time()
            if output is not None:
                rec.output = output
            if duration_seconds is not None:
                rec.duration_seconds = duration_seconds
            else:
                rec.duration_seconds = max(0.0, rec.completed_at - rec.started_at)
            rec.cost = cost
            rec.tokens_input = tokens_input
            rec.tokens_output = tokens_output

    def fail(
        self,
        task_id: str,
        *,
        output: str | None = None,
        error: str = "",
        error_type: str = "",
        duration_seconds: float | None = None,
    ) -> None:
        with self._lock:
            rec = self._tasks.get(task_id)
            if rec is None:
                return
            rec.status = "error"
            rec.completed_at = time.time()
            if output is not None:
                rec.output = output
            rec.error = error
            rec.error_type = error_type
            if duration_seconds is not None:
                rec.duration_seconds = duration_seconds
            else:
                rec.duration_seconds = max(0.0, rec.completed_at - rec.started_at)

    def get(self, task_id: str) -> TaskRecord | None:
        with self._lock:
            return self._tasks.get(task_id)

    def active(self) -> list[TaskRecord]:
        with self._lock:
            return [r for r in self._tasks.values() if r.status == "running"]

    def for_turn(self, turn_id: str | None = None) -> list[TaskRecord]:
        """Return tasks belonging to ``turn_id`` (defaults to current)."""
        target = turn_id or self._current_turn_id
        if target is None:
            return []
        with self._lock:
            return [r for r in self._tasks.values() if r.turn_id == target]

    def clear(self) -> None:
        with self._lock:
            self._tasks.clear()
            self._current_turn_id = None


# Singleton for current session
OUTPUT = OutputManager()
TASK_REGISTRY = TaskRegistry()
