"""Prompt input widget with permanent prompt prefix like Linux terminals"""

from typing import Optional
from textual.app import ComposeResult
from textual.widgets import Static
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.reactive import reactive
from textual import on
from textual.message import Message

from .autocomplete_input import AutocompleteInput, SuggestionsUpdated


class PromptInput(Horizontal):
    """Input widget with a permanent prompt prefix like Linux terminals"""
    
    DEFAULT_CSS = """
    PromptInput {
        height: auto;
        min-height: 1;
        width: 100%;
        background: transparent;
        layout: horizontal;
        align: left middle;
        padding: 0;
    }
    
    PromptInput Static {
        width: auto;
        height: 1;
        padding: 0 0 0 0;
        margin: 0 1 0 0;
        color: $text;
        text-style: bold;
        background: transparent;
        content-align: left middle;
    }
    
    PromptInput AutocompleteInput {
        width: 1fr;
        height: 1;
        background: transparent !important;
        border: none !important;
        padding: 0 !important;
        color: $text !important;
    }
    
    PromptInput AutocompleteInput:focus {
        background: transparent !important;
        border: none !important;
        color: $text !important;
    }

    /* Suggest dropdown under the input */
    #prompt-suggest-box {
        height: auto;
        max-height: 5;
        border: tall $primary 20%;
        background: $surface;
        padding: 0 1;
        margin: 0;
        overflow-y: auto;
        scrollbar-size: 1 1;
        scrollbar-color: #529d86;
        scrollbar-background: #2e4f46;
    }
    #prompt-suggest-box .suggest-item {
        height: 1;
        padding: 0 0;
        color: $text;
    }
    """
    
    prompt_text = reactive("CAI>")
    
    def __init__(self, prompt: str = "CAI>", **kwargs):
        super().__init__(**kwargs)
        self.prompt_text = prompt
        self._input_widget = None
        
    def compose(self) -> ComposeResult:
        """Compose the prompt input"""
        # Prefix + input stacked with suggestion panel under input
        yield Static("[bold cyan]CAI>[/bold cyan] ", id="prompt-prefix")
        with Vertical(id="prompt-stack"):
            self._input_widget = AutocompleteInput(placeholder="", id="prompt-input-field")
            yield self._input_widget
            # Suggestion list container
            self._suggest_box = VerticalScroll(id="prompt-suggest-box")
            yield self._suggest_box
        
    def on_mount(self) -> None:
        """Focus the input when mounted"""
        if self._input_widget:
            self._input_widget.focus()
        # Start hidden suggestions
        try:
            self._suggest_box.display = False
        except Exception:
            pass
            
    def focus_without_select(self) -> None:
        """Focus the input without selecting all text"""
        if self._input_widget:
            self._input_widget.focus()
            # Move cursor to end
            self._input_widget.cursor_position = len(self._input_widget.value)
            
    @property
    def value(self) -> str:
        """Get the current input value"""
        return self._input_widget.value if self._input_widget else ""
        
    @value.setter
    def value(self, text: str) -> None:
        """Set the input value"""
        if self._input_widget:
            self._input_widget.value = text
            
    def focus(self) -> None:
        """Focus the input field"""
        if self._input_widget:
            self._input_widget.focus()

    @on(SuggestionsUpdated)
    def _on_suggestions(self, evt: SuggestionsUpdated) -> None:
        """Render suggestion dropdown below the input."""
        try:
            self._suggest_box.clear()
            if not evt.suggestions:
                self._suggest_box.display = False
                return
            for s in evt.suggestions:
                item = Static(s, classes="suggest-item")
                self._suggest_box.mount(item)
            self._suggest_box.display = True
        except Exception:
            pass
            
    def clear(self) -> None:
        """Clear the input field"""
        if self._input_widget:
            self._input_widget.clear()
            
    def add_to_history(self, command: str) -> None:
        """Add a command to history"""
        if self._input_widget:
            self._input_widget.add_to_history(command)
            
    def update_prompt(self, new_prompt: str) -> None:
        """Update the prompt text"""
        self.prompt_text = new_prompt
        prompt_widget = self.query_one("#prompt-prefix", Static)
        if prompt_widget:
            prompt_widget.update(new_prompt)