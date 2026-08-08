"""Red Team Agent with Game-theoretic CTR (Cut The Rope) Integration.

Thin wrapper: clones the base red_teamer agent and attaches CTRHooks.
"""

from typing import Optional

from cai.agents.gctr_mixin import make_gctr_agent
from cai.agents.red_teamer import redteam_agent as _base_agent

_GCTR_KWARGS = dict(
    name="Red Team GCTR",
    description=(
        "Red team agent with integrated game-theoretic security analysis. "
        "Automatically runs CTR (Cut The Rope) analysis every few interactions "
        "to assess defender/attacker strategies and equilibrium."
    ),
    team_label="Red Team",
)

# Default instance
redteam_gctr_agent = make_gctr_agent(_base_agent, **_GCTR_KWARGS)


def create_redteam_gctr_agent(n_interactions: Optional[int] = None):
    """Create a Red Team GCTR agent (backward-compatible factory)."""
    return make_gctr_agent(_base_agent, n_interactions=n_interactions, **_GCTR_KWARGS)


def transfer_to_redteam_gctr_agent(**kwargs):
    """Transfer to red team GCTR agent."""
    return redteam_gctr_agent
