"""
Import-time smoke tests for the default selection agent (heavier dependency graph).

Marked `slow` so fast slices can use `pytest -m "not slow"`.
"""

from __future__ import annotations

import pytest

pytestmark = pytest.mark.slow


def test_selection_agent_handoffs_exist_and_use_tool_strip_filter() -> None:
    from cai.agents.selection_agent import selection_agent
    from cai.sdk.agents.extensions.handoff_filters import remove_all_tools

    assert selection_agent.name
    assert len(selection_agent.handoffs) >= 1
    for ho in selection_agent.handoffs:
        assert getattr(ho, "input_filter", None) is remove_all_tools
        assert getattr(ho, "tool_name", None)
        desc = getattr(ho, "tool_description", "") or ""
        assert len(desc) > 20
