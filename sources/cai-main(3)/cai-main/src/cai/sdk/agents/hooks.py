"""Agent loop hooks -- extensible turn-level callbacks for Runner.run().

AgentLoopHook defines a Protocol with three entry points that run
*around* each turn of Runner.run():

    on_turn_start   -- called before each agent turn
    on_turn_end     -- called after each agent turn
    should_continue -- gate that can terminate the loop early
"""

from __future__ import annotations

import logging
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, Protocol, runtime_checkable

if TYPE_CHECKING:
    from .agent import Agent
    from .run_context import RunContextWrapper

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Turn result -- lightweight container passed to on_turn_end
# ---------------------------------------------------------------------------


@dataclass
class TurnResult:
    """Lightweight summary of a completed turn, passed to on_turn_end."""

    turn: int
    """The turn number that just completed (1-based)."""

    agent_name: str = ""
    """Name of the agent that ran this turn."""

    generated_items_count: int = 0
    """How many items were generated in this turn."""

    extra: dict[str, Any] = field(default_factory=dict)
    """Arbitrary key-value pairs that callers may attach."""


# ---------------------------------------------------------------------------
# Protocol
# ---------------------------------------------------------------------------


@runtime_checkable
class AgentLoopHook(Protocol):
    """Extension point invoked by Runner.run() on every turn.

    Implementations need only define the methods they care about;
    the Runner treats missing/None returns as no-ops.
    """

    async def on_turn_start(
        self,
        turn: int,
        context: "RunContextWrapper[Any]",
        agent: "Agent[Any]",
    ) -> None:
        """Called just before the agent is invoked for this turn."""
        ...

    async def on_turn_end(
        self,
        turn: int,
        result: TurnResult,
        context: "RunContextWrapper[Any]",
        agent: "Agent[Any]",
    ) -> None:
        """Called after the agent completes a turn (including tool calls)."""
        ...

    async def should_continue(
        self,
        turn: int,
        context: "RunContextWrapper[Any]",
        agent: "Agent[Any]",
    ) -> bool:
        """Return False to stop the Runner loop early."""
        ...


# ---------------------------------------------------------------------------
# Default hook set
# ---------------------------------------------------------------------------

# Hooks that run.py should invoke by default.
# Other code can append to this list to inject custom turn-level logic.
DEFAULT_LOOP_HOOKS: list[AgentLoopHook] = []
