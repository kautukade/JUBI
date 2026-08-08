"""
Base classes for TUI display system
"""

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from datetime import datetime
from enum import Enum
from typing import Any, Dict, Optional, List
import threading
import uuid


class DisplayMode(Enum):
    """Display modes for different output types"""

    CLI = "cli"
    TUI = "tui"


class OutputType(Enum):
    """Types of output to display"""

    TOOL_CALL = "tool_call"
    TOOL_OUTPUT = "tool_output"
    AGENT_MESSAGE = "agent_message"
    STREAMING = "streaming"
    THINKING = "thinking"
    ERROR = "error"
    INFO = "info"


@dataclass
class DisplayContext:
    """Context for display operations"""

    terminal_id: str
    terminal_number: int
    mode: DisplayMode = DisplayMode.TUI
    agent_name: Optional[str] = None
    agent_id: Optional[str] = None
    is_parallel: bool = False
    interaction_counter: int = 0
    session_id: str = field(default_factory=lambda: str(uuid.uuid4())[:8])
    created_at: datetime = field(default_factory=datetime.now)
    metadata: Dict[str, Any] = field(default_factory=dict)

    def __post_init__(self):
        """Initialize thread-local storage"""
        self._lock = threading.Lock()
        self._outputs: List[Any] = []

    def add_output(self, output: Any) -> None:
        """Add output to context history"""
        with self._lock:
            self._outputs.append({"timestamp": datetime.now(), "output": output})

    def get_outputs(self) -> List[Any]:
        """Get all outputs for this context"""
        with self._lock:
            return self._outputs.copy()


class BaseDisplay(ABC):
    """Base class for all display implementations"""

    def __init__(self):
        """Initialize base display"""
        self._lock = threading.Lock()
        self._active_streams: Dict[str, Any] = {}
        self._displayed_items: set = set()
        self._console = None
        self._terminal_outputs: Dict[str, Any] = {}

    def set_console(self, console: Any) -> None:
        """Set the console for output"""
        with self._lock:
            self._console = console

    def get_console(self) -> Optional[Any]:
        """Get the current console"""
        with self._lock:
            return self._console

    def set_terminal_output(self, terminal_id: str, output: Any) -> None:
        """Set terminal output for a specific terminal"""
        with self._lock:
            self._terminal_outputs[terminal_id] = output

    def get_terminal_output(self, terminal_id: str) -> Optional[Any]:
        """Get terminal output for a specific terminal"""
        with self._lock:
            return self._terminal_outputs.get(terminal_id)

    @abstractmethod
    def display(self, context: DisplayContext, data: Dict[str, Any]) -> None:
        """Display data in the given context"""
        pass

    @abstractmethod
    def start_streaming(
        self, context: DisplayContext, stream_id: str, data: Dict[str, Any]
    ) -> None:
        """Start a streaming display session"""
        pass

    @abstractmethod
    def update_streaming(self, stream_id: str, data: Dict[str, Any]) -> None:
        """Update an active streaming session"""
        pass

    @abstractmethod
    def finish_streaming(self, stream_id: str, data: Dict[str, Any]) -> None:
        """Finish a streaming session"""
        pass

    def cleanup(self, context: DisplayContext) -> None:
        """Clean up resources for a context"""
        with self._lock:
            # Clean up any active streams for this context
            streams_to_remove = []
            for stream_id, stream_data in self._active_streams.items():
                if stream_data.get("context") == context:
                    streams_to_remove.append(stream_id)

            for stream_id in streams_to_remove:
                self._active_streams.pop(stream_id, None)

    def _is_duplicate(self, item_key: str) -> bool:
        """Check if an item has already been displayed"""
        with self._lock:
            # Periodic cleanup to prevent unbounded growth
            # Keep set under 500 items to prevent memory issues
            if len(self._displayed_items) > 500:
                # Clear oldest entries (since set doesn't track order,
                # we clear half the set to make room)
                items_to_keep = list(self._displayed_items)[-250:]
                self._displayed_items = set(items_to_keep)

            if item_key in self._displayed_items:
                return True
            self._displayed_items.add(item_key)
            return False

    def clear_displayed_items(self) -> None:
        """Clear all displayed items to allow fresh display.

        Call this when starting a new session or when you want to
        allow previously displayed items to be shown again.
        """
        with self._lock:
            self._displayed_items.clear()

    def _generate_item_key(self, context: DisplayContext, data: Dict[str, Any]) -> str:
        """Generate a unique key for deduplication.

        NOTE: This method includes interaction_counter which can cause
        re-display on turn changes. For tool calls, prefer using the
        tool-specific _generate_tool_key method instead.
        """
        # Include context info for proper deduplication
        parts = [
            context.terminal_id,
            context.agent_name or "",
            str(context.interaction_counter),
            data.get("tool_name", ""),
            str(data.get("args", "")),
        ]
        return ":".join(parts)
