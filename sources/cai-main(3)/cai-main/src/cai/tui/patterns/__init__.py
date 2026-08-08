"""
Design patterns for CAI TUI

This module implements the Observer Pattern for event handling and notifications.
"""

from .observer import (
    EventType,
    Observer,
    Subject,
    TerminalEvent,
    TerminalEventManager,
    terminal_event_manager,
)

__all__ = [
    # Observer
    "EventType",
    "TerminalEvent",
    "Observer",
    "Subject",
    "TerminalEventManager",
    "terminal_event_manager",
]

