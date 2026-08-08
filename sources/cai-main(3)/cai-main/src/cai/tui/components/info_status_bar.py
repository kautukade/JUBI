"""
Interactive status bar displaying useful information like costs, tokens, and context usage
"""

import os
from typing import Any, Dict

from rich.text import Text
from textual.app import ComposeResult
from textual.containers import Container, Horizontal
from textual.reactive import reactive
from textual.widgets import Static

from cai.config import compacted_memory_env_enabled
from cai.tui.components.info_status_bar_updater import register_info_bar, unregister_info_bar
from cai.util import COST_TRACKER, get_active_time, get_idle_time

class InfoStatusBar(Container):
    """Interactive status bar showing real-time information"""

    DEFAULT_CSS = """
    /* Global horizontal container scrollbar styling */
    Horizontal {
        scrollbar-size: 0 1;
        scrollbar-color: #529d86;
        scrollbar-background: #2e4f46;
    }
    
    InfoStatusBar {
        height: 2;
        width: 100%;
        background: $surface-darken-1;
        border-top: solid $border;
        padding: 0 2;
        layout: horizontal;
        overflow-x: auto;
        overflow-y: hidden;
        scrollbar-size: 0 1;
        scrollbar-color: #529d86;
        scrollbar-background: #2e4f46;
    }
    
    InfoStatusBar #status-bar-content {
        width: 100%;
        height: 100%;
        layout: horizontal;
        align: center middle;
        scrollbar-size: 0 1;
        scrollbar-color: #529d86;
        scrollbar-background: #2e4f46;
    }
    
    InfoStatusBar .info-section {
        width: auto;
        height: 100%;
        margin: 0 2;
        padding: 0 1;
        content-align: center middle;
        min-width: 1;
    }
    
    InfoStatusBar Static {
        height: 100%;
        width: auto;
        background: transparent;
        color: $text;
    }
    
    InfoStatusBar .separator {
        width: 1;
        color: $text-muted;
        content-align: center middle;
        height: 100%;
        margin: 0 1;
        padding: 0;
    }
    """

    # Reactive properties for dynamic updates
    total_cost = reactive(0.0)
    current_cost = reactive(0.0)
    context_usage = reactive(0.0)
    input_tokens = reactive(0)
    output_tokens = reactive(0)
    reasoning_tokens = reactive(0)
    active_time = reactive("0s")
    idle_time = reactive("0s")
    model_name = reactive("")
    auto_compact = reactive(True)
    memory_enabled = reactive(False)
    streaming_enabled = reactive(False)
    last_error = reactive("")  # Store last error from any terminal

    def __init__(self, terminal_number: int = 1, **kwargs):
        super().__init__(**kwargs)
        self.terminal_number = terminal_number
        self._update_timer = None
        self.agent_name = ""
        self.agent_type = ""

    def compose(self) -> ComposeResult:
        """Compose the status bar layout"""
        with Horizontal(id="status-bar-content"):
            # Agent/Model info
            yield Static("", classes="info-section", id="agent-model-info", markup=True)
            yield Static("•", classes="separator", markup=True)

            # Workspace info
            yield Static("", classes="info-section", id="workspace-info", markup=True)
            yield Static("•", classes="separator", markup=True)

            # Cost/Tokens combined
            yield Static("", classes="info-section", id="cost-token-info", markup=True)
            yield Static("•", classes="separator", markup=True)

            # Context/Memory
            yield Static("", classes="info-section", id="context-memory-info", markup=True)
            yield Static("•", classes="separator", markup=True)

            # Queue/History
            yield Static("", classes="info-section", id="queue-history-info", markup=True)
            yield Static("•", classes="separator", markup=True)

            # Status indicators
            yield Static("", classes="info-section", id="status-info", markup=True)

    def on_mount(self) -> None:
        """Initialize when mounted"""
        # Register this info bar for updates
        register_info_bar(self)

        # Initialize default values - get from parent terminal if available
        # Try to find UniversalTerminal in ancestors
        terminal = None
        current = self
        while current:
            if current.__class__.__name__ == "UniversalTerminal":
                terminal = current
                break
            current = current.parent

        if terminal and hasattr(terminal, 'state') and hasattr(terminal.state, 'model_name'):
            self.model_name = terminal.state.model_name or os.getenv("CAI_MODEL", "default")
        else:
            self.model_name = os.getenv("CAI_MODEL", "default")

        # Don't set Loading... text - let the update methods handle it
        # The update methods will set proper content with colors and emojis

        # Update initial values
        self._update_info()

        # Set up periodic updates
        # Update every 0.5 seconds for real-time updates
        self._update_timer = self.set_interval(0.5, self._update_info)

        # Initial responsive update
        self._update_responsive_display()

    def on_unmount(self) -> None:
        """Cleanup when unmounted"""
        # Unregister this info bar
        unregister_info_bar(self)

        # Cancel timer if it exists
        if self._update_timer:
            self._update_timer.stop()
            self._update_timer = None

    def _update_info(self) -> None:
        """Update all information displays"""
        # Early return if not mounted or app is not running
        if not self.is_attached or not self.app:
            return

        try:
            # Get model from parent terminal state instead of environment
            # Try to find UniversalTerminal in ancestors
            terminal = None
            current = self
            while current:
                if current.__class__.__name__ == "UniversalTerminal":
                    terminal = current
                    break
                current = current.parent

            if terminal and hasattr(terminal, 'state') and hasattr(terminal.state, 'model_name'):
                self.model_name = terminal.state.model_name or os.getenv("CAI_MODEL", "default")
            else:
                self.model_name = os.getenv("CAI_MODEL", "default")

            # Get fresh data per-terminal (do NOT use global interaction stats)
            try:
                # Resolve terminal for this status bar
                terminal = None
                current = self
                while current:
                    if current.__class__.__name__ == "UniversalTerminal":
                        terminal = current
                        break
                    current = current.parent

                term_id = None
                if terminal and hasattr(terminal, 'terminal_id'):
                    term_id = terminal.terminal_id
                if not term_id and self.terminal_number:
                    term_id = f"T{self.terminal_number}"

                # Find most recent agent_cost_state for this terminal
                best_state = None
                best_updated = -1
                for state in getattr(COST_TRACKER, 'agent_cost_states', {}).values():
                    if not isinstance(state, dict):
                        continue
                    if term_id and state.get('terminal_id') != term_id:
                        continue
                    upd = int(state.get('updated_at', 0) or 0)
                    if upd >= best_updated:
                        best_updated = upd
                        best_state = state

                if best_state:
                    self.input_tokens = int(best_state.get('last_interaction_input_tokens', 0) or 0)
                    self.output_tokens = int(best_state.get('last_interaction_output_tokens', 0) or 0)
                    self.reasoning_tokens = int(best_state.get('last_interaction_reasoning_tokens', 0) or 0)
                    self.current_cost = float(best_state.get('last_interaction_cost', 0.0) or 0.0)
                    # Aggregate total cost across all agents in this terminal
                    try:
                        sum_total = 0.0
                        for state in getattr(COST_TRACKER, 'agent_cost_states', {}).values():
                            if not isinstance(state, dict):
                                continue
                            if state.get('terminal_id') != term_id:
                                continue
                            tc = state.get('total_cost', 0.0)
                            try:
                                sum_total += float(tc or 0.0)
                            except Exception:
                                continue
                        if sum_total > 0.0:
                            self.total_cost = sum_total
                    except Exception:
                        pass
            except Exception:
                # Last resort: keep previous values
                pass

            # Compute context usage ratio from per-terminal input tokens
            try:
                from cai.util import get_model_input_tokens
                max_tokens = int(get_model_input_tokens(self.model_name)) if self.model_name else 0
                self.context_usage = (float(self.input_tokens) / float(max_tokens)) if max_tokens > 0 else 0.0
            except Exception:
                self.context_usage = 0.0

            # Get time information
            try:
                # Get fresh time values each update
                new_active_time = get_active_time()
                new_idle_time = get_idle_time()
                
                # Update reactive properties with new values
                self.active_time = new_active_time
                self.idle_time = new_idle_time
                
                # Debug log (only in debug mode)
                if os.getenv("CAI_DEBUG", "0") == "2":
                    import logging
                    logger = logging.getLogger(__name__)
                    logger.debug(f"InfoBar time update: active={new_active_time}, idle={new_idle_time}")
            except Exception as e:
                self.active_time = "0s"
                self.idle_time = "0s"

            # Feature flags — auto_compact from config (same source as engine)
            try:
                from cai.config import get_config

                self.auto_compact = bool(get_config().auto_compact)
            except Exception:
                self.auto_compact = (
                    os.getenv("CAI_AUTO_COMPACT", "true").lower() == "true"
                )
            self.memory_enabled = compacted_memory_env_enabled()
            self.streaming_enabled = os.getenv("CAI_STREAM", "false").lower() == "true"

            # Update displays with new compact layout
            self._update_agent_model_info()
            self._update_workspace_info()
            self._update_cost_token_info()
            self._update_context_memory_info()
            self._update_queue_history_info()
            self._update_status_info()
            
            # Force a refresh of the widget to ensure time updates are visible
            if hasattr(self, 'refresh'):
                self.refresh()

            # Update separator visibility based on content
            self._update_dynamic_separator_visibility()
        except Exception:
            # Fallback to show at least something
            try:
                # Try to update at least the model info
                agent_widget = self.query_one("#agent-model-info", Static)
                text = Text()
                text.append("Model: ", style="dim #03fcb180")
                text.append(self.model_name or "default", style="bold #03fcb1")
                agent_widget.update(text)

                # Try to update cost info
                cost_widget = self.query_one("#cost-token-info", Static)
                text = Text()
                text.append(f"${self.total_cost:.2f}", style="bold #00ff88")
                cost_widget.update(text)
            except Exception:
                # Last resort - just set simple text
                try:
                    agent_widget = self.query_one("#agent-model-info", Static)
                    agent_widget.update(f"Model: {self.model_name or 'default'}")
                except:
                    pass

    def _update_agent_model_info(self) -> None:
        """Update agent and model information in compact format"""
        try:
            widget = self.query_one("#agent-model-info", Static)

            text = Text()

            # Get agent name from parent terminal if available
            try:
                # Try to find UniversalTerminal in ancestors
                terminal = None
                current = self
                while current:
                    if current.__class__.__name__ == "UniversalTerminal":
                        terminal = current
                        break
                    current = current.parent

                if terminal and hasattr(terminal, 'state') and hasattr(terminal.state, 'agent_name'):
                    self.agent_name = terminal.state.agent_name or ""
            except:
                pass

            # Check if there's a recent error to display
            if self.last_error:
                # Show error icon and truncated error message
                text.append("❌ ", style="bold #ff0066")
                # Truncate error to fit
                error_msg = self.last_error
                if len(error_msg) > 20:
                    error_msg = error_msg[:17] + "..."
                text.append(error_msg, style="bold #ff0066")
            elif self.agent_name:
                # Agent icon and name (if available)
                # Short agent name
                short_name = self.agent_name[:12] + ".." if len(self.agent_name) > 14 else self.agent_name
                text.append("🤖 ", style="bold #00ff88")
                text.append(short_name, style="bold #00ff88")
            else:
                # If no agent name, show just the icon
                text.append("🤖 ", style="dim #00ff88")
                text.append("Ready", style="dim #00ff88")

            widget.update(text)
        except Exception:
            # Ensure widget exists and has content
            try:
                widget = self.query_one("#agent-model-info", Static)
                widget.update("🤖 Ready")
            except:
                pass

    def _update_workspace_info(self) -> None:
        """Update workspace information"""
        try:
            widget = self.query_one("#workspace-info", Static)

            text = Text()

            # Get workspace info
            workspace = os.getenv("CAI_WORKSPACE", "")
            if workspace:
                # Shorten workspace name
                short_ws = workspace[:8] + ".." if len(workspace) > 10 else workspace
                text.append(f"📁{short_ws}", style="bold #00d9ff")
            else:
                # Show current directory name as fallback
                try:
                    import os
                    current_dir = os.path.basename(os.getcwd())
                    if current_dir:
                        short_dir = current_dir[:8] + ".." if len(current_dir) > 10 else current_dir
                        text.append(f"📂{short_dir}", style="dim #00d9ff")
                    else:
                        text.append("📂~", style="dim #00d9ff")
                except:
                    text.append("📂~", style="dim #00d9ff")

            widget.update(text)
        except:
            pass

    def _update_cost_token_info(self) -> None:
        """Update cost and token information combined"""
        try:
            widget = self.query_one("#cost-token-info", Static)

            text = Text()

            # Cost label
            text.append("COST: ", style="dim white")

            # Always show cost even if 0
            cost_style = "#ff0066" if self.total_cost > 10 else "#ffff00" if self.total_cost > 5 else "#00ff88"
            text.append(f"${self.total_cost:.2f}", style=f"bold {cost_style}")

            # Tokens (compact format)
            if self.input_tokens > 0 or self.output_tokens > 0:
                text.append("  ", style="")  # Extra spacing
                # Format tokens compactly (e.g., "1.2k→0.8k")
                in_tok = f"{self.input_tokens/1000:.1f}k" if self.input_tokens >= 1000 else str(self.input_tokens)
                out_tok = f"{self.output_tokens/1000:.1f}k" if self.output_tokens >= 1000 else str(self.output_tokens)

                text.append("📝 ", style="dim #00ff88")
                text.append(in_tok, style="bold #00ff88")
                text.append(" → ", style="dim white")
                text.append(out_tok, style="bold #00d9ff")

                if self.reasoning_tokens > 0:
                    r_tok = f"{self.reasoning_tokens/1000:.1f}k" if self.reasoning_tokens >= 1000 else str(self.reasoning_tokens)
                    text.append(" +", style="dim white")
                    text.append(f"{r_tok}r", style="bold #ffff00")

            widget.update(text)
        except:
            pass

    def _update_context_memory_info(self) -> None:
        """Update context usage and memory status"""
        try:
            widget = self.query_one("#context-memory-info", Static)

            text = Text()

            # Context label
            text.append("CTX: ", style="dim white")

            # Context usage - always show
            usage_pct = int(self.context_usage * 100)
            if usage_pct >= 80:
                text.append(f"⚠️ {usage_pct}%", style="bold #ff0066")
            elif usage_pct >= 60:
                text.append(f"📊 {usage_pct}%", style="bold #ffff00")
            else:
                text.append(f"📊 {usage_pct}%", style="bold #00ff88")

            text.append("  ", style="")  # Extra spacing

            # Memory status
            if self.memory_enabled:
                text.append("🧠 ON", style="bold #00ff88")
            else:
                text.append("🧠 OFF", style="dim #666666")

            text.append("  ", style="")  # Extra spacing

            # Auto-compact
            if self.auto_compact:
                text.append("♻️ ON", style="bold #00ff88")
            else:
                text.append("♻️ OFF", style="dim #666666")

            widget.update(text)
        except:
            pass

    def _update_queue_history_info(self) -> None:
        """Update queue and history information"""
        try:
            widget = self.query_one("#queue-history-info", Static)

            text = Text()

            # Get queue count
            queue_count = 0
            try:
                from cai.repl.commands.queue import get_queue
                queue_items = get_queue()
                queue_count = len(queue_items) if queue_items else 0
            except:
                pass

            if queue_count > 0:
                text.append(f"📋{queue_count}", style="bold #ffff00")
            else:
                text.append("📋0", style="dim #666666")

            # Get history count from agent manager
            history_count = 0
            try:
                from cai.sdk.agents.models.openai_chatcompletions import message_history
                history_count = len(message_history) if message_history else 0
            except:
                pass

            if history_count > 0:
                text.append(f" 📜{history_count}", style="bold #03fcb1")
            else:
                text.append(" 📜0", style="dim #666666")

            widget.update(text)
        except:
            pass

    def _update_status_info(self) -> None:
        """Update status indicators"""
        try:
            widget = self.query_one("#status-info", Static)

            text = Text()

            # Streaming
            if self.streaming_enabled:
                text.append("⚡", style="bold #00ff88")
            else:
                text.append("⚡", style="dim #333333")

            # Parallel count
            parallel_count = int(os.getenv("CAI_PARALLEL", "1"))
            if parallel_count > 1:
                text.append(f" 🔀{parallel_count}", style="bold #00d9ff")

            # Tracing
            if os.getenv("CAI_TRACING", "false").lower() == "true":
                text.append(" 🔍", style="bold #ffff00")

            # Show both active and idle time
            # Active time
            if self.active_time != "0s":
                text.append(f" ⏱{self.active_time}", style="bold #03fcb1")
            else:
                text.append(" ⏱0s", style="dim #666666")
            
            # Idle time (show if > 0)
            if self.idle_time != "0s":
                text.append(f" 💤{self.idle_time}", style="bold #ffff00")

            widget.update(text)
        except:
            pass

    # Deprecated methods removed - using new compact update methods

    def _get_agent_features(self) -> Dict[str, tuple[bool, str]]:
        """Get agent-specific features based on agent type"""
        # Try to get agent name from parent terminal
        try:
            # Try to find UniversalTerminal in ancestors
            terminal = None
            current = self
            while current:
                if current.__class__.__name__ == "UniversalTerminal":
                    terminal = current
                    break
                current = current.parent

            if terminal and hasattr(terminal, 'state') and hasattr(terminal.state, 'agent_name'):
                self.agent_name = terminal.state.agent_name or ""
        except:
            pass

        if not self.agent_name:
            return {}

        # Determine agent type and features
        agent_lower = self.agent_name.lower()

        # Map of agent types to their specific features
        agent_features = {
            "web_research": {
                "WEB": (True, "#00d9ff"),
                "SRCH": (True, "#00ff88"),
                "SUMM": (True, "#ffff00"),
                "CITE": (True, "#03fcb1"),
            },
            "code_writer": {
                "CODE": (True, "#ff0066"),
                "EDIT": (True, "#00ff88"),
                "TEST": (True, "#ffff00"),
                "REF": (True, "#03fcb1"),
            },
            "exploit_developer": {
                "EXPL": (True, "#ff0066"),
                "VULN": (True, "#ffff00"),
                "POC": (True, "#00d9ff"),
                "SEC": (True, "#ff00ff"),
            },
            "nuclei": {
                "SCAN": (True, "#00ff88"),
                "TMPL": (True, "#00d9ff"),
                "VULN": (True, "#ff0066"),
                "REP": (True, "#ffff00"),
            },
            "subdomain": {
                "DNS": (True, "#00ff88"),
                "ENUM": (True, "#00d9ff"),
                "DISC": (True, "#ffff00"),
                "PERM": (True, "#03fcb1"),
            },
            "recon": {
                "OSINT": (True, "#00ff88"),
                "INFO": (True, "#00d9ff"),
                "MAP": (True, "#ffff00"),
                "DISC": (True, "#03fcb1"),
            },
            "linux_command": {
                "BASH": (True, "#00ff88"),
                "SYS": (True, "#00d9ff"),
                "NET": (True, "#ffff00"),
                "FILE": (True, "#03fcb1"),
            },
            "web_browser": {
                "BROW": (True, "#00d9ff"),
                "NAV": (True, "#00ff88"),
                "SCRP": (True, "#ffff00"),
                "JS": (True, "#ff00ff"),
            },
            "scraper": {
                "SCRP": (True, "#00ff88"),
                "PARS": (True, "#00d9ff"),
                "DATA": (True, "#ffff00"),
                "API": (True, "#03fcb1"),
            },
            "triage": {
                "ANLY": (True, "#00ff88"),
                "PRIO": (True, "#ff0066"),
                "SIFT": (True, "#ffff00"),
                "EVAL": (True, "#00d9ff"),
            }
        }

        # Check which agent type matches
        for agent_type, features in agent_features.items():
            if agent_type in agent_lower:
                self.agent_type = agent_type
                return features

        # Return empty for unknown agents - no placeholder text
        return {}

    def update_from_usage(self, usage_data: Dict[str, Any]) -> None:
        """Update from usage data (called from openai_chatcompletions)"""
        if "input_tokens" in usage_data:
            self.input_tokens = usage_data["input_tokens"]
        if "output_tokens" in usage_data:
            self.output_tokens = usage_data["output_tokens"]
        if "reasoning_tokens" in usage_data:
            self.reasoning_tokens = usage_data["reasoning_tokens"]
        if "total_cost" in usage_data:
            self.current_cost = usage_data.get("interaction_cost", 0.0)
            self.total_cost = usage_data["total_cost"]
        if "context_usage" in usage_data:
            self.context_usage = usage_data["context_usage"]

        # Force immediate update
        self._update_info()

    def on_resize(self, event) -> None:
        """Handle terminal resize events"""
        self._update_responsive_display()

    def _update_responsive_display(self) -> None:
        """Update display based on available width"""
        try:
            # Get terminal width
            if self.app and self.app.size:
                width = self.app.size.width
            else:
                width = 120  # Default width

            # Hide/show sections based on width
            if width < 60:
                # Very narrow - only show agent/model and cost/tokens
                self._hide_sections(["#workspace-info", "#context-memory-info", "#queue-history-info", "#status-info"])
                self._show_sections(["#agent-model-info", "#cost-token-info"])
            elif width < 80:
                # Narrow - hide queue/history and status
                self._hide_sections(["#queue-history-info", "#status-info"])
                self._show_sections(["#agent-model-info", "#workspace-info", "#cost-token-info", "#context-memory-info"])
            elif width < 100:
                # Medium - hide status only
                self._hide_sections(["#status-info"])
                self._show_sections(["#agent-model-info", "#workspace-info", "#cost-token-info", "#context-memory-info", "#queue-history-info"])
            else:
                # Wide - show all
                self._show_sections(["#agent-model-info", "#workspace-info", "#cost-token-info", "#context-memory-info", "#queue-history-info", "#status-info"])

            # Update separator visibility dynamically
            self._update_dynamic_separator_visibility()

        except Exception:
            # Ignore resize errors
            pass

    def _hide_sections(self, section_ids: list) -> None:
        """Hide specific sections"""
        for section_id in section_ids:
            try:
                section = self.query_one(section_id)
                section.display = False
            except:
                pass

    def _show_sections(self, section_ids: list) -> None:
        """Show specific sections"""
        for section_id in section_ids:
            try:
                section = self.query_one(section_id)
                section.display = True
            except:
                pass

    def _update_separator_visibility(self) -> None:
        """Update separator visibility based on visible sections"""
        # This method is deprecated - using _update_dynamic_separator_visibility instead
        pass

    def _update_dynamic_separator_visibility(self) -> None:
        """Hide separators next to empty sections"""
        try:
            # Check which sections have content by querying their actual text
            sections = [
                "#agent-model-info",
                "#workspace-info",
                "#cost-token-info",
                "#context-memory-info",
                "#queue-history-info",
                "#status-info"
            ]

            has_content = []
            for section_id in sections:
                try:
                    widget = self.query_one(section_id, Static)
                    # Check if the widget has any visible text
                    if widget.renderable:
                        if hasattr(widget.renderable, 'plain'):
                            # Rich Text object
                            content = widget.renderable.plain
                        else:
                            content = str(widget.renderable)
                        has_content.append(len(content.strip()) > 0)
                    else:
                        has_content.append(False)
                except:
                    has_content.append(False)

            # Get all separators
            separators = list(self.query(".separator"))

            # Hide separators between empty sections
            for i, sep in enumerate(separators):
                if i < len(has_content) - 1:
                    # Show separator only if current section has content and there's content after it
                    current_has = has_content[i] if i < len(has_content) else False
                    next_has = any(has_content[j] for j in range(i + 1, len(has_content)))
                    sep.display = current_has and next_has
                else:
                    sep.display = False

        except:
            pass

    def set_error(self, error_message: str) -> None:
        """Set the last error message to display"""
        self.last_error = error_message
        # Force immediate update
        self._update_agent_model_info()
        # Clear error after 5 seconds
        if hasattr(self, 'app') and self.app:
            self.set_timer(5.0, self.clear_error)
    
    def clear_error(self) -> None:
        """Clear the error message"""
        self.last_error = ""
        # Force update to show normal status
        self._update_agent_model_info()
