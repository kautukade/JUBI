"""Bug Bounty Hunter with Game-theoretic CTR (Cut The Rope) Integration.

Thin wrapper: clones the base bug_bounter agent and attaches CTRHooks.
"""

from typing import Optional

from cai.agents.gctr_mixin import make_gctr_agent
from cai.agents.bug_bounter import bug_bounter_agent as _base_agent

_GCTR_KWARGS = dict(
    name="Bug Bounter GCTR",
    description=(
        "Bug bounty hunter with integrated game-theoretic security analysis. "
        "Automatically runs CTR (Cut The Rope) analysis every few interactions "
        "to assess defender/attacker strategies and equilibrium."
    ),
    team_label="Bug Bounty",
)

# Default instance
bug_bounter_gctr_agent = make_gctr_agent(_base_agent, **_GCTR_KWARGS)


def create_bug_bounter_gctr_agent(n_interactions: Optional[int] = None):
    """Create a Bug Bounter GCTR agent (backward-compatible factory)."""
    return make_gctr_agent(_base_agent, n_interactions=n_interactions, **_GCTR_KWARGS)


def transfer_to_bug_bounter_gctr_agent(**kwargs):
    """Transfer to bug bounter GCTR agent."""
    return bug_bounter_gctr_agent
