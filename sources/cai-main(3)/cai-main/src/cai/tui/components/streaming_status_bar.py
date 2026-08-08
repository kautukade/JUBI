"""
Streaming status bar with animated indicators
"""
from typing import Any
import threading
from textual.widgets import Static, RichLog
from textual.reactive import reactive
from textual.app import ComposeResult
from textual.containers import Horizontal, VerticalScroll
from rich.text import Text
from rich.panel import Panel
from rich.box import ROUNDED
from rich.markdown import Markdown
from rich.syntax import Syntax
from rich.console import Group
import time
from datetime import datetime
from rich.console import Console
import os

_CAI_DEBUG_DIR = os.path.join(os.path.expanduser("~"), ".cai", "debug")


class StreamingStatusBar(Static):
    """An animated status bar for streaming display"""
    
    # Animation frames for different states
    ANIMATIONS = {
        "streaming": ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"],
        "thinking": ["🤔", "🧠", "💭", "✨", "💡"],
        "processing": ["◐", "◓", "◑", "◒"],
        "loading": ["▁", "▂", "▃", "▄", "▅", "▆", "▇", "█", "▇", "▆", "▅", "▄", "▃", "▂"],
        "dots": ["⣾", "⣽", "⣻", "⢿", "⡿", "⣟", "⣯", "⣷"],
        "squares": ["◰", "◳", "◲", "◱"],
        "arrows": ["←", "↖", "↑", "↗", "→", "↘", "↓", "↙"],
        # Special animation for code execution
        "code": ["{ }", "< >", "</>", "{/>", "</ }"],
    }
    
    DEFAULT_CSS = """
    StreamingStatusBar {
        height: 1;
        background: $surface-lighten-1;
        color: $text;
        padding: 0 2;
        dock: bottom;
        border-top: tall $border;
    }
    
    .status-icon {
        width: 3;
        text-align: center;
        text-style: bold;
    }
    
    .status-text {
        width: 1fr;
        color: $text;
    }
    """
    
    # Reactive properties
    animation_type = reactive("streaming")
    status_text = reactive("Streaming...")
    is_active = reactive(False)
    frame_index = reactive(0)
    _render_as_markdown = reactive(False)
    
    def __init__(self, animation_type: str = "streaming", **kwargs):
        super().__init__(**kwargs)
        self.animation_type = animation_type
        self._start_time = time.time()
        
    def on_mount(self) -> None:
        """Start animation when mounted"""
        if self.is_active:
            self.set_interval(0.15, self._animate)
    
    def start_streaming(self, text: str = "Streaming...", animation: str = "streaming") -> None:
        """Start the streaming animation"""
        self.status_text = text
        self.animation_type = animation
        self.is_active = True
        self.frame_index = 0
        self._start_time = time.time()
        self._render_as_markdown = False
        self.set_interval(0.1, self._animate)

    def start_execute_code(self, language: str = "python", filename: str = "exploit", code_preview: str | None = None) -> None:
        """Special start for execute_code: renders Markdown summary and uses code animation.

        Only a concise markdown header is shown here due to the 1-line height of the status bar.
        A full code block is rendered in the action bar panel.
        """
        filename = str(filename or "exploit")
        language = str(language or "python")
        # Inline Markdown summary (single line)
        md_summary = f"Executing: `execute_code` `{filename}` ({language})"
        # Store text and switch to markdown rendering
        self.status_text = md_summary
        self.animation_type = "code"
        self.is_active = True
        self.frame_index = 0
        self._start_time = time.time()
        self._render_as_markdown = True
        self.set_interval(0.1, self._animate)
        
    def stop_streaming(self, final_text: str = "Complete") -> None:
        """Stop the streaming animation"""
        self.is_active = False
        self.status_text = final_text
        self._render_as_markdown = False
        self.update(self._render_status())
        
    def _animate(self) -> None:
        """Update animation frame"""
        if not self.is_active:
            return
            
        frames = self.ANIMATIONS.get(self.animation_type, self.ANIMATIONS["streaming"])
        self.frame_index = (self.frame_index + 1) % len(frames)
        self.update(self._render_status())
        
    def _render_status(self):
        """Render the current status"""
        icon_text = Text()
        if self.is_active:
            frames = self.ANIMATIONS.get(self.animation_type, self.ANIMATIONS["streaming"])
            icon = frames[self.frame_index]
            icon_text.append(f"{icon} ", style="bold yellow")
        else:
            icon_text.append("✓ ", style="bold green")

        # Choose between Markdown and plain text
        if getattr(self, "_render_as_markdown", False):
            content = Markdown(self.status_text)
        else:
            plain = Text()
            plain.append(self.status_text, style="white")
            content = plain

        # Elapsed time
        if self.is_active:
            elapsed = time.time() - self._start_time
            elapsed_text = Text(f" ({elapsed:.1f}s)", style="dim")
            return Group(icon_text, content, elapsed_text)
        return Group(icon_text, content)


class ActualActionBar(VerticalScroll):
    """A scrollable action log showing all actions being performed"""
    
    BINDINGS = [
        ("ctrl+shift+c", "copy_visible", "Copy ActionBar"),
        ("ctrl+shift+a", "copy_all", "Copy ActionBar All"),
    ]
    
    DEFAULT_CSS = """
    ActualActionBar {
        height: 30%;
        min-height: 30%;
        max-height: 30%;
        background: $surface-darken-1;
        border-top: tall $border;
        border-bottom: none;
        border-left: none;
        border-right: none;
        padding: 1 2;
        dock: bottom;
        margin: 0;
        width: 100%;
        scrollbar-size: 1 1;
        scrollbar-color: #529d86;
        scrollbar-background: #2e4f46;
        display: block;
    }
    
    /* Compact mode for replay or constrained space */
    ActualActionBar.compact {
        height: 12% !important;
        min-height: 12% !important;
        max-height: 12% !important;
    }
    
    /* Smaller action bar only when 4+ terminals */
    .many-terminals ActualActionBar,
    UniversalTerminal.many-terminals ActualActionBar {
        height: 20% !important;
        min-height: 20% !important;
        max-height: 20% !important;
        scrollbar-size: 1 1 !important;
        scrollbar-color: #529d86 !important;
        border-top: solid $border !important;
        background: $surface-darken-2 !important;
        padding: 0 1 !important;
    }
    
    /* Subtle glow effect on scrollbar */
    ActualActionBar:focus-within {
        scrollbar-color: #529d86;
    }
    
    ActualActionBar > RichLog {
        background: transparent;
        color: $text-muted;
        padding: 0 1;
        margin: 0;
        height: 100%;
        width: 100%;
    }
    
    /* Better contrast when focused */
    ActualActionBar:focus-within > RichLog {
        color: $text;
        background: $surface-darken-1;
    }
    """
    
    # Reactive property for dynamic height
    log_lines = reactive(0)
    
    def __init__(self, terminal_number: int = 1, **kwargs):
        super().__init__(**kwargs)
        self.terminal_number = terminal_number
        self._current_text = ""
        self._is_streaming = False
        self._action_log = None
        self._current_prompt = f"cai@terminal-{terminal_number}"
        self._log_history = []  # Keep track of all log entries
        self._max_history_size = 100  # Limit history size for performance
        self._auto_scroll = True  # Auto-scroll enabled by default
        self._last_manual_scroll = 0  # Time of last manual scroll
        self._scroll_timer = None  # Timer to re-enable auto-scroll
        self._recent_tool_calls = []  # Track recent tool calls for deduplication
        self._dedup_window = 1.0  # 1 second window for deduplication
        self._update_throttle = 5.0  # Throttle updates to once every 5 seconds
        self._last_update = 0
        self._is_tool_stream = False
        # Synchronization to avoid race conditions between finalization and updates
        self._update_lock = threading.Lock()
        # Cursor blink timer handle (to stop cleanly on completion)
        self._cursor_timer = None
        # Execution indicator persistent line index
        self._exec_line_index = None
        # Tool execution indicator state
        self._tool_exec_line_index = None
        self._tool_exec_timer = None
        self._tool_exec_frame = 0
        self._tool_exec_message = None  # Base message for the tool line
        self._tool_exec_timestamp = None
        # Coalesced redraw control to avoid blocking under high refresh rates
        self._last_full_render = 0.0
        # Allow tuning via env var (e.g., CAI_TUI_MAX_RERENDERS_PER_SEC=8)
        try:
            fps = float(os.getenv("CAI_TUI_MAX_RERENDERS_PER_SEC", "10"))
            fps = 10.0 if fps <= 0 else fps
        except Exception:
            fps = 10.0
        self._render_min_interval = 1.0 / float(fps)  # ~10 FPS default for full-log rewrites
        self._render_timer_handle = None
        self._render_scheduled = False
        
        # BUGFIX: Throttling para scroll_end() sincronizado con frecuencia unificada
        self._last_scroll_end = 0.0
        self._scroll_end_min_interval = 0.15  # Sincronizado con frecuencia de actualizaciones (150ms)
        self._pending_scroll_end = False
        self._scroll_end_timer = None

    # --- Throttled scroll helpers -------------------------------------------------
    def _throttled_scroll_end(self, animate: bool = False) -> None:
        """Throttled scroll_end to prevent flicker during streaming"""
        if not self._action_log:
            return
        
        # BUGFIX: Skip auto-scroll during active streaming to prevent scroll jumps
        if self._is_streaming:
            return
            
        current_time = time.perf_counter()
        
        # If we recently scrolled, schedule one for later
        if current_time - self._last_scroll_end < self._scroll_end_min_interval:
            if not self._pending_scroll_end:
                self._pending_scroll_end = True
                # Cancela timer anterior si existe
                if self._scroll_end_timer:
                    try:
                        self._scroll_end_timer.stop()
                    except Exception:
                        pass
                
                # Schedule scroll for after minimum interval
                delay = self._scroll_end_min_interval - (current_time - self._last_scroll_end)
                self._scroll_end_timer = self.set_timer(delay, self._execute_pending_scroll_end)
            return
        
        # Execute scroll immediately
        self._last_scroll_end = current_time
        self._pending_scroll_end = False
        try:
            self._action_log.scroll_end(animate=animate)
        except Exception:
            pass
    
    def _execute_pending_scroll_end(self) -> None:
        """Execute pending scroll_end"""
        if self._pending_scroll_end and self._action_log:
            self._last_scroll_end = time.perf_counter()
            self._pending_scroll_end = False
            try:
                self._action_log.scroll_end(animate=False)
            except Exception:
                pass

    # --- Throttled render helpers -------------------------------------------------
    def _schedule_render(self, delay: float) -> None:
        try:
            if self._render_timer_handle:
                try:
                    self._render_timer_handle.stop()
                except Exception:
                    pass
            self._render_timer_handle = self.set_timer(delay, lambda: self._render_history(force=True))
            self._render_scheduled = True
        except Exception:
            pass

    def _render_history(self, force: bool = False) -> None:
        """Full log redraw with coalescing/throttling.

        Replaces immediate clear()+rewrite() calls sprinkled around the class.
        """
        if not self._action_log:
            return
        now = time.perf_counter()
        if not force:
            dt = now - float(self._last_full_render or 0.0)
            if dt < self._render_min_interval:
                remaining = max(self._render_min_interval - dt, 0.01)
                if not self._render_scheduled:
                    self._schedule_render(remaining)
                return
        # Cancel any pending scheduled render
        try:
            if self._render_timer_handle:
                self._render_timer_handle.stop()
        except Exception:
            pass
        self._render_timer_handle = None
        self._render_scheduled = False

        try:
            self._action_log.clear()
            for entry in self._log_history:
                self._action_log.write(entry)
            if self._auto_scroll and self._action_log:
                self._throttled_scroll_end(animate=False)
        except Exception:
            pass
        finally:
            self._last_full_render = time.perf_counter()
    
    def _throttled_render_history(self) -> None:
        """Throttled version of _render_history for frequent calls"""
        # Use the existing throttling mechanism in _render_history
        self._render_history(force=False)
        
    def compose(self) -> ComposeResult:
        """Compose the action bar"""
        # Scrollable log without header
        self._action_log = RichLog(
            highlight=False,  # Disable for performance
            markup=True,
            auto_scroll=True,  # Enable auto-scroll
            wrap=True,
            max_lines=1000,  # Same limit as main terminal for consistency
        )
        yield self._action_log
        
    def on_mount(self) -> None:
        """Initialize when mounted"""
        # Add initial prompt to all terminals
        if self._action_log:
            line = Text()
            line.append("• ", style="dim cyan")
            line.append(f"{datetime.now().strftime('%H:%M:%S')}", style="dim blue")
            line.append(" │ ", style="dim white")
            line.append("CAI> ", style="bold green")
            line.append("Ready", style="green")
            self._action_log.write(line)
            
    def on_scroll(self, event) -> None:
        """Handle scroll events to detect manual scrolling"""
        # User is manually scrolling
        self._auto_scroll = False
        self._last_manual_scroll = time.time()
        
        # Cancel existing timer
        if self._scroll_timer:
            self._scroll_timer.cancel()
            
        # Set a new timer to re-enable auto-scroll after 3 seconds
        self._scroll_timer = self.set_timer(3.0, self._re_enable_auto_scroll)
        
    def _re_enable_auto_scroll(self) -> None:
        """Re-enable auto-scroll after user stops scrolling"""
        self._auto_scroll = True
        self._scroll_timer = None
        
        # Scroll to bottom if we have content
        if self._action_log:
            self._throttled_scroll_end(animate=True)
        
    def start_streaming(self, agent_name: str = "agent", is_tool: bool = False) -> None:
        """Start streaming mode"""
        self._is_streaming = True
        # Reset completion guard for new stream
        try:
            if hasattr(self, '_stream_finalized'):
                delattr(self, '_stream_finalized')
        except Exception:
            pass
        self._is_tool_stream = is_tool
        self._current_text = ""
        self._current_prompt = f"cai@{agent_name.lower()}"
        self._stream_start_time = datetime.now()
        self._animation_frame = 0
        self._displayed_chars = 0
        self._last_update_time = time.time()
        self._cursor_visible = True
        self._cursor_blink_time = time.time()
        
        # Remember where streaming starts in history
        self._stream_line_start = len(self._log_history)
        
        # Track streaming lines in the log
        self._streaming_line_indices = []
        # No "Executing..." line in action bar – keep it clean; output will stream directly
        
        # Enable cursor blinking animation for streaming (store handle to stop later)
        try:
            if self._cursor_timer:
                self._cursor_timer.stop()
        except Exception:
            pass
        self._cursor_timer = self.set_interval(0.8, self._update_cursor_blink)
    
    def show_tool_execution(self, tool_name: str, args: dict = None) -> None:
        """Show tool execution in action bar"""
        # If an animated tool indicator is already present (streaming path created it), skip to avoid duplicates
        if self._tool_exec_line_index is not None:
            return
        if tool_name == "generic_linux_command":
            # Prefer the shell-style line even for non-streaming path
            try:
                import json
                command = None
                if isinstance(args, dict):
                    command = (
                        args.get('full_command')
                        or args.get('full')
                        or (
                            (args.get('command') or '') + (' ' + args.get('args') if args.get('args') else '')
                        ).strip()
                    )
                elif isinstance(args, str) and args.strip():
                    try:
                        parsed = json.loads(args)
                        if isinstance(parsed, dict):
                            command = (
                                parsed.get('full_command')
                                or parsed.get('full')
                                or (
                                    (parsed.get('command') or '') + (' ' + parsed.get('args') if parsed.get('args') else '')
                                ).strip()
                            )
                    except Exception:
                        command = args
                command = command or (str(args) if args else "")
                if self._action_log:
                    timestamp = datetime.now().strftime("%H:%M:%S")
                    line = Text()
                    line.append("• ", style="dim cyan")
                    line.append(f"{timestamp}", style="dim blue")
                    line.append(" │ ", style="dim white")
                    line.append("CAI> ", style="bold green")
                    line.append("$ ", style="bold yellow")
                    line.append(str(command), style="bold white")
                    line.append(" ", style="dim")
                    line.append("Executing...", style="dim")
                    self._action_log.write(line)
                    self._log_history.append(line)
                    self._tool_exec_line_index = len(self._log_history) - 1
                    self._tool_exec_message = None
                    self._tool_exec_timestamp = timestamp
                    try:
                        if self._tool_exec_timer:
                            self._tool_exec_timer.stop()
                    except Exception:
                        pass
                    self._tool_exec_frame = 0
                    self._tool_exec_timer = self.set_interval(0.15, self._animate_tool_exec)
                return
            except Exception:
                # Fallback to generic flow below
                pass
        # Deduplication check
        current_time = time.time()
        
        # Create a key for deduplication
        if args:
            if isinstance(args, dict):
                # Filter out internal args for key
                filtered_args = {k: v for k, v in args.items() 
                               if k not in ["call_counter", "input_to_session"] and v}
                args_key = str(sorted(filtered_args.items()))
            else:
                args_key = str(args)
        else:
            args_key = ""
        
        tool_key = f"{tool_name}:{args_key}"
        
        # Clean up old entries faster window to avoid suppression at high rate
        self._recent_tool_calls = [(t, k) for t, k in self._recent_tool_calls 
                                    if current_time - t < 0.1]
        
        # Check if this tool call was recently shown
        for _, key in self._recent_tool_calls:
            if key == tool_key:
                # Skip duplicate
                return
        
        # Add to recent calls
        self._recent_tool_calls.append((current_time, tool_key))
        self._last_update = current_time
        
        # Special handling for execute_code: show a compact code panel preview ONLY here.
        if tool_name == "execute_code":
            try:
                # Normalize args to dict
                import json as _json
                ad = args if isinstance(args, dict) else (_json.loads(args) if isinstance(args, str) else {})
            except Exception:
                ad = {}

            code = str(ad.get("code", ""))
            language = str(ad.get("language", "python"))
            filename = str(ad.get("filename", "exploit"))

            # No code? Don't show panel
            if not code or not code.strip():
                return

            # Short preview to keep panel lightweight
            max_chars = 2000
            preview = code if len(code) <= max_chars else (code[: max_chars - 20] + "\n# ...")

            # Deduplicate same code panel within a short window (avoid double rendering from
            # AgentDisplay and ToolDisplay streaming paths). Use an LRU list of recent signatures.
            try:
                import hashlib as _hashlib
                sig_seed = f"{filename}|{language}|{preview[:200]}"
                signature = _hashlib.sha1(sig_seed.encode("utf-8", errors="ignore")).hexdigest()
                now_t = time.time()
                # Initialize store
                if not hasattr(self, "_recent_exec_panels"):
                    self._recent_exec_panels = []  # list[(timestamp, signature)]
                # Remove old entries (>5s)
                self._recent_exec_panels = [x for x in self._recent_exec_panels if now_t - x[0] < 5.0]
                # If already present, skip rendering
                if any(sig == signature for (_, sig) in self._recent_exec_panels):
                    return
                # Record signature
                self._recent_exec_panels.append((now_t, signature))
            except Exception:
                pass

            # Pick theme based on current app theme
            try:
                from textual.app import App as _App
                app = _App.get_app()
                current_theme = getattr(app, "theme", "textual-dark") if app else "textual-dark"
            except Exception:
                current_theme = "textual-dark"
            pygments_theme_map = {
                "textual-dark": "monokai",
                "tokyo-night": "monokai",
                "nord": "nord-darker",
                "solarized-light": "solarized-light",
                "textual-light": "default",
                "nature": "monokai",
            }
            pygments_theme = pygments_theme_map.get(current_theme, "monokai")

            code_syntax = Syntax(
                preview,
                language or "text",
                theme=pygments_theme,
                line_numbers=True,
                indent_guides=True,
                word_wrap=True,
            )
            code_title = f"execute_code: {filename} ({language})"
            # Markdown summary above the syntax block
            md_summary = Markdown(f"**Executing** `execute_code` on `{filename}` ({language})")
            code_panel = Panel(Group(md_summary, Text("\n"), code_syntax), title=code_title, border_style="cyan", title_align="left", box=ROUNDED, padding=(0, 1))

            # Write code panel first (compact header + code)
            if self._action_log:
                try:
                    # Clear any pending tool exec indicator to avoid being above the code
                    if self._tool_exec_line_index is not None and 0 <= self._tool_exec_line_index < len(self._log_history):
                        try:
                            self._log_history.pop(self._tool_exec_line_index)
                        except Exception:
                            pass
                        self._tool_exec_line_index = None
                        self._tool_exec_message = None
                    # Also clear any generic 'Thinking...' indicator line
                    if getattr(self, '_exec_line_index', None) is not None and 0 <= self._exec_line_index < len(self._log_history):
                        try:
                            self._log_history.pop(self._exec_line_index)
                        except Exception:
                            pass
                        self._exec_line_index = None
                    # Reflect removals via throttled redraw
                    if self._action_log:
                        self._render_history()

                    self._action_log.write(code_panel)
                    self._log_history.append(code_panel)
                except Exception:
                    pass
            # Ensure there is a separator after the code panel to avoid it sticking to
            # previous content in very fast executions.
            if self._action_log:
                try:
                    sep = Text("\n")
                    self._action_log.write(sep)
                    self._log_history.append(sep)
                except Exception:
                    pass

            # Do NOT add the animated "Executing..." line here to ensure it stays at the bottom.
            # It will be appended after streaming has started by the streaming handler.
            # Store that we showed a code panel for this execution
            self._has_execute_code_panel = True
            
            # Create a placeholder streaming line immediately so output appears below the code
            try:
                self.update_streaming_text("")
            except Exception:
                pass
            return

        # For other tools, always append indicator at the bottom to avoid being stuck above new content
        try:
            self.add_tool_exec_indicator_bottom(tool_name, args)
        except Exception:
            pass
        return

        # No other special-casing (unreached)
        # Format the function call with arguments
        if args:
            if isinstance(args, dict):
                # Filter out internal args
                filtered_args = {k: v for k, v in args.items() 
                               if k not in ["call_counter", "input_to_session"] and v}
                
                # Format as function call
                if filtered_args:
                    args_str = ", ".join(f"{k}={repr(v)}" for k, v in filtered_args.items())
                    # Truncate if too long
                    if len(args_str) > 100:
                        args_str = args_str[:97] + "..."
                    message = f"{tool_name}({args_str})"
                else:
                    message = f"{tool_name}()"
            elif isinstance(args, str):
                # Single string argument
                message = f"{tool_name}({repr(args)})"
        else:
            # No arguments
            message = f"{tool_name}()"
        
        # Add a tool call line with animated Executing...
        if self._action_log:
            timestamp = datetime.now().strftime("%H:%M:%S")
            line = Text()
            line.append("• ", style="dim cyan")
            line.append(f"{timestamp}", style="dim blue")
            line.append(" │ ", style="dim white")
            line.append("CAI> ", style="bold green")
            line.append(" ▸ ", style="dim")
            line.append(message, style="yellow")
            line.append(" ", style="dim")
            line.append("Executing...", style="dim")
            try:
                self._action_log.write(line)
                self._log_history.append(line)
                self._tool_exec_line_index = len(self._log_history) - 1
                self._tool_exec_message = message
                self._tool_exec_timestamp = timestamp
                try:
                    if self._tool_exec_timer:
                        self._tool_exec_timer.stop()
                except Exception:
                    pass
                self._tool_exec_frame = 0
                self._tool_exec_timer = self.set_interval(0.15, self._animate_tool_exec)
            except Exception:
                pass
        
    def update_streaming_text(self, text: str) -> None:
        """Update the streaming text progressively with multi-line support"""
        if self._is_streaming and self._action_log and hasattr(self, '_log_history'):
            # BUGFIX: Add throttling to prevent scroll flicker (sync with other components at 150ms)
            current_time = time.time()
            if not hasattr(self, '_last_streaming_update'):
                self._last_streaming_update = 0
            if current_time - self._last_streaming_update < 0.15:
                return
            self._last_streaming_update = current_time
            
            # The text parameter contains the FULL accumulated text so far
            # We just need to display it
            
            try:
                with self._update_lock:
                    # Convert to string if needed
                    if not isinstance(text, str):
                        text = str(text)
                
                    # Clean control characters but KEEP newlines for multi-line display
                    text = ''.join(char for char in text if ord(char) >= 32 or char in '\n\r\t')
                
                    # Replace tabs with spaces
                    text = text.replace('\t', '    ')
                
                    # Store the full text (including newlines)
                    self._current_text = text
                
                    # Get the timestamp from when streaming started
                    if not hasattr(self, '_stream_start_time'):
                        self._stream_start_time = datetime.now()
                    timestamp = self._stream_start_time.strftime("%H:%M:%S")
                
                    # Split text into lines
                    lines = text.split('\n') if text else ['']
                
                    # Animation disabled
                    if not hasattr(self, '_animation_frame'):
                        self._animation_frame = 0
                
                    # First time - initialize streaming display
                    if not hasattr(self, '_streaming_line_index'):
                        # Ensure there is no leftover executing line
                        if hasattr(self, '_executing_line_index'):
                            try:
                                if self._executing_line_index < len(self._log_history):
                                    self._log_history.pop(self._executing_line_index)
                                delattr(self, '_executing_line_index')
                            except Exception:
                                pass
                        # Also ensure we remove the newer execution indicator if present
                        if hasattr(self, '_exec_line_index'):
                            try:
                                if self._exec_line_index is not None and 0 <= self._exec_line_index < len(self._log_history):
                                    self._log_history.pop(self._exec_line_index)
                                self._exec_line_index = None
                                # Reflect the removal via throttled redraw
                                if self._action_log:
                                    self._render_history()
                            except Exception:
                                pass
                        
                        # Create initial streaming line
                        line = Text()
                        if not self._is_tool_stream:
                            line.append("CAI> ", style="bold green")
                        line.append(f"[{timestamp}] ", style="dim")
                        # Start with empty content - will be filled character by character
                        self._action_log.write(line)
                        self._streaming_line_index = len(self._log_history)
                        self._log_history.append(line)
                        self._last_displayed_length = 0
                
                    # Always rewrite the complete current text without clearing history
                    # This ensures persistence and avoids flicker
                    line = Text()
                    if not self._is_tool_stream:
                        line.append("CAI> ", style="bold green")
                    line.append(f"[{timestamp}] ", style="dim")
                
                    # Helper to append content with stderr styling based on markers
                    def _append_with_error_styling(container: Text, content: str, is_first_line: bool) -> None:
                        STDERR_START = "[[STDERR]]"
                        STDERR_END = "[[/STDERR]]"
                        # Maintain cross-call state in case markers span updates
                        in_error = getattr(self, '_in_error_segment', False)
                        after_error_label = getattr(self, '_after_error_label', False)
                        LABEL = "error output:"
                        idx = 0
                        while idx < len(content):
                            start_pos = content.find(STDERR_START, idx)
                            end_pos = content.find(STDERR_END, idx)
                            label_pos = content.lower().find(LABEL, idx)
                            # If we're not in explicit stderr mode, check label first
                            if not in_error and not after_error_label and label_pos != -1 and (start_pos == -1 or label_pos < start_pos):
                                # Append text before the label normally
                                if label_pos > idx:
                                    container.append(content[idx:label_pos], style="white")
                                # Append the label itself in red
                                container.append(content[label_pos: label_pos + len(LABEL)], style="bold red")
                                after_error_label = True
                                idx = label_pos + len(LABEL)
                                continue
                            if not in_error:
                                # Not in error segment
                                if start_pos == -1:
                                    # No error start ahead; append rest as normal
                                    style = "red" if after_error_label else "white"
                                    container.append(content[idx:], style=style)
                                    idx = len(content)
                                else:
                                    # Append normal segment before error start
                                    if start_pos > idx:
                                        style = "red" if after_error_label else "white"
                                        container.append(content[idx:start_pos], style=style)
                                    # Enter error mode
                                    in_error = True
                                    idx = start_pos + len(STDERR_START)
                            else:
                                # Currently in error segment
                                if end_pos == -1:
                                    # No end marker; append rest as error and break
                                    if idx < len(content):
                                        container.append(content[idx:], style="red")
                                    idx = len(content)
                                else:
                                    # Append up to end marker as error, then exit error mode
                                    if end_pos > idx:
                                        container.append(content[idx:end_pos], style="red")
                                    in_error = False
                                    idx = end_pos + len(STDERR_END)
                        # Persist state
                        self._in_error_segment = in_error
                        self._after_error_label = after_error_label

                    # Add the complete text received so far
                    if text:
                        # Split into lines for proper display
                        lines = text.split('\n')
                        for i, line_text in enumerate(lines):
                            if i == 0:
                                # First line - add directly after prompt
                                _append_with_error_styling(line, line_text, True)
                            else:
                                # New lines - add with proper indentation
                                line.append("\n           ", style="dim")  # Newline + spacing to align
                                _append_with_error_styling(line, line_text, False)
                
                    # Add blinking cursor at the end
                    if hasattr(self, '_cursor_visible') and self._cursor_visible:
                        line.append("▌", style="bold white on black")
                
                    # Update the streaming line in history
                    if self._streaming_line_index < len(self._log_history):
                        self._log_history[self._streaming_line_index] = line
                    
                    # BUGFIX: Smart update without clear() to prevent scroll flicker
                    try:
                        # Try in-place update first
                        if (hasattr(self._action_log, '_lines') and 
                            len(self._action_log._lines) > self._streaming_line_index):
                            self._action_log._lines[self._streaming_line_index] = line
                            self._action_log.refresh()
                        else:
                            # Only if absolutely necessary, use full render
                            # but with more aggressive throttling
                            current_time = time.perf_counter()
                            if current_time - self._last_full_render > 0.5:  # Maximum every 500ms
                                self._render_history()
                    except Exception:
                        # If everything fails, do nothing rather than cause scroll flicker
                        pass
                
                    # BUGFIX: Disable auto-scroll during streaming to prevent scroll jumps
                    # The user can manually scroll if needed
                    # Auto-scroll will resume when streaming finishes
                    pass
                    
            except Exception as e:
                # Log any error
                try:
                    with open(f"{_CAI_DEBUG_DIR}/cai_rich_write_error.log", "a") as f:
                        import traceback
                        f.write(f"\n[{datetime.now().isoformat()}] Error in update_streaming_text\n")
                        f.write(f"  Error: {e}\n")
                        f.write(f"  Traceback: {traceback.format_exc()}\n")
                except:
                    pass
            
    def show_tool_call(self, tool_name: str, args: str = "") -> None:
        """Show a tool call in the action bar"""
        # Throttle for tool calls dramatically reduced (effectively off)
        current_time = time.time()
        
        # Deduplication check
        tool_key = f"{tool_name}:{args}"
        
        # Clean up old entries
        self._recent_tool_calls = [(t, k) for t, k in self._recent_tool_calls 
                                    if current_time - t < self._dedup_window]
        
        # Check if this tool call was recently shown
        for _, key in self._recent_tool_calls:
            if key == tool_key:
                # Skip duplicate
                return
        
        # Add to recent calls
        self._recent_tool_calls.append((current_time, tool_key))
        self._last_update = current_time
        
        # Special handling for generic_linux_command - show as regular command with animated Executing...
        if tool_name == "generic_linux_command":
            # Extract the actual command from args
            try:
                import json
                if isinstance(args, str) and args.strip():
                    # Best effort parse: prefer full_command, then compose command + args
                    command = None
                    try:
                        args_dict = json.loads(args)
                        if isinstance(args_dict, dict):
                            command = (
                                args_dict.get('full_command')
                                or args_dict.get('full')
                                or (
                                    (args_dict.get('command') or '')
                                    + (' ' + args_dict.get('args') if args_dict.get('args') else '')
                                ).strip()
                            )
                    except Exception:
                        command = args
                    command = command or args
                    # Format as a shell command and start animated suffix
                    if self._action_log:
                        timestamp = datetime.now().strftime("%H:%M:%S")
                        line = Text()
                        line.append("• ", style="dim cyan")
                        line.append(f"{timestamp}", style="dim blue")
                        line.append(" │ ", style="dim white")
                        line.append("CAI> ", style="bold green")
                        line.append("$ ", style="bold yellow")
                        line.append(str(command), style="bold white")
                        line.append(" ", style="dim")
                        line.append("Executing...", style="dim")
                        try:
                            self._action_log.write(line)
                            self._log_history.append(line)
                            self._tool_exec_line_index = len(self._log_history) - 1
                            self._tool_exec_message = None  # message embedded already
                            self._tool_exec_timestamp = timestamp
                            # Start/Restart tool exec animation timer
                            try:
                                if self._tool_exec_timer:
                                    self._tool_exec_timer.stop()
                            except Exception:
                                pass
                            self._tool_exec_frame = 0
                            self._tool_exec_timer = self.set_interval(0.15, self._animate_tool_exec)
                        except Exception:
                            pass
                        return
            except Exception:
                pass
                
        # Minimal, clean tool call display, but append an animated "Executing..."
        if tool_name in ("generic_linux_command", "execute_code"):
            # For generic_linux_command, render a compact context line unless the streaming path is handling it
            try:
                if tool_name == "generic_linux_command" and self._action_log:
                    timestamp = datetime.now().strftime("%H:%M:%S")
                    line = Text()
                    line.append("• ", style="dim cyan")
                    line.append(f"{timestamp}", style="dim blue")
                    line.append(" │ ", style="dim white")
                    line.append("CAI> ", style="bold green")
                    # Show session id and env if present in args
                    sid = None
                    env = None
                    if isinstance(args, str):
                        import json as _json
                        try:
                            parsed = _json.loads(args)
                            if isinstance(parsed, dict):
                                sid = parsed.get("session_id")
                                env = parsed.get("environment")
                        except Exception:
                            pass
                    elif isinstance(args, dict):
                        sid = args.get("session_id")
                        env = args.get("environment")
                    if sid:
                        line.append("[", style="dim")
                        line.append(str(sid), style="bold red")
                        line.append("] ", style="dim")
                    if env:
                        line.append(f"[{env}] ", style="magenta")
                    line.append("generic_linux_command", style="cyan")
                    line.append(" ", style="dim")
                    line.append("Executing...", style="dim")
                    self._action_log.write(line)
                    self._log_history.append(line)
            except Exception:
                pass
            return
        display_text = tool_name
        if args:
            arg_str = str(args)
            # Do not display raw JSON blobs
            if arg_str.startswith("{") and arg_str.endswith("}") and len(arg_str) > 80:
                display_text += "(...)"
            else:
                display_text += f"({arg_str})"

        # Build the line manually so we can animate the trailing status
        if self._action_log:
            timestamp = datetime.now().strftime("%H:%M:%S")
            line = Text()
            line.append("• ", style="dim cyan")
            line.append(f"{timestamp}", style="dim blue")
            line.append(" │ ", style="dim white")
            line.append("CAI> ", style="bold green")
            line.append(" ▸ ", style="dim")
            line.append(display_text, style="cyan")
            # Placeholder animated suffix; actual spinner is added in timer
            line.append(" ", style="dim")
            line.append("Executing...", style="dim")
            try:
                self._action_log.write(line)
                self._log_history.append(line)
                self._tool_exec_line_index = len(self._log_history) - 1
                self._tool_exec_message = display_text
                self._tool_exec_timestamp = timestamp
                # Start/Restart tool exec animation timer
                try:
                    if self._tool_exec_timer:
                        self._tool_exec_timer.stop()
                except Exception:
                    pass
                self._tool_exec_frame = 0
                self._tool_exec_timer = self.set_interval(0.15, self._animate_tool_exec)
            except Exception:
                pass
        
    def show_command(self, command: str) -> None:
        """Show a command execution in the action bar"""
        self._is_streaming = False
        
        # Apply 5 second throttle for commands (not streaming)
        current_time = time.time()
        if current_time - self._last_update < self._update_throttle:
            # Skip this update if too soon
            return
        self._last_update = current_time
        
        # Do not truncate: show full command with arguments to avoid confusion
        self._add_log_entry("CAI> ", f"$ {command}", "bold yellow")
        
    def complete_streaming(self) -> None:
        """Complete the streaming"""
        # Single-entry guard to avoid duplicate completion from concurrent callers
        with self._update_lock:
            if getattr(self, '_stream_finalized', False):
                return
            # Always allow a single finalize even if _is_streaming was never set.
            # This ensures instant outputs or last-tick completions still render.
            # Mark finalized immediately so concurrent callers bail out
            self._stream_finalized = True
            self._is_streaming = False
            
            # BUGFIX: Re-enable auto-scroll after streaming finishes and scroll to show final content
            if self._auto_scroll and self._action_log:
                # Force scroll to end to show all final content
                try:
                    self._action_log.scroll_end(animate=False)
                except Exception:
                    pass
            
            # Ensure all text is displayed with checkmark
            if self._current_text and hasattr(self, '_streaming_line_index'):
                timestamp = self._stream_start_time.strftime("%H:%M:%S") if hasattr(self, '_stream_start_time') else datetime.now().strftime("%H:%M:%S")
                
                # Split text into lines
                # Keep the content intact (including 'ERROR OUTPUT:' and exit code) for the final render
                sanitized_text = self._current_text.replace("[[STDERR]]", "").replace("[[/STDERR]]", "")
                lines = sanitized_text.split('\n') if sanitized_text else ['']
                
                        # Create final line (no checkmark here; we append below depending on error state)
                line = Text()
                if not self._is_tool_stream:
                    line.append("CAI> ", style="bold green")
                line.append(f"[{timestamp}] ", style="dim")
                
                # Add the complete final text
                for i, line_text in enumerate(lines):
                    if i == 0:
                        line.append(line_text, style="white")
                    else:
                        line.append("\n           ", style="dim")  # Newline + spacing
                        line.append(line_text, style="white")
                
                # Persist final output as a new entry at the end and remove the ephemeral streaming slot
                try:
                    if self._streaming_line_index < len(self._log_history):
                        self._log_history.pop(self._streaming_line_index)
                except Exception:
                    pass
                self._log_history.append(line)
                
                # BUGFIX: Avoid full render when finishing streaming
                # Only add final line without clear/rewrite
                if self._action_log:
                    try:
                        self._action_log.write(line)
                    except Exception:
                        pass
            
            # Append a separate status line so it never overwrites the final output (single-entry)
            try:
                status_line = Text()
                # If an error was marked during streaming, show it here
                if hasattr(self, '_stream_had_error') and getattr(self, '_stream_had_error', False):
                    status_line.append("  ✗ ", style="bold red")
                    err_msg = getattr(self, '_stream_error_message', "Error")
                    status_line.append(str(err_msg), style="red")
                else:
                    status_line.append("  ✓", style="green")
                # Always append a timestamp to make it visible as a new line
                status_line.append(f"  [{datetime.now().strftime('%H:%M:%S')}]", style="dim")
                # Avoid duplicating the same status line if already appended
                try:
                    if not hasattr(self, '_last_status_line') or str(self._last_status_line) != str(status_line):
                        self._action_log.write(status_line)
                        self._log_history.append(status_line)
                        self._last_status_line = status_line
                except Exception:
                    # Fallback: write once
                    self._action_log.write(status_line)
                    self._log_history.append(status_line)
                # Force a scroll to end to ensure the status is visible
                if self._auto_scroll and self._action_log:
                    self._throttled_scroll_end(animate=False)
            except Exception:
                pass

            # Clean up streaming state
            if hasattr(self, '_stream_start_time'):
                delattr(self, '_stream_start_time')
            if hasattr(self, '_displayed_chars'):
                delattr(self, '_displayed_chars')
            if hasattr(self, '_last_update_time'):
                delattr(self, '_last_update_time')
            if hasattr(self, '_stream_line_start'):
                delattr(self, '_stream_line_start')
            if hasattr(self, '_streaming_log_index'):
                delattr(self, '_streaming_log_index')
            if hasattr(self, '_last_streamed_text'):
                delattr(self, '_last_streamed_text')
            if hasattr(self, '_streaming_line_indices'):
                delattr(self, '_streaming_line_indices')
            if hasattr(self, '_streaming_start_index'):
                delattr(self, '_streaming_start_index')
            if hasattr(self, '_streaming_line_count'):
                delattr(self, '_streaming_line_count')
            if hasattr(self, '_streaming_line_index'):
                delattr(self, '_streaming_line_index')
            # Clear error markers
            if hasattr(self, '_stream_had_error'):
                delattr(self, '_stream_had_error')
            if hasattr(self, '_stream_error_message'):
                delattr(self, '_stream_error_message')
            if hasattr(self, '_in_error_segment'):
                delattr(self, '_in_error_segment')
            if hasattr(self, '_after_error_label'):
                delattr(self, '_after_error_label')
            if hasattr(self, '_pending_action_bar_update'):
                delattr(self, '_pending_action_bar_update')
            if hasattr(self, '_last_action_bar_update'):
                delattr(self, '_last_action_bar_update')
            # Stop cursor blink timer if running
            try:
                if self._cursor_timer:
                    self._cursor_timer.stop()
            except Exception:
                pass
            self._cursor_timer = None
            
            # Add ready message after a delay
            self.set_timer(0.5, self._show_ready)

    def mark_stream_error(self, message: str) -> None:
        """Mark the current stream as failed with an error message.

        This will cause complete_streaming to print a red error line instead of a checkmark.
        Safe to call from background threads; UI work is scheduled on the main thread.
        """
        # Normalize message early
        if not isinstance(message, str):
            message = str(message)
        message = (message or "Error").strip().splitlines()[0][:200]

        # Store flags immediately (thread-safe enough for simple attrs)
        try:
            self._stream_had_error = True
            self._stream_error_message = message
        except Exception:
            pass

        # Define UI update
        def _ui_update():
            try:
                # Append ephemeral error line for immediate visibility
                if hasattr(self, '_current_text') and hasattr(self, '_log_history'):
                    err_line = Text()
                    if not getattr(self, '_is_tool_stream', False):
                        err_line.append("CAI> ", style="bold green")
                    err_line.append(f"[{datetime.now().strftime('%H:%M:%S')}] ", style="dim")
                    err_line.append("✗ ", style="bold red")
                    err_line.append(self._stream_error_message, style="red")
                    self._log_history.append(err_line)
                    if self._action_log:
                        self._action_log.write(err_line)
                        if self._auto_scroll:
                            self._throttled_scroll_end(animate=False)
            except Exception:
                pass

        # Schedule on main thread if needed
        try:
            import threading
            if threading.current_thread() is threading.main_thread():
                _ui_update()
            else:
                from textual.app import App
                app = App.get_running_app()
                if app:
                    app.call_from_thread(_ui_update)
                else:
                    # No app available; will still show on complete_streaming()
                    pass
        except Exception:
            pass
            
    def _update_cursor_blink(self) -> None:
        """Update the blinking cursor for streaming text"""
        if not self._is_streaming:
            return
        if not hasattr(self, '_cursor_visible'):
            return
        # Toggle cursor state
        self._cursor_visible = not self._cursor_visible
        # Redraw only the streaming line without clearing entire log to prevent flicker
        try:
            if hasattr(self, '_streaming_line_index') and 0 <= self._streaming_line_index < len(self._log_history):
                # Rebuild the latest streaming line using current text
                timestamp = self._stream_start_time.strftime("%H:%M:%S") if hasattr(self, '_stream_start_time') else datetime.now().strftime("%H:%M:%S")
                line = Text()
                if not self._is_tool_stream:
                    line.append("CAI> ", style="bold green")
                line.append(f"[{timestamp}] ", style="dim")
                # Add content with styling (without markers)
                current_text = getattr(self, '_current_text', '')
                if current_text:
                    # We reuse the styling function but avoid timeline rebuild
                    def _append_with_error_styling(container: Text, content: str) -> None:
                        STDERR_START = "[[STDERR]]"
                        STDERR_END = "[[/STDERR]]"
                        in_error = getattr(self, '_in_error_segment', False)
                        after_error_label = getattr(self, '_after_error_label', False)
                        LABEL = "error output:"
                        idx = 0
                        while idx < len(content):
                            start_pos = content.find(STDERR_START, idx)
                            end_pos = content.find(STDERR_END, idx)
                            label_pos = content.lower().find(LABEL, idx)
                            if not in_error and not after_error_label and label_pos != -1 and (start_pos == -1 or label_pos < start_pos):
                                if label_pos > idx:
                                    container.append(content[idx:label_pos], style="white")
                                container.append(content[label_pos: label_pos + len(LABEL)], style="bold red")
                                after_error_label = True
                                idx = label_pos + len(LABEL)
                                continue
                            if not in_error:
                                if start_pos == -1:
                                    style = "red" if after_error_label else "white"
                                    container.append(content[idx:], style=style)
                                    idx = len(content)
                                else:
                                    if start_pos > idx:
                                        style = "red" if after_error_label else "white"
                                        container.append(content[idx:start_pos], style=style)
                                    in_error = True
                                    idx = start_pos + len(STDERR_START)
                            else:
                                if end_pos == -1:
                                    if idx < len(content):
                                        container.append(content[idx:], style="red")
                                    idx = len(content)
                                else:
                                    if end_pos > idx:
                                        container.append(content[idx:end_pos], style="red")
                                    in_error = False
                                    idx = end_pos + len(STDERR_END)
                        self._in_error_segment = in_error
                        self._after_error_label = after_error_label
                    # Split lines for alignment
                    for i, part in enumerate(current_text.split('\n')):
                        if i == 0:
                            _append_with_error_styling(line, part)
                        else:
                            line.append("\n           ", style="dim")
                            _append_with_error_styling(line, part)
                if getattr(self, '_cursor_visible', False):
                    line.append("▌", style="bold white on black")
                self._log_history[self._streaming_line_index] = line
                # BUGFIX: In-place update for cursor blink without clear()
                try:
                    if (hasattr(self._action_log, '_lines') and 
                        len(self._action_log._lines) > self._streaming_line_index):
                        self._action_log._lines[self._streaming_line_index] = line
                        self._action_log.refresh()
                except Exception:
                    # If it fails, do nothing - better than causing erratic refresh
                    pass
        except Exception:
            pass
    
    def _update_animation(self) -> None:
        """Update the animation frame"""
        if self._is_streaming:
            self._animation_frame = (self._animation_frame + 1) % 8
            # Update executing message animation
            if hasattr(self, '_executing_line_index'):
                self._update_executing_message()
    
    def _update_executing_message(self) -> None:
        """Update the animated executing message"""
        if hasattr(self, '_executing_line_index') and self._action_log:
            line = Text()
            line.append("CAI> ", style="bold green")
            line.append(f"[{self._stream_start_time.strftime('%H:%M:%S')}] ", style="dim")
            line.append("⚡ Executing", style="bold yellow")
            # Animated dots
            dots = "." * ((self._animation_frame % 3) + 1)
            line.append(dots.ljust(3), style="bold yellow")
            
            # Update in history
            if self._executing_line_index < len(self._log_history):
                self._log_history[self._executing_line_index] = line
    
    def _refresh_last_streaming_line(self) -> None:
        """Refresh the last streaming line with cursor"""
        if not hasattr(self, '_streaming_lines_added') or self._streaming_lines_added == 0:
            return
        if not self._action_log:
            return
            
        # Get the last line content
        lines = self._current_text.split('\n') if self._current_text else ['']
        last_line_idx = len(lines) - 1
        
        line = Text()
        if last_line_idx == 0:
            # First line with prompt
            line.append("CAI> ", style="bold green")
            line.append(f"[{self._stream_start_time.strftime('%H:%M:%S')}] ", style="dim")
            line.append(lines[last_line_idx], style="white")
        else:
            # Continuation lines (indented)
            line.append("           ", style="dim")  # Spacing to align
            line.append(lines[last_line_idx], style="white")
        
        # Add blinking cursor (vertical bar that alternates)
        if hasattr(self, '_cursor_visible') and self._cursor_visible:
            line.append("▌", style="bold white on black")  # Solid cursor
        
        # Update in history and refresh display
        history_start = len(self._log_history) - self._streaming_lines_added
        if history_start + last_line_idx < len(self._log_history):
            self._log_history[history_start + last_line_idx] = line
            
            # Clear and rewrite just the last line for cursor update
            # This is more efficient than rewriting everything
            try:
                # Move cursor up one line, clear, and rewrite
                self._action_log.write("\033[1A\033[2K", end="")  # ANSI escape to move up and clear line
                self._action_log.write(line)
            except Exception:
                # If ANSI escape sequences fail, just skip the update
                pass
    
    def _show_ready(self) -> None:
        """Do not show a trailing Ready message (kept for compatibility)."""
        return
        
    def _add_log_entry(self, prompt: str, message: str, style: str = "white") -> None:
        """Add an entry to the action log with modern formatting"""
        if self._action_log:
            # Sanitize inputs
            if not isinstance(prompt, str):
                prompt = str(prompt)
            if not isinstance(message, str):
                message = str(message)
                
            # Clean problematic characters
            prompt = ''.join(char for char in prompt if ord(char) >= 32)
            message = ''.join(char for char in message if ord(char) >= 32)
            
            timestamp = datetime.now().strftime("%H:%M:%S")
            
            # Create a modern styled log entry
            line = Text()
            
            # Subtle indicator
            line.append("• ", style="dim cyan")

            # Timestamp first for better scanning
            line.append(f"{timestamp}", style="dim blue")
            line.append(" │ ", style="dim white")

            # Constant prompt indicator
            line.append("CAI> ", style="bold green")
            line.append(" ▸ ", style="dim")
            
            # Message with enhanced but subtle styling
            if style == "cyan":
                line.append(message, style="cyan")
            elif style == "yellow":
                line.append(message, style="yellow") 
            elif style == "green":
                line.append(message, style="green")
            elif style == "dim green":
                line.append(message, style="dim green")
            else:
                line.append(message, style="white")
            
            try:
                self._action_log.write(line)
                self._log_history.append(line)
                
                # Auto-scroll to bottom if enabled
                if self._auto_scroll and self._action_log:
                    # Use throttled scroll to prevent flicker
                    self._throttled_scroll_end(animate=False)
                    
            except Exception:
                pass
            
            # Update line count for dynamic sizing
            self.log_lines = len(self._log_history)
            
            # Keep fixed height for stability
            # Dynamic height can cause layout issues
        
    def set_action(self, action: str, animation: str = "streaming") -> None:
        """Legacy method for compatibility"""
        # Extract agent name if provided
        if "from" in action:
            parts = action.split("from")
            if len(parts) > 1:
                agent_name = parts[1].strip().replace("...", "")
                self.start_streaming(agent_name)
        else:
            self.start_streaming("agent")
            
    def complete_action(self, message: str = "Ready") -> None:
        """Legacy method for compatibility"""
        self.complete_streaming()
    
    def stop_streaming(self) -> None:
        """Stop streaming mode and complete the current stream"""
        self.complete_streaming()
    
    def write(self, content: Any) -> None:
        """Write content directly to the action bar with subtle formatting"""
        if self._action_log:
            if isinstance(content, str) and content.strip():
                # Add subtle formatting to plain text
                formatted = Text()
                formatted.append("  ", style="")  # Indent for hierarchy
                formatted.append(content, style="dim white")
                self._action_log.write(formatted)
            else:
                self._action_log.write(content)
    
    # Clipboard helpers
    def _history_to_plain_text(self, max_items: int | None = None) -> str:
        try:
            console = Console(record=True, width=160)
            items = self._log_history[-max_items:] if max_items else self._log_history
            for item in items:
                console.print(item)
            return console.export_text(clear=False)
        except Exception:
            # Fallback: join str() representations
            parts = []
            for item in (self._log_history[-max_items:] if max_items else self._log_history):
                parts.append(str(item))
            return "\n".join(parts)
    
    def action_copy_visible(self) -> None:
        try:
            app = self.app
            text = self._history_to_plain_text(max_items=500)
            if app and hasattr(app, "copy_to_clipboard"):
                app.copy_to_clipboard(text)
        except Exception:
            pass
    
    def action_copy_all(self) -> None:
        try:
            app = self.app
            text = self._history_to_plain_text()
            if app and hasattr(app, "copy_to_clipboard"):
                app.copy_to_clipboard(text)
        except Exception:
            pass
    
    def show_execution_indicator(self, indicator: str) -> None:
        """Show execution indicator in action bar if not busy"""
        if not self._is_streaming and self._action_log:
            # Keep a static indicator line without clearing the log to avoid flicker
            line = Text()
            line.append("CAI> ", style="bold green")
            line.append(f"[{datetime.now().strftime('%H:%M:%S')}] ", style="dim")
            line.append(f"{indicator} ", style="yellow")
            line.append("Thinking...", style="dim")
            try:
                if self._exec_line_index is None:
                    # Append once
                    self._action_log.write(line)
                    self._log_history.append(line)
                    self._exec_line_index = len(self._log_history) - 1
                else:
                    # Update stored line in history, but avoid clearing to prevent blinking
                    self._log_history[self._exec_line_index] = line
                # Throttled redraw to reflect updated indicator frame
                self._throttled_render_history()
            except Exception:
                pass
    
    def clear_execution_indicator(self) -> None:
        """Clear the execution indicator"""
        if self._action_log:
            try:
                if self._exec_line_index is not None and 0 <= self._exec_line_index < len(self._log_history):
                    # Remove the indicator line from history
                    self._log_history.pop(self._exec_line_index)
                    self._exec_line_index = None
                    # Coalesced redraw for removal
                self._throttled_render_history()
            except Exception:
                pass

    # ---- Compact mode helpers ---------------------------------------------------
    def set_compact(self, compact: bool = True) -> None:
        """Toggle compact height (useful during replay)."""
        try:
            if compact:
                self.add_class("compact")
            else:
                self.remove_class("compact")
            # Force a refresh of our log after size change
            self._throttled_render_history()
        except Exception:
            pass

    def _animate_tool_exec(self) -> None:
        """Animate the spinner for the tool call line without touching tool output"""
        if self._tool_exec_line_index is None:
            return
        if not self._action_log:
            return
        try:
            self._tool_exec_frame += 1
            braille_frames = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"]
            indicator = braille_frames[self._tool_exec_frame % len(braille_frames)]

            # Rebuild the tool call line with stored timestamp and message
            line = Text()
            line.append("• ", style="dim cyan")
            line.append(f"{self._tool_exec_timestamp}", style="dim blue")
            line.append(" │ ", style="dim white")
            line.append("CAI> ", style="bold green")
            line.append(" ▸ ", style="dim")
            line.append(self._tool_exec_message or "tool()", style="cyan")
            line.append(" ", style="dim")
            line.append(f"{indicator} ", style="yellow")
            line.append("Executing...", style="dim")

            if 0 <= self._tool_exec_line_index < len(self._log_history):
                self._log_history[self._tool_exec_line_index] = line
                # BUGFIX: In-place update without clear() to prevent scroll flicker
                # Only update specific line instead of re-rendering everything
                try:
                    # Calculate line position in RichLog
                    if hasattr(self._action_log, '_lines') and len(self._action_log._lines) > self._tool_exec_line_index:
                        # Update line directly without clear/rewrite
                        self._action_log._lines[self._tool_exec_line_index] = line
                        # Forzar refresh sin scroll
                        self._action_log.refresh()
                    else:
                        # Fallback: no hacer nada para evitar clear()
                        pass
                except Exception:
                    # If in-place update fails, do nothing
                    # Better not to update than cause scroll flicker
                    pass
        except Exception:
            pass

    def finish_tool_execution_indicator(self, status: str = "completed") -> None:
        """Finalize the tool call indicator line (replace Executing... with result)."""
        try:
            # Stop timer if running
            try:
                if self._tool_exec_timer:
                    self._tool_exec_timer.stop()
            except Exception:
                pass
            self._tool_exec_timer = None
            # Clear flag for execute_code panel
            self._has_execute_code_panel = False
            # Update the line to show completion state
            if self._tool_exec_line_index is not None and 0 <= self._tool_exec_line_index < len(self._log_history):
                line = Text()
                line.append("• ", style="dim cyan")
                line.append(f"{self._tool_exec_timestamp}", style="dim blue")
                line.append(" │ ", style="dim white")
                line.append("CAI> ", style="bold green")
                line.append(" ▸ ", style="dim")
                line.append(self._tool_exec_message or "tool()", style="cyan")
                line.append(" ", style="dim")
                if status in ("error", "timeout"):
                    line.append("✗ ", style="bold red")
                    line.append("Failed", style="red")
                else:
                    line.append("✓ ", style="green")
                    line.append("Completed", style="dim")
                self._log_history[self._tool_exec_line_index] = line
                # BUGFIX: In-place update to prevent scroll flicker on completion
                if self._action_log:
                    try:
                        # Try in-place update first
                        if (hasattr(self._action_log, '_lines') and 
                            len(self._action_log._lines) > self._tool_exec_line_index):
                            self._action_log._lines[self._tool_exec_line_index] = line
                            self._action_log.refresh()
                        else:
                            # Only if absolutely necessary, use full render
                            self._render_history(force=True)
                    except Exception:
                        # Fallback: full render only if everything else fails
                        try:
                            self._render_history(force=True)
                        except Exception:
                            pass
            # Clear state
            self._tool_exec_line_index = None
            self._tool_exec_message = None
            self._tool_exec_timestamp = None
            # After finishing, append a fresh 'Thinking...' indicator at the very bottom
            try:
                self.show_execution_indicator(self.ANIMATIONS["thinking"][0])
            except Exception:
                pass
        except Exception:
            pass

    def add_tool_exec_indicator_bottom(self, tool_name: str, args: dict | str | None = None) -> None:
        """Append the animated Executing... indicator as the last line.

        Useful when we have already printed other panels (like the execute_code block)
        and we want the thinking/indicator to stay beneath the streaming output.
        """
        # For execute_code, only add indicator if we showed a code panel
        if tool_name == "execute_code" and not getattr(self, "_has_execute_code_panel", False):
            return
        
        # BUGFIX: Prevent duplicate tool indicators for the same command
        # Check if we already have an active tool execution indicator
        if hasattr(self, '_tool_exec_line_index') and self._tool_exec_line_index is not None:
            # Already have an active tool indicator, skip duplicate
            return
        
        try:
            timestamp = datetime.now().strftime("%H:%M:%S")
            # Build minimal message
            message = tool_name
            if isinstance(args, dict):
                if tool_name == "execute_code":
                    # Short args
                    lang = str(args.get("language", ""))
                    fname = str(args.get("filename", ""))
                    if fname or lang:
                        message = f"{tool_name}(file='{fname}', language='{lang}')"
                else:
                    # Compact dict
                    filtered = {k: v for k, v in args.items() if v and k not in ("call_counter", "input_to_session")}
                    if filtered:
                        inner = ", ".join(f"{k}={repr(v)}" for k, v in filtered.items())
                        if len(inner) > 80:
                            inner = inner[:77] + "..."
                        message = f"{tool_name}({inner})"
            elif isinstance(args, str) and args:
                if len(args) > 80:
                    args = args[:77] + "..."
                message = f"{tool_name}({args})"

            # Compose line
            line = Text()
            line.append("• ", style="dim cyan")
            line.append(f"{timestamp}", style="dim blue")
            line.append(" │ ", style="dim white")
            line.append("CAI> ", style="bold green")
            line.append(" ▸ ", style="dim")
            # Detect MCP UI tag using global registry if available
            try:
                from cai.repl.commands.mcp import get_mcp_server_for_tool
                srv = get_mcp_server_for_tool(tool_name)
                if srv:
                    message = f"MCP:{srv}:{tool_name}"
            except Exception:
                pass
            style = "magenta" if message.startswith("MCP:") else "cyan"
            line.append(message, style=style)
            line.append(" ", style="dim")
            line.append("Executing...", style="dim")
            if self._action_log:
                self._action_log.write(line)
                self._log_history.append(line)
                self._tool_exec_line_index = len(self._log_history) - 1
                self._tool_exec_message = message
                self._tool_exec_timestamp = timestamp
                try:
                    if self._tool_exec_timer:
                        self._tool_exec_timer.stop()
                except Exception:
                    pass
                self._tool_exec_frame = 0
                self._tool_exec_timer = self.set_interval(0.15, self._animate_tool_exec)
        except Exception:
            pass
