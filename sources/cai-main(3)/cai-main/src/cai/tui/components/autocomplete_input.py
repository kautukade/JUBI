"""Autocomplete input widget for CAI TUI with modern styling"""

import os
import json
from typing import List, Optional
from pathlib import Path
from textual.widgets import Input
from textual.message import Message
from textual import on
from textual.containers import VerticalScroll
from textual.widgets import Static
from rich.text import Text


class CommandSuggestion(Message):
    """Message sent when a command suggestion is selected"""

    def __init__(self, suggestion: str) -> None:
        self.suggestion = suggestion
        super().__init__()


class SuggestionsUpdated(Message):
    """Message emitted when suggestions should be displayed/updated."""

    def __init__(self, suggestions: List[str]) -> None:
        self.suggestions = suggestions
        super().__init__()


class AutocompleteInput(Input):
    """Modern input widget with command autocompletion"""
    
    DEFAULT_CSS = """
    AutocompleteInput {
        background: transparent;
        color: #ffffff;
        border: none;
        padding: 0;
        height: 1;
        width: 100%;
    }
    
    AutocompleteInput:focus {
        background: transparent;
        border: none;
        color: #ffffff;
    }
    """

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.command_history = []
        self.history_index = -1
        self.suggestions = []
        self.current_suggestion_index = 0
        self._temp_current_input = ""
        
        # Load history from file
        self._load_history()

        # Common commands for autocompletion
        self.commands = [
            "/help",
            "/help var",
            "/agent list",
            "/agent select",
            "/model",
            "/history",
            "/flush",
            "/replay",
            "/replay stop",
            "/load",
            "/save",
            "/env",
            "/parallel",
            "/pattern",
            "/clear",
            "/exit",
            "/quit",
            "$",  # Shell command prefix
        ]

    def on_key(self, event) -> None:
        """Handle key events for autocompletion and history"""
        
        # Tab completion
        if event.key == "tab":
            event.stop()
            self._handle_tab_completion()
            return

        # History navigation
        elif event.key == "up":
            event.stop()
            self._navigate_history(-1)
            return
        elif event.key == "down":
            event.stop()
            self._navigate_history(1)
            return

        # Live suggestions while typing (except submit)
        if event.key != "enter":
            self._emit_live_suggestions()
        # Show menu on Ctrl+Space as "manual trigger"
        if event.key == "ctrl+space":
            event.stop()
            self._emit_live_suggestions()

    # Removed action_submit override - it was blocking normal submit
    # def action_submit(self) -> None:
    #     """Handle submit action - override from Input base class"""
    #     # DEBUG: Log submit action
    #     import sys
    #     print(f"[DEBUG AUTOCOMPLETE] action_submit called with value: [{self.value}]", file=sys.stderr)
    #     
    #     # Call the parent's submit action
    #     super().action_submit()

    def _handle_tab_completion(self) -> None:
        """Handle tab completion"""
        current_value = self.value.strip()

        if not current_value:
            return

        # Get matching commands
        if not self.suggestions:
            self.suggestions = [cmd for cmd in self.commands if cmd.startswith(current_value)]
            self.current_suggestion_index = 0

        # Cycle through suggestions
        if self.suggestions:
            self.value = self.suggestions[self.current_suggestion_index]
            self.current_suggestion_index = (self.current_suggestion_index + 1) % len(
                self.suggestions
            )
            # Move cursor to end
            self.cursor_position = len(self.value)
            # Reflect in suggestion menu as well
            self.post_message(SuggestionsUpdated(self.suggestions))

    def _navigate_history(self, direction: int) -> None:
        """Navigate through command history"""
        if not self.command_history:
            return

        # Save current input if we're starting to navigate
        if self.history_index == -1 and self.value.strip():
            self._temp_current_input = self.value
        elif self.history_index == -1:
            self._temp_current_input = ""

        # Update history index
        if direction == -1:  # Up arrow - go back in history
            if self.history_index < len(self.command_history) - 1:
                self.history_index += 1
        else:  # Down arrow - go forward in history
            if self.history_index > -1:
                self.history_index -= 1

        # Set value based on history
        if self.history_index == -1:
            # Restore the original input that was being typed
            self.value = getattr(self, '_temp_current_input', "")
        else:
            self.value = self.command_history[self.history_index]

        # Move cursor to end
        self.cursor_position = len(self.value)

    def add_to_history(self, command: str) -> None:
        """Add a command to history"""
        if command:
            # Remove command if it already exists (to move it to front)
            if command in self.command_history:
                self.command_history.remove(command)
            
            # Add to front of history
            self.command_history.insert(0, command)
            
            # Limit history size
            if len(self.command_history) > 100:
                self.command_history.pop()
            
            # Save history to file
            self._save_history()
        
        # Reset history navigation
        self.history_index = -1

    def _emit_live_suggestions(self) -> None:
        """Compute and emit suggestions for current input."""
        prefix = self.value.strip()
        if not prefix:
            self.suggestions = []
            self.current_suggestion_index = 0
            self.post_message(SuggestionsUpdated([]))
            return
        # Build suggestions from known commands + history
        pool = list(dict.fromkeys(self.commands + self.command_history))
        suggestions = [c for c in pool if c.startswith(prefix)]
        self.suggestions = suggestions[:10]
        self.current_suggestion_index = 0
        self.post_message(SuggestionsUpdated(self.suggestions))
    
    def _get_history_file(self) -> Path:
        """Get the path to the history file"""
        config_dir = Path.home() / ".cai"
        config_dir.mkdir(exist_ok=True)
        return config_dir / "tui_history.json"
    
    def _load_history(self) -> None:
        """Load command history from file"""
        try:
            history_file = self._get_history_file()
            if history_file.exists():
                with open(history_file, 'r') as f:
                    data = json.load(f)
                    self.command_history = data.get('commands', [])[:100]  # Limit to 100 most recent
        except Exception:
            # If loading fails, start with empty history
            self.command_history = []
    
    def _save_history(self) -> None:
        """Save command history to file"""
        try:
            history_file = self._get_history_file()
            with open(history_file, 'w') as f:
                json.dump({
                    'commands': self.command_history[:100]  # Save only 100 most recent
                }, f, indent=2)
        except Exception:
            # Silently fail if we can't save history
            pass
