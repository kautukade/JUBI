"""
Observer pattern implementation for terminal events
"""

from abc import ABC, abstractmethod
from dataclasses import dataclass
from enum import Enum
from typing import Any, Callable

import os
_CAI_DEBUG_DIR = os.path.join(os.path.expanduser("~"), ".cai", "debug")


class EventType(Enum):
    """Terminal event types"""
    TERMINAL_CREATED = "terminal_created"
    TERMINAL_CONFIGURED = "terminal_configured"
    TERMINAL_FOCUSED = "terminal_focused"
    TERMINAL_UNFOCUSED = "terminal_unfocused"
    TERMINAL_CLEARED = "terminal_cleared"
    TERMINAL_OUTPUT = "terminal_output"
    TERMINAL_COMMAND = "terminal_command"
    TERMINAL_ROLE_CHANGED = "terminal_role_changed"
    TERMINAL_REMOVED = "terminal_removed"


@dataclass
class TerminalEvent:
    """Event data for terminal events"""
    event_type: EventType
    terminal_id: str
    data: dict[str, Any]


class Observer(ABC):
    """Base observer interface"""

    @abstractmethod
    def update(self, event: TerminalEvent) -> None:
        """Handle the event"""
        pass


class Subject:
    """Base subject interface for observables"""

    def __init__(self):
        self._observers: dict[EventType, list[Observer]] = {}
        self._event_callbacks: dict[EventType, list[Callable]] = {}

    def attach(self, observer: Observer, event_type: EventType = None) -> None:
        """Attach an observer for specific event type or all events"""
        if event_type:
            if event_type not in self._observers:
                self._observers[event_type] = []
            self._observers[event_type].append(observer)
        else:
            # Attach to all event types
            for event_type in EventType:
                if event_type not in self._observers:
                    self._observers[event_type] = []
                self._observers[event_type].append(observer)

    def detach(self, observer: Observer, event_type: EventType = None) -> None:
        """Detach an observer"""
        if event_type:
            if event_type in self._observers:
                self._observers[event_type].remove(observer)
        else:
            # Detach from all event types
            for observers in self._observers.values():
                if observer in observers:
                    observers.remove(observer)

    def attach_callback(self, callback: Callable, event_type: EventType) -> None:
        """Attach a simple callback function for an event type"""
        if event_type not in self._event_callbacks:
            self._event_callbacks[event_type] = []
        self._event_callbacks[event_type].append(callback)

    def notify(self, event: TerminalEvent) -> None:
        """Notify all observers of an event"""
        # DEBUG: Log event notifications
        import traceback
        if event.event_type == EventType.TERMINAL_OUTPUT:
            stack_size = len(traceback.extract_stack())
            if stack_size > 20:
                with open(f"{_CAI_DEBUG_DIR}/cai_recursion_debug.log", "a") as f:
                    f.write(f"[RECURSION DEBUG] TerminalEventManager.notify - TERMINAL_OUTPUT event, Stack size: {stack_size}\n")
                    if stack_size > 30:
                        f.write("Event notify stack trace:\n")
                        for frame in traceback.extract_stack()[-15:]:
                            f.write(f"  {frame.filename}:{frame.lineno} in {frame.name}\n")
        
        # Notify observers
        if event.event_type in self._observers:
            for observer in self._observers[event.event_type]:
                observer.update(event)

        # Call callbacks
        if event.event_type in self._event_callbacks:
            for callback in self._event_callbacks[event.event_type]:
                callback(event)


class TerminalEventManager(Subject):
    """Centralized event manager for terminal events"""

    _instance = None

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance

    def emit(self, event_type: EventType, terminal_id: str, **data) -> None:
        """Emit a terminal event"""
        event = TerminalEvent(
            event_type=event_type,
            terminal_id=terminal_id,
            data=data
        )
        self.notify(event)


# Global event manager instance
terminal_event_manager = TerminalEventManager()

