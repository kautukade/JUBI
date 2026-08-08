"""Tests for orchestration MAS heuristic nudge."""

from __future__ import annotations

from types import SimpleNamespace
import pytest

from cai.config import reset_config
from cai.sdk.agents import orchestration_mas_hint as omh


@pytest.fixture(autouse=True)
def _reset_cfg():
    reset_config()
    yield
    reset_config()


def test_user_message_suggests_multi_front_parallel_word() -> None:
    text = (
        "Please scan the web app and the API in parallel for obvious issues; "
        "then summarize risks."
    )
    assert omh.user_message_suggests_multi_front(text) is True


def test_user_message_suggests_multi_front_numbered_list() -> None:
    text = "Do the following:\n1. Enumerate hosts\n2. Check TLS\n3. Draft report"
    assert omh.user_message_suggests_multi_front(text) is True


def test_user_message_suggests_multi_front_short_false() -> None:
    assert omh.user_message_suggests_multi_front("run nmap on 10.0.0.1") is False


@pytest.mark.parametrize("enabled", [True, False])
def test_maybe_inject_respects_config(monkeypatch: pytest.MonkeyPatch, enabled: bool) -> None:
    monkeypatch.setenv("CAI_ORCHESTRATION_MAS_HINT", "true" if enabled else "false")
    reset_config()

    calls: list[dict] = []

    def add(msg: dict) -> None:
        calls.append(msg)

    agent = SimpleNamespace(
        model=SimpleNamespace(
            agent_type="orchestration_agent",
            add_to_message_history=add,
        )
    )
    tool = SimpleNamespace(name="run_specialist")
    fr = SimpleNamespace(tool=tool)
    rc = SimpleNamespace(trace_metadata={})

    omh.maybe_inject_orchestration_mas_hint_after_tools(
        agent=agent,
        original_input=(
            "Investigate in parallel: DNS records, HTTP headers, and TLS chain for example.com. "
            "Be thorough."
        ),
        function_results=[fr],
        run_config=rc,
    )

    if enabled:
        assert len(calls) == 1
        assert calls[0]["role"] == "user"
        assert "run_parallel_specialists" in calls[0]["content"]
        assert rc.trace_metadata.get("_cai_mas_hint_injected") is True
    else:
        assert calls == []


def test_maybe_inject_skips_non_orchestration_agent(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("CAI_ORCHESTRATION_MAS_HINT", "true")
    reset_config()

    calls: list[dict] = []

    agent = SimpleNamespace(
        model=SimpleNamespace(
            agent_type="redteam_agent",
            add_to_message_history=lambda m: calls.append(m),
        )
    )
    tool = SimpleNamespace(name="run_specialist")
    fr = SimpleNamespace(tool=tool)
    rc = SimpleNamespace(trace_metadata={})

    omh.maybe_inject_orchestration_mas_hint_after_tools(
        agent=agent,
        original_input="Do A and B in parallel for the target host.",
        function_results=[fr],
        run_config=rc,
    )
    assert calls == []


def test_maybe_inject_once_per_run_config() -> None:
    calls: list[dict] = []

    agent = SimpleNamespace(
        model=SimpleNamespace(
            agent_type="orchestration_agent",
            add_to_message_history=lambda m: calls.append(m),
        )
    )
    tool = SimpleNamespace(name="run_specialist")
    fr = SimpleNamespace(tool=tool)
    rc = SimpleNamespace(trace_metadata={"_cai_mas_hint_injected": True})

    omh.maybe_inject_orchestration_mas_hint_after_tools(
        agent=agent,
        original_input="Run three checks in parallel on the subnet.",
        function_results=[fr],
        run_config=rc,
    )
    assert calls == []


def test_maybe_inject_when_parallel_already_used() -> None:
    calls: list[dict] = []

    agent = SimpleNamespace(
        model=SimpleNamespace(
            agent_type="orchestration_agent",
            add_to_message_history=lambda m: calls.append(m),
        )
    )
    rs = SimpleNamespace(name="run_specialist")
    rp = SimpleNamespace(name="run_parallel_specialists")
    rc = SimpleNamespace(trace_metadata={})

    omh.maybe_inject_orchestration_mas_hint_after_tools(
        agent=agent,
        original_input="Do A and B in parallel for the target host.",
        function_results=[SimpleNamespace(tool=rs), SimpleNamespace(tool=rp)],
        run_config=rc,
    )
    assert calls == []
