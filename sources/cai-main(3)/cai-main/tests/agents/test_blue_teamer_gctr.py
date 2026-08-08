"""Tests for Blue Team GCTR Agent"""

import pytest
from cai.sdk.agents import Runner
from cai.agents import get_agent_by_name
from cai.agents.blue_teamer_gctr import create_blueteam_gctr_agent, blueteam_gctr_agent


def test_blueteam_gctr_agent_exists():
    """Test that the blue team GCTR agent is registered."""
    agent = get_agent_by_name("blueteam_gctr_agent")
    assert agent is not None
    assert agent.name == "Blue Team GCTR"
    assert "game-theoretic" in agent.description.lower()


def test_blueteam_gctr_agent_has_hooks():
    """Test that the blue team GCTR agent has CTR hooks configured."""
    agent = blueteam_gctr_agent
    assert agent.hooks is not None
    assert hasattr(agent.hooks, 'n_interactions')
    assert hasattr(agent.hooks, 'interaction_counter')
    assert hasattr(agent.hooks, '_run_ctr_background')


def test_blueteam_gctr_agent_has_tools():
    """Test that the blue team GCTR agent has required tools."""
    agent = blueteam_gctr_agent
    assert agent.tools is not None
    assert len(agent.tools) >= 3  # Should have at least: generic_linux_command, ssh, execute_code

    tool_names = [tool.name for tool in agent.tools]
    assert 'generic_linux_command' in tool_names
    assert 'run_ssh_command_with_credentials' in tool_names
    assert 'execute_code' in tool_names


def test_create_blueteam_gctr_agent_custom_iterations():
    """Test creating a blue team GCTR agent with custom iteration threshold."""
    agent = create_blueteam_gctr_agent(n_interactions=10)
    assert agent.hooks.n_interactions == 10


@pytest.mark.allow_call_model_methods
@pytest.mark.asyncio
@pytest.mark.skip(reason="Async test requires pytest-asyncio plugin configuration")
async def test_blue_team_gctr_agent_inference():
    """
    Non-streaming inference test for the blueteam_gctr_agent.
    """
    prompt = "What are your capabilities as a blue team agent?"
    result = await Runner.run(get_agent_by_name("blueteam_gctr_agent"), prompt)
    final_output = result.final_output or ""
    assert final_output, "Expected non-empty final output"
    assert any(keyword in final_output.lower() for keyword in ["blue team", "defense", "security", "protect"]), \
        f"Expected security-related keywords in output, got: {final_output}"
