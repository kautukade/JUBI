"""Unit tests for ``run_dual_approach_contest`` (mocked ``Runner.run``)."""

from __future__ import annotations

import json
from unittest.mock import MagicMock, patch

import pytest

from cai.config import reset_config


def _mock_agent() -> MagicMock:
    agent = MagicMock()
    agent.name = "Mock Agent"
    agent.handoffs = [MagicMock()]
    agent.model_settings = None
    agent.tools = [MagicMock(name="tool")]
    agent.tools[0].name = "allowed_tool"
    agent.clone.side_effect = lambda **kwargs: MagicMock(
        name=kwargs.get("name", "cloned"),
        tools=kwargs.get("tools", []),
        handoffs=kwargs.get("handoffs", []),
        model_settings=kwargs.get("model_settings"),
    )
    return agent


@pytest.fixture(autouse=True)
def _reset_cfg(monkeypatch: pytest.MonkeyPatch):
    """Keep worker ``max_turns`` at 2 so ``Runner.run`` mocks stay strict and fast."""
    monkeypatch.setenv("CAI_ORCHESTRATION_WORKER_MAX_TURNS", "2")
    reset_config()
    yield
    reset_config()


@pytest.mark.asyncio
async def test_contest_auto_continues_when_one_branch_runner_raises() -> None:
    from cai.sdk.agents.items import ItemHelpers
    from cai.tools.misc import approach_contest

    tool = approach_contest.run_dual_approach_contest

    async def fake_run(agent, user_input, max_turns=2, run_config=None):
        assert max_turns == 2
        assert agent.handoffs == []
        assert len(agent.tools) == 1
        assert agent.tools[0].name == "allowed_tool"
        assert agent.model_settings.parallel_tool_calls is False
        if "## Approach framing (A)" in user_input:
            raise RuntimeError("boom")
        m = MagicMock()
        m.new_items = []
        return m

    mock_agent = _mock_agent()
    with patch("cai.agents.get_agent_by_name", return_value=mock_agent):
        with patch("cai.sdk.agents.run.Runner.run", side_effect=fake_run):
            with patch.object(ItemHelpers, "text_message_outputs", return_value="branch b ok"):
                ctx = MagicMock()
                payload = json.dumps(
                    {
                        "agent_type_for_approach_a": "redteam_agent",
                        "agent_type_for_approach_b": "blueteam_agent",
                        "allowed_tool_for_approach_a": "allowed_tool",
                        "allowed_tool_for_approach_b": "allowed_tool",
                        "approach_a_framing": "hypothesis 1",
                        "approach_b_framing": "hypothesis 2",
                        "shared_user_task": "investigate X",
                        "contest_rationale": "ambiguous evidence",
                    }
                )
                out = await tool.on_invoke_tool(ctx, payload)

    assert "hitl" not in out.lower()
    assert "branch b ok" in out


@pytest.mark.asyncio
async def test_contest_auto_includes_both_sections() -> None:
    from cai.sdk.agents.items import ItemHelpers
    from cai.tools.misc import approach_contest

    tool = approach_contest.run_dual_approach_contest

    async def fake_run(agent, user_input, max_turns=2, run_config=None):
        if "## Approach framing (A)" in user_input:
            raise RuntimeError("boom")
        m = MagicMock()
        m.new_items = []
        return m

    mock_agent = _mock_agent()
    with patch("cai.agents.get_agent_by_name", return_value=mock_agent):
        with patch("cai.sdk.agents.run.Runner.run", side_effect=fake_run):
            with patch.object(ItemHelpers, "text_message_outputs", return_value="branch b ok"):
                ctx = MagicMock()
                payload = json.dumps(
                    {
                        "agent_type_for_approach_a": "redteam_agent",
                        "agent_type_for_approach_b": "redteam_agent",
                        "allowed_tool_for_approach_a": "allowed_tool",
                        "allowed_tool_for_approach_b": "allowed_tool",
                        "approach_a_framing": "x",
                        "approach_b_framing": "y",
                        "shared_user_task": "task",
                        "contest_rationale": "test",
                    }
                )
                out = await tool.on_invoke_tool(ctx, payload)

    assert "dual-approach contest" in out.lower()
    assert "branch b ok" in out
    assert "hitl" not in out.lower()


@pytest.mark.asyncio
async def test_contest_both_branches_invalid_agent() -> None:
    from cai.tools.misc import approach_contest

    tool = approach_contest.run_dual_approach_contest

    def boom(_a, **_k):
        raise ValueError("nope")

    with patch("cai.agents.get_agent_by_name", side_effect=boom):
        ctx = MagicMock()
        payload = json.dumps(
            {
                "agent_type_for_approach_a": "not_a_real_agent_type_ever",
                "agent_type_for_approach_b": "also_invalid",
                "allowed_tool_for_approach_a": "allowed_tool",
                "allowed_tool_for_approach_b": "allowed_tool",
                "approach_a_framing": "x",
                "approach_b_framing": "y",
                "shared_user_task": "t",
                "contest_rationale": "r",
            }
        )
        out = await tool.on_invoke_tool(ctx, payload)

    assert "both branches failed" in out.lower() or "invalid" in out.lower()


@pytest.mark.asyncio
async def test_run_specialist_filters_to_selected_tool() -> None:
    from cai.sdk.agents.items import ItemHelpers
    from cai.tools.misc import approach_contest

    tool = approach_contest.run_specialist

    async def fake_run(agent, user_input, max_turns=2, run_config=None):
        assert max_turns == 2
        assert agent.handoffs == []
        assert len(agent.tools) == 1
        assert agent.tools[0].name == "allowed_tool"
        assert "## Allowed worker tool\nallowed_tool" in user_input
        m = MagicMock()
        m.new_items = []
        return m

    with patch("cai.agents.get_agent_by_name", return_value=_mock_agent()):
        with patch("cai.sdk.agents.run.Runner.run", side_effect=fake_run):
            with patch.object(ItemHelpers, "text_message_outputs", return_value="specialist ok"):
                ctx = MagicMock()
                payload = json.dumps(
                    {
                        "agent_type": "redteam_agent",
                        "allowed_tool_name": "allowed_tool",
                        "task": "do it",
                        "framing": "carefully",
                    }
                )
                out = await tool.on_invoke_tool(ctx, payload)

    # The specialist tool wraps the worker output in an internal-only frame so
    # the orchestrator does not echo it back to the user verbatim.
    assert "specialist ok" in out
    assert "<orchestrator_internal>" in out
    assert "</orchestrator_internal>" in out


@pytest.mark.asyncio
async def test_run_specialist_silences_worker_display_during_runner() -> None:
    """While the worker ``Runner.run`` is executing, the silence flag must be on.

    This is what keeps the worker's final markdown panel and Rich streaming
    panels out of the user-facing transcript — only the orchestrator's
    synthesis is supposed to reach the user.
    """
    from cai.util._worker_silence import worker_display_silenced
    from cai.sdk.agents.items import ItemHelpers
    from cai.tools.misc import approach_contest

    captured: list[bool] = []

    async def fake_run(agent, user_input, max_turns=2, run_config=None):
        captured.append(worker_display_silenced())
        m = MagicMock()
        m.new_items = []
        return m

    tool = approach_contest.run_specialist
    assert worker_display_silenced() is False
    with patch("cai.agents.get_agent_by_name", return_value=_mock_agent()):
        with patch("cai.sdk.agents.run.Runner.run", side_effect=fake_run):
            with patch.object(ItemHelpers, "text_message_outputs", return_value="ok"):
                ctx = MagicMock()
                payload = json.dumps(
                    {
                        "agent_type": "redteam_agent",
                        "allowed_tool_name": "allowed_tool",
                        "task": "do it",
                        "framing": "carefully",
                    }
                )
                await tool.on_invoke_tool(ctx, payload)

    assert captured == [True], "silence_worker_display must wrap the Runner.run call"
    # Must restore silence to False after the tool returns.
    assert worker_display_silenced() is False


@pytest.mark.asyncio
async def test_worker_truncates_long_output() -> None:
    """A worker brief exceeding the per-worker cap is shortened with a marker."""
    from cai.sdk.agents.items import ItemHelpers
    from cai.tools.misc import approach_contest

    big_text = "X" * (approach_contest._MAX_WORKER_OUTPUT_CHARS * 3)

    async def fake_run(agent, user_input, max_turns=2, run_config=None):
        m = MagicMock()
        m.new_items = []
        return m

    tool = approach_contest.run_specialist
    with patch("cai.agents.get_agent_by_name", return_value=_mock_agent()):
        with patch("cai.sdk.agents.run.Runner.run", side_effect=fake_run):
            with patch.object(ItemHelpers, "text_message_outputs", return_value=big_text):
                ctx = MagicMock()
                payload = json.dumps(
                    {
                        "agent_type": "redteam_agent",
                        "allowed_tool_name": "allowed_tool",
                        "task": "long task",
                        "framing": "verbose",
                    }
                )
                out = await tool.on_invoke_tool(ctx, payload)

    assert "[truncated by orchestrator:" in out
    # Wrapper still in place after truncation.
    assert "<orchestrator_internal>" in out
    # Output is shorter than the raw worker output (truncation effective).
    assert len(out) < len(big_text)


# === New tests for the post-refactor surface =============================


def test_worker_result_failed_and_status_helpers() -> None:
    """``WorkerResult`` exposes typed ``failed``/``status`` instead of stringly-typed errors."""
    from cai.tools.misc.approach_contest import WorkerResult

    ok = WorkerResult(label="A", agent_name="x", allowed_tool_name="t", output="ok")
    bad = WorkerResult(
        label="B", agent_name="x", allowed_tool_name="t", output="boom", error="boom"
    )

    assert ok.failed is False
    assert ok.status == "completed"
    assert bad.failed is True
    assert bad.status == "failed"


def test_build_worker_input_contains_required_headings() -> None:
    """``_build_worker_input`` produces stable headings the orchestrator/tests rely on."""
    from cai.tools.misc.approach_contest import WorkerSpec, _build_worker_input

    spec = WorkerSpec(
        label="A",
        agent_type="redteam_agent",
        framing="hypothesis 1",
        user_task="investigate X",
        rationale="ambiguous evidence",
        allowed_tool_name="my_tool",
    )
    text = _build_worker_input(spec)
    for needle in (
        "## Contest constraints (mandatory)",
        "## Exploration discipline (mandatory)",
        "## Approach framing (A)",
        "## Shared user task",
        "## Allowed worker tool\nmy_tool",
        "## Contest rationale (from orchestrator)",
    ):
        assert needle in text, needle


def test_new_group_id_is_unique_and_prefixed() -> None:
    """Two consecutive calls must not collide; the kind prefix is preserved."""
    from cai.tools.misc.approach_contest import _new_group_id

    a = _new_group_id("contest")
    b = _new_group_id("contest")
    c = _new_group_id("specialist")

    assert a != b
    assert a.startswith("contest:") and c.startswith("specialist:")


@pytest.mark.asyncio
async def test_invalid_agent_type_error_lists_available(monkeypatch: pytest.MonkeyPatch) -> None:
    """When ``get_agent_by_name`` raises, the error string lists known factory keys."""
    from cai.tools.misc import approach_contest

    def boom(_name, **_kw):
        raise ValueError("nope")

    fake_catalogue = {"redteam_agent": MagicMock(), "blueteam_agent": MagicMock()}

    with patch("cai.agents.get_agent_by_name", side_effect=boom):
        with patch("cai.agents.get_available_agents", return_value=fake_catalogue):
            tool = approach_contest.run_specialist
            ctx = MagicMock()
            payload = json.dumps(
                {
                    "agent_type": "ghost_agent",
                    "allowed_tool_name": "allowed_tool",
                    "task": "anything",
                    "framing": "any",
                }
            )
            out = await tool.on_invoke_tool(ctx, payload)

    assert "Invalid agent_type `ghost_agent`" in out
    assert "blueteam_agent" in out and "redteam_agent" in out


@pytest.mark.asyncio
async def test_run_specialist_uses_unified_brief_format() -> None:
    """``run_specialist`` shares the contest brief shape (title, status, rationale, decision)."""
    from cai.sdk.agents.items import ItemHelpers
    from cai.tools.misc import approach_contest

    async def fake_run(agent, user_input, max_turns=2, run_config=None):
        m = MagicMock()
        m.new_items = []
        return m

    tool = approach_contest.run_specialist
    with patch("cai.agents.get_agent_by_name", return_value=_mock_agent()):
        with patch("cai.sdk.agents.run.Runner.run", side_effect=fake_run):
            with patch.object(ItemHelpers, "text_message_outputs", return_value="specialist ok"):
                ctx = MagicMock()
                payload = json.dumps(
                    {
                        "agent_type": "redteam_agent",
                        "allowed_tool_name": "allowed_tool",
                        "task": "do it",
                        "framing": "carefully",
                    }
                )
                out = await tool.on_invoke_tool(ctx, payload)

    for needle in (
        "## Specialist Brief",
        "- Overall status: `completed`",
        "- Rationale:",
        "### Approach S",
        "- Status: `completed`",
        "### Next Decision",
        "specialist ok",
    ):
        assert needle in out, needle

@pytest.mark.asyncio
async def test_run_parallel_specialists_invokes_two_workers() -> None:
    from cai.sdk.agents.items import ItemHelpers
    from cai.tools.misc import approach_contest

    tool = approach_contest.run_parallel_specialists
    calls: list[int] = []

    async def fake_run(agent, user_input, max_turns=2, run_config=None):
        calls.append(max_turns)
        assert max_turns == 2
        m = MagicMock()
        m.new_items = []
        return m

    mock_agent = _mock_agent()
    workers = [
        {
            "agent_type": "redteam_agent",
            "allowed_tool_name": "allowed_tool",
            "task": "task a",
            "framing": "fa",
        },
        {
            "agent_type": "blueteam_agent",
            "allowed_tool_name": "allowed_tool",
            "task": "task b",
            "framing": "fb",
        },
    ]
    with patch("cai.agents.get_agent_by_name", return_value=mock_agent):
        with patch("cai.sdk.agents.run.Runner.run", side_effect=fake_run):
            with patch.object(ItemHelpers, "text_message_outputs", return_value="ok"):
                ctx = MagicMock()
                payload = json.dumps(
                    {"workers_json": json.dumps(workers), "parallel_rationale": "orthogonal fronts"}
                )
                out = await tool.on_invoke_tool(ctx, payload)

    assert len(calls) == 2
    assert "parallel specialists" in out.lower()
    assert "<orchestrator_internal>" in out


def test_per_worker_output_cap_scales_down_for_multi_branch() -> None:
    from cai.tools.misc import approach_contest as ac

    # Single branch: the worker gets the full per-worker cap (no shared budget).
    assert ac._per_worker_output_cap(1) == ac._MAX_WORKER_OUTPUT_CHARS
    # Once branches saturate the combined budget, each worker's slice
    # drops strictly below the single-branch cap. That happens around n>=3
    # with the current 8500/4000 ratio; n=2 may still fit two full slices.
    assert ac._per_worker_output_cap(4) < ac._MAX_WORKER_OUTPUT_CHARS
    # Monotonic non-increasing as branches grow.
    assert ac._per_worker_output_cap(4) <= ac._per_worker_output_cap(2)
    assert ac._per_worker_output_cap(2) <= ac._per_worker_output_cap(1)


@pytest.mark.asyncio
async def test_run_parallel_specialists_strips_workers_json_whitespace() -> None:
    from cai.sdk.agents.items import ItemHelpers
    from cai.tools.misc import approach_contest

    tool = approach_contest.run_parallel_specialists

    async def fake_run(agent, user_input, max_turns=2, run_config=None):
        m = MagicMock()
        m.new_items = []
        return m

    mock_agent = _mock_agent()
    workers = [
        {"agent_type": "redteam_agent", "allowed_tool_name": "allowed_tool", "task": "a", "framing": "x"},
        {"agent_type": "redteam_agent", "allowed_tool_name": "allowed_tool", "task": "b", "framing": "y"},
    ]
    inner = json.dumps(workers)
    padded = f"\n  {inner}  \n"
    with patch("cai.agents.get_agent_by_name", return_value=mock_agent):
        with patch("cai.sdk.agents.run.Runner.run", side_effect=fake_run):
            with patch.object(ItemHelpers, "text_message_outputs", return_value="ok"):
                ctx = MagicMock()
                payload = json.dumps({"workers_json": padded, "parallel_rationale": "r"})
                out = await tool.on_invoke_tool(ctx, payload)

    assert "parallel specialists" in out.lower()


@pytest.mark.asyncio
async def test_run_parallel_specialists_rejects_single_worker() -> None:
    from cai.tools.misc import approach_contest

    tool = approach_contest.run_parallel_specialists
    ctx = MagicMock()
    one = [{"agent_type": "redteam_agent", "allowed_tool_name": "none", "task": "t", "framing": "f"}]
    payload = json.dumps({"workers_json": json.dumps(one), "parallel_rationale": "r"})
    out = await tool.on_invoke_tool(ctx, payload)
    assert "at least 2" in out.lower()

