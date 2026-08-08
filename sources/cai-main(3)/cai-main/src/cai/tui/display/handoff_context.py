"""
Handoff context propagation for TUI display system
"""

import os
import contextvars
from typing import Optional, Dict, Any, TYPE_CHECKING
from dataclasses import dataclass

if TYPE_CHECKING:
    from cai.sdk.agents import Agent


@dataclass
class DisplayContext:
    """Display context that needs to be propagated during handoffs"""

    terminal_id: Optional[str] = None
    terminal_number: Optional[int] = None
    agent_name: Optional[str] = None
    agent_id: Optional[str] = None
    is_parallel: bool = False
    is_tui_mode: bool = False
    display_manager: Optional[Any] = None


# Context variable to store display context
_display_context: contextvars.ContextVar[Optional[DisplayContext]] = contextvars.ContextVar(
    "display_context", default=None
)


def set_display_context(context: DisplayContext) -> contextvars.Token:
    """Set the display context in the current async context"""
    return _display_context.set(context)


def get_display_context() -> Optional[DisplayContext]:
    """Get the display context from the current async context"""
    return _display_context.get()


def clear_display_context(token: contextvars.Token) -> None:
    """Clear the display context"""
    _display_context.reset(token)


def propagate_display_context_to_agent(
    agent: "Agent", parent_agent: Optional["Agent"] = None
) -> None:
    """
    Propagate display context to a new agent during handoff

    Args:
        agent: The new agent being created
        parent_agent: The parent agent (if available)
    """
    # Get context from contextvars
    context = get_display_context()

    if context and context.is_tui_mode:
        # Set TUI mode environment variable for the new agent
        os.environ["CAI_TUI_MODE"] = "true"

        # If the agent has a model, configure it for TUI display
        if hasattr(agent, "model"):
            model = agent.model

            # Set agent identity
            if hasattr(model, "agent_name"):
                model.agent_name = agent.name
            if hasattr(model, "agent_id") and context.agent_id:
                # For sub-agents, create a new ID based on parent
                if (
                    parent_agent
                    and hasattr(parent_agent, "model")
                    and hasattr(parent_agent.model, "agent_id")
                ):
                    parent_id = parent_agent.model.agent_id
                    # Create sub-agent ID like "T1-S1" for sub-agent 1 of terminal 1
                    sub_id = f"{parent_id}-S{id(agent) % 100}"
                    model.agent_id = sub_id
                else:
                    model.agent_id = context.agent_id

            # Configure display settings
            if hasattr(model, "disable_rich_streaming"):
                model.disable_rich_streaming = True
            if hasattr(model, "suppress_final_output"):
                model.suppress_final_output = False

            # Store display context reference
            if hasattr(model, "_display_context"):
                model._display_context = context

            # Set terminal tracking info
            if context.terminal_id:
                if hasattr(model, "_terminal_id"):
                    model._terminal_id = context.terminal_id
                if hasattr(model, "_terminal_number"):
                    model._terminal_number = context.terminal_number

    # Also check parent agent for display context
    elif parent_agent and hasattr(parent_agent, "model"):
        parent_model = parent_agent.model

        # Check if parent has display context
        if hasattr(parent_model, "_display_context") and parent_model._display_context:
            parent_context = parent_model._display_context

            # Propagate to new agent
            if hasattr(agent, "model"):
                model = agent.model

                # Copy display context
                if hasattr(model, "_display_context"):
                    model._display_context = parent_context

                # Copy terminal info
                if hasattr(parent_model, "_terminal_id") and hasattr(model, "_terminal_id"):
                    model._terminal_id = parent_model._terminal_id
                if hasattr(parent_model, "_terminal_number") and hasattr(model, "_terminal_number"):
                    model._terminal_number = parent_model._terminal_number

                # Copy display settings
                if hasattr(parent_model, "disable_rich_streaming") and hasattr(
                    model, "disable_rich_streaming"
                ):
                    model.disable_rich_streaming = parent_model.disable_rich_streaming
                if hasattr(parent_model, "suppress_final_output") and hasattr(
                    model, "suppress_final_output"
                ):
                    model.suppress_final_output = parent_model.suppress_final_output

                # Set agent identity
                if hasattr(model, "agent_name"):
                    model.agent_name = agent.name
                if hasattr(model, "agent_id") and hasattr(parent_model, "agent_id"):
                    # Create sub-agent ID
                    parent_id = parent_model.agent_id
                    sub_id = f"{parent_id}-S{id(agent) % 100}"
                    model.agent_id = sub_id


def ensure_tui_mode_in_handoff() -> None:
    """Ensure TUI mode is set if we have display context"""
    context = get_display_context()
    if context and context.is_tui_mode:
        os.environ["CAI_TUI_MODE"] = "true"
