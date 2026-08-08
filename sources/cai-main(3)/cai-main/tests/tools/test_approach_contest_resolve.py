"""Unit tests for ``_resolve_worker_tool`` / ``_contest_worker`` (F-D).

These cover the comma-separated ``allowed_tool_name`` contract that lets the
orchestrator grant a worker a small toolbox in a single delegation, avoiding
the 1-tool / 1-delegation fan-out documented in debug session ab1027.

The functions under test are pure dispatchers around ``agent.tools`` and
``agent.clone(...)``; they do not invoke the LLM, so plain ``MagicMock``
agents are sufficient.
"""

from __future__ import annotations

from unittest.mock import MagicMock

from cai.tools.misc.approach_contest import _contest_worker, _resolve_worker_tool


def _mock_tool(name: str) -> MagicMock:
    tool = MagicMock()
    tool.name = name
    return tool


def _mock_agent(*tool_names: str) -> MagicMock:
    """Return a MagicMock agent carrying tools with the given ``name``s."""
    agent = MagicMock()
    agent.name = "Mock Agent"
    agent.handoffs = [MagicMock()]
    agent.model_settings = None
    agent.tools = [_mock_tool(n) for n in tool_names]
    # ``clone(**kwargs)`` returns a new MagicMock that records the kwargs
    # so tests can introspect them.
    agent.clone.side_effect = lambda **kwargs: MagicMock(
        tools=kwargs.get("tools", []),
        handoffs=kwargs.get("handoffs", []),
        model_settings=kwargs.get("model_settings"),
    )
    return agent


# ---------------------------------------------------------------------------
# _resolve_worker_tool — single name (backward-compatible).
# ---------------------------------------------------------------------------


def test_resolve_worker_tool_single_name_returns_one_tool() -> None:
    agent = _mock_agent("fetch_url", "generic_linux_command")
    tools, error = _resolve_worker_tool(agent, "fetch_url")
    assert error is None
    assert [t.name for t in tools] == ["fetch_url"]


def test_resolve_worker_tool_single_unknown_returns_error_listing_available() -> None:
    agent = _mock_agent("fetch_url", "generic_linux_command")
    tools, error = _resolve_worker_tool(agent, "does_not_exist")
    assert tools == []
    assert error is not None
    assert "does_not_exist" in error
    assert "fetch_url" in error  # available tools are advertised
    assert "generic_linux_command" in error


# ---------------------------------------------------------------------------
# _resolve_worker_tool — comma-separated list (F-D contract).
# ---------------------------------------------------------------------------


def test_resolve_worker_tool_comma_separated_returns_each_tool_in_order() -> None:
    agent = _mock_agent("fetch_url", "generic_linux_command", "execute_code")
    tools, error = _resolve_worker_tool(
        agent, "fetch_url,generic_linux_command"
    )
    assert error is None
    assert [t.name for t in tools] == ["fetch_url", "generic_linux_command"]


def test_resolve_worker_tool_tolerates_whitespace_around_names() -> None:
    agent = _mock_agent("fetch_url", "generic_linux_command")
    tools, error = _resolve_worker_tool(
        agent, "  fetch_url ,   generic_linux_command  "
    )
    assert error is None
    assert [t.name for t in tools] == ["fetch_url", "generic_linux_command"]


def test_resolve_worker_tool_dedupes_repeated_names() -> None:
    agent = _mock_agent("fetch_url", "generic_linux_command")
    tools, error = _resolve_worker_tool(
        agent, "fetch_url,fetch_url,generic_linux_command"
    )
    assert error is None
    assert [t.name for t in tools] == ["fetch_url", "generic_linux_command"]


def test_resolve_worker_tool_one_unknown_in_list_fails_whole_request() -> None:
    """If ANY requested tool is missing, the call fails and lists missing names."""
    agent = _mock_agent("fetch_url", "generic_linux_command")
    tools, error = _resolve_worker_tool(
        agent, "fetch_url,missing_tool"
    )
    assert tools == []
    assert error is not None
    assert "missing_tool" in error
    assert "fetch_url" in error  # available tools advertised
    assert "generic_linux_command" in error


# ---------------------------------------------------------------------------
# _resolve_worker_tool — reasoning-only sentinels.
# ---------------------------------------------------------------------------


def test_resolve_worker_tool_empty_returns_empty_list() -> None:
    agent = _mock_agent("fetch_url")
    tools, error = _resolve_worker_tool(agent, "")
    assert tools == []
    assert error is None


def test_resolve_worker_tool_none_sentinel_returns_empty_list() -> None:
    agent = _mock_agent("fetch_url")
    tools, error = _resolve_worker_tool(agent, "none")
    assert tools == []
    assert error is None


def test_resolve_worker_tool_no_tool_alias_returns_empty_list() -> None:
    agent = _mock_agent("fetch_url")
    tools, error = _resolve_worker_tool(agent, "reasoning_only")
    assert tools == []
    assert error is None


# ---------------------------------------------------------------------------
# _contest_worker — parallel_tool_calls toggles with toolbox size.
# ---------------------------------------------------------------------------


def test_contest_worker_keeps_sequential_when_single_tool() -> None:
    agent = _mock_agent("fetch_url", "generic_linux_command")
    worker, error = _contest_worker(agent, "fetch_url")
    assert error is None
    assert worker is not None
    # The clone receives exactly one tool and parallel disabled.
    settings = agent.clone.call_args.kwargs["model_settings"]
    assert settings.parallel_tool_calls is False
    granted = agent.clone.call_args.kwargs["tools"]
    assert [t.name for t in granted] == ["fetch_url"]


def test_contest_worker_enables_parallel_when_multi_tool() -> None:
    agent = _mock_agent("fetch_url", "generic_linux_command", "execute_code")
    worker, error = _contest_worker(agent, "fetch_url,generic_linux_command")
    assert error is None
    assert worker is not None
    settings = agent.clone.call_args.kwargs["model_settings"]
    assert settings.parallel_tool_calls is True
    granted = agent.clone.call_args.kwargs["tools"]
    assert [t.name for t in granted] == ["fetch_url", "generic_linux_command"]
    # Handoffs must always be stripped on contest workers.
    assert agent.clone.call_args.kwargs["handoffs"] == []


def test_contest_worker_propagates_resolver_error_unchanged() -> None:
    agent = _mock_agent("fetch_url")
    worker, error = _contest_worker(agent, "missing_tool")
    assert worker is None
    assert error is not None
    assert "missing_tool" in error
