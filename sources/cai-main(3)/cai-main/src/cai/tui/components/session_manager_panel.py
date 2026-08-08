"""
Panel for displaying and managing sessions.
"""

from textual.app import ComposeResult
from textual.widgets import Static, ListView, ListItem, Label
from textual.containers import Vertical

class SessionManagerPanel(Static):
    """A panel to display session information."""

    def compose(self) -> ComposeResult:
        """Compose the panel UI."""
        yield Vertical(
            Label("Sessions", classes="panel-title"),
            ListView(
                ListItem(Label("No sessions active.")),
                id="session-list"
            ),
            id="session-manager-panel-content"
        ) 