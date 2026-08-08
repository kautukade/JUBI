"""
Routing module for terminal output

This module ensures that output from agents and tools goes to the correct terminal,
especially important in parallel mode where multiple agents run simultaneously.
"""

from .output_router import (
    ContextVarRoutingStrategy,
    HybridRoutingStrategy,
    IRoutingStrategy,
    OutputRouter,
    TerminalRoutingContext,
    ThreadLocalRoutingStrategy,
    current_terminal_id,
    current_terminal_number,
    get_current_terminal_id,
    output_router,
    route_to_terminal,
    set_terminal_context,
)

__all__ = [
    # Interfaces and strategies
    "IRoutingStrategy",
    "ContextVarRoutingStrategy",
    "ThreadLocalRoutingStrategy",
    "HybridRoutingStrategy",
    # Router
    "OutputRouter",
    "TerminalRoutingContext",
    "output_router",
    # Helper functions
    "get_current_terminal_id",
    "set_terminal_context",
    "route_to_terminal",
    # Context variables
    "current_terminal_id",
    "current_terminal_number",
]
