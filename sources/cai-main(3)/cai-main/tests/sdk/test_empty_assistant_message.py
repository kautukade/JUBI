"""Empty assistant completion detection and recovery helpers."""

from __future__ import annotations

from types import SimpleNamespace
from unittest.mock import AsyncMock, MagicMock, patch

import pytest

from cai.sdk.agents.models.openai_chatcompletions import (
    EMPTY_COMPLETION_FORCE_COMPACT_CONTEXT_FRACTION,
    EMPTY_COMPLETION_FORCE_COMPACT_FLOOR,
    EMPTY_COMPLETION_FORCE_COMPACT_FRACTION,
    OpenAIChatCompletionsModel,
    _StreamingState,
    _assistant_reasoning_text,
    _empty_completion_force_compact_threshold,
    _empty_completion_max_failures,
    _is_effectively_empty_assistant_message,
    _is_effectively_empty_stream_accumulation,
    _should_force_compact_on_empty_streak,
)


def test_empty_when_content_none_and_no_tools():
    m = SimpleNamespace(content=None, tool_calls=None, refusal=None)
    assert _is_effectively_empty_assistant_message(m) is True


def test_not_empty_when_tool_calls():
    m = SimpleNamespace(content=None, tool_calls=[object()], refusal=None)
    assert _is_effectively_empty_assistant_message(m) is False


def test_not_empty_when_text():
    m = SimpleNamespace(content="hello", tool_calls=None, refusal=None)
    assert _is_effectively_empty_assistant_message(m) is False


def test_empty_when_whitespace_only():
    m = SimpleNamespace(content="   \n", tool_calls=None, refusal=None)
    assert _is_effectively_empty_assistant_message(m) is True


def test_not_empty_when_refusal():
    m = SimpleNamespace(content=None, tool_calls=None, refusal="no")
    assert _is_effectively_empty_assistant_message(m) is False


def test_not_empty_when_reasoning_content_only():
    m = SimpleNamespace(
        content=None,
        tool_calls=None,
        refusal=None,
        reasoning_content="Plan: scan host then enumerate ports.",
    )
    assert _is_effectively_empty_assistant_message(m) is False
    assert _assistant_reasoning_text(m) == "Plan: scan host then enumerate ports."


def test_empty_when_no_reasoning_no_content_no_tools():
    m = SimpleNamespace(content=None, tool_calls=None, refusal=None, reasoning_content=None)
    assert _is_effectively_empty_assistant_message(m) is True


def test_force_compact_threshold_scales_with_model_max():
    alias_threshold = _empty_completion_force_compact_threshold(150_000)
    assert alias_threshold == int(150_000 * EMPTY_COMPLETION_FORCE_COMPACT_FRACTION)
    assert alias_threshold >= EMPTY_COMPLETION_FORCE_COMPACT_FLOOR
    small_model = _empty_completion_force_compact_threshold(8_000)
    assert small_model == EMPTY_COMPLETION_FORCE_COMPACT_FLOOR


def test_should_force_compact_only_from_second_streak():
    model_max = 150_000
    tokens = _empty_completion_force_compact_threshold(model_max) + 1
    assert _should_force_compact_on_empty_streak(1, tokens, model_max) is False
    assert _should_force_compact_on_empty_streak(2, tokens, model_max) is True
    assert _should_force_compact_on_empty_streak(2, 100, model_max) is False


def test_stream_accumulation_not_empty_with_tool_calls():
    state = _StreamingState(
        function_calls={
            0: MagicMock(name="scan", arguments="{}", call_id="call_1"),
        }
    )
    assert (
        _is_effectively_empty_stream_accumulation(state, [], "", "") is False
    )


def test_stream_accumulation_not_empty_with_reasoning_only():
    state = _StreamingState()
    assert (
        _is_effectively_empty_stream_accumulation(
            state,
            [],
            "",
            "thinking step one",
        )
        is False
    )


@pytest.mark.asyncio
async def test_recover_after_empty_completion_forces_compact(monkeypatch):
    model = OpenAIChatCompletionsModel.__new__(OpenAIChatCompletionsModel)
    model.model = "alias1"
    model.logger = MagicMock()
    compact_estimates: list[int] = []

    async def fake_compact(est, inp, sys):
        compact_estimates.append(est)
        return inp, sys, True

    model._auto_compact_if_needed = fake_compact  # type: ignore[method-assign]
    model._get_model_max_tokens = lambda _m: 150_000  # type: ignore[method-assign]
    model._messages_for_token_count_after_history_mutation = (  # type: ignore[method-assign]
        lambda **_kw: []
    )
    model._retry_with_backoff = AsyncMock()  # type: ignore[method-assign]

    with patch(
        "cai.sdk.agents.models.openai_chatcompletions.count_tokens_with_tiktoken",
        return_value=(12_000, 0),
    ):
        _inp, _sys, new_est = await model._recover_after_empty_completion(
            empty_streak=2,
            estimated_input_tokens=40_000,
            input=[],
            system_instructions=None,
        )

    assert compact_estimates
    assert compact_estimates[0] == max(
        40_000,
        int(150_000 * EMPTY_COMPLETION_FORCE_COMPACT_CONTEXT_FRACTION),
    )
    assert new_est == 12_000
    model._retry_with_backoff.assert_awaited_once()


@pytest.mark.asyncio
async def test_recover_after_empty_completion_logs_compact_failure(monkeypatch):
    model = OpenAIChatCompletionsModel.__new__(OpenAIChatCompletionsModel)
    model.model = "alias1"
    model.logger = MagicMock()

    async def fail_compact(*_a, **_k):
        raise RuntimeError("compact boom")

    model._auto_compact_if_needed = fail_compact  # type: ignore[method-assign]
    model._get_model_max_tokens = lambda _m: 150_000  # type: ignore[method-assign]
    model._retry_with_backoff = AsyncMock()  # type: ignore[method-assign]

    await model._recover_after_empty_completion(
        empty_streak=2,
        estimated_input_tokens=50_000,
        input="hi",
        system_instructions=None,
    )

    model.logger.warning.assert_called()
    assert "Forced compaction after empty completion failed" in str(
        model.logger.warning.call_args
    )


def test_empty_completion_max_failures_clamped():
    import os

    old = os.environ.get("CAI_EMPTY_COMPLETION_MAX_FAILURES")
    try:
        os.environ["CAI_EMPTY_COMPLETION_MAX_FAILURES"] = "99"
        assert _empty_completion_max_failures() == 12
        os.environ["CAI_EMPTY_COMPLETION_MAX_FAILURES"] = "0"
        assert _empty_completion_max_failures() == 1
    finally:
        if old is None:
            os.environ.pop("CAI_EMPTY_COMPLETION_MAX_FAILURES", None)
        else:
            os.environ["CAI_EMPTY_COMPLETION_MAX_FAILURES"] = old
