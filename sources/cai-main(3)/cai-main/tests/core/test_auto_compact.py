"""Test automatic context compaction when limit is reached."""

import os
from unittest.mock import AsyncMock, MagicMock, patch
import pytest

from cai.sdk.agents.models.openai_chatcompletions import OpenAIChatCompletionsModel

# Common patch target prefix for the extracted auto_compactor module
_AC = "cai.sdk.agents.models.chatcompletions.auto_compactor"


def _make_cfg(auto_compact=True, threshold=0.8, debug=False):
    """Create a mock CAIConfig for auto-compaction tests."""
    cfg = MagicMock()
    cfg.auto_compact = auto_compact
    cfg.auto_compact_threshold = threshold
    cfg.debug = debug
    return cfg


class TestAutoCompact:
    """Test automatic context compaction functionality."""

    @pytest.mark.asyncio
    async def test_auto_compact_triggers_at_threshold(self):
        """Test that auto-compact triggers when context exceeds threshold."""
        from openai import AsyncOpenAI

        client = AsyncMock(spec=AsyncOpenAI)

        with patch("cai.sdk.agents.models.openai_chatcompletions.get_session_recorder"):
            model = OpenAIChatCompletionsModel(
                model="gpt-4",
                openai_client=client,
                agent_name="Test Agent",
                agent_id="TEST123",
            )

        # Patch auto_compactor internals
        with (
            patch(f"{_AC}.get_config", return_value=_make_cfg(True, 0.8)),
            patch(f"{_AC}.get_model_max_tokens", return_value=1000),
            patch("cai.repl.commands.memory.MEMORY_COMMAND_INSTANCE") as mock_memory,
            patch("cai.repl.commands.memory.COMPACTED_SUMMARIES", {}),
            patch("rich.console.Console"),
        ):
            mock_memory._ai_summarize_history = AsyncMock(return_value="Summary")

            # Call the auto-compact method directly — 850 > 1000*0.8 = 800
            new_input, new_instructions, compacted = await model._auto_compact_if_needed(
                estimated_tokens=850, input="Test message", system_instructions=None
            )

            assert compacted is True
            mock_memory._ai_summarize_history.assert_called_once_with("Test Agent")

    @pytest.mark.asyncio
    @pytest.mark.asyncio
    async def test_auto_compact_skips_internal_summary_agent_only(self):
        """Internal Phase-2 agent must not auto-compact; name is exact 'Summary Agent'."""
        from openai import AsyncOpenAI

        client = AsyncMock(spec=AsyncOpenAI)
        with patch("cai.sdk.agents.models.openai_chatcompletions.get_session_recorder"):
            model = OpenAIChatCompletionsModel(
                model="gpt-4",
                openai_client=client,
                agent_name="Summary Agent",
                agent_id="SUM",
            )
        with (
            patch(f"{_AC}.get_config", return_value=_make_cfg(True, 0.8)),
            patch(f"{_AC}.get_model_max_tokens", return_value=1000),
        ):
            _, _, compacted = await model._auto_compact_if_needed(
                estimated_tokens=850,
                input="Test",
                system_instructions=None,
            )
        assert compacted is False

    @pytest.mark.asyncio
    async def test_auto_compact_disabled(self):
        """Test that auto-compact doesn't trigger when disabled."""
        from openai import AsyncOpenAI

        client = AsyncMock(spec=AsyncOpenAI)

        with patch("cai.sdk.agents.models.openai_chatcompletions.get_session_recorder"):
            model = OpenAIChatCompletionsModel(
                model="gpt-4", openai_client=client, agent_name="Test Agent", agent_id="TEST123"
            )

        with patch(f"{_AC}.get_config", return_value=_make_cfg(False)):
            new_input, new_instructions, compacted = await model._auto_compact_if_needed(
                estimated_tokens=900, input="Test", system_instructions=None
            )

            assert compacted is False
            assert new_input == "Test"
            assert new_instructions is None

    @pytest.mark.asyncio
    async def test_auto_compact_below_threshold(self):
        """Test that auto-compact doesn't trigger below threshold."""
        from openai import AsyncOpenAI

        client = AsyncMock(spec=AsyncOpenAI)

        with patch("cai.sdk.agents.models.openai_chatcompletions.get_session_recorder"):
            model = OpenAIChatCompletionsModel(
                model="gpt-4", openai_client=client, agent_name="Test Agent", agent_id="TEST123"
            )

        with (
            patch(f"{_AC}.get_config", return_value=_make_cfg(True, 0.8)),
            patch(f"{_AC}.get_model_max_tokens", return_value=1000),
        ):
            new_input, new_instructions, compacted = await model._auto_compact_if_needed(
                estimated_tokens=700, input="Test", system_instructions=None  # 70% < 80%
            )

            assert compacted is False

    @pytest.mark.asyncio
    async def test_auto_compact_with_custom_threshold(self):
        """Test auto-compact with custom threshold value."""
        from openai import AsyncOpenAI

        client = AsyncMock(spec=AsyncOpenAI)

        with patch("cai.sdk.agents.models.openai_chatcompletions.get_session_recorder"):
            model = OpenAIChatCompletionsModel(
                model="gpt-4", openai_client=client, agent_name="Test Agent", agent_id="TEST123"
            )

        # 50% threshold — 600 > 1000*0.5 = 500 → should compact
        with (
            patch(f"{_AC}.get_config", return_value=_make_cfg(True, 0.5)),
            patch(f"{_AC}.get_model_max_tokens", return_value=1000),
            patch("cai.repl.commands.memory.MEMORY_COMMAND_INSTANCE") as mock_memory,
            patch("cai.repl.commands.memory.COMPACTED_SUMMARIES", {}),
            patch("rich.console.Console"),
        ):
            mock_memory._ai_summarize_history = AsyncMock(return_value="Summary")

            new_input, new_instructions, compacted = await model._auto_compact_if_needed(
                estimated_tokens=600, input="Test", system_instructions=None
            )

            assert compacted is True
            mock_memory._ai_summarize_history.assert_called_once()

    @pytest.mark.asyncio
    async def test_auto_compact_error_handling(self):
        """Test that errors during auto-compact are handled gracefully."""
        from openai import AsyncOpenAI

        client = AsyncMock(spec=AsyncOpenAI)

        with patch("cai.sdk.agents.models.openai_chatcompletions.get_session_recorder"):
            model = OpenAIChatCompletionsModel(
                model="gpt-4", openai_client=client, agent_name="Test Agent", agent_id="TEST123"
            )

        with (
            patch(f"{_AC}.get_config", return_value=_make_cfg(True, 0.8)),
            patch(f"{_AC}.get_model_max_tokens", return_value=1000),
            patch("cai.repl.commands.memory.MEMORY_COMMAND_INSTANCE") as mock_memory,
            patch("rich.console.Console"),
        ):
            mock_memory._ai_summarize_history = AsyncMock(side_effect=Exception("Failed"))

            new_input, new_instructions, compacted = await model._auto_compact_if_needed(
                estimated_tokens=850, input="Test", system_instructions=None
            )

            assert compacted is False
            assert new_input == "Test"
            assert new_instructions is None

    @pytest.mark.asyncio
    @pytest.mark.allow_call_model_methods
    async def test_auto_compact_integration(self):
        """Integration test for auto-compact during get_response."""
        from openai import AsyncOpenAI
        from openai.types.chat import ChatCompletion, ChatCompletionMessage
        from openai.types.chat.chat_completion import Choice, CompletionUsage
        from cai.sdk.agents.model_settings import ModelSettings
        from cai.sdk.agents.models.interface import ModelTracing

        client = AsyncMock(spec=AsyncOpenAI)
        client.base_url = "https://api.openai.com"

        mock_response = ChatCompletion(
            id="test-id",
            object="chat.completion",
            created=1234567890,
            model="gpt-4",
            choices=[
                Choice(
                    index=0,
                    message=ChatCompletionMessage(
                        role="assistant", content="Response after compaction"
                    ),
                    finish_reason="stop",
                )
            ],
            usage=CompletionUsage(
                prompt_tokens=200,
                completion_tokens=50,
                total_tokens=250,
            ),
        )

        with patch("cai.sdk.agents.models.openai_chatcompletions.get_session_recorder"):
            model = OpenAIChatCompletionsModel(
                model="gpt-4", openai_client=client, agent_name="Test Agent", agent_id="TEST123"
            )

        # Patch the auto_compactor to simulate compaction
        with (
            patch(f"{_AC}.get_config", return_value=_make_cfg(True, 0.8)),
            patch(f"{_AC}.get_model_max_tokens", return_value=1000),
            patch("cai.repl.commands.memory.MEMORY_COMMAND_INSTANCE") as mock_memory,
            patch("cai.repl.commands.memory.COMPACTED_SUMMARIES", {}),
            patch("rich.console.Console"),
            patch("cai.sdk.agents.models.openai_chatcompletions.stop_idle_timer"),
            patch("cai.sdk.agents.models.openai_chatcompletions.start_active_timer"),
            patch("cai.sdk.agents.models.openai_chatcompletions.stop_active_timer"),
            patch("cai.sdk.agents.models.openai_chatcompletions.start_idle_timer"),
            patch("cai.sdk.agents.models.openai_chatcompletions.COST_TRACKER"),
            patch.object(model, "_fetch_response", AsyncMock(return_value=mock_response)),
            patch(
                "cai.sdk.agents.models.openai_chatcompletions.count_tokens_with_tiktoken",
                return_value=(850, 0),
            ),
        ):
            mock_memory._ai_summarize_history = AsyncMock(return_value="Previous summary")

            result = await model.get_response(
                system_instructions=None,
                input="Test message",
                model_settings=ModelSettings(),
                tools=[],
                output_schema=None,
                handoffs=[],
                tracing=ModelTracing.DISABLED,
            )

            # Verify compaction was triggered
            mock_memory._ai_summarize_history.assert_called_once()

            # Verify response was returned
            assert result is not None


def test_cai_config_auto_compact_threshold_capped(monkeypatch):
    """CAI_AUTO_COMPACT_THRESHOLD above 80% is clamped so auto-compact cannot defer past 0.8."""
    from cai.config import AUTO_COMPACT_THRESHOLD_MAX, get_config, reset_config

    reset_config()
    monkeypatch.setenv("CAI_AUTO_COMPACT_THRESHOLD", "0.99")
    reset_config()
    try:
        assert get_config().auto_compact_threshold == AUTO_COMPACT_THRESHOLD_MAX
    finally:
        reset_config()
        monkeypatch.delenv("CAI_AUTO_COMPACT_THRESHOLD", raising=False)
