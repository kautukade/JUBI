"""
Universal terminal widget - used for all terminals (main, agents, etc)
"""

import asyncio
import io
import os

_CAI_DEBUG_DIR = os.path.join(os.path.expanduser("~"), ".cai", "debug")
import uuid
from contextlib import redirect_stderr, redirect_stdout
from dataclasses import dataclass, field
from datetime import datetime
from typing import Dict, Literal, Optional, Any
import time

from openai import AsyncOpenAI
from textual.app import ComposeResult
from textual import on
from textual.events import Click
from textual.containers import Container, Horizontal, Vertical
from textual.message import Message
from textual.reactive import reactive
from textual.css.query import NoMatches
from textual.widgets import RichLog, Select, Static
from textual.widget import Widget

from cai.tui.components.banner_widget import BannerWidget

# Import Observer pattern
from cai.tui.patterns.observer import EventType, terminal_event_manager

# Import error handling
try:
    from cai.tui.display.error_handler import (
        ContentValidator, safe_write_to_terminal, handle_streaming_errors
    )
    HAS_ERROR_HANDLER = True
except ImportError:
    HAS_ERROR_HANDLER = False
    ContentValidator = None

# Import streaming fix
try:
    # from cai.tui.core.async_streaming_fix import (
    #     StreamingMixin, StreamingUpdateMessage, STREAMING_BRIDGE
    # )
    HAS_STREAMING_FIX = False
except ImportError:
    HAS_STREAMING_FIX = False
    StreamingMixin = object  # Fallback to empty base class

# Terminal role types
TerminalRole = Literal["main", "agent", "empty", "monitor", "logger"]


class TerminalRoleChanged(Message):
    """Message sent when terminal role changes"""

    def __init__(self, terminal_id: str, old_role: TerminalRole, new_role: TerminalRole):
        super().__init__()
        self.terminal_id = terminal_id
        self.old_role = old_role
        self.new_role = new_role


@dataclass
class TerminalState:
    """Complete state of a terminal"""

    terminal_id: str
    role: TerminalRole = "empty"
    agent_name: Optional[str] = None
    agent_id: Optional[str] = None
    model_name: Optional[str] = None  # Add model name
    is_active: bool = False
    output_buffer: list[str] = field(default_factory=list)
    command_history: list[str] = field(default_factory=list)
    max_buffer_size: int = 10000  # Increased to show all history


class DeferredSelect(Select):
    """Select variant that waits for its overlay before populating options."""

    def __init__(self, *args, overlay_retry_limit: int = 5, **kwargs):
        super().__init__(*args, **kwargs)
        self._overlay_retry_limit = overlay_retry_limit
        self._overlay_retry_count = 0

    def _setup_options_renderables(self) -> None:
        try:
            super()._setup_options_renderables()
            self._overlay_retry_count = 0
        except NoMatches:
            if self._overlay_retry_count >= self._overlay_retry_limit:
                raise
            self._overlay_retry_count += 1
            # Schedule another attempt once Textual finishes mounting descendants.
            self.call_later(self._setup_options_renderables)

class UniversalTerminal(Container):
    """Universal terminal that can serve any role"""

    BINDINGS = [
        ("ctrl+shift+x", "copy_terminal_visible", "Copy Terminal"),
        ("ctrl+shift+z", "copy_terminal_all", "Copy Terminal All"),
    ]

    # Reactive properties
    role = reactive("empty")
    agent_name = reactive("")
    model_name = reactive("")  # Add reactive model name
    is_active = reactive(False)
    is_running = reactive(False)
    
    # Disable all focus behavior
    can_focus = False
    can_focus_children = False
    
    def watch_model_name(self, old_value: str, new_value: str) -> None:
        """React to model name changes"""
        if old_value != new_value:
            # Update state
            self.state.model_name = new_value
            # Skip UI updates in broadcast mode
            if os.getenv('CAI_BROADCAST_MODE') != 'true':
                # Update header
                self._update_header()
                # Force info bar update
                if hasattr(self, 'info_bar') and self.info_bar:
                    self.info_bar._update_info()
                # Force a refresh
                self.refresh()
    
    def watch_agent_name(self, old_value: str, new_value: str) -> None:
        """React to agent name changes"""
        if old_value != new_value:
            # Update state
            self.state.agent_name = new_value
            # Skip UI updates in broadcast mode
            if os.getenv('CAI_BROADCAST_MODE') != 'true':
                # Update header
                self._update_header()
                # Force info bar update
                if hasattr(self, 'info_bar') and self.info_bar:
                    self.info_bar._update_info()
                # Force a refresh
                self.refresh()
    
    # Modern CSS for terminal styling
    DEFAULT_CSS = """
    UniversalTerminal {
        layout: vertical;
        background: $background;
        border: none;
        padding: 0;
        margin: 0;
        width: 100%;
        height: 100%;
        overflow: hidden;
    }
    
    UniversalTerminal:hover {
        border: none;
    }
    
    UniversalTerminal .terminal-header-bar {
        height: 3;
        min-height: 3;
        max-height: 3;
        background: $surface-darken-1;
        border-top: solid $border;
        border-bottom: solid $border;
        border-left: none;
        border-right: none;
        layout: horizontal;
        content-align: left middle;
        padding: 1 0;
        margin: 0;
        width: 100%;
        overflow: hidden;    /* Textual supports auto|hidden|scroll */
    }
    /* Cluster containers to preserve right-side controls visibility */
    UniversalTerminal .terminal-left-cluster {
        layout: horizontal;
        width: 1fr;          /* take remaining space */
        height: 100%;
        overflow: hidden;    /* clip long title / selects */
        padding: 0;
        margin: 0;
    }
    UniversalTerminal .terminal-right-cluster {
        layout: horizontal;
        width: auto;         /* size to content */
        height: 100%;
        padding: 0;
        margin: 0;           /* no gap usage */
    }
    
    
    UniversalTerminal .terminal-header {
        width: 1fr;
        min-width: 7;      /* reserve space for terminal number like "T12 |" */
        color: $text;
        content-align: left middle;
        text-style: bold;
        padding: 0 1;
        background: transparent;
        height: 100%;
        overflow: hidden;   /* ensure long titles don’t push controls off */
    }

    /* Match the quad layout composition for two terminals: limit header width so
       only terminal number and agent are typically visible, mirroring 4-up */
    .two-terminals UniversalTerminal .terminal-header,
    UniversalTerminal.two-terminals .terminal-header {
        max-width: 20;
    }

    /* Inline compact selectors */
    /* Ensure the dropdown controls never exceed the bar height */
    UniversalTerminal .agent-select,
    UniversalTerminal .model-select,
    UniversalTerminal .container-select {
        height: 100%;
        min-height: 100%;
        max-height: 100%;
        min-width: 8;
        max-width: 36;
        width: auto;
        padding: 0 1;
        margin: 0;
        background: transparent;
        border: none;
        color: $text;
        content-align: center middle;
        overflow: hidden;
        offset-y: -1; /* nudge up to align perfectly with the header baseline */
    }
    /* Generic Select in header should also be forced to fit */
    UniversalTerminal Select.agent-select,
    UniversalTerminal Select.model-select,
    UniversalTerminal Select.container-select {
        height: 2;
        min-height: 2;
        max-height: 2;
        padding: 0 1;
        border: none;
        background: transparent;
        content-align: center middle;
    }

    /* Ensure dropdown overlays are styled minimally */
    UniversalTerminal .select--overlay,
    UniversalTerminal .dropdown,
    UniversalTerminal .menu {
        border: none;
        background: $surface;
    }

    /* Keep overlay menu content wide and readable regardless of trigger width */
    UniversalTerminal OptionList {
        min-width: 28;
        max-width: 36;
    }


    UniversalTerminal .agent-select { min-width: 10; }
    UniversalTerminal .model-select { min-width: 10; }
    UniversalTerminal .container-select { min-width: 16; }

    /* Reduce extra chrome inside selects */
    UniversalTerminal .agent-select:focus,
    UniversalTerminal .model-select:focus,
    UniversalTerminal .container-select:focus {
        border: none;
    }
    
    /* Terminal focus state styling */
    UniversalTerminal.terminal-focused .terminal-header-bar {
        background: $surface;
        border-bottom: solid $border;
        border-top: solid $border;
    }
    
    UniversalTerminal.terminal-focused .terminal-header { color: $text; }
    
    
    UniversalTerminal .status-indicator {
        width: 2;
        height: 100%;
        content-align: center middle;
        text-style: bold;
        padding: 0;
        margin: 0 1;          /* manual spacing, not gap */
        color: $error;
    }
    
    UniversalTerminal.agent-running .status-indicator { color: $success; }
    
    UniversalTerminal .terminal-output {
        height: 1fr;
        background: $background;
        color: $text;
        padding: 0;
        margin: 0;
        scrollbar-size: 1 1;
        scrollbar-color: #529d86;
        scrollbar-background: #2e4f46;
    }

    /* Close button — styled like the top bar burger for visual coherence */
    UniversalTerminal .terminal-close-button {
        width: 3;
        min-width: 3;
        max-width: 3;
        height: 100%;
        background: transparent;
        border: none;
        color: $text;
        text-style: bold;
        content-align: center middle;
        margin: 0;            /* no gap usage per constraint */
        padding: 0;           /* tight click target matching header height */
    }

    UniversalTerminal .terminal-close-button:hover {
        /* Match .sidebar-toggle:hover from cai_terminal.py */
        background: rgba(1, 120, 212, 0.3);
        border: none;
        color: $text;
    }
    
    /* Hide scrollbars when 4+ terminals are open */
    .many-terminals UniversalTerminal .terminal-output {
        scrollbar-size: 0 0 !important;
        overflow-y: auto !important;
        overflow-x: hidden !important;
    }
    
    /* Keep scrollbars hidden even during execution */
    .many-terminals UniversalTerminal.agent-running .terminal-output,
    UniversalTerminal.many-terminals.agent-running .terminal-output {
        scrollbar-size: 0 0 !important;
    }
    
    /* RichLog scrollbar styling */
    RichLog {
        scrollbar-size: 1 1;
        scrollbar-color: #529d86;
        scrollbar-background: #2e4f46;
    }
    
    /* Hide scrollbars on RichLog inside many-terminals */
    .many-terminals RichLog,
    UniversalTerminal.many-terminals RichLog {
        scrollbar-size: 0 0 !important;
    }
    
    /* Special visualization mode for 4+ terminals */
    .many-terminals UniversalTerminal {
        min-height: 8;
    }
    
    .many-terminals UniversalTerminal .terminal-header-bar {
        height: 3;
    }

    /* Keep dropdowns readable even with 4+ terminals: preserve normal widths.
       Allow overlays to overflow over neighboring panes rather than shrinking. */
    .many-terminals UniversalTerminal .agent-select,
    .many-terminals UniversalTerminal .model-select,
    .many-terminals UniversalTerminal .container-select,
    UniversalTerminal.many-terminals .agent-select,
    UniversalTerminal.many-terminals .model-select,
    UniversalTerminal.many-terminals .container-select {
        /* Keep container select tighter; allow agent/model to take more room */
        min-width: 8;    /* allow tighter header trigger */
        max-width: 12;   /* default for container-select; overridden below for agent/model */
        width: auto;
        overflow: hidden; /* textual only supports auto|hidden|scroll */
        padding: 0 1;
    }

    /* Wider headers for agent/model even with 4+ terminals */
    .many-terminals UniversalTerminal .agent-select,
    UniversalTerminal.many-terminals .agent-select,
    .many-terminals UniversalTerminal .model-select,
    UniversalTerminal.many-terminals .model-select {
        max-width: 22;  /* slightly reduced to give more to container */
    }

    /* Give container ~30% more width in compact modes */
    /* For 4+ terminals keep a fixed compact width */
    .many-terminals UniversalTerminal .container-select,
    UniversalTerminal.many-terminals .container-select {
        min-width: 20;   /* base 16 -> +25% */
        max-width: 20;
        padding: 0 0;   /* keep caret tight */
    }
    /* For 2 terminals, match 4-terminals compact width exactly */
    .two-terminals UniversalTerminal .container-select,
    UniversalTerminal.two-terminals .container-select {
        min-width: 20;
        max-width: 20;
        width: auto;
        padding: 0 0;   /* match compact padding to show caret */
    }

    /* Base compaction for 2 terminals, mirroring the compact header behavior */
    .two-terminals UniversalTerminal .agent-select,
    .two-terminals UniversalTerminal .model-select,
    UniversalTerminal.two-terminals .agent-select,
    UniversalTerminal.two-terminals .model-select {
        min-width: 8;
        max-width: 12;  /* overridden below per control */
        width: auto;
        overflow: hidden;
        padding: 0 1;
    }

    /* Wider headers for agent/model when 2 terminals (same as 4+) */
    .two-terminals UniversalTerminal .agent-select,
    UniversalTerminal.two-terminals .agent-select,
    .two-terminals UniversalTerminal .model-select,
    UniversalTerminal.two-terminals .model-select {
        max-width: 22;
    }

    /* For exactly 3 terminals, shrink slightly but keep a bit more room */
    .multiple-terminals UniversalTerminal .agent-select,
    .multiple-terminals UniversalTerminal .model-select,
    .multiple-terminals UniversalTerminal .container-select,
    UniversalTerminal.multiple-terminals .agent-select,
    UniversalTerminal.multiple-terminals .model-select,
    UniversalTerminal.multiple-terminals .container-select {
        min-width: 8;
        max-width: 14;  /* base for container; overridden below */
        width: auto;
        overflow: hidden;
        padding: 0 1;
    }

    /* Slightly wider for agent/model when 3 terminals */
    .multiple-terminals UniversalTerminal .agent-select,
    UniversalTerminal.multiple-terminals .agent-select,
    .multiple-terminals UniversalTerminal .model-select,
    UniversalTerminal.multiple-terminals .model-select {
        max-width: 16; /* trim a bit more to give space to container */
    }

    /* Container width for 3 terminals: match 2/4 terminal compact size */
    .multiple-terminals UniversalTerminal .container-select,
    UniversalTerminal.multiple-terminals .container-select {
        min-width: 20;
        max-width: 20;
        padding: 0 0;   /* keep caret visible */
    }

    /* Reduce header footprint slightly for 3 terminals to free space */
    .multiple-terminals UniversalTerminal .terminal-header,
    UniversalTerminal.multiple-terminals .terminal-header {
        max-width: 18;
        min-width: 6;   /* reserve a bit less than default 7 */
        padding: 0 0;   /* drop side padding to save columns */
    }

    /* Ensure overlays stay wide in condensed modes as well */
    .multiple-terminals UniversalTerminal OptionList,
    UniversalTerminal.multiple-terminals OptionList,
    .many-terminals UniversalTerminal OptionList,
    UniversalTerminal.many-terminals OptionList {
        min-width: 28;
        max-width: 36;
    }
    
    /* Ensure action bar is visible in many-terminals mode */
    .many-terminals UniversalTerminal ActualActionBar {
        display: block !important;
        visibility: visible !important;
    }
    
    UniversalTerminal BannerWidget {
        margin: 0;
        padding: 1;
        background: transparent;
        border: none;
    }
    
    /* Tooltip styling */
    Tooltip {
        background: #2e4f46;
        color: #c8ff00;
        border: solid #529d86;
        padding: 1;
        margin: 1;
    }
    """

    def __init__(self, terminal_id: Optional[str] = None, terminal_number: int = 1, **kwargs):
        super().__init__(**kwargs)
        self.terminal_id = terminal_id or str(uuid.uuid4())
        self.terminal_number = terminal_number  # Terminal 1, Terminal 2, etc.
        self.state = TerminalState(terminal_id=self.terminal_id)
        self.output = None
        self.agent = None
        self.banner = BannerWidget()
        self._role: TerminalRole = "empty"
        self._banner_shown = False
        
        # Character limit tracking
        self._char_count = 0
        self._max_chars = 10000  # 10,000 character limit
        
        # Streaming lines tracking
        self._streaming_lines = {}  # line_id -> {widget_id, content}
        
        # Performance optimization: throttle updates
        self._write_buffer = []
        self._write_timer = None
        self._last_write_time = 0
        self._write_throttle_ms = 30  # Throttle to max ~33 updates per second (faster)
        self._many_terminals_throttle_ms = 100  # Faster updates for 4+ terminals (was 150)
        
        # Execution indicator state
        self._execution_indicator_shown = False
        self._execution_frame = 0
        self._execution_timer = None
        self._execution_line_widget = None
        
        # Performance flag: disable events when many terminals are active
        self._emit_events = True  # Can be disabled for performance
        
        # Special mode for 4+ terminals
        self._summarized_mode = False

    def _get_session_runner(self):
        try:
            app = self.app
            if app and hasattr(app, "session_manager") and app.session_manager:
                return app.session_manager.terminal_runners.get(self.terminal_number)
        except Exception:
            return None
        return None

    def _resolve_initial_agent_name(self) -> str:
        if self.state.agent_name:
            return self.state.agent_name
        if self.agent_name:
            return self.agent_name
        runner = self._get_session_runner()
        if runner and getattr(runner, "config", None) and getattr(runner.config, "agent_name", ""):
            return runner.config.agent_name
        return os.getenv("CAI_DEFAULT_AGENT", "redteam_agent")

    def _resolve_active_container_id(self) -> str:
        active = os.getenv("CAI_ACTIVE_CONTAINER", "")
        try:
            from cai.tools.common import _get_agent_token_info

            token_info = _get_agent_token_info()
            if token_info and token_info.get("agent_id"):
                active = os.getenv(f"CAI_ACTIVE_CONTAINER_FOR_{token_info.get('agent_id')}", active)
            if token_info and token_info.get("agent_name"):
                import re

                safe = re.sub(r"[^A-Za-z0-9_]+", "_", str(token_info.get("agent_name")))
                active = os.getenv(f"CAI_ACTIVE_CONTAINER_FOR_NAME_{safe}", active)
        except Exception:
            pass

        return active[:12] if active else ""

    def _set_container_prompt(self, select_widget, value: str) -> None:
        """Update the Select prompt to reflect active container or placeholder.

        - When value is a container id: show "<id> (container)".
        - When empty: show neutral placeholder "container" (avoid duplicating 'host').
        """
        try:
            # Compact prompts in 3-terminal layout to preserve caret visibility
            is_three_terminals = ("multiple-terminals" in self.classes)

            if is_three_terminals:
                # In tight space, prefer the container id alone; keep placeholder readable
                if value:
                    display = f"{value}"
                else:
                    display = "container"
            else:
                display = f"{value} (container)" if value else "container"
            select_widget.prompt = display
        except Exception:
            pass

    def _set_select_options_safe(
        self,
        select_widget,
        options,
        *,
        selected_value=None,
    ) -> None:
        """Apply select options once the widget overlay is available."""

        attempt = {"count": 0}

        def apply_options() -> None:
            try:
                select_widget.set_options(options)
            except NoMatches:
                # The SelectOverlay isn't available yet; retry shortly.
                attempt["count"] += 1
                if attempt["count"] <= 5:
                    select_widget.call_after_refresh(apply_options)
                return

            if selected_value is not None:
                try:
                    select_widget.value = selected_value
                except Exception:
                    # Ignore selection errors – the caller can handle fallback states.
                    pass

        if select_widget.is_mounted and list(select_widget.children):
            apply_options()
        else:
            select_widget.call_after_refresh(apply_options)

    def compose(self) -> ComposeResult:
        """Compose the terminal"""
        # Since we inherit from Container, we just yield the child widgets
        # and Container will handle the layout
        
        # Import here to avoid circular imports
        from cai.tui.components.streaming_status_bar import ActualActionBar
        from cai.tui.components.info_status_bar import InfoStatusBar
        
        # Header area with focus indicator, status and selectors (agent/model/container)
        header_static = Static("", id=f"header-{self.terminal_id}", classes="terminal-header")
        header_static.tooltip = self._get_terminal_tooltip()

        # Dropdown selectors (populated on mount). Use narrow prompts to reduce chrome.
        agent_select = DeferredSelect([("select agent", "")], id=f"agent-select-{self.terminal_id}", classes="agent-select", prompt="agent")
        model_select = DeferredSelect([("select model", "")], id=f"model-select-{self.terminal_id}", classes="model-select", prompt="model")
        container_select = DeferredSelect([("host (no container)", "")], id=f"container-select-{self.terminal_id}", classes="container-select", prompt="container")
        
        # Compose a compact header row where selectors occupy the label slots
        # Wrap selectors in a fixed-height row to avoid overflow
        # Right cluster: status + close
        # Build status and close widgets explicitly to set tooltips post-init
        status_dot = Static("●", id=f"status-indicator-{self.terminal_id}", classes="status-indicator")
        close_btn = Static("×", id=f"close-{self.terminal_id}", classes="terminal-close-button")
        try:
            close_btn.tooltip = "Close terminal"
        except Exception:
            pass
        right_cluster = Horizontal(
            status_dot,
            close_btn,
            classes="terminal-right-cluster",
        )

        # Left cluster: title + selects (can shrink / clip)
        left_cluster = Horizontal(
            header_static,
            agent_select,
            model_select,
            container_select,
            classes="terminal-left-cluster",
        )

        row = Horizontal(
            left_cluster,
            right_cluster,
            classes="terminal-header-bar",
        )
        # Force fixed height to prevent vertical overflow and tighten internal gap
        row.styles.height = 3
        row.styles.min_height = 3
        row.styles.max_height = 3
        row.styles.padding = (0, 0)
        row.styles.margin = 0
        yield row
        
        # Output area
        # Reduce max_lines for better performance with multiple terminals
        yield RichLog(
            id=f"output-{self.terminal_id}",
            classes="terminal-output",
            highlight=False,  # Disable for performance
            markup=True,
            auto_scroll=True,
            wrap=True,
            max_lines=int(os.getenv("CAI_TUI_MAX_LINES", "0")) or None,  # Default None; can cap via env
        )
        
        # Info status bar
        yield InfoStatusBar(terminal_number=self.terminal_number, id=f"info-bar-{self.terminal_id}")
        
        # Action bar at bottom
        yield ActualActionBar(terminal_number=self.terminal_number, id=f"action-bar-{self.terminal_id}")

    def on_click(self, event) -> None:
        """Handle click events for header controls (e.g., close button)."""
        try:
            target = getattr(event, "widget", None) or getattr(event, "target", None)
            # Match by id prefix or class to be robust across Textual versions
            target_id = getattr(target, "id", "") or ""
            is_close = False
            try:
                if target_id.startswith("close-"):
                    is_close = True
            except Exception:
                pass
            if not is_close and target and getattr(target, "classes", None):
                try:
                    is_close = "terminal-close-button" in target.classes
                except Exception:
                    is_close = False
            if is_close:
                app = getattr(self, "app", None)
                if app and hasattr(app, "terminal_grid") and app.terminal_grid:
                    # Focus this terminal, then invoke the centralized close action
                    try:
                        app.terminal_grid.focus_terminal(self.terminal_id)
                    except Exception:
                        pass
                    try:
                        app.action_close_terminal()
                    except Exception:
                        pass
                # Stop further handling (avoid side-effects)
                try:
                    event.stop()
                except Exception:
                    pass
        except Exception:
            # Ignore click handling errors to avoid disrupting the UI
            pass

    @on(Click, ".terminal-close-button")
    def _on_close_button_click(self, event: Click) -> None:
        """Direct handler for clicks on the close button to ensure reliability."""
        try:
            app = getattr(self, "app", None)
            if app and hasattr(app, "terminal_grid") and app.terminal_grid:
                try:
                    app.terminal_grid.focus_terminal(self.terminal_id)
                except Exception:
                    pass
                try:
                    app.action_close_terminal()
                except Exception:
                    pass
            event.stop()
        except Exception:
            pass

    def on_mount(self) -> None:
        """Initialize when mounted"""
        try:
            self.output = self.query_one(f"#output-{self.terminal_id}", RichLog)
            # No longer need indicator - using border elements instead
            # self.indicator = self.query_one(f"#indicator-{self.terminal_id}", Static)
            self._update_header()
            
            # Get action bar reference
            from cai.tui.components.streaming_status_bar import ActualActionBar
            from cai.tui.components.info_status_bar import InfoStatusBar
            self.action_bar = self.query_one(f"#action-bar-{self.terminal_id}", ActualActionBar)
            self.info_bar = self.query_one(f"#info-bar-{self.terminal_id}", InfoStatusBar)
            # Initialize streaming widgets list
            self.streaming_widgets = []

            # Populate container dropdown with current docker ps list
            try:
                from cai.repl.commands.virtualization import DockerManager
                dm = DockerManager()
                options = []
                if dm.is_docker_installed() and dm.is_docker_running():
                    for c in dm.get_container_list():
                        cid = c.get("ID", "")[:12]
                        name = c.get("Names", "").lstrip("/")
                        image = c.get("Image", "")
                        label = f"{cid} | {name or image}"
                        options.append((label, cid))
                # Fallback option to clear selection
                options.insert(0, ("host (no container)", ""))
                select = self.query_one(f"#container-select-{self.terminal_id}")
                active_container = self._resolve_active_container_id()
                if active_container and not any(val == active_container for _, val in options):
                    options.insert(1, (f"{active_container} (container)", active_container))
                self._set_select_options_safe(
                    select,
                    options,
                    selected_value=active_container or None,
                )
                def _apply_prompt() -> None:
                    self._set_container_prompt(select, active_container)

                try:
                    select.call_after_refresh(_apply_prompt)
                except Exception:
                    _apply_prompt()
            except Exception:
                pass

            # Populate agent dropdown with all available agents (same logic as /agent list)
            try:
                from cai.agents import get_available_agents
                agents_to_display = get_available_agents()
                
                # Filter out ONLY parallel pattern pseudo-agents (same as /agent list command)
                agents = []
                for agent_key, agent in agents_to_display.items():
                    # Skip only parallel patterns in the dropdown
                    if hasattr(agent, "_pattern"):
                        pattern = agent._pattern
                        if hasattr(pattern, "type"):
                            pattern_type_value = getattr(pattern.type, "value", str(pattern.type))
                            if pattern_type_value == "parallel":
                                continue
                    agents.append(agent_key)
                # Pre-select current agent if available
                current_agent = self._resolve_initial_agent_name() or ""
                agent_options = [(a, a) for a in agents]
                agent_select = self.query_one(f"#agent-select-{self.terminal_id}")
                self._set_select_options_safe(
                    agent_select,
                    agent_options,
                    selected_value=current_agent or None,
                )
                if current_agent:
                    try:
                        self.state.agent_name = self.state.agent_name or current_agent
                        self.agent_name = self.agent_name or current_agent
                    except Exception:
                        pass
            except Exception:
                pass

            # Populate model dropdown from single source of truth (model.py)
            try:
                from cai.repl.commands.model import get_predefined_model_names
                import os

                canonical_models = get_predefined_model_names()

                # Allow optional comma-separated extras via env
                model_list_env = os.getenv("CAI_MODEL_LIST", "")
                extras = [m.strip() for m in model_list_env.split(",") if m.strip()]

                # Merge and de-duplicate while preserving order
                seen = set()
                merged = []
                for name in list(canonical_models) + extras:
                    if name not in seen:
                        seen.add(name)
                        merged.append(name)

                # Ensure current model is present and first
                current_model = self.state.model_name or os.getenv("CAI_MODEL", "alias1")
                if current_model:
                    if current_model in merged:
                        merged.remove(current_model)
                    merged.insert(0, current_model)

                model_options = [(m, m) for m in merged]
                model_select = self.query_one(f"#model-select-{self.terminal_id}")
                self._set_select_options_safe(
                    model_select,
                    model_options,
                    selected_value=current_model,
                )
            except Exception:
                pass

            # Periodically refresh container list to stay in sync
            try:
                def _refresh_containers():
                    try:
                        from cai.repl.commands.virtualization import DockerManager
                        dm = DockerManager()
                        options = []
                        if dm.is_docker_installed() and dm.is_docker_running():
                            for c in dm.get_container_list():
                                cid = c.get("ID", "")[:12]
                                name = c.get("Names", "").lstrip("/")
                                image = c.get("Image", "")
                                label = f"{cid} | {name or image}"
                                options.append((label, cid))
                        options.insert(0, ("host (no container)", ""))
                        select = self.query_one(f"#container-select-{self.terminal_id}")
                        active_container = self._resolve_active_container_id()
                        if active_container and not any(val == active_container for _, val in options):
                            options.insert(1, (f"{active_container} (container)", active_container))
                        self._set_select_options_safe(
                            select,
                            options,
                            selected_value=active_container or None,
                        )
                        def _apply_prompt() -> None:
                            self._set_container_prompt(select, active_container)

                        try:
                            select.call_after_refresh(_apply_prompt)
                        except Exception:
                            _apply_prompt()
                    except Exception:
                        pass
                # Refresh every 10s
                self.set_interval(10.0, _refresh_containers)
            except Exception:
                pass

            # Register this terminal's output widget for routing lookups
            try:
                from cai.tui.routing.output_router import register_terminal_output
                register_terminal_output(self.terminal_id, self.output)
            except Exception:
                pass
            
            # Set up periodic tooltip refresh (every 2 seconds)
            self.set_interval(2.0, self._update_header)
        except Exception:
            # Components might not be ready yet
            return

        # Register with streaming bridge if available
        if HAS_STREAMING_FIX:
            STREAMING_BRIDGE.register_terminal(self.terminal_id, self)
        
        # Register with terminal widget registry for action bar updates
        from cai.tui.core.terminal_widget_registry import register_terminal_widget
        register_terminal_widget(self.terminal_id, self)
        
        # Register with tool streaming handler for live output in action bar
        try:
            from cai.tui.display.tool_streaming_handler import get_tool_streaming_handler
            handler = get_tool_streaming_handler()
            handler.register_terminal(self.terminal_id, self)
        except ImportError:
            pass  # Handler not available

        # Emit terminal created event
        if self._emit_events:
            terminal_event_manager.emit(
                EventType.TERMINAL_CREATED,
                self.terminal_id,
                terminal_number=self.terminal_number,
                role=self._role
            )

        # If already configured, apply configuration
        if self._role != "empty":
            self.call_after_refresh(
                lambda: asyncio.create_task(
                    self.configure(self._role, self.agent_name, preserve_content=True)
                )
            )

    def on_click(self, event) -> None:
        """Handle click to select terminal"""
        # Prevent event bubbling
        event.stop()
        
        # Get the parent grid and request selection
        parent = self.parent
        while parent and parent.id != "terminal-grid-inner":
            parent = parent.parent
            
        if parent and parent.parent:
            # parent.parent should be StableTerminalGrid
            grid = parent.parent
            if hasattr(grid, 'focus_terminal'):
                grid.focus_terminal(self.terminal_id)
    
    async def configure(
        self, role: TerminalRole, agent_name: str = "", preserve_content: bool = False
    ) -> None:
        """Configure terminal for a specific role"""
        old_role = self._role

        # If already configured with the same role and agent, skip reconfiguration
        if self._role == role and self.state.agent_name == agent_name and self.output:
            return
        self._role = role
        self.state.role = role
        self.state.agent_name = agent_name
        self.state.is_active = role != "empty"
        self.agent_name = agent_name

        # Wait for terminal to be fully mounted
        if not self.output:
            return

        # Only clear if not preserving content AND if changing roles
        # Don't clear if banner has already been shown (unless explicitly changing roles)
        # Also don't clear when switching to agent role for streaming
        should_clear = not preserve_content and old_role != role and not (
            self._banner_shown and old_role == "empty" and role in ["main", "agent"]
        ) and not (role == "agent")  # Never clear when switching to agent role
        if should_clear:
            self.output.clear()
            self.state.output_buffer.clear()
            # Reset character count
            self._char_count = 0

        # Reset agent only if changing roles
        if old_role != role:
            self.agent = None

        # Update visual state
        await self._update_visual_state()
        
        # Update header to show agent name
        self._update_header()

        # Configure based on role
        if role == "main":
            await self._configure_as_main()
        elif role == "agent" and agent_name:
            await self._configure_as_agent(agent_name)
        elif role == "monitor":
            await self._configure_as_monitor()
        elif role == "logger":
            await self._configure_as_logger()
        else:
            await self._configure_as_empty()

        # Post role change message
        self.post_message(TerminalRoleChanged(self.terminal_id, old_role, role))

        # Emit terminal configured event
        terminal_event_manager.emit(
            EventType.TERMINAL_CONFIGURED,
            self.terminal_id,
            role=role,
            old_role=old_role,
            agent_name=agent_name
        )

    async def _configure_as_main(self) -> None:
        """Configure as main terminal"""
        # Show CAI banner instantly
        await self._show_banner_cascade()

        # Write initial message immediately
        self.output.write("")
        self.output.write(
            f"[dim]Terminal {self.terminal_number} ready - Type /help for commands[/dim]"
        )
        self.output.write("")

    async def _show_banner_cascade(self) -> None:
        """Show banner instantly"""
        from cai.repl.ui.banner import get_version

        version = get_version()

        # Full CAI banner - matching CLI colors
        banner_lines = [
            "[bold blue]                CCCCCCCCCCCCC      ++++++++   ++++++++      IIIIIIIIII[/bold blue]",
            "[bold blue]             CCC::::::::::::C  ++++++++++       ++++++++++  I::::::::I[/bold blue]",
            "[bold blue]           CC:::::::::::::::C ++++++++++         ++++++++++ I::::::::I[/bold blue]",
            "[bold blue]          C:::::CCCCCCCC::::C +++++++++    ++     +++++++++ II::::::II[/bold blue]",
            "[bold blue]         C:::::C       CCCCCC +++++++     +++++     +++++++   I::::I[/bold blue]",
            "[bold blue]        C:::::C                +++++     +++++++     +++++    I::::I[/bold blue]",
            "[bold blue]        C:::::C                ++++                   ++++    I::::I[/bold blue]",
            "[bold blue]        C:::::C                 ++                     ++     I::::I[/bold blue]",
            "[bold blue]        C:::::C                  +   +++++++++++++++   +      I::::I[/bold blue]",
            "[bold blue]        C:::::C                    +++++++++++++++++++        I::::I[/bold blue]",
            "[bold blue]        C:::::C                     +++++++++++++++++         I::::I[/bold blue]",
            "[bold blue]         C:::::C       CCCCCC        +++++++++++++++          I::::I[/bold blue]",
            "[bold blue]          C:::::CCCCCCCC::::C         +++++++++++++         II::::::II[/bold blue]",
            "[bold blue]           CC:::::::::::::::C           +++++++++           I::::::::I[/bold blue]",
            "[bold blue]             CCC::::::::::::C             +++++             I::::::::I[/bold blue]",
            "[bold blue]                CCCCCCCCCCCCC               ++              IIIIIIIIII[/bold blue]",
            "",
            f"[bold blue]                              Cybersecurity AI (CAI), v{version}[/bold blue]",
            "[white]                                  Bug bounty-ready AI[/white]",
        ]

        # Check if output exists
        if not self.output:
            return

        # Show all lines instantly
        for line in banner_lines:
            self.output.write(line)

        # Force immediate refresh
        self.output.refresh()

        # Also try to refresh the app screen
        if hasattr(self, 'app') and self.app:
            self.app.screen.refresh()

        # Yield control to ensure rendering
        await asyncio.sleep(0)

        # Mark banner as shown
        self._banner_shown = True

    async def _show_ready_message(self) -> None:
        """Show ready message after banner"""
        # No delay needed anymore
        if self.output:
            self.output.write("")
            self.output.write(
                f"[dim]Terminal {self.terminal_number} ready - Type /help for commands[/dim]"
            )
            self.output.write("")

    async def _configure_as_agent(self, agent_name: str) -> None:
        """Configure as agent terminal with truly isolated instance"""
        try:
            # Import here to avoid circular import
            from cai.agents import get_agent_by_name
            from cai.sdk.agents import Agent
            from cai.sdk.agents.models.openai_chatcompletions import OpenAIChatCompletionsModel
            
            # Create unique agent_id for this terminal using agent name
            agent_id = f"T{self.terminal_number}_{agent_name}"
            
            # Get a NEW agent instance with unique ID
            self.agent = get_agent_by_name(
                agent_name,
                agent_id=agent_id,
                model_override=os.getenv("CAI_MODEL", "alias1")
            )
            
            if self.agent:
                # Store agent info in state
                self.state.agent_name = agent_name
                self.state.agent_id = agent_id
                
                # Update agent display name to show terminal number
                if hasattr(self.agent, "name"):
                    original_name = self.agent.name
                    self.agent.name = f"{original_name} (T{self.terminal_number})"
                    
                # Ensure model has proper terminal context
                if hasattr(self.agent, "model"):
                    if hasattr(self.agent.model, "agent_id"):
                        self.agent.model.agent_id = agent_id
                    if hasattr(self.agent.model, "_terminal_id"):
                        self.agent.model._terminal_id = self.terminal_id
                    if hasattr(self.agent.model, "_terminal_number"):
                        self.agent.model._terminal_number = self.terminal_number

                # Show CAI banner with cascade effect - same as main terminal
                await self._show_banner_cascade()

                # Show ready message with agent info
                await asyncio.sleep(0.1)
                self.output.write(
                    f"[green]Agent '{agent_name}' loaded in Terminal {self.terminal_number}[/green]"
                )
                self.output.write(f"[dim]Agent ID: {agent_id}[/dim]")
                self.output.write(f"[dim]Type /help for commands[/dim]")
                self.output.write("")
            else:
                self.output.write(f"[red]Failed to create agent instance for '{agent_name}'[/red]")

        except Exception as e:
            self.output.write(f"[red]Error initializing: {e}[/red]")
            import traceback
            self.output.write(f"[red]{traceback.format_exc()}[/red]")

    async def _configure_as_monitor(self) -> None:
        """Configure as monitor terminal - same as any other terminal"""
        # Show CAI banner with cascade effect
        await self._show_banner_cascade()

        # Show ready message
        await asyncio.sleep(0.1)
        self.output.write(
            f"[dim]Terminal {self.terminal_number} ready - Type /help for commands[/dim]"
        )
        self.output.write("")

    async def _configure_as_logger(self) -> None:
        """Configure as logger terminal - same as any other terminal"""
        # Show CAI banner with cascade effect
        await self._show_banner_cascade()

        # Show ready message
        await asyncio.sleep(0.1)
        self.output.write(
            f"[dim]Terminal {self.terminal_number} ready - Type /help for commands[/dim]"
        )
        self.output.write("")

    async def _configure_as_empty(self) -> None:
        """Configure as empty terminal"""
        # Show CAI banner with cascade effect
        await self._show_banner_cascade()

        # Show ready message
        await asyncio.sleep(0.1)
        self.output.write(
            f"[dim]Terminal {self.terminal_number} ready - Type /help for commands[/dim]"
        )
        self.output.write("")

    async def _update_visual_state(self) -> None:
        """Update visual elements - all terminals look the same"""
        # Remove all role classes
        self.remove_class("role-empty", "role-main", "role-agent", "role-monitor", "role-logger")

        # All terminals use the same styling
        self.add_class("role-main")

        # No need to update indicator anymore - using border styling

        # Update header
        self._update_header()

    def watch_is_running(self, is_running: bool) -> None:
        """React to is_running changes"""
        # Skip UI updates in broadcast mode
        if os.getenv('CAI_BROADCAST_MODE') != 'true':
            if is_running:
                self.add_class("agent-running")
            else:
                self.remove_class("agent-running")
            
            # Update status indicator
            try:
                status = self.query_one(f"#status-indicator-{self.terminal_id}", Static)
                status.update("●")  # Visual indicator updates automatically via CSS
            except:
                pass
    
    def _update_header(self) -> None:
        """Update header based on terminal number and agent"""
        try:
            header = self.query_one(f"#header-{self.terminal_id}", Static)
        except Exception:
            # Terminal not fully mounted yet
            return

        # Build header text with terminal number, agent name, and model
        header_parts = [f"T{self.terminal_number}"]
        
        if self.state.agent_name:
            header_parts.append(self.state.agent_name)
            
        if self.state.model_name:
            # Show shortened model name for common models
            model_display = self.state.model_name
            if model_display.startswith("gpt-"):
                model_display = model_display.replace("gpt-", "")
            elif model_display.startswith("claude-"):
                model_display = model_display.replace("claude-3-", "c3-").replace("claude-", "c-")
            header_parts.append(f"[dim]{model_display}[/dim]")
        
        header_text = " | ".join(header_parts)
        header.update(f"[bold cyan]{header_text}[/bold cyan]")
        
        # Update tooltip with current message history
        new_tooltip = self._get_terminal_tooltip()
        if header.tooltip != new_tooltip:
            header.tooltip = new_tooltip

        # Note: dropdown selection is updated by command handlers when commands are used

        # Note: dropdown selection is updated by command handlers when commands are used

        # Reflect active container in dropdown placeholder
        try:
            select = self.query_one(f"#container-select-{self.terminal_id}")
            active_value = self._resolve_active_container_id()
            if hasattr(select, "_choices"):
                choices = list(getattr(select, "_choices", []))
                if active_value and not any(val == active_value for _, val in choices):
                    choices.insert(1, (f"{active_value} (container)", active_value))
                    try:
                        select.set_options(choices)
                    except Exception:
                        pass
                if active_value:
                    for _, value in getattr(select, "_choices", []):
                        if value == active_value:
                            select.value = value
                            break
                else:
                    # Clear selection so only the placeholder is shown
                    try:
                        select.value = None
                    except Exception:
                        pass
            self._set_container_prompt(select, active_value)
        except Exception:
            pass

    def on_select_changed(self, event) -> None:
        """Handle selection change for container dropdown."""
        try:
            from textual.widgets import Select
            # Container selector
            if isinstance(event.control, Select) and event.control.id == f"container-select-{self.terminal_id}":
                selected = (event.value or "").strip()
                # Set per-agent env var to route commands for this terminal's agent
                from cai.tools.common import _get_agent_token_info
                token_info = _get_agent_token_info()
                if token_info and token_info.get("agent_id"):
                    os.environ[f"CAI_ACTIVE_CONTAINER_FOR_{token_info.get('agent_id')}"] = selected
                if token_info and token_info.get("agent_name"):
                    import re
                    safe = re.sub(r"[^A-Za-z0-9_]+", "_", str(token_info.get("agent_name")))
                    os.environ[f"CAI_ACTIVE_CONTAINER_FOR_NAME_{safe}"] = selected
                # Also set default if nothing else
                if not selected:
                    os.environ.pop("CAI_ACTIVE_CONTAINER", None)
                    # Clear selection in UI so only the placeholder is shown
                    try:
                        event.control.value = None
                    except Exception:
                        pass
                else:
                    os.environ["CAI_ACTIVE_CONTAINER"] = selected
                self._set_container_prompt(event.control, selected)
                return

            # Agent selector
            if isinstance(event.control, Select) and event.control.id == f"agent-select-{self.terminal_id}":
                new_agent = (event.value or "").strip()
                if not new_agent:
                    return
                
                # Only show confirmation if this is actually a change from current agent
                current_agent = getattr(self.state, 'agent_name', None) or getattr(self, 'agent_name', None)
                if current_agent and current_agent != new_agent:
                    # Show agent change confirmation with system prompt (unified with command behavior)
                    self._show_agent_change_confirmation(new_agent)
                
                # Switch agent for this terminal via SessionManager
                try:
                    import asyncio
                    app = self.app
                    if hasattr(app, 'session_manager') and app.session_manager:
                        asyncio.create_task(app.session_manager.update_terminal_agent(self.terminal_number, new_agent))
                except Exception:
                    pass
                return

            # Model selector
            if isinstance(event.control, Select) and event.control.id == f"model-select-{self.terminal_id}":
                new_model = (event.value or "").strip()
                if not new_model:
                    return
                
                # Only show processing message if this is actually a change from current model
                current_model = getattr(self.state, 'model_name', None) or getattr(self, 'model_name', None)
                if current_model and current_model != new_model:
                    # Show processing feedback (unified with command behavior)
                    self.output.write(f"[yellow]Processing model change to: {new_model}...[/yellow]")
                
                # Update model via TerminalRunner with unified panel display
                try:
                    import asyncio
                    app = self.app
                    if hasattr(app, 'session_manager') and app.session_manager:
                        runner = app.session_manager.terminal_runners.get(self.terminal_number)
                        if runner and hasattr(runner, 'update_model'):
                            # Use the unified panel display from TerminalRunner
                            asyncio.create_task(runner.update_model(new_model, silent=False))
                            # Update UI state
                            self.state.model_name = new_model
                            self.model_name = new_model
                            self._update_header()
                except Exception:
                    pass
        except Exception:
            pass
    
    def _get_terminal_tooltip(self) -> str:
        """Get tooltip text with terminal's message history summary"""
        tooltip_parts = [f"[bold bright_green]Terminal {self.terminal_number}[/bold bright_green]"]
        
        # Add agent info with ID if available
        agent_id = None
        if self.state.agent_name:
            # Get agent ID from state or session manager
            if hasattr(self.state, 'agent_id') and self.state.agent_id:
                agent_id = self.state.agent_id
            else:
                # Try to get from session manager
                try:
                    app = self.app
                    if hasattr(app, 'session_manager') and app.session_manager:
                        if self.terminal_number in app.session_manager.terminal_runners:
                            runner = app.session_manager.terminal_runners[self.terminal_number]
                            if hasattr(runner, 'agent') and runner.agent:
                                if hasattr(runner.agent, 'model') and hasattr(runner.agent.model, 'agent_id'):
                                    agent_id = runner.agent.model.agent_id
                except:
                    pass
            
            if agent_id:
                tooltip_parts.append(f"\n[cyan]{self.state.agent_name}[/cyan] [{agent_id}]")
            else:
                tooltip_parts.append(f"\n[cyan]{self.state.agent_name}[/cyan]")
        
        # Model info (shortened)
        if self.state.model_name:
            model_display = self.state.model_name
            if model_display.startswith("gpt-"):
                model_display = model_display.replace("gpt-", "")
            elif model_display.startswith("claude-"):
                model_display = model_display.replace("claude-3-", "c3-").replace("claude-", "c-")
            tooltip_parts.append(f"[green]{model_display}[/green]")
        
        # Temperature info
        from cai.sdk.agents.model_settings import DEFAULT_TEMPERATURE, DEFAULT_TOP_P

        temperature = DEFAULT_TEMPERATURE
        top_p = DEFAULT_TOP_P
        try:
            # Try to get from runner's agent
            app = self.app
            if hasattr(app, 'session_manager') and app.session_manager:
                if self.terminal_number in app.session_manager.terminal_runners:
                    runner = app.session_manager.terminal_runners[self.terminal_number]
                    if hasattr(runner, 'agent') and runner.agent:
                        if hasattr(runner.agent, 'model') and hasattr(runner.agent.model, 'temperature'):
                            temperature = runner.agent.model.temperature
                        if hasattr(runner.agent, 'model_settings') and runner.agent.model_settings:
                            if runner.agent.model_settings.top_p is not None:
                                top_p = runner.agent.model_settings.top_p
                    else:
                        # If no agent yet, get from environment
                        temperature = float(os.getenv("CAI_TEMPERATURE", str(DEFAULT_TEMPERATURE)))
                        top_p = float(os.getenv("CAI_TOP_P", str(DEFAULT_TOP_P)))
        except:
            # Fallback to environment
            temperature = float(os.getenv("CAI_TEMPERATURE", str(DEFAULT_TEMPERATURE)))
            top_p = float(os.getenv("CAI_TOP_P", str(DEFAULT_TOP_P)))

        tooltip_parts.append(f"[yellow]Temperature: {temperature:.1f} | Top-P: {top_p:.1f}[/yellow]")
        
        # Message count by type (user, assistant, tool)
        user_count = 0
        assistant_count = 0
        tool_count = 0
        total_count = 0
        
        try:
            app = self.app
            if hasattr(app, 'session_manager') and app.session_manager:
                if self.terminal_number in app.session_manager.terminal_runners:
                    runner = app.session_manager.terminal_runners[self.terminal_number]
                    messages = []
                    
                    # Try to get from agent's model first (most accurate)
                    if hasattr(runner, 'agent') and runner.agent:
                        if hasattr(runner.agent, 'model') and hasattr(runner.agent.model, 'message_history'):
                            messages = runner.agent.model.message_history
                    # Fallback to runner's message history
                    elif hasattr(runner, 'message_history'):
                        messages = runner.message_history
                    
                    # Count messages by role
                    for msg in messages:
                        if isinstance(msg, dict) and 'role' in msg:
                            role = msg['role']
                            if role == 'user':
                                user_count += 1
                            elif role == 'assistant':
                                assistant_count += 1
                            elif role in ['tool', 'function']:
                                tool_count += 1
                    
                    total_count = len(messages)
                    
                    if total_count > 0:
                        tooltip_parts.append(f"\n[yellow]Messages ({total_count}):[/yellow]")
                        if user_count > 0:
                            tooltip_parts.append(f"  [cyan]User: {user_count}[/cyan]")
                        if assistant_count > 0:
                            tooltip_parts.append(f"  [green]Assistant: {assistant_count}[/green]")
                        if tool_count > 0:
                            tooltip_parts.append(f"  [magenta]Tool: {tool_count}[/magenta]")
                    else:
                        tooltip_parts.append(f"\n[dim]No messages yet[/dim]")
                else:
                    tooltip_parts.append(f"\n[dim]Terminal not initialized[/dim]")
        except Exception as e:
            tooltip_parts.append(f"\n[dim]No message data[/dim]")
        
        return "\n".join(tooltip_parts)


    def set_summarized_mode(self, enabled: bool) -> None:
        """Enable or disable summarized mode for 4+ terminals"""
        # Only update if the mode actually changes
        if self._summarized_mode == enabled:
            return
            
        self._summarized_mode = enabled
        # No notification needed - just enable the mode silently
    
    def write(self, text: Any, end: str = "\n") -> None:
        """Write text or Rich renderable to terminal - thread-safe with error handling"""
        if self.output:
            # In summarized mode, filter content AND skip Rich panels
            if self._summarized_mode:
                # Skip Rich Panel objects completely for performance
                if hasattr(text, '__rich__') or hasattr(text, '__rich_console__'):
                    # Check if it's a Panel or similar Rich object
                    class_name = text.__class__.__name__
                    if class_name in ['Panel', 'Table', 'Columns', 'Group', 'Padding', 'Align']:
                        return
                    # Allow simple Text objects
                    if class_name != 'Text':
                        return
                
                # For string content, apply filtering
                if isinstance(text, str):
                    # Skip tool output and verbose content
                    text_lower = text.lower()
                    if any(skip in text_lower for skip in [
                        "executing", "running", "starting", "loading", "initializing",
                        "processing", "fetching", "calling", "invoking", "applying",
                        "debug:", "trace:", "verbose:", "info:", "[dim]"
                    ]):
                        return
                    # Skip empty lines and separators
                    if not text.strip() or text.strip() in ["─" * 50, "═" * 50, "-" * 50, "━" * 50]:
                        return
                    # Skip progress indicators
                    if any(char in text for char in ["⣾", "⣽", "⣻", "⢿", "⡿", "⣟", "⣯", "⣷", "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"]):
                        return
                    # Keep only important messages
                    important_keywords = ["error", "warning", "success", "complete", "done", "failed", "result", "answer", "response", "tool"]
                    if not any(keyword in text_lower for keyword in important_keywords):
                        # Check if it's a command or user input
                        if not (text.strip().startswith(">") or text.strip().startswith("[bold cyan]>") or ">>>" in text):
                            # Not important, skip
                            return
            
            # Skip sanitization to preserve content formatting
            # ContentValidator was causing issues with streaming display
            
            # Performance optimization: batch writes for throttling
            import time
            current_time = time.time() * 1000  # milliseconds
            
            # Check if we're in the main thread
            import threading

            if threading.current_thread() != threading.main_thread():
                # We're in a background thread, use call_from_thread
                try:
                    from textual.app import App
                    app = App.get_running_app()
                    app.call_from_thread(self._write_with_throttle, text, end, current_time)
                except Exception as e:
                    # Fall back to direct write if app not available
                    try:
                        self._write_with_throttle(text, end, current_time)
                    except Exception:
                        # Last resort - write to stderr
                        import sys
                        print(f"[TUI Error] Failed to write: {str(text)[:100]}", file=sys.stderr)
            else:
                # Main thread, safe to write directly
                try:
                    self._write_with_throttle(text, end, current_time)
                except Exception as e:
                    # Handle widget errors
                    import sys
                    print(f"[TUI Error] Write failed: {str(e)}", file=sys.stderr)

    def _write_with_throttle(self, text: Any, end: str, current_time: float) -> None:
        """Render scheduler: coalesce writes and pace frames (30/15 FPS)."""
        # Add to buffer
        self._write_buffer.append((text, end))

        # Frame pacing
        throttle_ms = self._many_terminals_throttle_ms if self._summarized_mode else self._write_throttle_ms
        time_since_last = current_time - self._last_write_time

        if time_since_last >= throttle_ms:
            self._last_write_time = current_time
            self._flush_write_buffer()
        else:
            if self._write_timer is None:
                remaining_time = max(0, throttle_ms - time_since_last)
                self._write_timer = self.set_timer(remaining_time / 1000.0, self._flush_write_buffer)
    
    def _flush_write_buffer(self) -> None:
        """Flush all buffered writes"""
        if not self._write_buffer:
            return
            
        # Clear timer
        if self._write_timer:
            self._write_timer.stop()
            self._write_timer = None
            
        # Process all buffered writes with coalescing
        buffer_copy = self._write_buffer[:]
        self._write_buffer.clear()

        merged: list[tuple[Any, str]] = []
        current_text_parts: list[str] = []

        for text, end in buffer_copy:
            if isinstance(text, str):
                current_text_parts.append(text)
            else:
                if current_text_parts:
                    merged.append(("".join(current_text_parts), "\n"))
                    current_text_parts = []
                merged.append((text, end))

        if current_text_parts:
            merged.append(("".join(current_text_parts), "\n"))

        # Single UI write per merged item
        for item, end in merged:
            try:
                self._write_internal(item, end)
            except Exception:
                pass

    def _write_internal(self, text: Any, end: str = "\n") -> None:
        """Internal write method - must be called from main thread"""
        if self.output:
            try:
                # DISABLED - Character limit clearing causes TUI blocking
                # The clear() operation blocks the entire UI when multiple terminals
                # are active. Let RichLog handle its own max_lines limit instead.
                pass
                
                # RichLog doesn't support 'end' parameter
                # Just write the text/renderable as-is
                # If it's a Rich Panel or other renderable, try to pass expand=True
                if hasattr(text, '__rich__') or hasattr(text, '__rich_console__'):
                    try:
                        self.output.write(text, expand=True)
                    except TypeError as e:
                        if "expand" in str(e):
                            self.output.write(text)
                        else:
                            raise
                else:
                    self.output.write(text)

                # Add to buffer without limit (only for strings)
                if self.state.is_active and isinstance(text, str):
                    self.state.output_buffer.append(text)
            except Exception as e:
                # Handle edge cases like widget not mounted
                if "not mounted" in str(e).lower():
                    # Widget not mounted yet, queue for later
                    pass
                else:
                    raise

    def add_streaming_widget(self, widget) -> None:
        """Add a streaming widget to the terminal"""
        if not hasattr(self, "streaming_widgets"):
            self.streaming_widgets = []
        
        # Get the output container
        try:
            output_container = self.query_one(".terminal-output-container", Vertical)
            if output_container:
                # Add spacing before widget
                self.output.write("")
                # Mount the widget
                output_container.mount(widget)
                self.streaming_widgets.append(widget)
        except Exception as e:
            print(f"[Terminal] Could not add streaming widget: {e}")
    
    def mount_in_output(self, widget) -> None:
        """Mount a widget in the terminal output area"""
        try:
            output_container = self.query_one(".terminal-output-container", Vertical)
            if output_container:
                output_container.mount(widget)
        except Exception as e:
            print(f"[Terminal] Could not mount widget: {e}")

    def write_command(self, command: str) -> None:
        """Write command with formatting"""
        self.write(f"[bold cyan]>[/bold cyan] {command}")
        self.state.command_history.append(command)
        
        # Update header to refresh tooltip with new message count
        self._update_header()

        # Emit command event
        terminal_event_manager.emit(
            EventType.TERMINAL_COMMAND,
            self.terminal_id,
            command=command,
            terminal_number=self.terminal_number
        )

    async def run_command(self, command: str, use_cli_logic: bool = True) -> None:
        """Run command in this terminal using cli.py logic"""
        self.write_command(command)

        if self._role == "agent" and self.agent:
            if use_cli_logic:
                # Use the proper CLI command processing logic
                await self._run_command_with_cli_logic(command)
            else:
                await self._run_agent_command(command)
        elif self._role == "main":
            # Main terminal processes commands normally
            pass

    async def _run_agent_command(self, command: str) -> None:
        """Run command in agent context"""
        if not self.agent:
            return

        # Capture output
        captured_output = io.StringIO()
        with redirect_stdout(captured_output):
            with redirect_stderr(captured_output):
                try:
                    from cai.sdk.agents import Runner
                    await Runner.run(self.agent, command)

                    # Write any captured output
                    captured = captured_output.getvalue()
                    if captured:
                        self.output.write(captured)

                except Exception as e:
                    self.output.write(f"[red]Error: {e}[/red]")
                    
    async def _run_command_with_cli_logic(self, command: str) -> None:
        """Run command using proper CLI logic from cli.py"""
        if not self.agent:
            return
            
        try:
            # Check if this is a command or chat message
            if command.startswith("/"):
                # Process as REPL command - handle internally
                from cai.repl.commands import available_commands
                
                parts = command.split()
                cmd_name = parts[0][1:]  # Remove the /
                args = parts[1:] if len(parts) > 1 else []
                
                # Handle common commands locally
                if cmd_name == "help":
                    self._show_help()
                elif cmd_name == "model" and args:
                    # Update model for this agent - message will be shown by TerminalRunner
                    new_model = args[0]
                    if hasattr(self.agent, "model"):
                        self.agent.model.model = new_model
                        # Panel message will be shown by the unified system
                elif cmd_name == "clear":
                    self.output.clear()
                    # Reset character count
                    self._char_count = 0
                else:
                    # For other commands, show not implemented
                    self.output.write(f"[yellow]Command /{cmd_name} not implemented for agent terminals[/yellow]")
            else:
                # Process as chat message using Runner
                from cai.sdk.agents import Runner
                result = await Runner.run(self.agent, command)
                # Output is handled by the streaming/display system
                
        except Exception as e:
            self.output.write(f"[red]Error: {e}[/red]")
            import traceback
            self.output.write(f"[red]{traceback.format_exc()}[/red]")
            
    def _show_help(self) -> None:
        """Show help for terminal commands"""
        self.output.write("[bold cyan]Terminal Commands:[/bold cyan]")
        self.output.write("  /help     - Show this help")
        self.output.write("  /model    - Change model (e.g., /model gpt-4)")
        self.output.write("  /clear    - Clear terminal output")
        self.output.write("")
        self.output.write("[bold cyan]Navigation:[/bold cyan]")
        self.output.write("  Tab       - Next terminal")
        self.output.write("  Shift+Tab - Previous terminal")
        self.output.write("  Ctrl+C    - Cancel execution")
        self.output.write("")
    
    # Streaming API methods
    def start_streaming_line(self, line_id: str, header: str = "") -> None:
        """Start a new streaming line with error handling"""
        if not self.output:
            return
        
        # NO TOCAR EL HEADER
        
        # Update action bar to show streaming
        if hasattr(self, 'action_bar'):
            # Extract agent name from header
            agent_name = "agent"
            if ">>" in header:
                # Remove ANSI/markup codes first
                import re
                clean_header = re.sub(r'\[.*?\]', '', header)
                parts = clean_header.split(">>")
                if len(parts) > 0:
                    # Get the part after the number and before >>
                    agent_part = parts[0].strip()
                    # Remove the number prefix if exists
                    if " " in agent_part:
                        agent_name = agent_part.split(" ", 1)[1].strip()
                    else:
                        agent_name = agent_part
            
            try:
                self.action_bar.start_streaming(agent_name)
            except Exception:
                pass
        
        # First, write an empty line to separate from previous content
        try:
            self.output.write("")
        except:
            pass
        
        # Create a container for this streaming line if not exists
        from textual.containers import Container
        from textual.widgets import Label
        
        if line_id not in self._streaming_lines:
            try:
                # Since we're showing streaming in the action bar, we don't need a label
                # Just store the streaming info
                self._streaming_lines[line_id] = {
                    "widget": None,
                    "header": header,
                    "content": "",
                    "char_index": 0,
                    "error_count": 0
                }
            except Exception as e:
                # Fallback to regular write
                self.write(header + "[yellow]⚡ Streaming...[/yellow]")
                # Log error
                if hasattr(self, '_log_streaming_error'):
                    self._log_streaming_error(line_id, e)
    
    def update_streaming_line(self, line_id: str, content: str) -> None:
        """Update content for a streaming line with validation"""
        if line_id in self._streaming_lines:
            # NO TOCAR NADA - pasar el contenido tal cual
            self._streaming_lines[line_id]["content"] = content
            
            # Batch updates for action bar - store pending update
            if not hasattr(self, '_pending_action_bar_update'):
                self._pending_action_bar_update = None
                self._last_action_bar_update = 0
            
            self._pending_action_bar_update = content
            current_time = time.time()
            
            # BUGFIX: Synchronize with unified frequency (150ms) to prevent scroll flicker
            if current_time - self._last_action_bar_update > 0.15:
                self._last_action_bar_update = current_time
                if hasattr(self, 'action_bar'):
                    try:
                        self.action_bar.update_streaming_text(self._pending_action_bar_update)
                        self._pending_action_bar_update = None
                    except Exception:
                        pass
    
    def _update_streaming_display(self, line_id: str) -> None:
        """Progressive character display - not needed anymore since we use action bar"""
        # This method is kept for compatibility but doesn't do anything
        # The actual streaming display is handled by the action bar
        pass
    
    def finish_streaming_line(self, line_id: str, final_content: str, stats: Optional[Dict] = None) -> None:
        """Finish a streaming line and move it to the log with cleanup"""
        if line_id not in self._streaming_lines:
            return
        
        line_data = self._streaming_lines[line_id]
        
        try:
            widget = line_data.get("widget")
            
            # NO TOCAR EL CONTENIDO FINAL
            
            # Build final text
            final_text = line_data["header"] + final_content + " [green]✓[/green]"
            
            if stats:
                stats_parts = []
                if stats.get("output_tokens"):
                    stats_parts.append(f"{stats['output_tokens']} tokens")
                if stats.get("interaction_cost"):
                    stats_parts.append(f"${stats['interaction_cost']:.4f}")
                if stats.get("session_total_cost"):
                    stats_parts.append(f"session: ${stats['session_total_cost']:.4f}")
                
                if stats_parts:
                    final_text += f" [dim]({', '.join(stats_parts)})[/dim]"
            
            # Remove the streaming widget first
            if widget:
                try:
                    # Stop any timers first
                    if hasattr(widget, "stop"):
                        widget.stop()
                    widget.remove()
                except Exception:
                    # Widget might already be removed
                    pass
            
            # Create a panel for the final message
            from cai.tui.display.panel_formatter import PanelFormatter
            
            # Extract agent name and content from the header and final content
            agent_name = ""
            if "Agent" in line_data["header"]:
                # Extract agent name from header like "[1] Agent >> "
                parts = line_data["header"].split("]")
                if len(parts) > 1:
                    agent_part = parts[1].strip()
                    if ">>" in agent_part:
                        agent_name = agent_part.split(">>")[0].strip()
            
            # Create metadata for the panel
            metadata = {
                "interaction": 1,  # We'll extract this from the header
                "timestamp": datetime.now().strftime("%H:%M:%S"),
                "model": stats.get("model") if stats else None
            }
            
            # Try to extract interaction counter from header
            if "[" in line_data["header"] and "]" in line_data["header"]:
                try:
                    counter_part = line_data["header"].split("[")[1].split("]")[0]
                    metadata["interaction"] = int(counter_part)
                except:
                    pass
            
            # Check if this is an agent message using the programmatic flag
            is_agent_message = stats.get("is_agent_message", False) if stats else False
            
            # If it's an agent message, create the panel
            if is_agent_message:
                # Use agent_name from stats if available
                if stats and stats.get("agent_name"):
                    agent_name = stats["agent_name"]
                elif not agent_name:
                    # Fallback: try to extract from header
                    header = line_data.get("header", "")
                    if ">>" in header:
                        agent_name = header.split(">>")[0].strip()
                        if "]" in agent_name:
                            agent_name = agent_name.split("]")[1].strip()
                
                # Create the panel
                panel = PanelFormatter.create_agent_panel(
                    agent_name=agent_name or "Agent",
                    message=final_content,
                    metadata=metadata,
                    streaming=False,
                    token_info=stats
                )
                
                # Write the panel to the output
                if self.output:
                    self.output.write(panel)
                    self.output.write("")  # Add spacing
            
            # Update action bar to show completion (ensure last chunk is flushed)
            if hasattr(self, 'action_bar'):
                try:
                    if isinstance(final_content, str):
                        # Push the full final content before completing
                        self.action_bar.update_streaming_text(final_content)
                    # If stats indicates an error, mark it so the action bar shows it
                    if stats and isinstance(stats, dict) and stats.get("is_error"):
                        if hasattr(self.action_bar, 'mark_stream_error'):
                            self.action_bar.mark_stream_error(str(stats.get("error_message", "Error")))
                    self.action_bar.complete_streaming()
                except Exception:
                    pass
        finally:
            # Always clean up
            self._streaming_lines.pop(line_id, None)
            
        # Add empty line
        try:
            self.output.write("")
        except:
            pass

    def clear(self) -> None:
        """Clear terminal"""
        if self.output:
            try:
                self.output.clear()
                # Reset character count
                self._char_count = 0
                # Always re-show banner for all terminals
                asyncio.create_task(self._show_banner_cascade())
                # Show ready message after banner
                asyncio.create_task(self._show_ready_message())
            except Exception as e:
                # Handle clear errors
                import sys
                print(f"[TUI Error] Failed to clear terminal: {str(e)}", file=sys.stderr)

            # Emit clear event
            terminal_event_manager.emit(
                EventType.TERMINAL_CLEARED,
                self.terminal_id,
                terminal_number=self.terminal_number
            )

    
        
        
    
    
    
    def set_running(self, running: bool) -> None:
        """Set the running state of the agent"""
        import os
        self.is_running = running
        
        # Skip UI updates if we're in broadcast mode (running in thread pool)
        if os.getenv('CAI_BROADCAST_MODE') != 'true':
            if running:
                self.add_class("agent-running")
                self._start_execution_animation()
            else:
                self.remove_class("agent-running")
                self._stop_execution_animation()
    

    def show_tool_execution(self, tool_name: str, args: Any = None) -> None:
        """Show tool execution in action bar"""
        if hasattr(self, 'action_bar') and self.action_bar:
            try:
                # Avoid duplication: generic_linux_command is handled elsewhere
                if tool_name == "generic_linux_command":
                    return
                # Default path
                self.action_bar.show_tool_execution(tool_name, args)
            except Exception:
                # If action bar update fails, continue
                pass
    
    def on_unmount(self) -> None:
        """Cleanup when unmounted"""
        # Unregister from streaming bridge if available
        if HAS_STREAMING_FIX:
            STREAMING_BRIDGE.unregister_terminal(self.terminal_id)

    @property
    def is_configured(self) -> bool:
        """Check if terminal is configured"""
        return self._role != "empty"


    async def reset(self) -> None:
        """Reset terminal to empty state with cleanup"""
        try:
            # Clean up any streaming lines
            for line_id in list(self._streaming_lines.keys()):
                self.finish_streaming_line(line_id, "[Interrupted]", None)
            
            # Reset state
            self.state = TerminalState(terminal_id=self.terminal_id)
            
            # Clear output
            if self.output:
                try:
                    self.output.clear()
                    # Reset character count
                    self._char_count = 0
                except:
                    pass
            
            # Reconfigure
            await self.configure("empty")
        except Exception as e:
            import sys
            print(f"[TUI Error] Failed to reset terminal: {str(e)}", file=sys.stderr)
    
    def _log_streaming_error(self, line_id: str, error: Exception):
        """Log streaming errors"""
        try:
            import traceback
            with open(f"{_CAI_DEBUG_DIR}/cai_streaming_errors.log", "a") as f:
                f.write(f"Streaming error for {line_id}: {error}\n")
                f.write(traceback.format_exc())
                f.write("\n")
        except:
            pass
    
    
    def _start_execution_animation(self) -> None:
        """Start the execution indicator animation in action bar"""
        if not self._execution_indicator_shown:
            self._execution_indicator_shown = True
            self._execution_frame = 0
            
            # Start timer to update action bar with animation
            if self._execution_timer is None:
                self._execution_timer = self.set_interval(0.5, self._animate_execution)
    
    def _stop_execution_animation(self) -> None:
        """Stop the execution indicator animation"""
        if self._execution_indicator_shown:
            self._execution_indicator_shown = False
            
            # Stop timer
            if self._execution_timer is not None:
                self._execution_timer.stop()
                self._execution_timer = None
            
            # Clear the execution indicator from action bar
            if hasattr(self, 'action_bar'):
                try:
                    self.action_bar.clear_execution_indicator()
                except:
                    pass
    
    def _animate_execution(self) -> None:
        """Animate the execution indicator in action bar"""
        if self._execution_indicator_shown and hasattr(self, 'action_bar'):
            self._execution_frame += 1
            
            # Rotating braille animation
            braille_frames = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"]
            indicator = braille_frames[self._execution_frame % len(braille_frames)]
            
            # Update action bar with animated indicator only if not streaming
            try:
                if not hasattr(self.action_bar, '_is_streaming') or not self.action_bar._is_streaming:
                    self.action_bar.show_execution_indicator(indicator)
            except:
                pass

    # Clipboard helpers for terminal output
    def action_copy_terminal_visible(self) -> None:
        try:
            app = self.app
            console = Console(record=True, width=180)
            # RichLog does not expose lines; re-render from our history if available via action_bar
            if hasattr(self.action_bar, '_log_history') and self.action_bar._log_history:
                # Best-effort: collect last 800 lines from action bar + output area title
                for item in self.action_bar._log_history[-800:]:
                    console.print(item)
            text = console.export_text(clear=False)
            if app and hasattr(app, 'copy_to_clipboard'):
                app.copy_to_clipboard(text)
        except Exception:
            pass
    
    def action_copy_terminal_all(self) -> None:
        try:
            app = self.app
            console = Console(record=True, width=180)
            if hasattr(self.action_bar, '_log_history') and self.action_bar._log_history:
                for item in self.action_bar._log_history:
                    console.print(item)
            text = console.export_text(clear=False)
            if app and hasattr(app, 'copy_to_clipboard'):
                app.copy_to_clipboard(text)
        except Exception:
            pass

    def _show_agent_change_confirmation(self, agent_name: str) -> None:
        """Show agent change confirmation with system prompt (unified with command behavior)"""
        try:
            from rich.panel import Panel
            from cai.agents import get_agent_by_name
            
            # Show basic confirmation first
            self.output.write(f"[green]Switched to agent: {agent_name}[/green]")
            
            # Try to get the agent and show system prompt
            try:
                agent = get_agent_by_name(agent_name)
                if agent:
                    # Display the system prompt (same format as /agent command)
                    self.output.write("\n[bold yellow]System Prompt:[/bold yellow]")
                    instructions = agent.instructions
                    if callable(instructions):
                        instructions = instructions()
                    
                    # Truncate very long instructions (same logic as /agent command)
                    if len(instructions) > 500:
                        self.output.write(f"[dim]{instructions[:500]}...[/dim]")
                        self.output.write(
                            "[dim italic](Truncated for display - full prompt used by agent)[/dim italic]"
                        )
                    else:
                        self.output.write(f"[dim]{instructions}[/dim]")
                    
                    self.output.write("")  # Add blank line for spacing
            except Exception:
                # If we can't get the agent details, just show basic confirmation
                pass
                
        except Exception:
            # Fallback to simple message if Panel fails
            self.output.write(f"[green]Switched to agent: {agent_name}[/green]")
