"""
InputController -- keyboard/command routing extracted from CAITerminal.

Centralises the logic that decides *where* a user command should go:
- CLI command (``/`` or ``$`` prefix) to a specific terminal
- Chat prompt to a single agent, broadcast to all, or agent selector
- Terminal-prefix syntax (``T2:/model gpt-4o``)

The controller never touches Textual widgets directly; it returns
routing decisions that the App layer (``CAITerminal``) acts upon.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass
from enum import Enum, auto
from typing import TYPE_CHECKING, Any, List, Optional, Tuple

if TYPE_CHECKING:
    from cai.tui.model.state import TUIState


# ---------------------------------------------------------------------------
# Routing decision types
# ---------------------------------------------------------------------------

class RouteKind(Enum):
    """Describes where / how a command should be dispatched."""
    CLI_SINGLE = auto()       # CLI command for one terminal
    CLI_BROADCAST = auto()    # CLI command broadcast to all terminals
    CHAT_SINGLE = auto()      # Chat prompt for one terminal
    CHAT_BROADCAST = auto()   # Chat prompt broadcast to all terminals
    CHAT_SELECT = auto()      # Multiple agents -- show selector
    EXIT = auto()             # User typed exit/quit
    PARALLEL_PATTERN = auto() # Agent number >= 20 -- parallel pattern
    META_AGENT = auto()       # Meta agent interception
    DEBUG = auto()            # Internal debug command
    NOP = auto()              # Nothing to do


@dataclass
class RouteDecision:
    """Immutable description of how a command should be routed."""
    kind: RouteKind
    command: str = ""
    target_terminal_num: Optional[int] = None
    broadcast: bool = False
    pattern_num: Optional[int] = None
    active_agents: List[Tuple[int, str, str]] = None  # type: ignore[assignment]

    def __post_init__(self):
        if self.active_agents is None:
            self.active_agents = []


class InputController:
    """Stateless command router.

    Parameters
    ----------
    state:
        The shared :class:`TUIState` for read-only inspection of widget
        references (terminal_grid, session_manager, etc.).
    """

    def __init__(self, state: "TUIState") -> None:
        self._state = state

    # ------------------------------------------------------------------
    # Public entry-point
    # ------------------------------------------------------------------

    def route(self, raw_command: str) -> RouteDecision:
        """Determine routing for *raw_command* entered by the user.

        This is a pure-logic function: it inspects the command text and
        current state but performs no I/O or widget mutations.
        """
        command = raw_command.strip()
        if not command:
            return RouteDecision(kind=RouteKind.NOP)

        # --- Terminal-prefix syntax: T2:/model gpt-4o ---
        prefix_match = re.match(r"^T(\d+):(.+)$", command)
        if prefix_match:
            target = int(prefix_match.group(1))
            inner = prefix_match.group(2).strip()
            kind = RouteKind.CLI_SINGLE if self._is_cli(inner) else RouteKind.CHAT_SINGLE
            return RouteDecision(kind=kind, command=inner, target_terminal_num=target)

        # --- Meta-agent interception ---
        if (
            os.environ.get("CAI_META_AGENT", "false").lower() == "true"
            and not self._is_cli(command)
        ):
            return RouteDecision(kind=RouteKind.META_AGENT, command=command)

        # --- Debug ---
        if command == "/debug terminals":
            return RouteDecision(kind=RouteKind.DEBUG, command=command)

        # --- Exit ---
        if command.lower() in ("exit", "quit"):
            return RouteDecision(kind=RouteKind.EXIT, command=command)

        # --- CLI commands ---
        if self._is_cli(command):
            return self._route_cli(command)

        # --- Chat prompts ---
        return self._route_chat(command)

    # ------------------------------------------------------------------
    # Helpers -- exposed for testing
    # ------------------------------------------------------------------

    @staticmethod
    def _is_cli(command: str) -> bool:
        return command.startswith("/") or command.startswith("$")

    @staticmethod
    def parse_terminal_target(args: List[str]) -> Tuple[List[str], Optional[int]]:
        """Parse ``t1``, ``t2`` etc. from the tail of *args*.

        Returns (cleaned_args, terminal_number | None).
        """
        from cai.tui.utils.terminal_parser import parse_terminal_target
        return parse_terminal_target(args)

    @staticmethod
    def detect_broadcast(args: List[str]) -> Tuple[List[str], bool]:
        """Strip a trailing ``all`` keyword from args."""
        if args and args[-1].lower() == "all":
            return args[:-1], True
        return args, False

    def _collect_active_agents(self) -> List[Tuple[int, str, str]]:
        """Return (terminal_number, agent_name, agent_id) for every active terminal."""
        grid = self._state.terminal_grid
        if grid is None:
            return []
        agents = []
        for t in grid.active_terminals:
            if hasattr(t, "state") and t.state.agent_name:
                agents.append((
                    t.terminal_number,
                    t.state.agent_name,
                    getattr(t.state, "agent_id", "unknown"),
                ))
        return agents

    # ------------------------------------------------------------------
    # Internal routing
    # ------------------------------------------------------------------

    def _route_cli(self, command: str) -> RouteDecision:
        """Route a ``/`` or ``$`` prefixed command."""
        parts = command.split()
        cmd_name = parts[0][1:] if parts[0].startswith("/") else parts[0]
        args = parts[1:] if len(parts) > 1 else []

        cleaned_args, target_num = self.parse_terminal_target(args)
        cleaned_args, broadcast = self.detect_broadcast(cleaned_args)

        # Parallel-pattern check (/agent 20+)
        if cmd_name == "agent" and len(parts) > 1 and parts[1].isdigit():
            agent_num = int(parts[1])
            if agent_num >= 20:
                return RouteDecision(
                    kind=RouteKind.PARALLEL_PATTERN,
                    command=command,
                    pattern_num=agent_num,
                )

        # Reconstruct command without terminal suffix
        if target_num is not None or broadcast:
            command = f"{parts[0]} {' '.join(cleaned_args)}" if cleaned_args else parts[0]

        kind = RouteKind.CLI_BROADCAST if broadcast else RouteKind.CLI_SINGLE
        return RouteDecision(
            kind=kind,
            command=command,
            target_terminal_num=target_num,
            broadcast=broadcast,
            active_agents=self._collect_active_agents() if broadcast else [],
        )

    def _route_chat(self, command: str) -> RouteDecision:
        """Route a plain chat prompt (no ``/`` prefix)."""
        words = command.split()
        cleaned_words, target_num = self.parse_terminal_target(words)
        cleaned_words, broadcast = self.detect_broadcast(cleaned_words)

        if target_num is not None or broadcast:
            command = " ".join(cleaned_words)

        active = self._collect_active_agents()

        if broadcast:
            return RouteDecision(
                kind=RouteKind.CHAT_BROADCAST,
                command=command,
                broadcast=True,
                active_agents=active,
            )

        if target_num is not None:
            return RouteDecision(
                kind=RouteKind.CHAT_SINGLE,
                command=command,
                target_terminal_num=target_num,
            )

        # Single agent -> direct; multiple -> selector
        if len(active) <= 1:
            term_num = active[0][0] if active else None
            return RouteDecision(
                kind=RouteKind.CHAT_SINGLE,
                command=command,
                target_terminal_num=term_num,
                active_agents=active,
            )

        return RouteDecision(
            kind=RouteKind.CHAT_SELECT,
            command=command,
            active_agents=active,
        )
