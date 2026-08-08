"""
Sidebar widget for agent selection and navigation with modern design
"""

import time
import os
import asyncio
from datetime import datetime
from typing import Any

from textual.app import ComposeResult
from textual.containers import Container, Vertical, VerticalScroll, Horizontal
from textual.message import Message
from textual.reactive import reactive
from textual.widgets import Label, ListItem, ListView, Static, Button, Tooltip, TabbedContent, TabPane, Input
from textual.binding import Binding
from textual import on
from textual.events import Click, MouseMove, Key
from rich.text import Text

# Import will be done locally to avoid circular import


class RefreshKeysMessage(Message):
    """Message to refresh API keys in sidebar"""
    pass


class AddApiKeyDialog(Container):
    """Modal dialog for adding new API keys"""
    
    DEFAULT_CSS = """
    AddApiKeyDialog {
        layer: overlay;
        width: 100%;
        height: 100%;
        display: none;
    }
    
    AddApiKeyDialog.visible {
        display: block;
    }
    
    /* Dark overlay */
    #add-key-overlay {
        width: 100%;
        height: 100%;
        background: $surface;
        align: left top;
        padding: 2 0;
    }
    
    /* Dialog content box - positioned at top with optimal height */
    #add-key-content {
        width: 31;
        height: 22;
        background: #2e4f46;
        border: solid #529d86;
        padding: 0;
    }
    
    /* Dialog header */
    #add-key-header {
        height: 2;
        color: #c8ff00;
        padding: 0 1;
        text-align: center;
        text-style: bold;
        border-bottom: solid #529d86;
        content-align: center middle;
        width: 100%;
    }
    
    /* Form fields */
    #add-key-form {
        height: 14;
        padding: 0 1;
    }
    
    .form-label {
        height: 1;
        color: #c8ff00;
        text-style: bold;
        margin: 0 0 1 0;
        padding: 0;
    }
    
    .form-spacer {
        height: 1;
        background: transparent;
        margin: 0;
        padding: 0;
    }
    
    .error-message {
        height: 1;
        color: #ff6b6b;
        text-style: italic;
        margin: 0;
        padding: 0 1;
        text-align: center;
        display: none;
    }
    
    .error-message.visible {
        display: block;
    }
    
    /* Dialog inputs */
    AddApiKeyDialog Input {
        background: $surface;
        border: solid #529d86;
        color: #c8ff00;
        padding: 0 1;
    }
    
    AddApiKeyDialog Input:focus {
        background: #181c1a;
        border: solid #529d86;
        color: #ffd97b;
    }
    
    /* Dialog buttons */
    #add-key-buttons {
        height: 3;
        padding: 0;
        align: center middle;
        background: #2e4f46;
        width: 100%;
        min-height: 3;
    }
    
    #add-key-buttons .dialog-cancel-btn {
        width: 54% !important;
        height: 3 !important;
        margin: 0 1 0 1 !important;
        background: #529d86 !important;
        border: solid #529d86 !important;
        color: #181c1a !important;
        padding: 0 !important;
        text-align: center !important;
        text-style: bold !important;
        max-width: 54% !important;
        min-width: 54% !important;
        content-align: center middle !important;
    }
    
    #add-key-buttons .dialog-cancel-btn:hover {
        background: #3a6657;
        border: solid #529d86;
        color: #ffd97b !important;
    }
    
    #add-key-buttons .dialog-save-btn {
        width: 43% !important;
        height: 3 !important;
        margin: 0 1 0 0 !important;
        background: #c8ff00 !important;
        border: solid #c8ff00 !important;
        color: #181c1a !important;
        padding: 0 !important;
        text-align: center !important;
        text-style: bold !important;
        max-width: 43% !important;
        min-width: 43% !important;
        content-align: center middle !important;
    }
    
    #add-key-buttons .dialog-save-btn:hover {
        background: #a6d400;
        border: solid #c8ff00;
        color: #0f1311 !important;
    }
    
    #add-key-buttons .dialog-save-btn:disabled {
        background: #4a4a4a !important;
        border: solid #4a4a4a !important;
        color: #888888 !important;
        opacity: 0.5;
    }
    
    #add-key-buttons .dialog-save-btn:disabled:hover {
        background: #4a4a4a !important;
        border: solid #4a4a4a !important;
        color: #888888 !important;
    }
    """
    
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.model_input_value = ""
        self.api_key_input_value = ""
        self.autocomplete_options = []
        self.selected_autocomplete_index = -1
        self.callback = None
        self.save_button = None
        self.error_message_label = None
        
        # Common API key patterns for autocomplete
        self.api_key_patterns = [
            "OPENAI_API_KEY",
            "ANTHROPIC_API_KEY",
            "OPENROUTER_API_KEY",
            "OLLAMA",
            "GROQ_API_KEY",
            "DEEPSEEK_API_KEY",
            "CLAUDE_API_KEY",
            "GEMINI_API_KEY",
            "MISTRAL_API_KEY",
            "COHERE_API_KEY"
        ]
    
    def compose(self) -> ComposeResult:
        """Compose the dialog UI"""
        with Container(id="add-key-overlay"):
            with Vertical(id="add-key-content"):
                # Header
                yield Label("Add API Key", id="add-key-header")
                
                # Form
                with Vertical(id="add-key-form"):
                    # Model field
                    yield Label("PROVIDER:", classes="form-label")
                    yield Input(placeholder="OPENAI, ANTHROPIC...", id="model-input")
                    
                    # Small spacer
                    yield Static("", classes="form-spacer")
                    
                    # API Key field  
                    yield Label("API_KEY:", classes="form-label")
                    yield Input(placeholder="sk-...", id="api-key-input", password=True)
                    
                    # Error message area (initially hidden)
                    yield Label("", id="error-message", classes="error-message")
                    
                    # Small spacer before buttons
                    yield Static("", classes="form-spacer")
                
                # Buttons
                with Horizontal(id="add-key-buttons"):
                    yield Button("Cancel", id="cancel-btn", classes="dialog-cancel-btn")
                    yield Button("Save", id="save-btn", classes="dialog-save-btn")
    
    def show_dialog(self, callback=None):
        """Show the dialog"""
        self.callback = callback
        self.add_class("visible")
        
        # Get references to key widgets
        self.save_button = self.query_one("#save-btn", Button)
        self.error_message_label = self.query_one("#error-message", Label)
        
        # Initialize validation state
        self._update_save_button_state()
        self._hide_error_message()
        
        # Focus on model input
        model_input = self.query_one("#model-input", Input)
        model_input.focus()
    
    def hide_dialog(self):
        """Hide the dialog"""
        self.remove_class("visible")
        
        # Clear inputs
        model_input = self.query_one("#model-input", Input)
        api_key_input = self.query_one("#api-key-input", Input)
        model_input.value = ""
        api_key_input.value = ""
        
        self.model_input_value = ""
        self.api_key_input_value = ""
        
        # Clear error message
        self._hide_error_message()
    
    def _update_save_button_state(self):
        """Update the save button enabled/disabled state based on input validation"""
        if self.save_button is None:
            return
            
        # Check if both fields have content
        model_has_content = bool(self.model_input_value.strip())
        api_key_has_content = bool(self.api_key_input_value.strip())
        
        # Enable button only if both fields have content
        should_enable = model_has_content and api_key_has_content
        self.save_button.disabled = not should_enable
    
    def _show_error_message(self, message):
        """Show an error message"""
        if self.error_message_label is None:
            return
        self.error_message_label.update(message)
        self.error_message_label.add_class("visible")
    
    def _hide_error_message(self):
        """Hide the error message"""
        if self.error_message_label is None:
            return
        self.error_message_label.update("")
        self.error_message_label.remove_class("visible")
    
    def _get_best_suggestion(self, text):
        """Get the best autocomplete suggestion for the input text"""
        if not text:
            return None
        
        text_upper = text.upper()
        
        # Find exact matches first
        for pattern in self.api_key_patterns:
            if pattern.startswith(text_upper):
                return pattern
        
        # Then find partial matches
        for pattern in self.api_key_patterns:
            if text_upper in pattern:
                return pattern
        
        return None
    
    @on(Input.Changed)
    def on_input_changed(self, event: Input.Changed) -> None:
        """Handle input changes"""
        if event.input.id == "model-input":
            self.model_input_value = event.value
            
            # Simple autocompletion: if user types enough, suggest completion
            if len(event.value) >= 3:
                suggestion = self._get_best_suggestion(event.value)
                if suggestion and suggestion != event.value.upper():
                    # Update placeholder to show suggestion
                    event.input.placeholder = f"Press Tab: {suggestion}"
                else:
                    event.input.placeholder = "OPENAI, ANTHROPIC..."
            else:
                event.input.placeholder = "OPENAI, ANTHROPIC..."
                
        elif event.input.id == "api-key-input":
            self.api_key_input_value = event.value
        
        # Update save button state and hide error messages when user types
        self._update_save_button_state()
        self._hide_error_message()
    
    @on(Key)
    def on_key_press(self, event: Key) -> None:
        """Handle key presses for autocompletion"""
        if event.key == "tab":
            # Check if we're in the model input
            try:
                model_input = self.query_one("#model-input", Input)
                if model_input.has_focus:
                    suggestion = self._get_best_suggestion(self.model_input_value)
                    if suggestion:
                        model_input.value = suggestion
                        self.model_input_value = suggestion
                        # Move focus to API key input
                        api_key_input = self.query_one("#api-key-input", Input)
                        api_key_input.focus()
                    event.prevent_default()
            except Exception:
                pass
    
    @on(Button.Pressed)
    def on_button_pressed(self, event: Button.Pressed) -> None:
        """Handle button presses"""
        if event.button.id == "cancel-btn":
            self.hide_dialog()
        elif event.button.id == "save-btn":
            self._save_api_key()
    
    def _save_api_key(self):
        """Save the new API key"""
        model = self.model_input_value.strip().upper()
        api_key = self.api_key_input_value.strip()
        
        # Validation with error messages as fallback
        if not model and not api_key:
            self._show_error_message("Both PROVIDER and API_KEY fields are required")
            return
        elif not model:
            self._show_error_message("PROVIDER field is required")
            return
        elif not api_key:
            self._show_error_message("API_KEY field is required")
            return
        
        # Hide error message if validation passes
        self._hide_error_message()
        
        # Format the key name properly
        if not model.endswith("_API_KEY") and model != "OLLAMA":
            key_name = f"{model}_API_KEY"
        else:
            key_name = model
        
        # Call the callback with the new key data
        if self.callback:
            self.callback(key_name, api_key)
        
        self.hide_dialog()


class AgentDoubleClicked(Message):
    """Message sent when an agent is double-clicked"""
    def __init__(self, agent_name: str) -> None:
        super().__init__()
        self.agent_name = agent_name
    bubble = True


class AgentActionDialog(Container):
    """Modal dialog for choosing agent action"""
    
    DEFAULT_CSS = """
    AgentActionDialog {
        display: none;
        layer: overlay;
        align: left top;
        offset: 0 4;
    }
    
    #agent-action-overlay {
        width: 32;
        height: 100%;
        background: transparent;
        align: left top;
    }
    
    #agent-action-content {
        width: 28;
        height: 16;
        background: #2e4f46;
        border: solid #c8ff00;
        align: center middle;
    }
    
    #agent-action-header {
        height: 3;
        width: 100%;
        background: #2e4f46;
        color: #c8ff00;
        text-align: center;
        text-style: bold;
        content-align: center middle;
        border-bottom: solid #c8ff00;
    }
    
    #agent-action-buttons {
        height: 11;
        width: 100%;
        background: #2e4f46;
        layout: vertical;
        padding: 1;
        align: center middle;
    }
    
    #agent-action-buttons .dialog-action-btn {
        width: 100%;
        height: 3;
        margin: 0 0 1 0;
        background: #529d86;
        border: solid #529d86;
        color: #181c1a;
        text-align: center;
        text-style: bold;
        content-align: center middle;
    }
    
    #agent-action-buttons .dialog-action-btn:hover {
        background: #3a6657;
        border: solid #529d86;
        color: #ffd97b;
    }
    """
    
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.agent_name = ""
        self.callback = None
    
    def compose(self) -> ComposeResult:
        with Container(id="agent-action-overlay"):
            with Container(id="agent-action-content"):
                yield Label("Choose Action", id="agent-action-header")
                with Container(id="agent-action-buttons"):
                    yield Button("Update T1", id="update-terminal-btn", classes="dialog-action-btn")
                    yield Button("New Terminal", id="new-terminal-btn", classes="dialog-action-btn")
                    yield Button("Cancel", id="cancel-action-btn", classes="dialog-action-btn")
    
    def show_dialog(self, agent_name: str, callback=None):
        """Show the dialog for the given agent"""
        self.agent_name = agent_name
        self.callback = callback
        header = self.query_one("#agent-action-header")
        header.update(f"Agent: {agent_name}")
        self.display = True
    
    def hide_dialog(self):
        """Hide the dialog"""
        self.display = False
        self.agent_name = ""
        self.callback = None
    
    @on(Button.Pressed)
    def handle_button_pressed(self, event: Button.Pressed) -> None:
        """Handle button press in the dialog"""
        if event.button.id == "update-terminal-btn":
            if self.callback:
                self.callback("update", self.agent_name)
        elif event.button.id == "new-terminal-btn":
            if self.callback:
                self.callback("new", self.agent_name)
        elif event.button.id == "cancel-action-btn":
            # Just close the dialog without any action
            pass
        
        self.hide_dialog()
        event.stop()


class TeamSelected(Message):
    """Message sent when a preconfigured team is selected."""
    def __init__(self, team_name: str, agents: list[str]) -> None:
        super().__init__()
        self.team_name = team_name
        self.agents = agents  # Desired agents ordered for terminals 1..N
    bubble = True


class AgentListItem(ListItem):
    """Custom ListItem for agent display"""
    def __init__(self, text: str, agent_name: str, agent_index: int) -> None:
        super().__init__()
        self.text = text
        self.agent_name = agent_name
        self.agent_index = agent_index
    
    def compose(self) -> ComposeResult:
        """Compose the list item content"""
        # Use Label instead of Static for better text rendering
        yield Label(self.text, classes="agent-item-text")


class QueueListItem(ListItem):
    """Custom ListItem for queue display"""
    def __init__(self, text: str, queue_index: int, full_prompt: str) -> None:
        super().__init__()
        self.text = text
        self.queue_index = queue_index
        self.full_prompt = full_prompt
    
    def compose(self) -> ComposeResult:
        """Compose the list item content"""
        # Use Label instead of Static for better text rendering
        yield Label(self.text, classes="queue-item-text")


class Sidebar(Container):
    """Modern sidebar with agent list and navigation"""

    DEFAULT_CSS = """
    Sidebar {
        width: 32;
        background: $surface;
        border-right: solid #529d86;
        dock: left;
    }
    
    Sidebar > Vertical {
        height: 100%;
    }
    
    Sidebar #sidebar-content {
        height: 100%;
    }
    
    Sidebar .sidebar-header {
        height: 4;
        background: #2e4f46;
        color: #529d86;
        padding: 1 2;
        text-align: center;
        text-style: bold;
        border-bottom: solid #529d86;
        content-align: center middle;
        text-opacity: 1.0;
    }
    
    Sidebar TabbedContent {
        height: 1fr;
        background: $surface;
    }
    
    Sidebar TabbedContent Tabs {
        background: #2e4f46;
        border-bottom: solid #529d86;
        height: 4;
        min-height: 4;
        max-height: 4;
        dock: top;
        width: 100%;
        padding: 1 1 0 1;
    }
    
    /* Properly aligned square tabs */
    Sidebar TabbedContent Tab {
        height: 3;
        max-height: 3;
        background: #2e4f46;
        padding: 0 2;
        margin: 0 1 0 0;
        border: none;
        color: #c8ff00 !important;
        text-opacity: 1.0 !important;
        content-align: center middle;
        text-align: center;
    }
    
    /* Active tab - with highlight effect */
    Sidebar TabbedContent Tab.-active {
        color: #ffd97b !important;
        background: #3a6657;
        text-style: bold;
        text-opacity: 1.0 !important;
        border: none;
        margin: 0 1 0 0;
        height: 3;
    }
    
    /* Remove hover effect - keep default appearance */
    Sidebar TabbedContent Tab:hover {
        color: #c8ff00 !important;
        background: #2e4f46;
        text-opacity: 1.0 !important;
        border: none;
    }
    
    /* Force all tab text to be visible and centered */
    Sidebar Tab * {
        color: #c8ff00 !important;
        text-opacity: 1.0 !important;
        background: transparent !important;
        text-align: center !important;
        width: 100%;
        height: 100%;
        content-align: center middle;
    }
    
    Sidebar Tab.-active * {
        color: #ffd97b !important;
        text-opacity: 1.0 !important;
    }
    
    Sidebar Tab:hover * {
        color: #c8ff00 !important;
        text-opacity: 1.0 !important;
    }
    
    /* Ensure tab labels are properly displayed */
    Sidebar TabbedContent Tab Label {
        color: #c8ff00 !important;
        text-opacity: 1.0 !important;
        background: transparent !important;
        width: 100%;
        height: 100%;
        content-align: center middle;
        text-align: center;
    }
    
    Sidebar TabbedContent Tab.-active Label {
        color: #ffd97b !important;
        text-opacity: 1.0 !important;
    }
    
    Sidebar TabbedContent TabPane {
        padding: 0;
        margin: 0;
        background: $surface;
    }
    
    Sidebar .sidebar-list {
        height: 1fr;
        background: transparent;
        padding: 0;
        scrollbar-color: #529d86;
        scrollbar-background: #2e4f46;
        overflow-y: auto;
        scrollbar-size: 1 1;
    }
    
    Sidebar .agent-button {
        width: 100%;
        height: 3;
        margin: 0 1 1 1;
        background: #2e4f46;
        border: solid #529d86;
        color: #c8ff00 !important;
        padding: 1 2;
        text-align: left;
        min-height: 3;
        text-opacity: 1.0 !important;
    }
    
    Sidebar .agent-button:hover {
        background: #181c1a;
        border: solid #529d86;
        color: #ffd97b !important;
    }
    
    Sidebar .queue-button {
        width: 100%;
        height: 3;
        margin: 0 1 1 1;
        background: #2e4f46;
        border: solid #529d86;
        color: #ffd97b !important;
        padding: 1 2;
        text-align: left;
        min-height: 3;
        text-opacity: 1.0 !important;
    }
    
    Sidebar .queue-button:hover {
        background: #181c1a;
        border: solid #529d86;
        color: #c8ff00 !important;
    }

    /* Teams */
    Sidebar .team-button {
        width: 100%;
        height: 3;
        margin: 0 1 1 1;
        background: #2e4f46;
        border: solid #529d86;
        color: #ffd97b !important;
        padding: 1 2;
        text-align: left;
        min-height: 3;
        text-opacity: 1.0 !important;
    }

    Sidebar .team-button:hover {
        background: #181c1a;
        border: solid #529d86;
        color: #c8ff00 !important;
    }
    
    
    Sidebar .agent-separator {
        width: 100%;
        height: 1;
        background: #0e0f11;
        margin: 0 1;
        border: none;
    }
    
    
    Sidebar .queue-list {
        height: 1fr;
        background: transparent;
        padding: 0;
        scrollbar-color: #529d86;
        scrollbar-background: #2e4f46;
        overflow-y: auto;
        scrollbar-size: 1 1;
    }
    
    
    Sidebar .queue-separator {
        width: 100%;
        height: 1;
        background: #0e0f11;
        margin: 0 1;
        border: none;
    }
    
    Sidebar .queue-empty {
        padding: 2;
        text-align: center;
        color: #8fe6c3;
        text-style: italic;
    }

    /* Teams title */
    Sidebar .teams-title {
        padding: 1 2;
        margin: 1 1 0 1;
        background: transparent;
        color: #ffd97b;
        text-style: bold;
    }
    
    Sidebar .state-list {
        height: 1fr;
        background: transparent;
        padding: 0;
        scrollbar-color: #529d86;
        scrollbar-background: #2e4f46;
        overflow-y: auto;
        scrollbar-size: 1 1;
    }
    
    Sidebar .state-section {
        width: 100%;
        background: #2e4f46;
        padding: 1;
        margin-bottom: 1;
    }
    
    Sidebar .state-section-title {
        color: #529d86;
        text-style: bold;
        margin-bottom: 1;
    }
    
    Sidebar .state-content {
        width: 100%;
        padding: 0;
        background: transparent;
        overflow: hidden;
        max-width: 30;
    }
    
    /* Special styling for history graph section to match sidebar */
    Sidebar #history_section {
        background: #2e4f46;
        border: none;
        margin: 0;
        padding: 1;
    }
    
    Sidebar #history_section .state-content {
        padding: 0 1;
        margin: 0;
        color: #e0f1e7;
    }
    
    /* Keys section styling */
    Sidebar .keys-list {
        height: 1fr;
        background: transparent;
        padding: 0;
        scrollbar-color: #529d86;
        scrollbar-background: #2e4f46;
        overflow-y: auto;
        scrollbar-size: 1 1;
    }
    
    Sidebar .key-item-header {
        color: #c8ff00;
        text-style: bold;
        margin: 0 0 1 0;
        padding: 0 0 0 1;
    }
    
    Sidebar .key-add-button {
        width: 100%;
        height: 3;
        margin: 0 1 1 1;
        background: #2e4f46;
        border: solid #529d86;
        color: #c8ff00;
        padding: 1 2;
        text-align: center;
        min-height: 3;
        text-style: bold;
    }
    
    Sidebar .key-add-button:hover {
        background: #181c1a;
        border: solid #529d86;
        color: #ffd97b !important;
    }
    
    Sidebar .keys-empty {
        padding: 2;
        text-align: center;
        color: #8fe6c3;
        text-style: italic;
    }
    
    Sidebar .key-value-display {
        width: 100%;
        height: 1;
        margin: 0 0 1 0;
        color: $text;
        padding: 0 0 0 1;
        max-width: 30;
    }
    
    Sidebar .key-edit-button {
        width: 56% !important;
        height: 3 !important;
        margin: 0 1 1 0 !important;
        background: #2e4f46 !important;
        border: solid #529d86 !important;
        color: #c8ff00 !important;
        padding: 0 !important;
        text-align: center !important;
        text-style: bold !important;
        max-width: 56% !important;
        min-width: 56% !important;
        content-align: center middle !important;
    }
    
    Sidebar .key-edit-button:hover {
        background: #181c1a;
        border: solid #529d86;
        color: #ffd97b !important;
    }
    
    Sidebar .key-delete-button {
        width: 44% !important;
        height: 3 !important;
        margin: 0 0 1 1 !important;
        background: #2e4f46 !important;
        border: solid #529d86 !important;
        color: #ff6b6b !important;
        padding: 0 !important;
        text-align: center !important;
        text-style: bold !important;
        max-width: 44% !important;
        min-width: 44% !important;
        content-align: center middle !important;
    }
    
    Sidebar .key-delete-button:hover {
        background: #181c1a;
        border: solid #529d86;
        color: #ffffff !important;
    }
    
    
    Sidebar .key-buttons-container-simple {
        height: 3;
        margin: 0;
        padding: 0;
        width: 100%;
        layout: horizontal;
        align: left middle;
    }
    
    /* Container with expanded width for edit mode */
    Sidebar .key-buttons-container-simple.editing-mode {
        width: 101% !important;
        margin: 0 0 1 -1 !important;
        padding: 0 !important;
    }
    
    Sidebar .key-save-button-simple {
        width: 48%;
        height: 3;
        margin: 0 0 1 1;
        background: #2e4f46;
        border: solid #529d86;
        color: #c8ff00;
        padding: 1;
        text-align: center;
        text-style: bold;
        max-width: 14;
    }
    
    Sidebar .key-save-button-simple:hover {
        background: #181c1a;
        border: solid #529d86;
        color: #ffd97b !important;
    }
    
    /* Edit mode specific styles - Back button (50% width, aligned left) */
    Sidebar .key-edit-button.editing-mode {
        width: 50% !important;
        height: 3 !important;
        margin: 0 0 1 0 !important;
        background: #529d86 !important;
        border: solid #529d86 !important;
        color: #181c1a !important;
        padding: 0 !important;
        text-align: center !important;
        text-style: bold !important;
        max-width: 50% !important;
        min-width: 50% !important;
        content-align: center middle !important;
    }
    
    Sidebar .key-edit-button.editing-mode:hover {
        background: #3a6657 !important;
        border: solid #529d86 !important;
        color: #ffd97b !important;
    }
    
    /* Edit mode specific styles - Save button (50% width, aligned right) */
    Sidebar .key-save-button-simple.editing-mode {
        width: 50% !important;
        height: 3 !important;
        margin: 0 0 1 0 !important;
        background: #c8ff00 !important;
        border: solid #c8ff00 !important;
        color: #181c1a !important;
        padding: 0 !important;
        text-align: center !important;
        text-style: bold !important;
        max-width: 50% !important;
        min-width: 50% !important;
        content-align: center middle !important;
    }
    
    Sidebar .key-save-button-simple.editing-mode:hover {
        background: #a6d400 !important;
        border: solid #c8ff00 !important;
        color: #0f1311 !important;
    }

    Sidebar .key-item-container {
        width: 100%;
        height: auto;
        margin: 0 1 1 1;
        padding: 1 1 1 0;
        background: #2e4f46;
        border: solid #529d86;
    }
    
    
    Sidebar .key-buttons-container {
        height: 3;
        margin: 0;
        padding: 0;
        width: 100%;
        max-width: 30;
    }
    
    Sidebar .key-save-button {
        width: 50%;
        height: 3;
        margin: 0 0 0 1;
        background: #2e4f46;
        border: solid #529d86;
        color: #c8ff00;
        padding: 0 1;
        text-align: center;
        text-style: bold;
        max-width: 12;
    }
    
    Sidebar .key-save-button:hover {
        background: #181c1a;
        border: solid #529d86;
        color: #ffd97b !important;
    }

    /* Inputs in Keys section - match palette */
    Sidebar .keys-list Input {
        background: #2e4f46;
        border: solid #529d86;
        color: #c8ff00;
        padding: 0 1;
    }

    Sidebar .keys-list Input:focus {
        background: #181c1a;
        border: solid #529d86;
        color: #ffd97b;
    }
    """

    visible = reactive(False)

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.agent_names = []
        self.last_click_time = 0
        self.last_clicked_index = -1
        self.double_click_threshold = 0.5  # 500ms threshold for double-click
        self._agents_cache = {}  # Cache agent objects for tooltip
        self._stats_cache = {}  # Cache for stats to avoid flicker
        self._stats_widgets = {}  # Store widget references
        self._refresh_counter = 0  # Counter to ensure unique IDs
        # Teams
        self._teams = self._build_preconfigured_teams()

    def compose(self) -> ComposeResult:
        """Compose the sidebar"""
        with Vertical(id="sidebar-content"):
            # Tabbed content
            with TabbedContent(initial="stats"):
                with TabPane("Teams", id="teams"):
                    self.button_container = VerticalScroll(classes="sidebar-list")
                    yield self.button_container
                with TabPane("Queue", id="queue"):
                    self.queue_container = VerticalScroll(classes="queue-list")
                    yield self.queue_container
                with TabPane("Stats", id="stats"):
                    self.state_container = VerticalScroll(classes="state-list")
                    yield self.state_container
                with TabPane("Keys", id="keys"):
                    self.keys_container = VerticalScroll(classes="keys-list")
                    yield self.keys_container
        
        # Add the modal dialog for adding API keys
        self.add_key_dialog = AddApiKeyDialog()
        yield self.add_key_dialog
        
        # Add the modal dialog for agent actions
        self.agent_action_dialog = AgentActionDialog()
        yield self.agent_action_dialog

    def on_mount(self) -> None:
        """Initialize sidebar when mounted"""
        self.load_agents()
        self.refresh_queue()
        self.refresh_state()
        # Initial load of keys (only once at startup)
        self.call_after_refresh(self.refresh_keys)
        # Update queue every 2 seconds
        self.set_interval(2.0, self.refresh_queue)
        # Update state every 0.5 seconds
        self.set_interval(0.5, self.refresh_state)
        # Update agent list every 5 seconds to catch new agents
        self.set_interval(5.0, self.refresh_agents)
        # Keys will only refresh manually - no automatic refresh

    def load_agents(self) -> None:
        """Load teams into the list (individual agents removed from sidebar)"""
        # Increment refresh counter to ensure unique IDs
        self._refresh_counter += 1

        # Clear existing buttons - ensure all children are removed
        for child in list(self.button_container.children):
            child.remove()

        # Render Teams section only (no individual agents)
        self._render_teams_in_sidebar()
    
    def refresh_agents(self) -> None:
        """Refresh the agent list to include new agents"""
        self.load_agents()

    def _build_preconfigured_teams(self) -> list[dict]:
        """Return static list of preconfigured teams.

        Each team has:
        - name: display label
        - agents: list of agent identifiers desired for terminals 1..N
        """
        # Ensure these agents exist in sidebar's whitelist
        teams = [
            {
                "name": "Team 4: 2 Red + 2 Bug",
                "agents": [
                    "redteam_agent",
                    "redteam_agent",
                    "bug_bounter_agent",
                    "bug_bounter_agent",
                ],
            },
            {
                "name": "Team 4: 1 Red (T1) + 3 Bug",
                "agents": [
                    "redteam_agent",
                    "bug_bounter_agent",
                    "bug_bounter_agent",
                    "bug_bounter_agent",
                ],
            },
            {
                "name": "Team 4: 2 Red + 2 Blue",
                "agents": [
                    "redteam_agent",
                    "redteam_agent",
                    "blueteam_agent",
                    "blueteam_agent",
                ],
            },
            {
                "name": "Team 4: 2 Blue + 2 Bug",
                "agents": [
                    "blueteam_agent",
                    "blueteam_agent",
                    "bug_bounter_agent",
                    "bug_bounter_agent",
                ],
            },
            {
                "name": "Team 4: Red + Blue + Retester + Bug",
                "agents": [
                    "redteam_agent",
                    "blueteam_agent",
                    "retester_agent",
                    "bug_bounter_agent",
                ],
            },
            {
                "name": "Team 4: 2 Red + 2 Retester",
                "agents": [
                    "redteam_agent",
                    "redteam_agent",
                    "retester_agent",
                    "retester_agent",
                ],
            },
            {
                "name": "Team 4: 2 Blue + 2 Retester",
                "agents": [
                    "blueteam_agent",
                    "blueteam_agent",
                    "retester_agent",
                    "retester_agent",
                ],
            },
            {
                "name": "Team 4: 4 Red",
                "agents": [
                    "redteam_agent",
                    "redteam_agent",
                    "redteam_agent",
                    "redteam_agent",
                ],
            },
            {
                "name": "Team 4: 4 Blue",
                "agents": [
                    "blueteam_agent",
                    "blueteam_agent",
                    "blueteam_agent",
                    "blueteam_agent",
                ],
            },
            {
                "name": "Team 4: 4 Bug",
                "agents": [
                    "bug_bounter_agent",
                    "bug_bounter_agent",
                    "bug_bounter_agent",
                    "bug_bounter_agent",
                ],
            },
            {
                "name": "Team 4: 4 Retester",
                "agents": [
                    "retester_agent",
                    "retester_agent",
                    "retester_agent",
                    "retester_agent",
                ],
            },
        ]
        # Generate compact display names
        for i, team in enumerate(teams, 1):
            compact_name = self._generate_compact_team_name(i, team["agents"])
            team["name"] = compact_name
        return teams
    
    def _generate_compact_team_name(self, team_number: int, agents: list[str]) -> str:
        """Generate compact team name by counting agents and removing _agent suffix.
        Uses full names when they fit, abbreviated names when needed."""
        from collections import Counter
        agent_counts = Counter(agents)
        
        # Map full agent names to different length versions
        agent_versions = {
            "redteam_agent": ["redteam_agent", "red"],
            "blueteam_agent": ["blueteam_agent", "blue"],
            "bug_bounter_agent": ["bug_bounter", "bug"],
            "retester_agent": ["retester_agent", "retest"],
        }
        
        # Max width for sidebar button text (accounting for padding and borders)
        max_width = 28
        
        # Helper function to build parts list
        def build_parts(use_short: bool):
            parts = []
            for agent_name, count in agent_counts.items():
                versions = agent_versions.get(agent_name, [agent_name.replace("_agent", "")])
                name = versions[-1] if use_short and len(versions) > 1 else versions[0]
                parts.append(f"{count} {name}" if count > 1 else name)
            return parts
        
        # Try with full names first (without _agent suffix)
        parts_full = build_parts(use_short=False)
        full_text = f"#{team_number}: {' + '.join(parts_full)}"
        
        # If it fits, use full names
        if len(full_text) <= max_width:
            return full_text
        
        # Otherwise, use abbreviated names
        parts_short = build_parts(use_short=True)
        return f"#{team_number}: {' + '.join(parts_short)}"

    def _render_teams_in_sidebar(self) -> None:
        """Render team selection buttons as the main content of the Teams tab."""
        # Render teams directly without header or initial separator
        for idx, team in enumerate(self._teams):
            btn = Button(team["name"], id=f"team-{self._refresh_counter}-{idx}", classes="team-button")
            btn.team_name = team["name"]
            btn.team_agents = list(team["agents"])  # attach payload
            
            # Add tooltip showing agent composition
            btn.tooltip = self._get_team_tooltip(team)
            
            self.button_container.mount(btn)
            # Divider after each team (except the last one)
            if idx < len(self._teams) - 1:
                self.button_container.mount(Static("", classes="agent-separator"))
    
    def _get_team_tooltip(self, team: dict) -> str:
        """Generate tooltip text showing team composition"""
        agents = team.get("agents", [])
        if not agents:
            return team.get("name", "Team")
        
        # Count agent occurrences
        from collections import Counter
        agent_counts = Counter(agents)
        
        # Build descriptive title with agent counts
        agent_parts = []
        for agent_name, count in agent_counts.items():
            agent_parts.append(f"{count} {agent_name}")
        
        # Extract team number from name (e.g., "#2" from "#2: 2 red + 2 bug")
        team_name = team['name']
        team_number = team_name.split(':')[0] if ':' in team_name else team_name.split()[0]
        
        # Create title with team number and full agent names (no abbreviations)
        title = f"{team_number}: {' + '.join(agent_parts)}"
        
        # Build tooltip with descriptive title and terminal assignments
        tooltip_lines = [
            f"[bold #ffd97b]{title}[/bold #ffd97b]",
            ""
        ]
        
        for i, agent in enumerate(agents, start=1):
            tooltip_lines.append(f"[#529d86]T{i}:[/#529d86] [white]{agent}[/white]")
        
        return "\n".join(tooltip_lines)

    def toggle(self) -> None:
        """Toggle sidebar visibility"""
        self.visible = not self.visible

    def watch_visible(self, value: bool) -> None:
        """Watch visibility changes"""
        if value:
            self.add_class("sidebar-visible")
            self.display = True
        else:
            self.remove_class("sidebar-visible")
            self.display = False

    def get_selected_agent(self) -> str:
        """Get the currently selected agent name"""
        list_view = self.query_one("#agent-list", ListView)
        index = list_view.index
        if index is not None and 0 <= index < len(self.agent_names):
            return self.agent_names[index]
        return None
    
    def _handle_agent_action(self, action: str, agent_name: str):
        """Handle the selected action from the agent modal"""
        if action == "update":
            # Update current terminal with the selected agent
            from cai.tui.cai_terminal import CAITerminal
            app = self.app
            if isinstance(app, CAITerminal):
                asyncio.create_task(app._process_command(f"/agent select {agent_name}"))
        elif action == "new":
            # Create new terminal with the selected agent
            self.post_message(AgentDoubleClicked(agent_name))

    
    @on(Button.Pressed)
    def handle_button_pressed(self, event: Button.Pressed) -> None:
        """Handle button press"""
        button = event.button

        # Handle team buttons
        if hasattr(button, 'team_agents'):
            try:
                team_name = getattr(button, 'team_name', 'Team')
                agents = list(getattr(button, 'team_agents'))
                # Emit event for the app to handle orchestration
                self.post_message(TeamSelected(team_name, agents))
            except Exception:
                pass
            return

        # Handle agent buttons
        if hasattr(button, 'agent_name'):
            agent_name = button.agent_name
            current_time = time.time()

            # If this agent is a parallel pattern, translate to a team selection
            try:
                agents_map = self._agents_cache or {}
                agent_obj = agents_map.get(agent_name)
                if agent_obj is not None and hasattr(agent_obj, '_pattern'):
                    pattern = getattr(agent_obj, '_pattern')
                    # Check parallel type
                    pattern_type_value = getattr(getattr(pattern, 'type', None), 'value', str(getattr(pattern, 'type', '')))
                    if str(pattern_type_value) == 'parallel':
                        # Build list of agent names from pattern configs/agents
                        team_agents: list[str] = []
                        if hasattr(pattern, 'configs') and pattern.configs:
                            for cfg in pattern.configs:
                                name = getattr(cfg, 'agent_name', None)
                                if not name and isinstance(cfg, str):
                                    name = cfg
                                if name:
                                    team_agents.append(name)
                        elif hasattr(pattern, 'agents') and pattern.agents:
                            for a in pattern.agents:
                                name = getattr(a, 'name', None) or str(a)
                                team_agents.append(name)
                        # Emit team selection to open/reuse terminals
                        if team_agents:
                            self.post_message(TeamSelected(agent_name, team_agents))
                            return
            except Exception:
                # Fallback to normal behavior if detection fails
                pass

            # Show modal with agent action options
            self.agent_action_dialog.show_dialog(agent_name, self._handle_agent_action)
        
        # Handle queue item buttons
        elif hasattr(button, 'queue_index'):
            # This is a queue item - remove it
            queue_index = button.queue_index
            try:
                from cai.repl.commands.queue import get_queue
                from cai.tui.core.prompt_queue import PROMPT_QUEUE
                
                # Remove from the appropriate terminal queue
                if os.getenv("CAI_TUI_MODE") == "true":
                    from cai.tui.core.terminal_queue import TERMINAL_QUEUE_MANAGER
                    # Find which terminal this queue item belongs to
                    all_queues = TERMINAL_QUEUE_MANAGER.get_all_queues_status()
                    current_index = 0
                    for terminal_num in sorted(all_queues.keys()):
                        terminal_queue = all_queues[terminal_num]
                        if queue_index < current_index + len(terminal_queue['prompts']):
                            # Found the terminal, remove from its queue
                            terminal_index = queue_index - current_index
                            if terminal_num in TERMINAL_QUEUE_MANAGER._queues:
                                queue = TERMINAL_QUEUE_MANAGER._queues[terminal_num]
                                if terminal_index < len(queue._queue):
                                    removed_item = queue._queue.pop(terminal_index)
                                    break
                        current_index += len(terminal_queue['prompts'])
                else:
                    # Use global queue
                    if queue_index < len(PROMPT_QUEUE._queue):
                        removed_item = PROMPT_QUEUE._queue.pop(queue_index)
                    
                    # Show feedback in main terminal
                    from cai.tui.cai_terminal import CAITerminal
                    app = self.app
                    if isinstance(app, CAITerminal):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[yellow]🗑️ Removed from queue: {removed_item.prompt[:30]}...[/yellow]")
                    
                    # Refresh the queue display
                    self.refresh_queue()
            except Exception as e:
                pass
    
    def _get_agent_tooltip(self, agent_name: str) -> str:
        """Get tooltip text with agent information"""
        if agent_name in self._agents_cache:
            agent = self._agents_cache[agent_name]
            tooltip_parts = [f"[bold bright_green]{agent_name}[/bold bright_green]"]
            
            # Add description if available
            if hasattr(agent, 'description') and agent.description:
                tooltip_parts.append(f"\n\n[bright_cyan]Description:[/bright_cyan] [white]{agent.description}[/white]")
            
            # Add system prompt preview if available
            if hasattr(agent, 'instructions') and agent.instructions:
                instructions = agent.instructions
                # If it's a callable, show that it's dynamic
                if callable(instructions):
                    if hasattr(instructions, '__name__'):
                        tooltip_parts.append(f"\n\n[bright_cyan]System Prompt:[/bright_cyan] [yellow]<Dynamic - {instructions.__name__}>[/yellow]")
                    else:
                        tooltip_parts.append(f"\n\n[bright_cyan]System Prompt:[/bright_cyan] [yellow]<Dynamic Function>[/yellow]")
                else:
                    # It's a string, show the actual prompt
                    prompt_text = str(instructions)
                    if len(prompt_text) > 400:
                        prompt_text = prompt_text[:400] + "..."
                    tooltip_parts.append(f"\n\n[bright_cyan]System Prompt:[/bright_cyan]\n[white]{prompt_text}[/white]")
            
            # Add tools count if available
            if hasattr(agent, 'tools') and agent.tools:
                tool_count = len(agent.tools)
                tooltip_parts.append(f"\n\n[bright_cyan]Tools:[/bright_cyan] [bright_green]{tool_count}[/bright_green] available")
            
            # Add handoffs if available
            if hasattr(agent, 'handoffs') and agent.handoffs:
                handoff_count = len(agent.handoffs)
                tooltip_parts.append(f"\n\n[bright_cyan]Handoffs:[/bright_cyan] [bright_green]{handoff_count}[/bright_green] available")
            
            return "".join(tooltip_parts)
        
        # Fallback to just the full name if no agent info
        return f"[bright_green]{agent_name}[/bright_green]"
    
    def refresh_queue(self) -> None:
        """Refresh the queue display"""
        # Import here to avoid circular import
        import os
        try:
            # In TUI mode, show terminal-specific queues
            if os.getenv("CAI_TUI_MODE") == "true":
                from cai.tui.core.terminal_queue import TERMINAL_QUEUE_MANAGER
                # Get all terminal queues
                all_queues = TERMINAL_QUEUE_MANAGER.get_all_queues_status()
                # Combine all queues for display
                queue = []
                for terminal_num in sorted(all_queues.keys()):
                    terminal_queue = all_queues[terminal_num]
                    for prompt_info in terminal_queue['prompts']:
                        queue_item = {
                            'prompt': f"[T{terminal_num}] {prompt_info['prompt']}",
                            'timestamp': prompt_info.get('timestamp', ''),
                            'priority': prompt_info.get('priority', 0),
                            'terminal': terminal_num
                        }
                        queue.append(queue_item)
            else:
                from cai.repl.commands.queue import get_queue
                queue = get_queue()
        except ImportError:
            queue = []
        
        # Clear existing items
        self.queue_container.remove_children()
        
        if not queue:
            # Show empty message
            empty_msg = Static("Queue is empty", classes="queue-empty")
            self.queue_container.mount(empty_msg)
        else:
            # Display queue items as buttons matching agent style
            for i, item in enumerate(queue, 1):
                # Get prompt text and truncate if needed
                prompt_text = item["prompt"]
                
                # Add icon based on prompt type
                if prompt_text.startswith("/"):
                    icon = "⚡"  # Command
                elif prompt_text.startswith("$"):
                    icon = "💻"  # Shell
                else:
                    icon = "💬"  # Chat
                
                # Extract terminal prefix if present
                terminal_prefix = ""
                actual_prompt = prompt_text
                if prompt_text.startswith("[T") and "] " in prompt_text:
                    bracket_end = prompt_text.find("] ")
                    terminal_prefix = prompt_text[:bracket_end+1]
                    actual_prompt = prompt_text[bracket_end+2:]
                
                # Truncate prompt to max characters
                max_prompt_length = 10  # Increased for better readability
                display_text = actual_prompt
                if len(actual_prompt) > max_prompt_length:
                    display_text = actual_prompt[:max_prompt_length-2] + ".."
                
                # Escape any markup characters in the display text
                display_text = display_text.replace("[", "\\[").replace("]", "\\]")
                
                # Format button text with terminal prefix, number and delete button
                if terminal_prefix:
                    button_text = f"{icon} #{i}: {terminal_prefix} {display_text} [red]✕[/red]"
                else:
                    button_text = f"{icon} #{i}: {display_text} [red]✕[/red]"
                
                # Use Button widget for queue display
                from textual.widgets import Button
                import time
                # Create unique ID using timestamp to avoid conflicts
                unique_id = f"queue-{i}-{int(time.time() * 1000)}"
                queue_button = Button(button_text, id=unique_id, classes="queue-button", variant="warning")
                queue_button.queue_index = i - 1
                queue_button.full_prompt = prompt_text
                self.queue_container.mount(queue_button)
                
                # Add separator line after each item (except the last one)
                if i < len(queue):
                    separator = Static("", classes="queue-separator")
                    self.queue_container.mount(separator)
    
    def refresh_state(self) -> None:
        """Refresh the state display with minimal visual updates"""
        from textual.containers import Vertical
        import os
        
        try:
            from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
            from cai.repl.commands.parallel import PARALLEL_CONFIGS
            from cai.sdk.agents.parallel_isolation import PARALLEL_ISOLATION
            
            # Collect current state data
            current_state = {}
            
            # Get agent info - prefer session manager for multi-agent accuracy
            agents_info = {}
            agent_costs_snapshot: dict[str, float] = {}
            terminal_costs_snapshot: dict[str, float] = {}
            all_histories = {}
            has_isolated_histories = False
            terminal_labels: dict[str, str] = {}
            active_agent_ids: set[str] = set()
            
            # Try to get info from TUI session manager first (for multi-agent mode)
            try:
                from cai.tui.cai_terminal import CAITerminal
                app = self.app
                if isinstance(app, CAITerminal) and hasattr(app, 'session_manager') and app.session_manager:
                    session_manager = app.session_manager
                    
                    # Get info from active terminal runners
                    for terminal_num, runner in session_manager.terminal_runners.items():
                        if runner.agent:
                            # Get agent name and ID
                            agent_name = runner.config.agent_name
                            agent_id = f"T{terminal_num}"
                            
                            # Always try to get the actual ID from the agent's model first
                            if hasattr(runner.agent, 'model') and hasattr(runner.agent.model, 'agent_id'):
                                # Use the agent's actual ID if available
                                agent_id = runner.agent.model.agent_id
                            elif runner.config.is_parallel and runner.config.parallel_config:
                                # For parallel agents without model ID, use their config ID
                                agent_id = runner.config.parallel_config.id or f"P{terminal_num}"
                                agent_name = runner.config.parallel_config.agent_name
                            
                            # Get message count from agent's model history
                            msg_count = 0
                            
                            # Get additional info if available
                            if runner.agent and hasattr(runner.agent, 'model') and hasattr(runner.agent.model, 'message_history'):
                                # Use model's message history for more accurate count
                                msg_count = len(runner.agent.model.message_history)
                                all_histories[f"{agent_name} [{agent_id}]"] = runner.agent.model.message_history
                            
                            display_name = f"{agent_name} [{agent_id}]"
                            agents_info[display_name] = msg_count
                            if agent_id:
                                active_agent_ids.add(agent_id)

                            # Track cost directly from the agent model when available
                            cost_value = None
                            agent_model = getattr(runner.agent, 'model', None)
                            if agent_model is not None:
                                try:
                                    cost_value = float(getattr(agent_model, 'total_cost', 0.0) or 0.0)
                                except Exception:
                                    cost_value = None
                            if cost_value is not None:
                                agent_costs_snapshot[display_name] = cost_value

                            # Build terminal label for cost display
                            friendly_agent = agent_name.strip()
                            if "[" in friendly_agent:
                                friendly_agent = friendly_agent.split("[")[0].strip()
                            friendly_label = f"T{terminal_num}"
                            if friendly_agent:
                                friendly_label = f"{friendly_label} · {friendly_agent}"

                            term_id = getattr(runner.config, "terminal_id", None)
                            if term_id:
                                terminal_labels[term_id] = friendly_label
                                if cost_value is not None:
                                    terminal_costs_snapshot[term_id] = cost_value
                            predictable_id = f"terminal-{terminal_num}"
                            terminal_labels.setdefault(predictable_id, friendly_label)
                            if cost_value is not None:
                                terminal_costs_snapshot[predictable_id] = cost_value
                            terminal_labels.setdefault(f"T{terminal_num}", friendly_label)
                            if cost_value is not None:
                                terminal_costs_snapshot[f"T{terminal_num}"] = cost_value
                    
                    # Mark that we have multi-agent info
                    if len(agents_info) > 1:
                        has_isolated_histories = True
            except Exception:
                pass
            
            try:
                from cai.util import COST_TRACKER

                for state in getattr(COST_TRACKER, "agent_cost_states", {}).values():
                    display_name_state = state.get("display_name")
                    total_cost_state = float(state.get("total_cost", 0.0) or 0.0)
                    if display_name_state and total_cost_state > 0:
                        agent_costs_snapshot[display_name_state] = total_cost_state

                    term_id_state = state.get("terminal_id")
                    if term_id_state and total_cost_state > 0:
                        terminal_costs_snapshot[term_id_state] = total_cost_state
                        if term_id_state not in terminal_labels:
                            term_label = None
                            if term_id_state.startswith("terminal-"):
                                try:
                                    term_number = term_id_state.split("-", 1)[1]
                                except Exception:
                                    term_number = None
                                base_name = (
                                    display_name_state.split("[")[0].strip()
                                    if display_name_state
                                    else "Agent"
                                )
                                if term_number:
                                    term_label = (
                                        f"T{term_number} · {base_name}"
                                        if base_name
                                        else f"T{term_number}"
                                    )
                            if not term_label:
                                term_label = display_name_state or term_id_state
                            terminal_labels[term_id_state] = term_label

                        if term_id_state.startswith("terminal-"):
                            try:
                                term_number = term_id_state.split("-", 1)[1]
                            except Exception:
                                term_number = None
                            if term_number:
                                shorthand = f"T{term_number}"
                                terminal_labels.setdefault(
                                    shorthand,
                                    terminal_labels.get(term_id_state, shorthand),
                                )
            except Exception:
                pass
            
            # Fallback to original method if no session manager info
            if not agents_info:
                all_histories = AGENT_MANAGER.get_all_histories()
                has_isolated_histories = len(PARALLEL_ISOLATION._isolated_histories) > 0
                
                if PARALLEL_CONFIGS and has_isolated_histories:
                    for idx, config in enumerate(PARALLEL_CONFIGS, 1):
                        agent_id = config.id or f"P{idx}"
                        isolated_history = PARALLEL_ISOLATION.get_isolated_history(agent_id)
                        if isolated_history is None:
                            isolated_history = []
                        display_name = f"{config.agent_name} [{agent_id}]"
                        agents_info[display_name] = len(isolated_history)
                        if agent_id:
                            active_agent_ids.add(agent_id)
                else:
                    for display_name, history in all_histories.items():
                        agents_info[display_name] = len(history)
                        if "[" in display_name and "]" in display_name:
                            try:
                                aid = display_name[display_name.rindex("[") + 1 : display_name.rindex("]")]
                                if aid:
                                    active_agent_ids.add(aid)
                            except Exception:
                                pass
            
            if not terminal_labels and agents_info:
                for idx, display_name in enumerate(agents_info.keys(), 1):
                    clean_name = display_name.split("[")[0].strip() if display_name else ""
                    friendly_label = f"T{idx}"
                    if clean_name:
                        friendly_label = f"{friendly_label} · {clean_name}"
                    terminal_labels[f"terminal-{idx}"] = friendly_label
                    terminal_labels[f"T{idx}"] = friendly_label

            for display_name_state in agent_costs_snapshot.keys():
                agents_info.setdefault(display_name_state, 0)

            # Calculate totals
            total_messages = sum(agents_info.values())
            current_state['agents'] = agents_info
            current_state['total_messages'] = total_messages
            current_state['terminal_labels'] = terminal_labels.copy()

            # System info
            # Model removed - now shown per terminal

            # Get cost info - prefer live model totals
            session_cost = 0.0
            try:
                from cai.util import COST_TRACKER
                session_cost = float(getattr(COST_TRACKER, 'session_total_cost', 0.0) or 0.0)
            except Exception:
                session_cost = 0.0

            agent_costs = dict(agent_costs_snapshot)
            terminal_costs = dict(terminal_costs_snapshot)
            if terminal_labels:
                valid_terminal_ids = set(terminal_labels.keys())
                terminal_costs = {
                    key: value
                    for key, value in terminal_costs.items()
                    if key in valid_terminal_ids
                }

            # Fallback to CostTracker aggregation if we couldn't gather live data
            if not agent_costs:
                try:
                    from cai.util import COST_TRACKER
                    if hasattr(COST_TRACKER, 'agent_costs'):
                        for agent_key, cost in COST_TRACKER.agent_costs.items():
                            try:
                                cost_value = float(cost or 0.0)
                            except Exception:
                                continue
                            if cost_value <= 0:
                                continue
                            if isinstance(agent_key, tuple) and len(agent_key) >= 2:
                                agent_name = agent_key[0]
                                agent_id = agent_key[1]
                                display_key = f"{agent_name} [{agent_id}]" if agent_id else agent_name
                            else:
                                display_key = str(agent_key)
                            agent_costs[display_key] = cost_value
                    if hasattr(COST_TRACKER, 'terminal_costs'):
                        for key, value in COST_TRACKER.terminal_costs.items():
                            try:
                                cost_value = float(value or 0.0)
                            except Exception:
                                continue
                            if cost_value <= 0:
                                continue
                            terminal_costs[key] = cost_value
                except Exception:
                    pass

            def _parse_display_parts(name: str) -> tuple[str, str, str]:
                """Extract base name, normalized base, and the most relevant ID."""
                import re

                raw_name = name or ""
                bracket_contents = re.findall(r"\[([^\]]+)\]", raw_name)
                id_part = ""
                invalid_markers = {"", "?", "??", "-", "--", "n/a", "na", "unknown"}
                if bracket_contents:
                    for candidate in reversed(bracket_contents):
                        candidate = candidate.strip()
                        if not candidate or candidate.lower() in invalid_markers:
                            continue
                        id_part = candidate
                        break
                    if not id_part:
                        fallback = bracket_contents[-1].strip()
                        if fallback.lower() not in invalid_markers:
                            id_part = fallback

                base = re.sub(r"\s*\[[^\]]*\]\s*", " ", raw_name).strip()
                if not base:
                    base = raw_name.strip()

                normalized_base = re.sub(r"[^a-z0-9]+", " ", base.lower()).strip()
                if not normalized_base:
                    normalized_base = base.lower()

                return base, normalized_base, id_part

            # Build a quick lookup of known IDs keyed by normalized base name
            known_ids: dict[str, str] = {}
            for source_name in list(agents_info.keys()) + list(agent_costs_snapshot.keys()):
                _, norm_base, id_part = _parse_display_parts(source_name)
                if id_part:
                    known_ids.setdefault(norm_base, id_part)

            def _canonical_agent_key(name: str) -> tuple[tuple[str, str], str]:
                base, norm_base, id_part = _parse_display_parts(name)
                canonical_id = id_part or known_ids.get(norm_base, "")

                canonical_display = base.strip() if base else name
                if canonical_id:
                    canonical_display = f"{canonical_display} [{canonical_id}]".strip()

                return (canonical_id or "", norm_base), canonical_display

            aggregated_view: dict[tuple[str, str], dict[str, Any]] = {}

            for display_name, msgs in agents_info.items():
                key, canonical_display = _canonical_agent_key(display_name)
                entry = aggregated_view.setdefault(key, {"display": canonical_display, "messages": 0, "cost": 0.0})
                if msgs > entry["messages"]:
                    entry["messages"] = msgs
                    entry["display"] = canonical_display

            for display_name, cost in list(agent_costs.items()):
                key, canonical_display = _canonical_agent_key(display_name)
                entry = aggregated_view.setdefault(key, {"display": canonical_display, "messages": 0, "cost": 0.0})
                if cost > entry.get("cost", 0.0):
                    entry["cost"] = cost
                    entry["display"] = canonical_display

            if active_agent_ids:
                filtered_view: dict[tuple[str, str], dict[str, Any]] = {}
                for key, entry in aggregated_view.items():
                    canonical_id, _ = key
                    if canonical_id and canonical_id not in active_agent_ids:
                        continue
                    filtered_view[key] = entry
                aggregated_view = filtered_view

            agent_costs = {
                entry["display"]: entry["cost"]
                for entry in aggregated_view.values()
                if entry.get("cost", 0.0) > 0
            }
            agents_info = {
                entry["display"]: entry.get("messages", 0)
                for entry in aggregated_view.values()
            }

            agent_cost_total = sum(agent_costs.values())
            computed_total = max(session_cost, agent_cost_total)
            unattributed_cost = max(0.0, session_cost - agent_cost_total) if session_cost > agent_cost_total else 0.0

            current_state['cost'] = f"${computed_total:.4f}" if computed_total > 0 else "--"
            current_state['agent_costs'] = agent_costs
            current_state['terminal_costs'] = terminal_costs
            current_state['session_cost'] = session_cost
            current_state['unattributed_cost'] = unattributed_cost

            # Meta Agent activity (if enabled)
            try:
                from cai.tui.meta_agent_controller import get_meta_agent_controller
                mac = get_meta_agent_controller()
                if mac and getattr(mac, 'enabled', False):
                    dbg = mac.get_debug_info()
                    current_state['meta_agent'] = {
                        'enabled': True,
                        'last_agent': dbg.get('last_selected_agent') or '-',
                        'parallel': dbg.get('parallel_added', []),
                        'recent': dbg.get('activity_log', []),
                    }
            except Exception:
                pass
            
            # Detect mode based on active agents
            if hasattr(self, 'app'):
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = self.app
                    if isinstance(app, CAITerminal) and hasattr(app, 'session_manager') and app.session_manager:
                        active_terminals = len(app.session_manager.terminal_runners)
                        if active_terminals > 1:
                            current_state['mode'] = f"Multi-Agent ({active_terminals})"
                        else:
                            current_state['mode'] = "Single"
                    else:
                        # Fallback to env var
                        parallel_count = int(os.getenv("CAI_PARALLEL", "1"))
                        current_state['mode'] = f"Parallel ({parallel_count})" if parallel_count > 1 else "Single"
                except:
                    # Fallback to env var
                    parallel_count = int(os.getenv("CAI_PARALLEL", "1"))
                    current_state['mode'] = f"Parallel ({parallel_count})" if parallel_count > 1 else "Single"
            else:
                parallel_count = int(os.getenv("CAI_PARALLEL", "1"))
                current_state['mode'] = f"Parallel ({parallel_count})" if parallel_count > 1 else "Single"
            
            # Memory status
            try:
                from cai.repl.commands.memory import COMPACTED_SUMMARIES
                current_state['memory_count'] = len(COMPACTED_SUMMARIES) if COMPACTED_SUMMARIES else 0
            except:
                current_state['memory_count'] = 0
            
            # Queue status
            try:
                if os.getenv("CAI_TUI_MODE") == "true":
                    from cai.tui.core.terminal_queue import TERMINAL_QUEUE_MANAGER
                    all_queues = TERMINAL_QUEUE_MANAGER.get_all_queues_status()
                    total_count = 0
                    for terminal_queue in all_queues.values():
                        total_count += terminal_queue['queue_length']
                    current_state['queue_count'] = total_count
                else:
                    from cai.repl.commands.queue import get_queue
                    queue_items = get_queue()
                    current_state['queue_count'] = len(queue_items) if queue_items else 0
            except:
                current_state['queue_count'] = 0
            
            # Check if state has changed
            if current_state == self._stats_cache:
                return  # No changes, skip update
            
            # Update cache
            self._stats_cache = current_state.copy()
            
            # First time initialization
            if not self._stats_widgets:
                # Clear any existing children first
                self.state_container.remove_children()
                
                # Create sections with children already composed
                self._stats_widgets['agent_title'] = Static("🤖 AGENTS", classes="state-section-title")
                self._stats_widgets['agent_section'] = Vertical(
                    self._stats_widgets['agent_title'],
                    classes="state-section"
                )
                
                self._stats_widgets['system_title'] = Static("⚙️ SYSTEM", classes="state-section-title")
                self._stats_widgets['system_section'] = Vertical(
                    self._stats_widgets['system_title'],
                    classes="state-section"
                )
                
                # Mount sections
                self.state_container.mount(self._stats_widgets['agent_section'])
                self.state_container.mount(self._stats_widgets['system_section'])
            
            # Update agent section content
            agent_section = self._stats_widgets['agent_section']
            # Remove all children except title
            for child in list(agent_section.children)[1:]:
                child.remove()
            
            # Add agent lines
            for display_name, msg_count in sorted(agents_info.items()):
                base_display = display_name
                if "[" in display_name:
                    base_display = display_name[: display_name.index("[")].strip()
                agent_name = base_display or display_name
                agent_id = "?"
                if "[" in display_name and "]" in display_name:
                    try:
                        agent_id = display_name[display_name.rindex("[") + 1 : display_name.rindex("]")]
                    except Exception:
                        agent_id = "?"
                
                if len(agent_name) > 14:
                    agent_name = agent_name[:12] + ".."
                
                # Check if agent is running
                is_running = False
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = self.app
                    if isinstance(app, CAITerminal) and hasattr(app, 'session_manager') and app.session_manager:
                        # Find runner for this agent
                        for runner in app.session_manager.terminal_runners.values():
                            runner_agent_id = agent_id
                            if runner.config.is_parallel and runner.config.parallel_config:
                                runner_agent_id = runner.config.parallel_config.id or f"P{runner.config.terminal_number}"
                            else:
                                runner_agent_id = f"T{runner.config.terminal_number}"
                            
                            if runner_agent_id == agent_id and runner.is_running:
                                is_running = True
                                break
                except:
                    pass
                
                # Skip agents without activity unless currently running
                if not is_running and msg_count <= 0 and agent_name.lower() not in {"unattributed", "unassigned"}:
                    continue

                # Set status based on state
                if is_running:
                    status = "🔵"  # Blue for running
                elif msg_count > 0:
                    status = "🟢"  # Green for has messages
                else:
                    status = "⚪"  # White for idle

                agent_section.mount(
                    Static(
                        f"[cyan]{agent_name}[/cyan] {status} ({msg_count})",
                        classes="state-content",
                    )
                )
            
            # Update system section content
            system_section = self._stats_widgets['system_section']
            # Remove all children except title
            for child in list(system_section.children)[1:]:
                child.remove()
            
            display_cost = current_state.get('cost', '--')
            if display_cost != "--":
                system_section.mount(Static(f"Cost: [yellow]{display_cost}[/yellow]", classes="state-content"))
            else:
                system_section.mount(Static("Cost: [dim]--[/dim]", classes="state-content"))

            terminal_costs_map = current_state.get('terminal_costs', {})
            terminal_labels_map = current_state.get('terminal_labels', {})
            if terminal_costs_map:
                import re

                terminal_lines: dict[str, float] = {}
                for key, cost in terminal_costs_map.items():
                    if cost is None or float(cost) <= 0:
                        continue
                    normalized_key = key
                    if key.startswith("T") and key[1:].isdigit():
                        normalized_key = f"terminal-{key[1:]}"

                    label = terminal_labels_map.get(normalized_key) or terminal_labels_map.get(key)
                    if not label and normalized_key.startswith("terminal-") and normalized_key[9:].isdigit():
                        label = f"T{normalized_key[9:]}"
                    if not label:
                        label = normalized_key

                    # Keep the first (typically actual terminal_id) snapshot for the label
                    terminal_lines.setdefault(label, cost)

                if terminal_lines:
                    def _sort_key(item: tuple[str, float]) -> tuple[int, str]:
                        match = re.search(r"T(\d+)", item[0])
                        if match:
                            return (int(match.group(1)), item[0])
                        return (9999, item[0])

                    system_section.mount(Static("[dim]Per Terminal[/dim]", classes="state-content"))
                    for label, cost in sorted(terminal_lines.items(), key=_sort_key):
                        system_section.mount(
                            Static(f"{label}: [yellow]${cost:.4f}[/yellow]", classes="state-content")
                        )
            unattributed_cost = float(current_state.get('unattributed_cost', 0.0) or 0.0)
            if unattributed_cost > 0.00005:
                system_section.mount(
                    Static(
                        f"Unattributed: [yellow]${unattributed_cost:.4f}[/yellow]",
                        classes="state-content",
                    )
                )
            system_section.mount(Static(f"Mode: [cyan]{current_state['mode']}[/cyan]", classes="state-content"))

            # Show Meta Agent brief panel if active
            meta = current_state.get('meta_agent')
            if meta and meta.get('enabled'):
                last_agent = meta.get('last_agent', '-')
                parallel = ", ".join(meta.get('parallel', [])) or '-'
                system_section.mount(Static("Meta Agent: [green]ON[/green]", classes="state-content"))
                system_section.mount(Static(f"Last Agent: [cyan]{last_agent}[/cyan]", classes="state-content"))
                system_section.mount(Static(f"Parallel: [magenta]{parallel}[/magenta]", classes="state-content"))
                # Recent activity (last 3)
                recent = meta.get('recent', [])[-3:]
                for a in recent:
                    try:
                        system_section.mount(Static(f"[dim]{a['ts']}[/dim] {a['message'][:60]}", classes="state-content"))
                    except Exception:
                        pass
            
            # Add optional sections only if needed
            if current_state['memory_count'] > 0:
                if 'memory_section' not in self._stats_widgets:
                    self._stats_widgets['memory_title'] = Static("🧠 MEMORY", classes="state-section-title")
                    self._stats_widgets['memory_section'] = Vertical(
                        self._stats_widgets['memory_title'],
                        classes="state-section"
                    )
                    self.state_container.mount(self._stats_widgets['memory_section'])
                
                memory_section = self._stats_widgets['memory_section']
                # Remove all children except title
                for child in list(memory_section.children)[1:]:
                    child.remove()
                memory_section.mount(Static(f"Active: [magenta]{current_state['memory_count']} agents[/magenta]", classes="state-content"))
            elif 'memory_section' in self._stats_widgets:
                self._stats_widgets['memory_section'].remove()
                del self._stats_widgets['memory_section']
            
            if current_state['queue_count'] > 0:
                if 'queue_section' not in self._stats_widgets:
                    self._stats_widgets['queue_title'] = Static("📋 QUEUE STATS", classes="state-section-title")
                    self._stats_widgets['queue_section'] = Vertical(
                        self._stats_widgets['queue_title'],
                        classes="state-section"
                    )
                    self.state_container.mount(self._stats_widgets['queue_section'])
                
                queue_section = self._stats_widgets['queue_section']
                # Remove all children except title
                for child in list(queue_section.children)[1:]:
                    child.remove()
                queue_section.mount(Static(f"Items: [green]{current_state['queue_count']}[/green]", classes="state-content"))
            elif 'queue_section' in self._stats_widgets:
                self._stats_widgets['queue_section'].remove()
                del self._stats_widgets['queue_section']
            
            # Add history graph section
            if 'history_section' not in self._stats_widgets:
                self._stats_widgets['history_section'] = Vertical(
                    classes="state-section",
                    id="history_section"
                )
                self.state_container.mount(self._stats_widgets['history_section'])
            
            # Generate history graph - use session info if available
            parallel_configs = []
            if hasattr(self, 'app'):
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = self.app
                    if isinstance(app, CAITerminal) and hasattr(app, 'session_manager') and app.session_manager:
                        # Create parallel configs from active runners
                        for terminal_num, runner in app.session_manager.terminal_runners.items():
                            if runner.config.is_parallel and runner.config.parallel_config:
                                parallel_configs.append(runner.config.parallel_config)
                except:
                    pass
            
            # Use PARALLEL_CONFIGS if no session info
            if not parallel_configs:
                parallel_configs = PARALLEL_CONFIGS
                
            history_graph = self._generate_history_graph(agents_info, all_histories, parallel_configs, has_isolated_histories)
            
            # Update history section if graph changed
            if history_graph != self._stats_cache.get('history_graph', ''):
                self._stats_cache['history_graph'] = history_graph
                history_section = self._stats_widgets['history_section']
                # Remove all children
                for child in list(history_section.children):
                    child.remove()
                
                # Add graph content
                for line in history_graph.split('\n'):
                    if line.strip():
                        history_section.mount(Static(line, classes="state-content"))
            
        except Exception as e:
            # Only show error if not already showing
            if 'error' not in self._stats_cache:
                self._stats_cache['error'] = str(e)
                self.state_container.remove_children()
                error_section = Vertical(
                    Static("❌ ERROR", classes="state-section-title"),
                    Static(f"[red]{str(e)[:50]}[/red]", classes="state-content"),
                    classes="state-section"
                )
                self.state_container.mount(error_section)
    
    def _generate_history_graph(self, agents_info, all_histories, parallel_configs, has_isolated_histories):
        """Generate a special compact visualization for the sidebar"""
        from cai.sdk.agents.parallel_isolation import PARALLEL_ISOLATION
        
        # Build detailed agent info
        detailed_agents = {}
        
        if parallel_configs and has_isolated_histories:
            # Parallel mode with isolation
            for idx, config in enumerate(parallel_configs, 1):
                agent_id = config.id or f"P{idx}"
                isolated_history = PARALLEL_ISOLATION.get_isolated_history(agent_id)
                if isolated_history is None:
                    isolated_history = []
                
                display_name = f"{config.agent_name} [{agent_id}]"
                detailed_agents[display_name] = {
                    'history': isolated_history,
                    'is_current': True,
                    'has_memory': False
                }
        else:
            # Regular mode - use all_histories
            for display_name, history in all_histories.items():
                detailed_agents[display_name] = {
                    'history': history,
                    'is_current': True,
                    'has_memory': False
                }
        
        # Check for memory
        try:
            from cai.repl.commands.memory import COMPACTED_SUMMARIES, APPLIED_MEMORY_IDS
            for display_name in detailed_agents:
                base_name = display_name.split(" [")[0] if "[" in display_name else display_name
                if " #" in base_name:
                    base_name = base_name.split(" #")[0]
                if base_name in COMPACTED_SUMMARIES:
                    detailed_agents[display_name]['has_memory'] = True
        except:
            pass
        
        # Sort agents by ID
        sorted_agents = sorted(detailed_agents.items(), key=lambda x: (
            0 if "[P" in x[0] and x[0].split("[P")[1].split("]")[0].isdigit() else 1,
            int(x[0].split("[P")[1].split("]")[0]) if "[P" in x[0] and x[0].split("[P")[1].split("]")[0].isdigit() else x[0]
        ))
        
        # Create special visualization
        lines = []
        
        # Summary statistics only
        total_messages = sum(len(info['history']) for _, info in sorted_agents)
        active_agents = sum(1 for _, info in sorted_agents if len(info['history']) > 0)
        
        lines.append(f"[#c8ff00]Total:[/#c8ff00] {total_messages} msgs")
        lines.append(f"[#529d86]Active:[/#529d86] {active_agents}/{len(sorted_agents)} agents")
        
        return '\n'.join(lines)

    def force_refresh_keys(self) -> None:
        """Force refresh of keys - called externally when .env changes"""
        # Add debug logging if enabled
        import os
        if os.getenv("CAI_DEBUG"):
            try:
                from cai.tui.cai_terminal import CAITerminal
                app = CAITerminal._instance
                if app and hasattr(app, 'terminal_grid'):
                    main_terminal = app.terminal_grid.get_main_terminal()
                    if main_terminal:
                        main_terminal.write("[dim]Debug: Force refreshing sidebar keys...[/dim]")
            except Exception:
                pass
        
        # Simple direct refresh
        self.refresh_keys()
        
        # Force a complete UI refresh of the sidebar
        try:
            self.refresh()
        except Exception:
            pass

    def refresh_keys(self) -> None:
        """Refresh the API keys display"""
        # Skip if container is not ready
        if not hasattr(self, 'keys_container') or self.keys_container is None:
            return
            
        # Debug logging
        import os
        if os.getenv("CAI_DEBUG"):
            try:
                from cai.tui.cai_terminal import CAITerminal
                app = CAITerminal._instance
                if app and hasattr(app, 'terminal_grid'):
                    main_terminal = app.terminal_grid.get_main_terminal()
                    if main_terminal:
                        main_terminal.write("[dim]Debug: Starting refresh_keys...[/dim]")
            except Exception:
                pass
            
        try:
            # Radical approach: recreate the entire keys_container to avoid any ID conflicts
            if os.getenv("CAI_DEBUG"):
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[dim]Debug: Recreating keys_container completely[/dim]")
                except Exception:
                    pass
            
            # Remove the old container
            old_container = self.keys_container
            try:
                old_container.remove()
            except Exception:
                pass
            
            # Create a brand new container with unique ID
            import time
            unique_id = f"keys-list-{int(time.time() * 1000)}"
            self.keys_container = VerticalScroll(classes="keys-list")
            self.keys_container.id = unique_id
            
            if os.getenv("CAI_DEBUG"):
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[dim]Debug: Created new container with ID: {unique_id}[/dim]")
                except Exception:
                    pass
            
            # Mount the new container to the keys tab
            keys_tab = self.query_one("#keys")
            keys_tab.mount(self.keys_container)
            
            # Get API keys from .env file
            api_keys = self._get_api_keys()
            
            # Debug logging for loaded keys
            if os.getenv("CAI_DEBUG"):
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[dim]Debug: Loaded {len(api_keys)} keys: {list(api_keys.keys())}[/dim]")
                            for k, v in api_keys.items():
                                main_terminal.write(f"[dim]Debug: {k} = {v[:10]}...[/dim]")
                except Exception:
                    pass
            
            if not api_keys:
                # Show empty state
                empty_label = Label("No API keys found", classes="keys-empty")
                self.keys_container.mount(empty_label)
            else:
                # Display each API key - always recreate after cleanup
                for key_name, key_value in api_keys.items():
                    try:
                        if os.getenv("CAI_DEBUG"):
                            try:
                                from cai.tui.cai_terminal import CAITerminal
                                app = CAITerminal._instance
                                if app and hasattr(app, 'terminal_grid'):
                                    main_terminal = app.terminal_grid.get_main_terminal()
                                    if main_terminal:
                                        main_terminal.write(f"[dim]Debug: Creating key item for {key_name}[/dim]")
                            except Exception:
                                pass
                        
                        self._create_key_item_simple(key_name, key_value)
                    except Exception as e:
                        if os.getenv("CAI_DEBUG"):
                            try:
                                from cai.tui.cai_terminal import CAITerminal
                                app = CAITerminal._instance
                                if app and hasattr(app, 'terminal_grid'):
                                    main_terminal = app.terminal_grid.get_main_terminal()
                                    if main_terminal:
                                        main_terminal.write(f"[red]Debug: Error creating key item for {key_name}: {e}[/red]")
                            except Exception:
                                pass
                        # Create a simple fallback label
                        try:
                            masked_value = self._mask_key_value(key_value)
                            simple_label = Label(f"{key_name}: {masked_value}", classes="key-item-header")
                            self.keys_container.mount(simple_label)
                        except Exception:
                            pass
            
            # Always add "Add New Key" button at the end (after cleanup, it shouldn't exist)
            add_button = Button("+ Add New Key", classes="key-add-button")
            add_button.id = "add-key-button"
            self.keys_container.mount(add_button)
            if os.getenv("CAI_DEBUG"):
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[dim]Debug: Created Add button[/dim]")
                except Exception:
                    pass
            
            # Force refresh all input values after all widgets are mounted
            self.call_later(self._refresh_input_values)
            
        except Exception as e:
            # Handle errors gracefully - only mount error if container is empty
            # Debug logging for the error
            if os.getenv("CAI_DEBUG"):
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[red]Debug: Error in refresh_keys: {e}[/red]")
                            import traceback
                            main_terminal.write(f"[red]Debug: Traceback: {traceback.format_exc()}[/red]")
                except Exception:
                    pass
            
            try:
                if len(self.keys_container.children) == 0:
                    error_label = Label(f"Error loading keys: {str(e)[:50]}...", classes="keys-empty")
                    self.keys_container.mount(error_label)
            except:
                # Last resort - just pass
                pass

    def _get_api_keys(self) -> dict:
        """Get API keys from environment and .env file"""
        import re
        import os
        
        api_keys = {}
        
        # Debug logging
        if os.getenv("CAI_DEBUG"):
            try:
                from cai.tui.cai_terminal import CAITerminal
                app = CAITerminal._instance
                if app and hasattr(app, 'terminal_grid'):
                    main_terminal = app.terminal_grid.get_main_terminal()
                    if main_terminal:
                        main_terminal.write("[dim]Debug: Starting _get_api_keys...[/dim]")
            except Exception:
                pass
        
        # Skip environment variables - only read from .env file
        
        # Read from .env file only
        env_file_path = self._get_env_file_path()
        file_keys = {}
        
        # Debug logging for file path
        if os.getenv("CAI_DEBUG"):
            try:
                from cai.tui.cai_terminal import CAITerminal
                app = CAITerminal._instance
                if app and hasattr(app, 'terminal_grid'):
                    main_terminal = app.terminal_grid.get_main_terminal()
                    if main_terminal:
                        main_terminal.write(f"[dim]Debug: Looking for .env at: {env_file_path}[/dim]")
                        main_terminal.write(f"[dim]Debug: File exists: {os.path.exists(env_file_path)}[/dim]")
            except Exception:
                pass
        
        if os.path.exists(env_file_path):
            try:
                # Force fresh read without any caching
                with open(env_file_path, "r", encoding="utf-8") as file:
                    content = file.read()
                
                # Debug logging for file content
                if os.getenv("CAI_DEBUG"):
                    try:
                        from cai.tui.cai_terminal import CAITerminal
                        app = CAITerminal._instance
                        if app and hasattr(app, 'terminal_grid'):
                            main_terminal = app.terminal_grid.get_main_terminal()
                            if main_terminal:
                                main_terminal.write(f"[dim]Debug: File content length: {len(content)}[/dim]")
                    except Exception:
                        pass
                
                # Find all API key lines (keys that contain "API_KEY" or "KEY")
                for line in content.split('\n'):
                    line = line.strip()
                    if '=' in line and ('API_KEY' in line.upper() or line.upper().endswith('_KEY')):
                        if not line.startswith('#'):  # Skip comments
                            key_match = re.match(r'^([^=]+)=(.*)$', line)
                            if key_match:
                                key_name = key_match.group(1).strip()
                                key_value = key_match.group(2).strip().strip('"\'')
                                # Only include non-empty keys and exclude CAI_API_KEY
                                if key_value and key_name != 'CAI_API_KEY':
                                    file_keys[key_name] = key_value
            except Exception as e:
                if os.getenv("CAI_DEBUG"):
                    try:
                        from cai.tui.cai_terminal import CAITerminal
                        app = CAITerminal._instance
                        if app and hasattr(app, 'terminal_grid'):
                            main_terminal = app.terminal_grid.get_main_terminal()
                            if main_terminal:
                                main_terminal.write(f"[red]Debug: Error reading .env file: {e}[/red]")
                    except Exception:
                        pass
        
        # Use only file keys (no environment variables)
        api_keys.update(file_keys)
        
        # Debug logging for final result
        if os.getenv("CAI_DEBUG"):
            try:
                from cai.tui.cai_terminal import CAITerminal
                app = CAITerminal._instance
                if app and hasattr(app, 'terminal_grid'):
                    main_terminal = app.terminal_grid.get_main_terminal()
                    if main_terminal:
                        main_terminal.write(f"[dim]Debug: Found {len(api_keys)} total keys: {list(api_keys.keys())}[/dim]")
                        main_terminal.write(f"[dim]Debug: Env keys: {len(env_keys)}, File keys: {len(file_keys)}[/dim]")
            except Exception:
                pass
        
        return api_keys

    def _refresh_input_values(self) -> None:
        """Refresh input values after widgets are fully mounted"""
        try:
            api_keys = self._get_api_keys()
            for key_name, key_value in api_keys.items():
                try:
                    input_widget = self.query_one(f"#input-{key_name}", Input)
                    if hasattr(input_widget, '_real_value'):
                        # Update both the real value and displayed value
                        input_widget._real_value = key_value
                        if input_widget.disabled:
                            # Show masked value when disabled
                            input_widget.value = self._mask_key_value(key_value)
                        else:
                            # Show real value when editing
                            input_widget.value = key_value
                        
                        if os.getenv("CAI_DEBUG"):
                            try:
                                from cai.tui.cai_terminal import CAITerminal
                                app = CAITerminal._instance
                                if app and hasattr(app, 'terminal_grid'):
                                    main_terminal = app.terminal_grid.get_main_terminal()
                                    if main_terminal:
                                        main_terminal.write(f"[dim]Debug: Refreshed input {key_name} with value: '{input_widget.value}'[/dim]")
                            except Exception:
                                pass
                except Exception:
                    pass  # Widget might not exist yet
        except Exception:
            pass

    def _get_env_file_path(self) -> str:
        """Get the path to the .env file"""
        # Look for .env file in current working directory or project root
        current_dir = os.getcwd()
        
        # Try current directory first
        env_path = os.path.join(current_dir, ".env")
        if os.path.exists(env_path):
            return env_path
        
        # Try to find project root by looking for specific files
        search_dir = current_dir
        for _ in range(5):  # Limit search depth
            if any(os.path.exists(os.path.join(search_dir, marker)) 
                   for marker in ["pyproject.toml", "setup.py", ".git"]):
                env_path = os.path.join(search_dir, ".env")
                if os.path.exists(env_path):
                    return env_path
            parent = os.path.dirname(search_dir)
            if parent == search_dir:  # Reached root
                break
            search_dir = parent
        
        # Default to current directory if not found
        return os.path.join(current_dir, ".env")

    def _create_env_backup(self) -> bool:
        """Create a backup of the .env file before modifications"""
        try:
            import shutil
            import datetime
            
            env_file_path = self._get_env_file_path()
            
            if not os.path.exists(env_file_path):
                return True  # No file to backup
            
            # Create backup with timestamp
            timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
            backup_path = f"{env_file_path}.backup.{timestamp}"
            
            # Copy the current .env to backup
            shutil.copy2(env_file_path, backup_path)
            
            # Also create/update a simple .env.backup (latest backup)
            latest_backup_path = f"{env_file_path}.backup"
            shutil.copy2(env_file_path, latest_backup_path)
            
            # Debug logging
            if os.getenv("CAI_DEBUG"):
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[dim]Debug: Created .env backup: {backup_path}[/dim]")
                            main_terminal.write(f"[dim]Debug: Updated latest backup: {latest_backup_path}[/dim]")
                except Exception:
                    pass
            
            return True
            
        except Exception as e:
            # Debug logging for errors
            if os.getenv("CAI_DEBUG"):
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[red]Debug: Error creating .env backup: {e}[/red]")
                except Exception:
                    pass
            return False

    def _create_key_item_simple(self, key_name: str, key_value: str) -> None:
        """Create a widget for API key with visible text and inline editing"""
        # Use full key name without truncation
        display_name = key_name
        
        # Key name header
        key_label = Label(f"{display_name}:", classes="key-item-header")
        
        # Value display (visible by default)
        masked_value = self._mask_key_value(key_value)
        value_label = Label(masked_value, classes="key-value-display")
        value_label.id = f"display-{key_name}"
        
        # Simple input without any custom styles (hidden by default)
        key_input = Input()
        key_input.id = f"input-{key_name}"
        key_input.value = key_value
        key_input.display = False  # Initially hidden
        
        # Store the real value in widgets
        value_label._real_value = key_value
        value_label._key_name = key_name
        key_input._real_value = key_value
        key_input._key_name = key_name
        key_input._is_editing = False
        
        # Buttons container
        buttons_container = Horizontal(classes="key-buttons-container-simple")
        
        # Edit/Cancel button
        edit_btn = Button("Edit", classes="key-edit-button")
        edit_btn.id = f"edit-{key_name}"
        
        # Delete button
        delete_btn = Button("x", classes="key-delete-button")
        delete_btn.id = f"delete-{key_name}"
        
        # Save button (initially hidden)
        save_btn = Button("Save", classes="key-save-button-simple")
        save_btn.id = f"save-{key_name}"
        save_btn.display = False
        
        # Create a container for this key item to avoid overlapping
        key_container = Vertical(classes="key-item-container")
        key_container.id = f"container-{key_name}"
        
        # FIRST: Mount the key container to the main container
        self.keys_container.mount(key_container)
        
        # THEN: Mount everything to the key container (after it's mounted)
        key_container.mount(key_label)
        key_container.mount(value_label)      # Visible by default
        key_container.mount(key_input)        # Hidden by default
        key_container.mount(buttons_container)
        
        # FINALLY: Mount buttons to their container (after buttons_container is mounted)
        buttons_container.mount(edit_btn)
        buttons_container.mount(delete_btn)
        buttons_container.mount(save_btn)
        
        # Debug logging for button creation
        if os.getenv("CAI_DEBUG"):
            try:
                from cai.tui.cai_terminal import CAITerminal
                app = CAITerminal._instance
                if app and hasattr(app, 'terminal_grid'):
                    main_terminal = app.terminal_grid.get_main_terminal()
                    if main_terminal:
                        main_terminal.write(f"[dim]Debug: Created buttons for {key_name} - Edit: {edit_btn.id}, Save: {save_btn.id}[/dim]")
                        main_terminal.write(f"[dim]Debug: Save button display: {save_btn.display}[/dim]")
            except Exception:
                pass
        
        # Add minimal separator
        separator = Label("", classes="agent-separator")
        self.keys_container.mount(separator)

    def _mask_key_value(self, key_value: str) -> str:
        """Mask the API key value for security"""
        if not key_value:
            return ""
        if len(key_value) <= 10:
            return key_value[:3] + "*" * max(0, len(key_value) - 3)
        else:
            return key_value[:6] + "*" * max(0, len(key_value) - 10) + key_value[-4:]


    @on(Button.Pressed)
    def handle_key_button_press(self, event: Button.Pressed) -> None:
        """Handle button presses in the keys section"""
        button_id = event.button.id
        
        if button_id == "add-key-button":
            self._show_add_key_dialog()
        elif button_id and button_id.startswith("edit-"):
            key_name = button_id[5:]  # Remove "edit-" prefix
            self._toggle_edit_mode(key_name)
        elif button_id and button_id.startswith("delete-"):
            key_name = button_id[7:]  # Remove "delete-" prefix
            self._delete_api_key(key_name)
        elif button_id and button_id.startswith("save-"):
            key_name = button_id[5:]  # Remove "save-" prefix
            self._save_key_changes(key_name)

    @on(RefreshKeysMessage)
    def handle_refresh_keys_message(self, message: RefreshKeysMessage) -> None:
        """Handle refresh keys message from external command"""
        self.refresh_keys()

    def _toggle_edit_mode(self, key_name: str) -> None:
        """Toggle edit mode for a specific API key"""
        import os
        
        # Debug logging
        if os.getenv("CAI_DEBUG"):
            try:
                from cai.tui.cai_terminal import CAITerminal
                app = CAITerminal._instance
                if app and hasattr(app, 'terminal_grid'):
                    main_terminal = app.terminal_grid.get_main_terminal()
                    if main_terminal:
                        main_terminal.write(f"[dim]Debug: Toggle edit mode for {key_name}[/dim]")
            except Exception:
                pass
        
        try:
            # Find the widgets for this key
            display_widget = self.query_one(f"#display-{key_name}", Label)
            input_widget = self.query_one(f"#input-{key_name}", Input)
            edit_button = self.query_one(f"#edit-{key_name}", Button)
            delete_button = self.query_one(f"#delete-{key_name}", Button)
            save_button = self.query_one(f"#save-{key_name}", Button)
            
            if hasattr(input_widget, '_is_editing') and input_widget._is_editing:
                # Currently editing - switch to view mode (cancel)
                if os.getenv("CAI_DEBUG"):
                    try:
                        from cai.tui.cai_terminal import CAITerminal
                        app = CAITerminal._instance
                        if app and hasattr(app, 'terminal_grid'):
                            main_terminal = app.terminal_grid.get_main_terminal()
                            if main_terminal:
                                main_terminal.write(f"[dim]Debug: Switching {key_name} to view mode[/dim]")
                    except Exception:
                        pass
                
                display_widget.display = True
                input_widget.display = False
                display_widget.update(self._mask_key_value(input_widget._real_value))
                input_widget._is_editing = False
                edit_button.label = "Edit"
                edit_button.remove_class("editing-mode")
                save_button.remove_class("editing-mode")
                # Remove editing-mode class from the buttons container
                buttons_container = edit_button.parent
                if buttons_container:
                    buttons_container.remove_class("editing-mode")
                delete_button.display = True  # Show delete button again
                save_button.display = False
            else:
                # Currently viewing - switch to edit mode
                if os.getenv("CAI_DEBUG"):
                    try:
                        from cai.tui.cai_terminal import CAITerminal
                        app = CAITerminal._instance
                        if app and hasattr(app, 'terminal_grid'):
                            main_terminal = app.terminal_grid.get_main_terminal()
                            if main_terminal:
                                main_terminal.write(f"[dim]Debug: Switching {key_name} to edit mode[/dim]")
                    except Exception:
                        pass
                
                display_widget.display = False
                input_widget.display = True
                input_widget.value = input_widget._real_value  # Show full value for editing
                input_widget._is_editing = True
                input_widget.focus()
                edit_button.label = "Back"
                edit_button.add_class("editing-mode")
                save_button.add_class("editing-mode")
                # Add editing-mode class to the buttons container for expanded width
                buttons_container = edit_button.parent
                if buttons_container:
                    buttons_container.add_class("editing-mode")
                delete_button.display = False  # Hide delete button during editing
                save_button.display = True
                
                # Debug logging
                import os
                if os.getenv("CAI_DEBUG"):
                    try:
                        from cai.tui.cai_terminal import CAITerminal
                        app = CAITerminal._instance
                        if app and hasattr(app, 'terminal_grid'):
                            main_terminal = app.terminal_grid.get_main_terminal()
                            if main_terminal:
                                main_terminal.write(f"[dim]Debug: Added editing-mode class to buttons for {key_name}[/dim]")
                                main_terminal.write(f"[dim]Debug: Edit button classes: {edit_button.classes}[/dim]")
                                main_terminal.write(f"[dim]Debug: Save button classes: {save_button.classes}[/dim]")
                    except Exception:
                        pass
                
        except Exception as e:
            # Debug logging
            import os
            if os.getenv("CAI_DEBUG"):
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[red]Debug: Error toggling edit mode: {e}[/red]")
                except Exception:
                    pass

    def _save_key_changes(self, key_name: str) -> None:
        """Save changes to an API key"""
        try:
            # Find the input field
            input_widget = self.query_one(f"#input-{key_name}", Input)
            new_value = input_widget.value.strip()
            
            if not new_value:
                # Show error message
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write("[red]Error: API key cannot be empty[/red]")
                except Exception:
                    pass
                return
            
            # Update the .env file
            success = self._update_env_file(key_name, new_value)
            
            if success:
                # Update the stored value
                input_widget._real_value = new_value
                
                # Switch back to view mode
                display_widget = self.query_one(f"#display-{key_name}", Label)
                
                display_widget.display = True
                input_widget.display = False
                display_widget.update(self._mask_key_value(new_value))
                input_widget._is_editing = False
                
                # Update buttons
                edit_button = self.query_one(f"#edit-{key_name}", Button)
                delete_button = self.query_one(f"#delete-{key_name}", Button)
                save_button = self.query_one(f"#save-{key_name}", Button)
                edit_button.label = "Edit"
                edit_button.remove_class("editing-mode")
                save_button.remove_class("editing-mode")
                # Remove editing-mode class from the buttons container
                buttons_container = edit_button.parent
                if buttons_container:
                    buttons_container.remove_class("editing-mode")
                delete_button.display = True  # Show delete button again
                save_button.display = False
                
                # Update environment variable
                import os
                os.environ[key_name] = new_value
                
                # Show success message
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            masked_new = self._mask_key_value(new_value)
                            from rich.panel import Panel
                            main_terminal.write(Panel(
                                f"{key_name} successfully updated to: [bold green]{masked_new}[/bold green]\n"
                                "[yellow]Note: Changes will take effect on the next agent interaction[/yellow]",
                                border_style="green",
                                title="API Key Updated",
                            ))
                except Exception:
                    pass
            else:
                # Show error message
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[red]Error: Failed to update {key_name}[/red]")
                except Exception:
                    pass
                    
        except Exception as e:
            # Debug logging
            import os
            if os.getenv("CAI_DEBUG"):
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[red]Debug: Error saving key changes: {e}[/red]")
                except Exception:
                    pass

    def _update_env_file(self, key_name: str, new_value: str) -> bool:
        """Update a specific key in the .env file"""
        try:
            import re
            env_file_path = self._get_env_file_path()
            
            # Create backup before modification
            self._create_env_backup()
            
            # Debug logging
            if os.getenv("CAI_DEBUG"):
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[dim]Debug: Updating {key_name} in {env_file_path}[/dim]")
                            main_terminal.write(f"[dim]Debug: File exists: {os.path.exists(env_file_path)}[/dim]")
                except Exception:
                    pass
            
            if not os.path.exists(env_file_path):
                if os.getenv("CAI_DEBUG"):
                    try:
                        from cai.tui.cai_terminal import CAITerminal
                        app = CAITerminal._instance
                        if app and hasattr(app, 'terminal_grid'):
                            main_terminal = app.terminal_grid.get_main_terminal()
                            if main_terminal:
                                main_terminal.write(f"[red]Debug: .env file not found at {env_file_path}[/red]")
                    except Exception:
                        pass
                return False
            
            # Read current content
            with open(env_file_path, "r", encoding="utf-8") as file:
                lines = file.readlines()
            
            # Update the specific key
            updated = False
            for i, line in enumerate(lines):
                if line.strip() and not line.strip().startswith('#'):
                    if '=' in line:
                        current_key = line.split('=', 1)[0].strip()
                        if current_key == key_name:
                            lines[i] = f'{key_name}="{new_value}"\n'
                            updated = True
                            if os.getenv("CAI_DEBUG"):
                                try:
                                    from cai.tui.cai_terminal import CAITerminal
                                    app = CAITerminal._instance
                                    if app and hasattr(app, 'terminal_grid'):
                                        main_terminal = app.terminal_grid.get_main_terminal()
                                        if main_terminal:
                                            main_terminal.write(f"[dim]Debug: Updated line {i}: {key_name}=...{new_value[-4:]}[/dim]")
                                except Exception:
                                    pass
                            break
            
            # If key wasn't found, add it at the end
            if not updated:
                lines.append(f'{key_name}="{new_value}"\n')
                if os.getenv("CAI_DEBUG"):
                    try:
                        from cai.tui.cai_terminal import CAITerminal
                        app = CAITerminal._instance
                        if app and hasattr(app, 'terminal_grid'):
                            main_terminal = app.terminal_grid.get_main_terminal()
                            if main_terminal:
                                main_terminal.write(f"[dim]Debug: Added new line: {key_name}=...{new_value[-4:]}[/dim]")
                    except Exception:
                        pass
            
            # Write back to file
            with open(env_file_path, "w", encoding="utf-8") as file:
                file.writelines(lines)
            
            if os.getenv("CAI_DEBUG"):
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[dim]Debug: Successfully wrote .env file[/dim]")
                except Exception:
                    pass
            
            return True
            
        except Exception as e:
            if os.getenv("CAI_DEBUG"):
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[red]Debug: Error updating .env file: {e}[/red]")
                            import traceback
                            main_terminal.write(f"[red]Debug: Traceback: {traceback.format_exc()}[/red]")
                except Exception:
                    pass
            return False

    def _delete_api_key(self, key_name: str) -> None:
        """Delete an API key from .env file"""
        try:
            # Show confirmation in terminal
            try:
                from cai.tui.cai_terminal import CAITerminal
                app = CAITerminal._instance
                if app and hasattr(app, 'terminal_grid'):
                    main_terminal = app.terminal_grid.get_main_terminal()
                    if main_terminal:
                        main_terminal.write(f"[yellow]Deleting API key: {key_name}[/yellow]")
            except Exception:
                pass
            
            # Remove from .env file
            success = self._remove_from_env_file(key_name)
            
            if success:
                # Refresh the keys display (same pattern as edit)
                self.call_later(self.refresh_keys)
                
                # Show success message
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[green]✓[/green] API Key deleted: {key_name}")
                except Exception:
                    pass
            else:
                # Show error message
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[red]✗[/red] Error deleting API key: {key_name}")
                except Exception:
                    pass
                    
        except Exception as e:
            # Debug logging
            import os
            if os.getenv("CAI_DEBUG"):
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[red]Debug: Error deleting key: {e}[/red]")
                except Exception:
                    pass

    def _remove_from_env_file(self, key_name: str) -> bool:
        """Remove a specific key from the .env file"""
        try:
            import re
            env_file_path = self._get_env_file_path()
            
            # Create backup before modification
            self._create_env_backup()
            
            # Debug logging
            if os.getenv("CAI_DEBUG"):
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[dim]Debug: Removing {key_name} from {env_file_path}[/dim]")
                except Exception:
                    pass
            
            if not os.path.exists(env_file_path):
                return False
            
            # Read current content
            with open(env_file_path, 'r', encoding='utf-8') as f:
                lines = f.readlines()
            
            # Remove the key line (with or without quotes)
            pattern = rf'^{re.escape(key_name)}\s*=.*$'
            filtered_lines = []
            
            for line in lines:
                if not re.match(pattern, line.strip()):
                    filtered_lines.append(line)
            
            # Write back the filtered content
            with open(env_file_path, 'w', encoding='utf-8') as f:
                f.writelines(filtered_lines)
            
            return True
            
        except Exception as e:
            if os.getenv("CAI_DEBUG"):
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[red]Debug: Error removing from .env file: {e}[/red]")
                except Exception:
                    pass
            return False

    def _show_add_key_dialog(self):
        """Show the add API key dialog"""
        if hasattr(self, 'add_key_dialog'):
            self.add_key_dialog.show_dialog(callback=self._save_new_api_key)
    
    def _save_new_api_key(self, key_name: str, api_key: str):
        """Save a new API key to the .env file"""
        import os
        
        try:
            # Debug logging
            if os.getenv("CAI_DEBUG"):
                try:
                    from cai.tui.cai_terminal import CAITerminal
                    app = CAITerminal._instance
                    if app and hasattr(app, 'terminal_grid'):
                        main_terminal = app.terminal_grid.get_main_terminal()
                        if main_terminal:
                            main_terminal.write(f"[dim]Debug: Saving new API key: {key_name}[/dim]")
                except Exception:
                    pass
            
            # Update the .env file
            self._update_env_file(key_name, api_key)
            
            # Refresh the keys display
            self.call_later(self.refresh_keys)
            
            # Show success message in terminal
            try:
                from cai.tui.cai_terminal import CAITerminal
                app = CAITerminal._instance
                if app and hasattr(app, 'terminal_grid'):
                    main_terminal = app.terminal_grid.get_main_terminal()
                    if main_terminal:
                        masked_key = self._mask_key_value(api_key)
                        main_terminal.write(f"[green]✓[/green] API Key Added: {key_name} = {masked_key}")
            except Exception:
                pass
                
        except Exception as e:
            # Show error message in terminal
            try:
                from cai.tui.cai_terminal import CAITerminal
                app = CAITerminal._instance
                if app and hasattr(app, 'terminal_grid'):
                    main_terminal = app.terminal_grid.get_main_terminal()
                    if main_terminal:
                        main_terminal.write(f"[red]✗[/red] Error adding API key: {e}")
            except Exception:
                pass
