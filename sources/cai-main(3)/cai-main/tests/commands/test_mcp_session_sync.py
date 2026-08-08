"""Tests for syncing MCP tools to the REPL session agent (factory clone vs module singleton)."""

from unittest.mock import AsyncMock, Mock

import pytest

from cai.repl.commands.mcp import (
    _AGENT_MCP_ASSOCIATIONS,
    _GLOBAL_MCP_SERVERS,
    _MCP_TOOL_NAME_TO_SERVER,
    merge_mcp_tools_into_session_agent,
    register_mcp_tool_name,
    strip_mcp_server_from_session_agents,
    unregister_mcp_tools_for_server,
)
from cai.sdk.agents import Agent
from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
from cai.sdk.agents.tool import FunctionTool


@pytest.fixture(autouse=True)
def _clear_mcp_globals():
    _GLOBAL_MCP_SERVERS.clear()
    _AGENT_MCP_ASSOCIATIONS.clear()
    _MCP_TOOL_NAME_TO_SERVER.clear()
    yield
    _GLOBAL_MCP_SERVERS.clear()
    _AGENT_MCP_ASSOCIATIONS.clear()
    _MCP_TOOL_NAME_TO_SERVER.clear()


@pytest.fixture(autouse=True)
def _reset_agent_manager():
    old_ref = AGENT_MANAGER._active_agent
    old_name = AGENT_MANAGER._active_agent_name
    AGENT_MANAGER._active_agent = None
    AGENT_MANAGER._active_agent_name = None
    yield
    AGENT_MANAGER._active_agent = old_ref
    AGENT_MANAGER._active_agent_name = old_name


def _noop_tool(name: str) -> FunctionTool:
    async def _invoke(_cfg, _ctx, _inp):
        return "ok"

    return FunctionTool(
        name=name,
        description="d",
        params_json_schema={},
        on_invoke_tool=_invoke,
        strict_json_schema=False,
    )


def test_merge_mcp_tools_into_session_agent_when_types_match():
    session = Agent(name="Red Team", model=Mock(), tools=[_noop_tool("bash_tool")])
    session.model.agent_type = "redteam_agent"

    mcp_ft = _noop_tool("mcp_navigate")
    setattr(mcp_ft, "_mcp_server", "devtools")
    setattr(mcp_ft, "_is_mcp_tool", True)

    AGENT_MANAGER.set_active_agent(session, "Red Team")

    merge_mcp_tools_into_session_agent("redteam_agent", [mcp_ft])

    names = {t.name for t in session.tools}
    assert "bash_tool" in names
    assert "mcp_navigate" in names


def test_merge_mcp_tools_skips_when_session_agent_type_differs():
    session = Agent(name="Blue", model=Mock(), tools=[_noop_tool("keep")])
    session.model.agent_type = "blueteam_agent"

    AGENT_MANAGER.set_active_agent(session, "Blue")

    mcp_ft = _noop_tool("mcp_only")
    merge_mcp_tools_into_session_agent("redteam_agent", [mcp_ft])

    assert [t.name for t in session.tools] == ["keep"]


def test_strip_mcp_server_removes_tools_from_singleton_and_active(monkeypatch):
    singleton = Agent(name="Red Team", model=Mock(), tools=[])
    singleton.model.agent_type = "redteam_agent"
    keep = _noop_tool("keep")
    mcp_t = _noop_tool("navigate_page")
    setattr(mcp_t, "_mcp_server", "devtools")
    singleton.tools = [keep, mcp_t]
    register_mcp_tool_name("navigate_page", "devtools")

    session = Agent(name="Red Team", model=Mock(), tools=[])
    session.model.agent_type = "redteam_agent"
    keep2 = _noop_tool("keep2")
    click_t = _noop_tool("click")
    setattr(click_t, "_mcp_server", "devtools")
    session.tools = [keep2, click_t]

    AGENT_MANAGER.set_active_agent(session, "Red Team")

    monkeypatch.setattr(
        "cai.repl.commands.mcp.get_available_agents",
        lambda: {"redteam_agent": singleton},
    )

    strip_mcp_server_from_session_agents("devtools")
    unregister_mcp_tools_for_server("devtools")

    assert {t.name for t in singleton.tools} == {"keep"}
    assert {t.name for t in session.tools} == {"keep2"}
    assert "navigate_page" not in _MCP_TOOL_NAME_TO_SERVER
