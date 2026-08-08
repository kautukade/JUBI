"""
Purple Team GCTR Pattern - Red and Blue teams with shared CTR tracking.

This pattern runs red and blue team agents in parallel with:
- Unified context (shared message history)
- Combined tool usage tracking across both teams
- Shared CTR analysis triggered every CAI_GCTR_NITERATIONS combined tool uses
- CTR digest injected into both agents' system prompts using CAI_CTR_DIGEST_MODE
"""

from cai.repl.commands.parallel import ParallelConfig

# Note: This pattern uses the standard red and blue team GCTR agents
# For true purple team coordination with shared tool counting, you need to
# use a custom implementation that shares CTRHooks across both agents.
#
# The agents defined in purple_teamer_gctr.py (redteam_agent and blueteam_agent)
# use a SharedCTRHooks instance to track combined tool usage across both teams.

# Pattern configuration for purple team with shared GCTR
purple_team_gctr_pattern = {
    "name": "purple_team_gctr",
    "type": "parallel",
    "description": "Purple team (red + blue) with shared GCTR tracking - combines red and blue team activity for unified game-theoretic analysis",
    "configs": [
        # Using the purple team variants from purple_teamer_gctr.py
        # These agents share a common CTRHooks instance for combined tracking
        ParallelConfig("purple_redteam_agent", unified_context=True),
        ParallelConfig("purple_blueteam_agent", unified_context=True),
    ],
    "unified_context": True,
}
