"""
TUI Display System - Independent from CLI display
"""

from .base import DisplayMode, DisplayContext, BaseDisplay
from .manager import DisplayManager
from .tool_display import ToolDisplay
from .agent_display import AgentDisplay
from .streaming_display import StreamingDisplay
from .panel_formatter import PanelFormatter
from .handoff_context import (
    DisplayContext as HandoffDisplayContext,
    set_display_context,
    get_display_context,
    propagate_display_context_to_agent,
)

__all__ = [
    # Base classes
    "DisplayMode",
    "DisplayContext",
    "BaseDisplay",
    # Core components
    "DisplayManager",
    "PanelFormatter",
    # Display implementations
    "ToolDisplay",
    "AgentDisplay",
    "StreamingDisplay",
    # Handoff context support
    "HandoffDisplayContext",
    "set_display_context",
    "get_display_context",
    "propagate_display_context_to_agent",
]
