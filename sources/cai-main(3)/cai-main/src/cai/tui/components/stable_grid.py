"""
Stable grid implementation based on Textual best practices
"""

import asyncio
import os
from collections import deque
from typing import Optional

from textual.containers import Container, VerticalScroll, Horizontal, ScrollableContainer
from textual.reactive import reactive

from cai.tui.components.universal_terminal import TerminalRole, UniversalTerminal


# Simple recursion guard
class GridRecursionGuard:
    def __init__(self):
        self.initialization_done = False
        self.attempts = 0
        self.max_attempts = 3

    def can_initialize(self):
        if self.initialization_done:
            return False
        if self.attempts >= self.max_attempts:
            return False
        self.attempts += 1
        return True

    def mark_done(self):
        self.initialization_done = True


_grid_guard = GridRecursionGuard()



class StableTerminalGrid(ScrollableContainer):
    """Stable terminal grid that follows Textual best practices"""

    DEFAULT_CSS = """
    StableTerminalGrid {
        width: 100%;
        height: 100%;
        margin: 0;
        padding: 0;
        scrollbar-size: 1 1;
        scrollbar-color: #529d86;
        scrollbar-background: #2e4f46;
    }
    
    /* The inner grid container */
    StableTerminalGrid #terminal-grid-inner {
        layout: grid;
        grid-size: 1 1;
        grid-gutter: 1;
        padding: 0;
        width: 100%;
        height: auto;
        margin: 0;
    }

    #terminal-grid-inner > UniversalTerminal {
        width: 1fr;
        height: 1fr;
        min-height: 10;
        min-width: 30;
        margin: 0;
        padding: 0;
    }

    #terminal-grid-inner.layout-single {
        grid-size: 1 1;
        height: 100%;
    }

    #terminal-grid-inner.layout-split {
        grid-size: 2 1;
        height: 100%;
    }

    #terminal-grid-inner.layout-triple {
        grid-size: 3 1;
        height: 100%;
    }

    #terminal-grid-inner.layout-quad {
        grid-size: 2 2;
        height: 100%;
    }
    
    /* For more than 4 terminals, use grid with fixed row heights */
    #terminal-grid-inner.layout-scrollable {
        grid-size: 2;
        grid-gutter: 1;
        height: auto;
    }
    
    /* In scrollable mode, terminals have fixed height */
    #terminal-grid-inner.layout-scrollable UniversalTerminal {
        width: 1fr;
        height: 50vh;
        min-height: 50vh;
    }
    
    /* Fullscreen mode for single terminal */
    #terminal-grid-inner.layout-fullscreen {
        grid-size: 1 1;
        grid-gutter: 0;
        width: 100%;
        height: 100%;
    }
    
    #terminal-grid-inner.layout-fullscreen UniversalTerminal {
        width: 100%;
        height: 100%;
        min-width: 100%;
        min-height: 100%;
    }
    """

    terminal_count = reactive(0)
    layout_mode = reactive("single")

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.terminals: dict[str, UniversalTerminal] = {}
        self.main_terminal_id: Optional[str] = None
        self._mounting = False
        self._terminal_counter = 0  # For assigning terminal numbers
        self._used_terminal_numbers: set[int] = set()  # Track used terminal numbers
        self._focused_terminal_id: Optional[str] = None  # Track focused terminal
        self._show_only_focused = False  # Toggle state for showing only focused terminal
        # Create inner grid container
        self.grid_container = Container(id="terminal-grid-inner")
        # Queue for sequentially mounting additional terminals
        self._pending_mounts = deque()
        self._mount_worker: asyncio.Task | None = None
        self._sequential_mount_delay = 0.12

    def on_mount(self) -> None:
        """Initialize with main terminal on mount"""
        # Mount the inner grid container
        self.mount(self.grid_container)
        # Use async task instead of call_after_refresh
        asyncio.create_task(self._init_main_terminal())
        self._ensure_mount_worker()

    def _ensure_mount_worker(self) -> None:
        """Start the background worker that mounts queued terminals sequentially."""
        if self._pending_mounts and (self._mount_worker is None or self._mount_worker.done()):
            self._mount_worker = asyncio.create_task(self._process_mount_queue())

    async def _process_mount_queue(self) -> None:
        """Mount pending terminals one at a time with a short delay between each."""
        try:
            while self._pending_mounts:
                terminal, role, agent_name = self._pending_mounts.popleft()
                self._mounting = True

                # Mount into the grid container and update counts
                self.grid_container.mount(terminal)
                self.terminal_count = len(self.terminals)

                # Configure the terminal and refresh layout before mounting the next one
                await self._delayed_configure(terminal, role, agent_name)
                await self._delayed_layout_update()

                # Small gap so Select overlays finish constructing before the next terminal mounts
                await asyncio.sleep(self._sequential_mount_delay)
        finally:
            self._mounting = False
            self._mount_worker = None

    async def wait_for_pending_mounts(self, timeout: float = 3.0) -> None:
        """Wait until all queued terminals have been mounted or timeout elapses."""
        start = asyncio.get_running_loop().time()
        while True:
            pending = self._pending_mounts
            worker_active = self._mount_worker and not self._mount_worker.done()
            if (not pending) and not worker_active and not self._mounting:
                return
            if asyncio.get_running_loop().time() - start >= timeout:
                return
            await asyncio.sleep(0.05)

    async def _init_main_terminal(self) -> None:
        """Initialize main terminal immediately"""
        if _grid_guard.can_initialize():
            self._create_main_terminal()
            _grid_guard.mark_done()

    def _create_main_terminal(self) -> None:
        """Create the main terminal"""
        if self._mounting:
            return

        self._mounting = True

        # Create main terminal with number 1 using UniversalTerminal
        self._terminal_counter = 1
        self._used_terminal_numbers.add(1)  # Mark 1 as used
        main_terminal = UniversalTerminal(terminal_number=self._terminal_counter)
        main_terminal.add_class("terminal-cell")
        self.main_terminal_id = main_terminal.terminal_id
        self.terminals[main_terminal.terminal_id] = main_terminal
        
        # Don't focus any terminal initially
        self._focused_terminal_id = None
        # Don't add class here - wait until terminal is fully mounted
        # main_terminal.add_class("terminal-focused")

        # Mount it to the grid container
        self.grid_container.mount(main_terminal)

        # Configure after mount using async task
        asyncio.create_task(self._delayed_configure(main_terminal, "main"))
        
        # Start with terminal unfocused
        main_terminal.add_class("terminal-unfocused")
    
    def _get_next_terminal_number(self) -> int:
        """Get the next available terminal number"""
        # Find the lowest available number starting from 2
        num = 2
        while num in self._used_terminal_numbers:
            num += 1
        return num

    async def _delayed_configure(
        self,
        terminal: UniversalTerminal,
        role: str,
        agent_name: Optional[str] = None,
        preserve_content: bool = False,
    ) -> None:
        """Configure terminal immediately"""
        # Wait for terminal to be fully ready (increased from 0.05 to fix banner display)
        await asyncio.sleep(0.15)
        await self._configure_terminal(terminal, role, agent_name, preserve_content)

        self._mounting = False
        # Don't update terminal count here - it's managed elsewhere
        self._update_layout_class()
    
    async def _delayed_layout_update(self) -> None:
        """Update layout after a delay to ensure proper mounting"""
        await asyncio.sleep(0.2)  # Slightly longer delay for layout
        self._update_layout_class()
        # Force multiple refreshes to ensure proper layout
        self.refresh(layout=True)
        await asyncio.sleep(0.1)
        self.refresh(layout=True)

    async def _configure_terminal(
        self,
        terminal: UniversalTerminal,
        role: TerminalRole,
        agent_name: str = "",
        preserve_content: bool = False,
    ) -> None:
        """Safely configure a terminal"""
        try:
            await terminal.configure(role, agent_name, preserve_content)
        except Exception as e:
            # Log error but don't crash
            if terminal.output:
                terminal.write(f"[red]Configuration error: {e}[/red]")

    def add_agent_terminal(self, agent_name: str) -> Optional[UniversalTerminal]:
        """Add an agent terminal using safe mounting."""
        # Get next available terminal number (reuse freed numbers)
        terminal_number = self._get_next_terminal_number()
        self._used_terminal_numbers.add(terminal_number)
        agent_terminal = UniversalTerminal(terminal_number=terminal_number)
        self.terminals[agent_terminal.terminal_id] = agent_terminal

        # Enqueue the terminal so it mounts sequentially
        self._pending_mounts.append((agent_terminal, "agent", agent_name))
        self._ensure_mount_worker()

        return agent_terminal

    def remove_agent_terminals(self) -> None:
        """Remove all agent terminals safely"""
        # Clear any pending mounts since we are removing terminals
        self._pending_mounts.clear()
        if self._mount_worker and not self._mount_worker.done():
            self._mount_worker.cancel()
        self._mount_worker = None
        self._mounting = False

        # Find agent terminals
        to_remove = []
        for tid, terminal in self.terminals.items():
            if tid != self.main_terminal_id:
                to_remove.append((tid, terminal))

        # Remove them safely
        for tid, terminal in to_remove:
            try:
                # Remove from tracking
                del self.terminals[tid]
                # Remove from UI
                terminal.remove()
            except Exception:
                pass  # Ignore removal errors

        # Clear used terminal numbers except 1 (main terminal)
        self._used_terminal_numbers = {1}
        self.terminal_count = len(self.terminals)
        self._update_layout_class()

    def _update_layout_class(self) -> None:
        """Update CSS classes based on terminal count"""
        # Save current focused terminal
        focused_terminal_id = self._focused_terminal_id
        
        # Remove all layout classes from grid container
        self.grid_container.remove_class(
            "layout-single",
            "layout-split",
            "layout-triple",
            "layout-quad",
            "layout-scrollable"
        )

        # Add appropriate class
        count = self.terminal_count
        if count <= 1:
            self.grid_container.add_class("layout-single")
            self.layout_mode = "single"
        elif count == 2:
            self.grid_container.add_class("layout-split")
            self.layout_mode = "split"
        elif count == 3:
            self.grid_container.add_class("layout-triple")
            self.layout_mode = "triple"
        elif count == 4:
            self.grid_container.add_class("layout-quad")
            self.layout_mode = "quad"
        else:
            # For more than 4 terminals, use scrollable grid
            self.grid_container.add_class("layout-scrollable")
            self.layout_mode = "scrollable"

        # Apply classes to all terminals
        for terminal_id, terminal in self.terminals.items():
            # Preserve focus state
            is_focused = terminal_id == focused_terminal_id
            
            terminal.remove_class("multiple-terminals", "many-terminals", "two-terminals")
            if count >= 4:
                # Only add many-terminals class for 4+ terminals
                terminal.add_class("many-terminals")
                # Disable summarized mode - we want to see tool panels
                terminal.set_summarized_mode(False)
            elif count == 2:
                # Apply a dedicated class for 2 terminals (same width as quad horizontally)
                terminal.add_class("two-terminals")
                terminal.set_summarized_mode(False)
            elif count == 3:
                # For exactly 3 terminals, use a lighter responsive mode
                terminal.add_class("multiple-terminals")
                terminal.set_summarized_mode(False)
            else:
                # For less than 4 terminals, no special classes needed
                # Disable summarized mode
                terminal.set_summarized_mode(False)
            
            # Restore focus state
            if is_focused:
                if "terminal-focused" not in terminal.classes:
                    terminal.add_class("terminal-focused")
                terminal.remove_class("terminal-unfocused")
            else:
                terminal.remove_class("terminal-focused")
                if "terminal-unfocused" not in terminal.classes:
                    terminal.add_class("terminal-unfocused")

        # Force a layout refresh
        self.refresh(layout=True)


    def setup_parallel_agents(self, agent_configs: list[any]) -> None:
        """Setup parallel agent execution"""
        # This method is now handled directly in cai_terminal.py
        # to ensure proper terminal count (N terminals for N agents)
        pass

    def split_terminal(self, agent_name: str) -> None:
        """Split terminal to add an agent - like terminal emulator split"""
        self.add_agent_terminal(agent_name)

    def get_main_terminal(self) -> Optional[UniversalTerminal]:
        """Get the main terminal"""
        if self.main_terminal_id:
            return self.terminals.get(self.main_terminal_id)
        return None

    def get_terminal_by_number(self, terminal_number: int) -> Optional[UniversalTerminal]:
        """Return a terminal by its numeric identifier."""
        for terminal in self.terminals.values():
            if getattr(terminal, "terminal_number", None) == terminal_number:
                return terminal
        return None
    
    def get_focused_terminal(self) -> Optional[UniversalTerminal]:
        """Get the currently focused/selected terminal"""
        if self._focused_terminal_id:
            terminal = self.terminals.get(self._focused_terminal_id)
            # Check if terminal actually has the focused class
            if terminal and "terminal-focused" in terminal.classes:
                return terminal
        # Return None if no terminal is actually focused
        return None
    
    def unfocus_all_terminals(self) -> None:
        """Remove focus from all terminals"""
        self._focused_terminal_id = None
        for terminal in self.terminals.values():
            terminal.remove_class("terminal-focused")
            if "terminal-unfocused" not in terminal.classes:
                terminal.add_class("terminal-unfocused")
    
    def remove_terminal(self, terminal_id: str) -> bool:
        """Remove a terminal by ID"""
        if terminal_id not in self.terminals:
            return False
            
        terminal = self.terminals[terminal_id]
        
        # Don't remove the main terminal
        if terminal_id == self.main_terminal_id:
            return False
            
        # Free up the terminal number for reuse
        terminal_number = terminal.terminal_number
        if terminal_number in self._used_terminal_numbers:
            self._used_terminal_numbers.remove(terminal_number)
            
        # Remove from tracking first
        del self.terminals[terminal_id]
        
        # Update terminal count
        self.terminal_count = len(self.terminals)
        
        # If focused terminal was removed, focus another
        if self._focused_terminal_id == terminal_id:
            # Focus main terminal or first available
            if self.main_terminal_id and self.main_terminal_id in self.terminals:
                self.focus_terminal(self.main_terminal_id)
            elif self.terminals:
                first_id = list(self.terminals.keys())[0]
                self.focus_terminal(first_id)
            else:
                self._focused_terminal_id = None
        
        # If we're showing only focused, update the view
        if self._show_only_focused:
            self.show_only_focused()
            
        # Remove from DOM
        try:
            terminal.remove()
        except Exception:
            pass
            
        # Update grid layout
        self._update_layout_class()
                
        return True

    def get_agent_terminals(self) -> list[UniversalTerminal]:
        """Get all agent terminals"""
        agents = []
        for tid, terminal in self.terminals.items():
            if tid != self.main_terminal_id:
                agents.append(terminal)
        return agents

    async def broadcast_command(
        self, command: str, target_role: Optional[TerminalRole] = None
    ) -> None:
        """Broadcast command to terminals"""
        tasks = []

        for tid, terminal in self.terminals.items():
            if target_role == "agent" and tid == self.main_terminal_id:
                continue  # Skip main for agent commands

            if hasattr(terminal, "run_command"):
                tasks.append(terminal.run_command(command))

        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)

    def clear_all(self) -> None:
        """Clear all terminals"""
        for terminal in self.terminals.values():
            if hasattr(terminal, "clear"):
                terminal.clear()

    def focus_next_terminal(self) -> None:
        """Focus next terminal in order - DISABLED"""
        pass

    def focus_previous_terminal(self) -> None:
        """Focus previous terminal in order - DISABLED"""
        pass

    # Compatibility methods for cai_terminal.py
    def clear_agents(self) -> None:
        """Clear all agent terminals (alias for remove_agent_terminals)"""
        self.remove_agent_terminals()

    @property
    def active_terminals(self) -> list[UniversalTerminal]:
        """Get all active terminals for compatibility"""
        return list(self.terminals.values())

    def focus_terminal(self, terminal_id: str) -> None:
        """Focus a specific terminal by ID"""
        if terminal_id not in self.terminals:
            return
            
        # Don't update if already focused
        if self._focused_terminal_id == terminal_id:
            return
            
        # Update focused terminal
        old_focused = self._focused_terminal_id
        self._focused_terminal_id = terminal_id
        
        # Update visual indicators safely
        try:
            for tid, terminal in self.terminals.items():
                if tid == terminal_id:
                    if "terminal-focused" not in terminal.classes:
                        terminal.add_class("terminal-focused")
                    terminal.remove_class("terminal-unfocused")
                else:
                    terminal.remove_class("terminal-focused")
                    if "terminal-unfocused" not in terminal.classes:
                        terminal.add_class("terminal-unfocused")
        except Exception:
            # Ignore any CSS class errors
            pass
        
        # Emit focus events
        if old_focused and old_focused != terminal_id:
            from cai.tui.patterns.observer import EventType, terminal_event_manager
            terminal_event_manager.emit(EventType.TERMINAL_UNFOCUSED, old_focused)
        
        from cai.tui.patterns.observer import EventType, terminal_event_manager
        terminal_event_manager.emit(EventType.TERMINAL_FOCUSED, terminal_id)
        
        # Log for debugging
        if hasattr(self, 'app') and self.app:
            main_terminal = self.get_main_terminal()
            if main_terminal and os.getenv("CAI_DEBUG") == "2":
                main_terminal.write(f"[dim]Terminal {terminal_id} focused[/dim]")

    def cycle_focus(self, forward: bool = True) -> None:
        """Cycle focus through terminals"""
        if not self.terminals:
            return
            
        # Get sorted list of terminal IDs
        terminal_ids = sorted(self.terminals.keys())
        
        if not terminal_ids:
            return
            
        # Find current focus index
        current_index = 0
        if self._focused_terminal_id in terminal_ids:
            current_index = terminal_ids.index(self._focused_terminal_id)
        
        # Calculate next index
        if forward:
            next_index = (current_index + 1) % len(terminal_ids)
        else:
            next_index = (current_index - 1) % len(terminal_ids)
            
        # Focus the next terminal
        self.focus_terminal(terminal_ids[next_index])
    
    def show_all_terminals(self) -> None:
        """Show all terminals in grid layout"""
        # Mark that we're showing all terminals
        self._show_only_focused = False
        
        # Reset all terminals to normal display
        for terminal in self.terminals.values():
            terminal.styles.display = "block"
            # Reset to grid fractional sizes
            terminal.styles.width = "1fr"
            terminal.styles.height = "1fr"
            terminal.styles.min_width = None
            terminal.styles.min_height = None
            # Reset grid constraints
            terminal.styles.column_span = None
            terminal.styles.row_span = None
        
        # Remove fullscreen class
        self.grid_container.remove_class("layout-fullscreen")
        
        # Reset grid container
        self.grid_container.styles.width = "100%"
        self.grid_container.styles.height = "100%"
        
        # Restore proper layout based on terminal count
        self._update_layout_class()
        
        # Restore grid gutter based on layout
        if self.terminal_count > 1:
            self.grid_container.styles.grid_gutter = (1, 1)
        
        # Force refresh
        self.refresh(layout=True)
    
    def show_only_focused(self) -> None:
        """Show only the focused terminal in fullscreen"""
        if not self._focused_terminal_id:
            # If no terminal is focused, focus the main terminal
            if self.main_terminal_id:
                self.focus_terminal(self.main_terminal_id)
                self._focused_terminal_id = self.main_terminal_id
            else:
                # Fallback to first terminal
                if self.terminals:
                    first_id = list(self.terminals.keys())[0]
                    self.focus_terminal(first_id)
                    self._focused_terminal_id = first_id
        
        # Mark that we're showing only focused
        self._show_only_focused = True
        
        # Hide all terminals except the focused one
        for terminal_id, terminal in self.terminals.items():
            if terminal_id == self._focused_terminal_id:
                # Make the focused terminal fullscreen
                terminal.styles.display = "block"
                terminal.styles.width = "100%"
                terminal.styles.height = "100%"
                terminal.styles.min_width = "100%"
                terminal.styles.min_height = "100%"
                # Remove grid constraints
                terminal.styles.column_span = 1
                terminal.styles.row_span = 1
            else:
                # Hide all other terminals
                terminal.styles.display = "none"
                
        # Update grid container to single cell layout for fullscreen
        self.grid_container.styles.grid_size = (1, 1)
        self.grid_container.styles.grid_gutter = (0, 0)
        self.grid_container.styles.width = "100%"
        self.grid_container.styles.height = "100%"
        
        # Remove layout classes
        self.grid_container.remove_class(
            "layout-single", "layout-split", "layout-triple", 
            "layout-quad", "layout-scrollable"
        )
        self.grid_container.add_class("layout-fullscreen")
        
        # Force refresh
        self.refresh(layout=True)
    
    def is_showing_only_focused(self) -> bool:
        """Check if we're showing only the focused terminal"""
        return self._show_only_focused
