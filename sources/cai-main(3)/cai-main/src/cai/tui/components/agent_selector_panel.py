"""
Agent selector panel for sending prompts to multiple agents with modern design
"""


from textual import on
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Container, Horizontal, Vertical
from textual.message import Message
from textual.reactive import reactive
from textual.widgets import Button, Static, Label, ListItem, ListView


class AgentSelectionConfirmed(Message):
    """Message sent when agent selection is confirmed"""
    def __init__(self, selected_agents: list[str], prompt: str) -> None:
        super().__init__()
        self.selected_agents = selected_agents
        self.prompt = prompt


class AgentSelectionCancelled(Message):
    """Message sent when agent selection is cancelled"""
    pass


class AgentSelectorPanel(Container):
    """Modern panel for selecting which agents to send a prompt to"""
    
    DEFAULT_CSS = """
    AgentSelectorPanel {
        layer: overlay;
        width: 100%;
        height: 100%;
        display: none;
    }
    
    AgentSelectorPanel.visible {
        display: block;
    }
    
    /* Simple dark overlay */
    #agent-selector-overlay {
        width: 100%;
        height: 100%;
        background: rgba(0, 0, 0, 0.8);
        align: center middle;
    }
    
    /* Clean box design - wide */
    #agent-selector-content {
        width: 80%;
        min-width: 40;
        max-width: 120;
        height: 1fr;
        max-height: 90%;
        background: $surface;
        border: solid $primary;
        padding: 0;
        overflow: hidden;
    }
    
    /* No body wrapper needed; list will take remaining height */
    
    /* Clean prompt preview - compact */
    #prompt-preview {
        height: 3;
        padding: 1 2;
        margin: 1 3 1 3;
        color: $text-muted;
        background: $background;
        border: solid $surface-lighten-1;
        overflow: hidden;
    }
    
    /* Agent list view fills remaining space and scrolls */
    #agent-list-view {
        height: 1fr;
        margin: 1 3;
        padding: 0;
        background: $background;
        border: solid $surface-lighten-1;
        overflow-y: scroll; /* show scrollbar even on large screens */
        overflow-x: hidden;
        scrollbar-size: 1 1;
        scrollbar-color: #529d86;
        scrollbar-background: #2e4f46;
    }
    
    /* List items styling */
    #agent-list-view > ListItem {
        width: 100%;
        height: auto;
        padding: 1 2;
        background: transparent;
        border: none;
        margin: 0;
    }
    
    #agent-list-view > ListItem:hover {
        background: $surface-lighten-1;
        color: $primary;
    }
    
    #agent-list-view > ListItem.--highlight {
        background: $surface-lighten-2;
        color: $primary;
        text-style: bold;
    }
    
    /* Selected items */
    #agent-list-view > ListItem.selected {
        background: $primary-darken-3;
        color: $primary;
        text-style: bold;
    }
    
    /* Simple button bar - centered; a bit taller to avoid clipping */
    #selector-buttons {
        height: 4;
        min-height: 4;
        padding: 0 1;
        margin: 1 0 1 0;
        align: center middle;
        width: 100%;
        background: $surface-darken-1;
        border-top: solid $surface-lighten-2;
    }
    
    #selector-buttons Button {
        margin: 0 2;
        width: 1fr;      /* responsive: distribute evenly */
        min-width: 10;   /* keep readable on small terminals */
        height: 3;
    }
    
    #selector-buttons Button.primary {
        background: $surface;
        color: $text;
        border: solid $surface-lighten-1;
    }
    
    #selector-buttons Button.primary:hover {
        background: $surface-lighten-1;
        border: solid $primary;
    }
    
    #selector-buttons Button.success {
        background: $primary;
        color: $background;
        border: solid $primary;
    }
    
    #selector-buttons Button.success:hover {
        background: $primary-lighten-1;
        border: solid $primary-lighten-1;
    }
    
    #selector-buttons Button.error {
        background: $surface;
        color: $error;
        border: solid $surface-lighten-1;
    }
    
    #selector-buttons Button.error:hover {
        background: $error-darken-3;
        color: $text;
        border: solid $error;
    }
    """

    BINDINGS = [
        Binding("enter", "confirm", "Send"),
        Binding("escape", "cancel", "Cancel"),
        Binding("space", "toggle", "Toggle"),
        Binding("a", "select_all", "Select All"),
        Binding("n", "select_none", "Select None"),
    ]

    visible = reactive(False)

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.selected_agents = set()
        self.prompt = ""
        self.available_agents = []

    def compose(self) -> ComposeResult:
        """Compose the panel UI"""
        with Container(id="agent-selector-overlay"):
            with Vertical(id="agent-selector-content"):
                # Prompt preview (no header)
                yield Static("", id="prompt-preview")

                # Agent list using ListView fills remaining height
                yield ListView(id="agent-list-view")

                # Simple action buttons - centered
                with Horizontal(id="selector-buttons"):
                    yield Button("All", id="btn-select-all", variant="primary")
                    yield Button("None", id="btn-select-none", variant="primary")
                    yield Button("Send", id="btn-send", variant="success")

    def show_for_prompt(self, prompt: str, available_agents: list[str]) -> None:
        """Show the panel for selecting agents to send a prompt to"""
        self.prompt = prompt
        self.available_agents = available_agents

        # Update prompt preview - show more text
        preview = self.query_one("#prompt-preview", Static)
        truncated_prompt = prompt[:70] + "..." if len(prompt) > 70 else prompt
        preview.update(f"Prompt: {truncated_prompt}")

        # Clear and populate agent list
        list_view = self.query_one("#agent-list-view", ListView)
        list_view.clear()
        self.selected_agents.clear()

        # Create list items for each agent and store base name on the item
        for agent_name in available_agents:
            label = Label(agent_name)
            list_item = ListItem(label)
            # Persist the base agent name to avoid parsing label text
            setattr(list_item, "agent_name", agent_name)
            list_view.append(list_item)

        # Show the panel
        self.visible = True
        self.display = True
        self.add_class("visible")
        
        # Force refresh to ensure proper rendering
        self.refresh()
        
        # Ensure the panel is on top
        if hasattr(self, 'app') and self.app:
            self.app.screen.refresh()

        # Focus the list view
        list_view.focus()

    def hide(self) -> None:
        """Hide the panel"""
        self.visible = False
        self.display = False
        self.remove_class("visible")

    def action_confirm(self) -> None:
        """Confirm selection and send prompt"""
        if self.selected_agents:
            self.post_message(AgentSelectionConfirmed(list(self.selected_agents), self.prompt))
            self.hide()

    def action_cancel(self) -> None:
        """Cancel selection"""
        self.post_message(AgentSelectionCancelled())
        self.hide()

    def action_select_all(self) -> None:
        """Select all agents"""
        list_view = self.query_one("#agent-list-view", ListView)
        self.selected_agents.clear()
        for item in list_view.children:
            if isinstance(item, ListItem):
                agent_name = getattr(item, "agent_name", None)
                if not agent_name:
                    # Fallback to label text if attribute missing
                    label = item.query_one(Label)
                    agent_name = str(getattr(label, "text", "") or getattr(label, "render", lambda: "")())
                    agent_name = str(agent_name).replace("✓ ", "")
                self.selected_agents.add(agent_name)
                item.add_class("selected")
                # Update label marking
                label = item.query_one(Label)
                label.update(f"✓ {agent_name}")

    def action_select_none(self) -> None:
        """Deselect all agents"""
        list_view = self.query_one("#agent-list-view", ListView)
        self.selected_agents.clear()
        for item in list_view.children:
            if isinstance(item, ListItem):
                item.remove_class("selected")
                agent_name = getattr(item, "agent_name", None)
                if not agent_name:
                    label = item.query_one(Label)
                    agent_name = str(getattr(label, "text", "") or getattr(label, "render", lambda: "")())
                    agent_name = str(agent_name).replace("✓ ", "")
                label = item.query_one(Label)
                label.update(agent_name)

    def action_toggle(self) -> None:
        """Toggle current item"""
        list_view = self.query_one("#agent-list-view", ListView)
        if list_view.highlighted_child and isinstance(list_view.highlighted_child, ListItem):
            # Manually toggle the selection
            agent_name = getattr(list_view.highlighted_child, "agent_name", None)
            if not agent_name:
                label = list_view.highlighted_child.query_one(Label)
                agent_name = str(getattr(label, "text", "") or getattr(label, "render", lambda: "")())
                agent_name = str(agent_name).replace("✓ ", "")
            
            if agent_name in self.selected_agents:
                self.selected_agents.remove(agent_name)
                list_view.highlighted_child.remove_class("selected")
                label = list_view.highlighted_child.query_one(Label)
                label.update(agent_name)
            else:
                self.selected_agents.add(agent_name)
                list_view.highlighted_child.add_class("selected")
                label = list_view.highlighted_child.query_one(Label)
                label.update(f"✓ {agent_name}")

    @on(Button.Pressed)
    def on_button_pressed(self, event: Button.Pressed) -> None:
        """Handle button presses"""
        button_id = event.button.id
        if button_id == "btn-send":
            self.action_confirm()
        elif button_id == "btn-select-all":
            self.action_select_all()
        elif button_id == "btn-select-none":
            self.action_select_none()
        # Stop event propagation
        event.stop()
    
    @on(ListView.Selected)
    def on_list_selected(self, event: ListView.Selected) -> None:
        """Handle item selection in the list view"""
        if event.item and isinstance(event.item, ListItem):
            # Get the agent base name from stored attribute (robust across Textual versions)
            agent_name = getattr(event.item, "agent_name", None)
            if not agent_name:
                label = event.item.query_one(Label)
                agent_name = str(getattr(label, "text", "") or getattr(label, "render", lambda: "")())
                agent_name = str(agent_name).replace("✓ ", "")
            
            if agent_name in self.selected_agents:
                self.selected_agents.remove(agent_name)
                event.item.remove_class("selected")
                label = event.item.query_one(Label)
                label.update(agent_name)
            else:
                self.selected_agents.add(agent_name)
                event.item.add_class("selected")
                label = event.item.query_one(Label)
                label.update(f"✓ {agent_name}")
        event.stop()
