"""
Terminal Console - Custom console that redirects Rich output to TUI terminals
"""

from rich.console import Console, ConsoleOptions, RenderResult
from rich.text import Text
from typing import Any, Optional
import threading


class TerminalConsole(Console):
    """Custom Rich console that redirects output to TUI terminal widgets"""

    def __init__(self, terminal_output=None, **kwargs):
        # Initialize with no file output - we'll redirect to terminal
        super().__init__(file=None, **kwargs)
        self.terminal_output = terminal_output
        self._lock = threading.Lock()

    def set_terminal_output(self, terminal_output):
        """Set or update the terminal output widget"""
        with self._lock:
            self.terminal_output = terminal_output

    def print(self, *objects: Any, **kwargs) -> None:
        """Override print to redirect to terminal"""
        if not self.terminal_output:
            return

        # Convert objects to renderable format
        renderables = []
        for obj in objects:
            renderables.append(obj)

        # Render to string
        with self.capture() as capture:
            super().print(*renderables, **kwargs)

        output = capture.get()

        # Write to terminal
        with self._lock:
            if self.terminal_output:
                # For Rich panels and formatted output, write as a single renderable
                # This preserves Rich formatting in the RichLog widget
                if renderables and len(renderables) == 1:
                    # Single Rich object - write directly to RichLog
                    self.terminal_output.write(renderables[0])
                elif output:
                    # Multiple objects or plain text - write the rendered output
                    self.terminal_output.write(
                        output.rstrip("\n") if output.endswith("\n") else output
                    )

    def log(self, *objects: Any, **kwargs) -> None:
        """Override log to redirect to terminal"""
        self.print(*objects, **kwargs)


# Global terminal console instances for each terminal
_terminal_consoles = {}
_console_lock = threading.Lock()


def get_terminal_console(terminal_id: str) -> TerminalConsole:
    """Get or create a terminal console for a specific terminal"""
    with _console_lock:
        if terminal_id not in _terminal_consoles:
            _terminal_consoles[terminal_id] = TerminalConsole()
        return _terminal_consoles[terminal_id]


def set_terminal_output(terminal_id: str, terminal_output):
    """Set the output widget for a terminal console"""
    console = get_terminal_console(terminal_id)
    console.set_terminal_output(terminal_output)


def clear_terminal_console(terminal_id: str):
    """Clear a terminal console"""
    with _console_lock:
        if terminal_id in _terminal_consoles:
            del _terminal_consoles[terminal_id]


def set_terminal_console(terminal_id: str, console: TerminalConsole) -> None:
    """Set a specific terminal console instance"""
    with _console_lock:
        _terminal_consoles[terminal_id] = console


def get_terminal_output(terminal_id: Optional[str] = None):
    """Get the terminal output widget for a specific terminal"""
    # If no terminal_id provided, try to get from routing context
    if not terminal_id:
        from cai.tui.core.terminal_tracking import get_current_terminal_id
        terminal_id = get_current_terminal_id()
    
    if not terminal_id:
        return None
    
    console = get_terminal_console(terminal_id)
    terminal_output = console.terminal_output if console else None
    
    # Return the terminal output directly - streaming_display will handle getting the RichLog
    return terminal_output
