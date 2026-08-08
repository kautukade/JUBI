"""Blue Team Agent with Game-theoretic CTR (Cut The Rope) Integration.

Thin wrapper: clones the base blue_teamer agent and attaches CTRHooks.
"""

from typing import Optional

from cai.agents.gctr_mixin import make_gctr_agent
from cai.agents.blue_teamer import blueteam_agent as _base_agent

_GCTR_KWARGS = dict(
    name="Blue Team GCTR",
    description=(
        "Blue team agent with integrated game-theoretic security analysis. "
        "Automatically runs CTR (Cut The Rope) analysis every few interactions "
        "to assess defender/attacker strategies and equilibrium."
    ),
    team_label="Blue Team",
)

# Default instance (used by transfer function and direct imports)
blueteam_gctr_agent = make_gctr_agent(_base_agent, **_GCTR_KWARGS)


def create_blueteam_gctr_agent(n_interactions: Optional[int] = None):
    """Create a Blue Team GCTR agent (backward-compatible factory).

    Args:
        n_interactions: CTR trigger threshold. If None, uses CAI_GCTR_NITERATIONS env.
    """
    return make_gctr_agent(_base_agent, n_interactions=n_interactions, **_GCTR_KWARGS)


def transfer_to_blueteam_gctr_agent(**kwargs):
    """Transfer to blue team GCTR agent."""
    return blueteam_gctr_agent
