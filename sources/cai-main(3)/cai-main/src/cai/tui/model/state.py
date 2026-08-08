"""
TUIState -- pure-data representation of mutable TUI state.

Extracted from CAITerminal.__init__ (cai_terminal.py) so that state can be
inspected, serialized, and tested without instantiating the Textual App.

The CAITerminal class keeps a single ``TUIState`` instance and delegates
all state reads/writes through it.  Reactive properties on the App still
exist for Textual-level reactivity, but they mirror values stored here.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from enum import Enum, auto
from pathlib import Path
from typing import Any, Dict, List, Optional


class TUIMode(Enum):
    """Operating mode of the TUI."""
    SINGLE = "single"
    PARALLEL = "parallel"


class ViewTab(Enum):
    """Active tab in the main tabbed content."""
    TERMINAL = "terminal"
    CTR = "ctr"
    HELP = "help"


@dataclass
class TabState:
    """State for the active tab and tab cycling."""
    active: ViewTab = ViewTab.TERMINAL
    ordered: List[ViewTab] = field(
        default_factory=lambda: [ViewTab.TERMINAL, ViewTab.CTR, ViewTab.HELP]
    )

    @property
    def active_id(self) -> str:
        return self.active.value

    def cycle(self) -> ViewTab:
        """Cycle to the next tab and return the new active tab."""
        idx = self.ordered.index(self.active)
        self.active = self.ordered[(idx + 1) % len(self.ordered)]
        return self.active


@dataclass
class TUIState:
    """Mutable state bag for the CAI TUI.

    Every field corresponds to an instance variable that previously lived
    directly on ``CAITerminal``.  Grouping them here makes the state
    surface explicit and keeps the App class focused on UI wiring.
    """

    # -- mode / view ----------------------------------------------------------
    current_mode: str = "single"
    current_view: str = "terminal"
    sidebar_visible: bool = True
    tab_state: TabState = field(default_factory=TabState)

    # -- widget back-references (set after compose) ---------------------------
    #    Typed as Any to avoid importing heavy Textual widgets at module level.
    terminal_grid: Any = None
    sidebar: Any = None
    command_handler: Any = None
    agent_manager: Any = None
    prompt_input: Any = None
    session_manager: Any = None

    # -- agent routing --------------------------------------------------------
    terminal_agent_map: Dict[str, Any] = field(default_factory=dict)
    show_all_terminals: bool = True

    # -- ESC double-tap detection ---------------------------------------------
    last_esc_time: float = 0.0
    esc_exit_threshold: float = 0.5  # seconds

    # -- history --------------------------------------------------------------
    history_file: Optional[Path] = None

    # -- cancellation ---------------------------------------------------------
    cancelling: bool = False
    cancel_task: Any = None  # asyncio.Task | None

    # -- lifecycle flags ------------------------------------------------------
    startup_config_applied: bool = False
    session_start_time: float = field(default_factory=time.time)

    # -- theme ----------------------------------------------------------------
    theme_cycle: List[str] = field(default_factory=lambda: [
        "textual-dark", "textual-light", "tokyo-night", "nord",
        "solarized-light", "solarized-dark", "alias-robotics", "nature",
    ])

    # ---- convenience helpers ------------------------------------------------

    def reset_esc(self) -> None:
        """Reset the ESC double-tap timer."""
        self.last_esc_time = 0.0

    def record_esc(self) -> bool:
        """Record an ESC press; return True if it was a double-tap."""
        now = time.time()
        is_double = (now - self.last_esc_time) < self.esc_exit_threshold
        self.last_esc_time = now
        return is_double

    def next_theme(self, current: Optional[str]) -> str:
        """Return the next theme in the cycle list."""
        idx = self.theme_cycle.index(current) if current in self.theme_cycle else -1
        return self.theme_cycle[(idx + 1) % len(self.theme_cycle)]
