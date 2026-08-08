"""Purple Team Agent with Game-theoretic CTR (Cut The Rope) Integration.

Creates coordinated red + blue team agents that share a single
SharedCTRHooks instance so combined tool usage triggers CTR analysis.
"""

import os

from cai.agents.gctr_mixin import SharedCTRHooks
from cai.sdk.agents import OpenAIChatCompletionsModel
from openai import AsyncOpenAI


def create_purple_team_agents(
    n_interactions: int = None,
    use_base_agents: bool = True,
):
    """Create coordinated red and blue team agents with shared CTR tracking.

    Args:
        n_interactions: Combined tool interactions before CTR triggers.
                       Defaults to CAI_GCTR_NITERATIONS env var (or 5).
        use_base_agents: If True, clones red_teamer / blue_teamer.
                        If False, clones their GCTR variants (whose
                        individual hooks will be replaced).

    Returns:
        Tuple of (red_agent, blue_agent) sharing a single CTR tracker.
    """
    if n_interactions is None:
        n_interactions = int(os.getenv("CAI_GCTR_NITERATIONS", "5"))

    shared_hooks = SharedCTRHooks(
        n_interactions=n_interactions,
        team_name="Combined",
    )

    if use_base_agents:
        from cai.agents.red_teamer import redteam_agent as base_red
        from cai.agents.blue_teamer import blueteam_agent as base_blue
    else:
        from cai.agents.red_teamer_gctr import redteam_gctr_agent as base_red
        from cai.agents.blue_teamer_gctr import blueteam_gctr_agent as base_blue

    model_name = os.getenv("CAI_MODEL", "alias1")

    red_agent = base_red.clone(
        name="Purple Team - Red",
        hooks=shared_hooks,
        model=OpenAIChatCompletionsModel(
            model=model_name,
            openai_client=AsyncOpenAI(),
        ),
    )
    blue_agent = base_blue.clone(
        name="Purple Team - Blue",
        hooks=shared_hooks,
        model=OpenAIChatCompletionsModel(
            model=model_name,
            openai_client=AsyncOpenAI(),
        ),
    )
    return red_agent, blue_agent


def create_purple_team_gctr_pattern():
    """Create a purple team pattern for parallel execution."""
    from cai.repl.commands.parallel import ParallelConfig

    return {
        "name": "purple_team_gctr",
        "type": "parallel",
        "description": "Purple team (red + blue) with shared CTR tracking and unified context",
        "configs": [
            ParallelConfig("redteam_gctr_agent", unified_context=True),
            ParallelConfig("blueteam_gctr_agent", unified_context=True),
        ],
        "unified_context": True,
        "shared_ctr": True,
    }


# Default instances
purple_redteam_agent, purple_blueteam_agent = create_purple_team_agents(
    use_base_agents=True,
)
purple_team_gctr_pattern = create_purple_team_gctr_pattern()


def transfer_to_purple_redteam_agent(**kwargs):
    """Transfer to purple team red agent."""
    return purple_redteam_agent


def transfer_to_purple_blueteam_agent(**kwargs):
    """Transfer to purple team blue agent."""
    return purple_blueteam_agent
