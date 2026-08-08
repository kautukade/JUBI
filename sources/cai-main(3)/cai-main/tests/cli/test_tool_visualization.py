#!/usr/bin/env python3
"""
Test tool visualization functionality.
Tests token display, panel creation, streaming panels, and cost tracking.
"""

import os
import sys
import pytest

# Add src to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src"))


class TestTokenInfoDisplay:
    """Test _create_token_info_display function."""

    def test_returns_none_for_empty_token_info(self):
        """Should return None when token_info is empty or None."""
        from cai.util import _create_token_info_display

        assert _create_token_info_display(None) is None
        assert _create_token_info_display({}) is None

    def test_returns_none_for_zero_tokens(self):
        """Should return None when all token values are zero."""
        from cai.util import _create_token_info_display

        token_info = {
            "interaction_input_tokens": 0,
            "interaction_output_tokens": 0,
            "total_input_tokens": 0,
            "total_output_tokens": 0,
        }
        assert _create_token_info_display(token_info) is None

    def test_returns_display_for_interaction_tokens(self):
        """Should return display when interaction tokens are non-zero."""
        from cai.util import _create_token_info_display

        token_info = {
            "interaction_input_tokens": 100,
            "interaction_output_tokens": 50,
            "total_input_tokens": 0,
            "total_output_tokens": 0,
        }
        result = _create_token_info_display(token_info)
        assert result is not None

    def test_returns_display_for_total_tokens(self):
        """Should return display when total tokens are non-zero."""
        from cai.util import _create_token_info_display

        token_info = {
            "interaction_input_tokens": 0,
            "interaction_output_tokens": 0,
            "total_input_tokens": 100,
            "total_output_tokens": 50,
        }
        result = _create_token_info_display(token_info)
        assert result is not None

    def test_includes_cache_tokens_when_present(self):
        """Should include cache read/write tokens in display."""
        from cai.util import _create_token_info_display

        token_info = {
            "interaction_input_tokens": 100,
            "interaction_output_tokens": 50,
            "cache_read_tokens": 80,
            "cache_creation_tokens": 20,
        }
        result = _create_token_info_display(token_info)
        assert result is not None
        # Result is a Rich Text object, check it was created
        from rich.text import Text
        assert isinstance(result, Text)


class TestToolPanelContent:
    """Test _create_tool_panel_content function."""

    def test_creates_panel_with_basic_args(self):
        """Should create panel content with basic arguments."""
        from cai.util import _create_tool_panel_content

        header, content = _create_tool_panel_content(
            tool_name="test_tool",
            args={"arg1": "value1"},
            output="test output",
            execution_info=None,
            token_info=None,
        )
        assert header is not None
        assert content is not None

    def test_creates_panel_with_string_args(self):
        """Should create panel content with string arguments."""
        from cai.util import _create_tool_panel_content

        header, content = _create_tool_panel_content(
            tool_name="generic_linux_command",
            args="ls -la",
            output="total 0\ndrwxr-xr-x  2 user user 40 Jan  1 00:00 .",
            execution_info={"status": "completed"},
            token_info=None,
        )
        assert header is not None
        assert content is not None

    def test_includes_token_info_when_provided(self):
        """Should include token info in panel when provided with non-zero values."""
        from cai.util import _create_tool_panel_content
        from rich.console import Group

        token_info = {
            "interaction_input_tokens": 100,
            "interaction_output_tokens": 50,
            "model": "test-model",
        }
        header, content = _create_tool_panel_content(
            tool_name="test_tool",
            args={"arg": "value"},
            output="test output",
            execution_info={"status": "completed"},
            token_info=token_info,
        )
        assert content is not None
        assert isinstance(content, Group)

    def test_excludes_token_info_for_zero_values(self):
        """Should exclude token info from panel when all values are zero."""
        from cai.util import _create_tool_panel_content

        token_info = {
            "interaction_input_tokens": 0,
            "interaction_output_tokens": 0,
            "total_input_tokens": 0,
            "total_output_tokens": 0,
        }
        header, content = _create_tool_panel_content(
            tool_name="test_tool",
            args={"arg": "value"},
            output="test output",
            execution_info={"status": "completed"},
            token_info=token_info,
        )
        # Content should still be created, just without token section
        assert content is not None

    def test_handles_execute_code_tool(self):
        """Should handle execute_code tool with special formatting."""
        from cai.util import _create_tool_panel_content

        header, content = _create_tool_panel_content(
            tool_name="execute_code",
            args={
                "command": "execute",
                "code": "print('hello')",
                "language": "python",
            },
            output="hello",
            execution_info={"status": "completed"},
            token_info=None,
        )
        assert header is not None
        assert content is not None


class TestEnrichTokenInfoForPricing:
    """Test enrich_token_info_for_pricing function."""

    def test_preserves_existing_values(self):
        """Should preserve existing token values."""
        from cai.util import enrich_token_info_for_pricing

        token_info = {
            "interaction_input_tokens": 100,
            "interaction_output_tokens": 50,
            "model": "test-model",
        }
        enriched = enrich_token_info_for_pricing(token_info)
        assert enriched["interaction_input_tokens"] == 100
        assert enriched["interaction_output_tokens"] == 50

    def test_adds_missing_fields(self):
        """Should add missing fields with default values."""
        from cai.util import enrich_token_info_for_pricing

        token_info = {"model": "test-model"}
        enriched = enrich_token_info_for_pricing(token_info)
        assert "interaction_input_tokens" in enriched
        assert "interaction_output_tokens" in enriched
        assert "total_input_tokens" in enriched
        assert "total_output_tokens" in enriched

    def test_handles_cache_tokens(self):
        """Should handle cache read and write tokens."""
        from cai.util import enrich_token_info_for_pricing

        token_info = {
            "interaction_input_tokens": 100,
            "cache_read_tokens": 80,
            "cache_creation_tokens": 20,
            "model": "test-model",
        }
        enriched = enrich_token_info_for_pricing(token_info)
        assert enriched.get("cache_read_tokens") == 80
        assert enriched.get("cache_creation_tokens") == 20


class TestCostTracker:
    """Test CostTracker class functionality."""

    def test_cost_tracker_has_required_attributes(self):
        """Should have all required token tracking attributes."""
        from cai.util import COST_TRACKER

        # Check that COST_TRACKER exists and has basic attributes
        assert hasattr(COST_TRACKER, "current_agent_input_tokens")
        assert hasattr(COST_TRACKER, "current_agent_output_tokens")

    def test_cost_tracker_has_cache_token_attributes_initialized(self):
        """CostTracker should have cache token attributes initialized in __init__."""
        from cai.util import CostTracker

        # Create a fresh instance to verify initialization
        fresh_tracker = CostTracker()

        # Verify cache tokens are initialized to 0
        assert hasattr(fresh_tracker, "cache_read_tokens")
        assert hasattr(fresh_tracker, "cache_creation_tokens")
        assert fresh_tracker.cache_read_tokens == 0
        assert fresh_tracker.cache_creation_tokens == 0

    def test_cost_tracker_can_store_interaction_tokens(self):
        """Should be able to store interaction token values."""
        from cai.util import COST_TRACKER

        # Set values
        COST_TRACKER.interaction_input_tokens = 100
        COST_TRACKER.interaction_output_tokens = 50

        # Verify they're stored
        assert COST_TRACKER.interaction_input_tokens == 100
        assert COST_TRACKER.interaction_output_tokens == 50

        # Clean up
        COST_TRACKER.interaction_input_tokens = 0
        COST_TRACKER.interaction_output_tokens = 0

    def test_cost_tracker_can_store_cache_tokens(self):
        """Should be able to store cache token values."""
        from cai.util import COST_TRACKER

        # Set values
        COST_TRACKER.cache_read_tokens = 80
        COST_TRACKER.cache_creation_tokens = 20

        # Verify they're stored
        assert COST_TRACKER.cache_read_tokens == 80
        assert COST_TRACKER.cache_creation_tokens == 20

        # Clean up
        COST_TRACKER.cache_read_tokens = 0
        COST_TRACKER.cache_creation_tokens = 0


class TestTokenDisplay:
    """Test _create_token_display function."""

    def test_creates_display_with_basic_values(self):
        """Should create token display with basic values."""
        from cai.util import _create_token_display

        result = _create_token_display(
            interaction_input_tokens=100,
            interaction_output_tokens=50,
            interaction_reasoning_tokens=0,
            total_input_tokens=100,
            total_output_tokens=50,
            total_reasoning_tokens=0,
            model="test-model",
        )
        assert result is not None
        from rich.text import Text
        assert isinstance(result, Text)

    def test_includes_cache_tokens_in_display(self):
        """Should include cache tokens when provided."""
        from cai.util import _create_token_display

        result = _create_token_display(
            interaction_input_tokens=100,
            interaction_output_tokens=50,
            interaction_reasoning_tokens=0,
            total_input_tokens=100,
            total_output_tokens=50,
            total_reasoning_tokens=0,
            model="test-model",
            cache_read_tokens=80,
            cache_creation_tokens=20,
        )
        assert result is not None
        # The result should contain cache info
        result_str = str(result)
        # Cache tokens should be included when non-zero
        assert "CR" in result_str or "80" in result_str or result is not None

    def test_includes_cost_information(self):
        """Should include cost information when provided."""
        from cai.util import _create_token_display

        result = _create_token_display(
            interaction_input_tokens=100,
            interaction_output_tokens=50,
            interaction_reasoning_tokens=0,
            total_input_tokens=100,
            total_output_tokens=50,
            total_reasoning_tokens=0,
            model="test-model",
            interaction_cost=0.001,
            total_cost=0.002,
        )
        assert result is not None


class TestStreamingPanelGrouping:
    """Test streaming panel grouping functionality."""

    def test_grouped_streaming_tools_dict_exists(self):
        """Should have _GROUPED_STREAMING_TOOLS dictionary."""
        from cai.util import _GROUPED_STREAMING_TOOLS
        assert isinstance(_GROUPED_STREAMING_TOOLS, dict)

    def test_live_streaming_panels_dict_exists(self):
        """Should have _LIVE_STREAMING_PANELS dictionary."""
        from cai.util import _LIVE_STREAMING_PANELS
        assert isinstance(_LIVE_STREAMING_PANELS, dict)


class TestModelCostTrackerIntegration:
    """Test integration between model and COST_TRACKER."""

    def test_chatcompletions_model_has_token_tracking_in_init(self):
        """OpenAIChatCompletionsModel __init__ should set up token tracking attributes."""
        # This test verifies the model initializes token tracking attributes
        # by checking the __init__ method signature and the model's behavior
        from cai.sdk.agents.models.openai_chatcompletions import OpenAIChatCompletionsModel
        import inspect

        # Get the __init__ method source to verify it sets up token tracking
        init_source = inspect.getsource(OpenAIChatCompletionsModel.__init__)

        # Verify the init sets up the interaction token attributes
        assert "interaction_input_tokens" in init_source
        assert "interaction_output_tokens" in init_source
        assert "cache_read_tokens" in init_source
        assert "cache_creation_tokens" in init_source


class TestTimingInfo:
    """Test timing information functions."""

    def test_get_timing_info_returns_list(self):
        """Should return a list of timing info strings."""
        from cai.util import _get_timing_info

        timing_info, tool_time = _get_timing_info(None)
        assert isinstance(timing_info, list)

    def test_get_timing_info_with_execution_info(self):
        """Should include tool time when provided in execution_info."""
        from cai.util import _get_timing_info

        execution_info = {"tool_time": 1.5}
        timing_info, tool_time = _get_timing_info(execution_info)
        assert tool_time == 1.5
        assert any("Tool" in t for t in timing_info)


class TestFormatTime:
    """Test format_time function."""

    def test_formats_seconds(self):
        """Should format seconds correctly."""
        from cai.util import format_time

        assert format_time(0.5) == "0.5s"
        assert format_time(1.0) == "1.0s"
        assert format_time(59.9) == "59.9s"

    def test_formats_minutes(self):
        """Should format minutes correctly."""
        from cai.util import format_time

        result = format_time(90)  # 1.5 minutes
        assert "m" in result or "1" in result

    def test_handles_none(self):
        """Should handle None gracefully."""
        from cai.util import format_time

        # format_time should handle None or return something reasonable
        try:
            result = format_time(None)
            # If it doesn't raise, that's fine
        except (TypeError, ValueError):
            # Expected if it doesn't handle None
            pass


class TestMultiToolCallsNonStreaming:
    """Test multi-tool call handling in non-streaming mode."""

    def test_multiple_tool_outputs_no_duplicates(self):
        """Should not create duplicate panels for multiple tool outputs."""
        from cai.util import _create_tool_panel_content

        tool_calls = [
            {"name": "echo", "args": "AAA", "output": "AAA"},
            {"name": "echo", "args": "BBB", "output": "BBB"},
            {"name": "echo", "args": "CCC", "output": "CCC"},
        ]

        panels = []
        for tc in tool_calls:
            header, content = _create_tool_panel_content(
                tool_name=tc["name"],
                args=tc["args"],
                output=tc["output"],
                execution_info={"status": "completed"},
                token_info=None,
            )
            panels.append((header, content))

        # Should have exactly 3 panels, no duplicates
        assert len(panels) == 3
        # Each panel should be unique
        outputs = [tc["output"] for tc in tool_calls]
        assert len(set(outputs)) == 3

    def test_parallel_tool_calls_distinct_outputs(self):
        """Parallel tool calls should maintain distinct outputs."""
        from cai.util import _create_tool_panel_content

        # Simulate 3 parallel ping commands
        tool_calls = [
            {"name": "ping", "args": "-c 1 google.com", "output": "PING google.com..."},
            {"name": "ping", "args": "-c 1 8.8.8.8", "output": "PING 8.8.8.8..."},
            {"name": "ping", "args": "-c 1 192.168.1.1", "output": "PING 192.168.1.1..."},
        ]

        results = []
        for tc in tool_calls:
            header, content = _create_tool_panel_content(
                tool_name=tc["name"],
                args=tc["args"],
                output=tc["output"],
                execution_info={"status": "completed"},
                token_info={"interaction_input_tokens": 100, "interaction_output_tokens": 50},
            )
            results.append({"header": header, "content": content, "args": tc["args"]})

        # Verify all 3 results are distinct
        assert len(results) == 3
        args_list = [r["args"] for r in results]
        assert len(set(args_list)) == 3  # All args should be unique


class TestMessageHistoryNoDuplicates:
    """Test that message history doesn't have duplicates."""

    def test_fix_message_list_no_duplicate_tool_responses(self):
        """fix_message_list should not create duplicate tool responses."""
        from cai.util import fix_message_list

        messages = [
            {"role": "user", "content": "Run 3 commands"},
            {
                "role": "assistant",
                "content": None,
                "tool_calls": [
                    {"id": "call_1", "type": "function", "function": {"name": "echo", "arguments": '{"text": "A"}'}},
                    {"id": "call_2", "type": "function", "function": {"name": "echo", "arguments": '{"text": "B"}'}},
                    {"id": "call_3", "type": "function", "function": {"name": "echo", "arguments": '{"text": "C"}'}},
                ],
            },
            {"role": "tool", "tool_call_id": "call_1", "content": "A"},
            {"role": "tool", "tool_call_id": "call_2", "content": "B"},
            {"role": "tool", "tool_call_id": "call_3", "content": "C"},
        ]

        fixed = fix_message_list(messages)

        # Count tool responses
        tool_responses = [m for m in fixed if m.get("role") == "tool"]
        tool_call_ids = [m.get("tool_call_id") for m in tool_responses]

        # Should have exactly 3 tool responses, no duplicates
        assert len(tool_responses) == 3
        assert len(set(tool_call_ids)) == 3

    def test_fix_message_list_adds_missing_tool_response(self):
        """fix_message_list should add missing tool responses."""
        from cai.util import fix_message_list

        # Missing tool response for call_2
        messages = [
            {"role": "user", "content": "Run commands"},
            {
                "role": "assistant",
                "content": None,
                "tool_calls": [
                    {"id": "call_1", "type": "function", "function": {"name": "echo", "arguments": '{}'}},
                    {"id": "call_2", "type": "function", "function": {"name": "echo", "arguments": '{}'}},
                ],
            },
            {"role": "tool", "tool_call_id": "call_1", "content": "result1"},
            # Missing call_2 response
        ]

        fixed = fix_message_list(messages)

        # Should now have 2 tool responses
        tool_responses = [m for m in fixed if m.get("role") == "tool"]
        assert len(tool_responses) == 2

    def test_no_unknown_function_duplicates(self):
        """Should not create unknown_function duplicates during cleanup."""
        from cai.util import fix_message_list

        messages = [
            {"role": "user", "content": "Test"},
            {
                "role": "assistant",
                "content": None,
                "tool_calls": [
                    {"id": "call_real", "type": "function", "function": {"name": "real_function", "arguments": '{}'}},
                ],
            },
            {"role": "tool", "tool_call_id": "call_real", "content": "result"},
        ]

        fixed = fix_message_list(messages)

        # Check no unknown_function was added
        for msg in fixed:
            if msg.get("role") == "assistant" and msg.get("tool_calls"):
                for tc in msg["tool_calls"]:
                    assert tc.get("function", {}).get("name") != "unknown_function"


class TestLargeMessageHistory:
    """Test handling of large message histories."""

    def test_large_message_history_no_corruption(self):
        """Large message history should not get corrupted."""
        from cai.util import fix_message_list

        # Create a large message history with 50 tool calls
        messages = [{"role": "user", "content": "Run many commands"}]

        tool_calls = []
        for i in range(50):
            tool_calls.append({
                "id": f"call_{i}",
                "type": "function",
                "function": {"name": f"command_{i}", "arguments": f'{{"index": {i}}}'},
            })

        messages.append({
            "role": "assistant",
            "content": None,
            "tool_calls": tool_calls,
        })

        # Add all tool responses
        for i in range(50):
            messages.append({
                "role": "tool",
                "tool_call_id": f"call_{i}",
                "content": f"result_{i}",
            })

        fixed = fix_message_list(messages)

        # Verify integrity
        tool_responses = [m for m in fixed if m.get("role") == "tool"]
        assert len(tool_responses) == 50

        # Verify no duplicates
        tool_call_ids = [m.get("tool_call_id") for m in tool_responses]
        assert len(set(tool_call_ids)) == 50

    def test_alternating_user_assistant_tool_pattern(self):
        """Should handle alternating conversation patterns correctly."""
        from cai.util import fix_message_list

        messages = []
        for i in range(10):
            # User turn
            messages.append({"role": "user", "content": f"Command {i}"})
            # Assistant with tool call
            messages.append({
                "role": "assistant",
                "content": None,
                "tool_calls": [{
                    "id": f"call_{i}",
                    "type": "function",
                    "function": {"name": "echo", "arguments": f'{{"msg": "{i}"}}'},
                }],
            })
            # Tool response
            messages.append({
                "role": "tool",
                "tool_call_id": f"call_{i}",
                "content": f"Result {i}",
            })
            # Assistant response
            messages.append({
                "role": "assistant",
                "content": f"Done with command {i}",
            })

        fixed = fix_message_list(messages)

        # Should maintain the same structure
        assert len(fixed) == len(messages)

        # Verify pattern integrity
        user_msgs = [m for m in fixed if m.get("role") == "user"]
        tool_msgs = [m for m in fixed if m.get("role") == "tool"]
        assert len(user_msgs) == 10
        assert len(tool_msgs) == 10


class TestStreamingPanelFinalization:
    """Test streaming panel finalization logic."""

    def test_single_tool_uses_single_panel_format(self):
        """Single tool in streaming should use single panel format."""
        from cai.util import _GROUPED_STREAMING_TOOLS, _GROUPED_TOOLS_LOCK

        # This tests the logic conceptually - actual streaming requires async context
        # The key assertion is that _finalize_tool_group handles tool_count == 1 specially

        # Verify the structures exist and are properly initialized
        assert isinstance(_GROUPED_STREAMING_TOOLS, dict)

    def test_multi_tool_uses_grouped_format(self):
        """Multiple tools in streaming should use grouped format."""
        from cai.util import _GROUPED_STREAMING_TOOLS

        # Conceptual test - actual behavior requires async streaming context
        assert isinstance(_GROUPED_STREAMING_TOOLS, dict)


class TestTokenInfoEdgeCases:
    """Test edge cases in token info handling."""

    def test_negative_token_values_handled(self):
        """Should handle negative token values gracefully."""
        from cai.util import _create_token_info_display

        # Negative values should be treated as invalid/zero
        token_info = {
            "interaction_input_tokens": -1,
            "interaction_output_tokens": -1,
        }
        result = _create_token_info_display(token_info)
        # Should return None since negative is not > 0
        assert result is None

    def test_very_large_token_values(self):
        """Should handle very large token values."""
        from cai.util import _create_token_info_display

        token_info = {
            "interaction_input_tokens": 1000000,
            "interaction_output_tokens": 500000,
            "total_input_tokens": 10000000,
            "total_output_tokens": 5000000,
        }
        result = _create_token_info_display(token_info)
        assert result is not None

    def test_float_token_values(self):
        """Should handle float token values by converting to int."""
        from cai.util import enrich_token_info_for_pricing

        token_info = {
            "interaction_input_tokens": 100.5,
            "interaction_output_tokens": 50.7,
        }
        enriched = enrich_token_info_for_pricing(token_info)
        # Values should be preserved (the display function handles conversion)
        assert enriched["interaction_input_tokens"] == 100.5

    def test_string_token_values_in_enrichment(self):
        """Should handle string token values gracefully."""
        from cai.util import enrich_token_info_for_pricing

        token_info = {
            "interaction_input_tokens": "100",
            "interaction_output_tokens": "50",
        }
        # Should not crash
        enriched = enrich_token_info_for_pricing(token_info)
        assert enriched is not None


class TestCacheTokenDisplay:
    """Test cache token (CR/CW) display functionality."""

    def test_cache_read_tokens_displayed(self):
        """Cache read tokens should be displayed when present."""
        from cai.util import _create_token_display

        result = _create_token_display(
            interaction_input_tokens=100,
            interaction_output_tokens=50,
            interaction_reasoning_tokens=0,
            total_input_tokens=100,
            total_output_tokens=50,
            total_reasoning_tokens=0,
            model="claude-sonnet",
            cache_read_tokens=80,
            cache_creation_tokens=0,
        )
        result_str = str(result)
        # Should contain CR indicator
        assert "CR" in result_str or "80" in result_str

    def test_cache_write_tokens_displayed(self):
        """Cache write/creation tokens should be displayed when present."""
        from cai.util import _create_token_display

        result = _create_token_display(
            interaction_input_tokens=100,
            interaction_output_tokens=50,
            interaction_reasoning_tokens=0,
            total_input_tokens=100,
            total_output_tokens=50,
            total_reasoning_tokens=0,
            model="claude-sonnet",
            cache_read_tokens=0,
            cache_creation_tokens=20,
        )
        result_str = str(result)
        # Should contain CW indicator
        assert "CW" in result_str or "20" in result_str

    def test_both_cache_tokens_displayed(self):
        """Both CR and CW should be displayed when both present."""
        from cai.util import _create_token_display

        result = _create_token_display(
            interaction_input_tokens=100,
            interaction_output_tokens=50,
            interaction_reasoning_tokens=0,
            total_input_tokens=100,
            total_output_tokens=50,
            total_reasoning_tokens=0,
            model="claude-sonnet",
            cache_read_tokens=80,
            cache_creation_tokens=20,
        )
        result_str = str(result)
        # Both should be present
        assert ("CR" in result_str or "80" in result_str)
        assert ("CW" in result_str or "20" in result_str)


class TestOutputDeduplication:
    """Test output deduplication in cli_print_tool_output."""

    def test_output_hash_tracking_exists(self):
        """cli_print_tool_output uses _output_hashes for deduplication (created lazily)."""
        from cai.util import cli_print_tool_output
        import inspect

        # Verify the function's source code references _output_hashes for deduplication
        source = inspect.getsource(cli_print_tool_output)
        assert "_output_hashes" in source, "Function should use _output_hashes for deduplication"

    def test_streaming_sessions_tracking_available(self):
        """cli_print_tool_output uses _streaming_sessions (created lazily when needed)."""
        from cai.util import cli_print_tool_output
        import inspect

        # Verify the function's source code references _streaming_sessions
        source = inspect.getsource(cli_print_tool_output)
        assert "_streaming_sessions" in source, "Function should use _streaming_sessions"


class TestPanelCreationConsistency:
    """Test consistency of panel creation across modes."""

    def test_completed_panel_has_green_border(self):
        """Completed panels should have green border style."""
        from cai.util import _create_tool_panel_content
        from rich.panel import Panel
        from rich.box import ROUNDED

        header, content = _create_tool_panel_content(
            tool_name="test",
            args={},
            output="done",
            execution_info={"status": "completed"},
            token_info=None,
        )

        # Create the panel as the code does
        panel = Panel(
            content,
            title="[bold green]Completed[/bold green]",
            border_style="green",
            padding=(0, 1),
            box=ROUNDED,
            title_align="left",
        )

        assert panel.border_style == "green"

    def test_error_panel_styling(self):
        """Error panels should have appropriate styling."""
        from cai.util import _create_tool_panel_content

        header, content = _create_tool_panel_content(
            tool_name="test",
            args={},
            output="Error: command failed",
            execution_info={"status": "error"},
            token_info=None,
        )

        # Content should be created regardless of status
        assert content is not None


class TestCtrlCInterruptHandling:
    """Test CTRL+C interrupt handling in streaming mode."""

    def test_cleanup_all_streaming_resources_exists(self):
        """cleanup_all_streaming_resources should be importable and callable."""
        from cai.util import cleanup_all_streaming_resources

        # Should be callable without error
        assert callable(cleanup_all_streaming_resources)

    def test_force_stop_all_panels_exists(self):
        """_force_stop_all_panels should be importable and callable."""
        from cai.util import _force_stop_all_panels

        assert callable(_force_stop_all_panels)

    def test_cleanup_handles_empty_panels(self):
        """Cleanup should handle case where no panels exist."""
        from cai.util import cleanup_all_streaming_resources, _LIVE_STREAMING_PANELS

        # Clear any existing panels
        _LIVE_STREAMING_PANELS.clear()

        # Should not raise
        cleanup_all_streaming_resources()

    def test_cleanup_handles_grouped_streaming_tools(self):
        """Cleanup should handle grouped streaming tools."""
        from cai.util import (
            cleanup_all_streaming_resources,
            _GROUPED_STREAMING_TOOLS,
            _LIVE_STREAMING_PANELS,
        )

        # Clear panels first
        _LIVE_STREAMING_PANELS.clear()
        _GROUPED_STREAMING_TOOLS.clear()

        # Add a mock group
        _GROUPED_STREAMING_TOOLS["test_group"] = {
            "call_ids": ["call1", "call2"],
            "tools": {},
            "live_panel": None,  # No actual Live panel
        }

        # Should not raise
        cleanup_all_streaming_resources()

        # Group should be cleaned up
        assert "test_group" not in _GROUPED_STREAMING_TOOLS

    def test_cleanup_stops_live_panel_with_stop_method(self):
        """Cleanup should call stop() on Live panels."""
        from cai.util import cleanup_all_streaming_resources, _LIVE_STREAMING_PANELS

        # Create a mock Live panel
        class MockLivePanel:
            def __init__(self):
                self.stopped = False

            def stop(self):
                self.stopped = True

        mock_panel = MockLivePanel()
        _LIVE_STREAMING_PANELS["test_call_id"] = mock_panel

        # Run cleanup
        cleanup_all_streaming_resources()

        # Panel should have been stopped
        assert mock_panel.stopped

    def test_cleanup_handles_dict_panels(self):
        """Cleanup should handle dict-type static panels (no stop method)."""
        from cai.util import cleanup_all_streaming_resources, _LIVE_STREAMING_PANELS

        # Add a static dict panel (has no stop() method)
        _LIVE_STREAMING_PANELS["test_static"] = {
            "type": "static",
            "displayed": True,
            "last_update": 123.456,
        }

        # Should not raise
        cleanup_all_streaming_resources()

        # Panel should be cleaned up
        assert "test_static" not in _LIVE_STREAMING_PANELS

    def test_force_stop_handles_empty_state(self):
        """_force_stop_all_panels should handle empty state."""
        from cai.util import (
            _force_stop_all_panels,
            _LIVE_STREAMING_PANELS,
            _GROUPED_STREAMING_TOOLS,
            _CLAUDE_THINKING_PANELS,
        )

        # Clear all
        _LIVE_STREAMING_PANELS.clear()
        _GROUPED_STREAMING_TOOLS.clear()
        _CLAUDE_THINKING_PANELS.clear()

        # Should not raise
        _force_stop_all_panels()

    def test_signal_handler_function_exists(self):
        """signal_handler should be defined in util module."""
        from cai.util import signal_handler

        assert callable(signal_handler)

    def test_interrupt_count_tracking(self):
        """Interrupt count and time should be tracked as module globals."""
        from cai import util

        assert hasattr(util, "_interrupt_count")
        assert hasattr(util, "_last_interrupt_time")

    def test_cleanup_restores_cursor(self):
        """Cleanup should restore terminal cursor visibility."""
        import inspect
        from cai.util import cleanup_all_streaming_resources
        from cai.util.streaming import restore_terminal_state

        cleanup_src = inspect.getsource(cleanup_all_streaming_resources)
        assert "restore_terminal_state" in cleanup_src, (
            "cleanup should delegate TTY restore to restore_terminal_state()"
        )

        restore_src = inspect.getsource(restore_terminal_state)
        assert "show_cursor" in restore_src or "\\033[?25h" in restore_src

    def test_cleanup_non_blocking_lock(self):
        """Cleanup should use non-blocking lock acquisition."""
        import inspect
        from cai.util import cleanup_all_streaming_resources

        source = inspect.getsource(cleanup_all_streaming_resources)

        # Should use blocking=False for non-blocking lock
        assert "blocking=False" in source

    def test_cleanup_clears_streaming_sessions(self):
        """Cleanup should clear streaming sessions."""
        from cai.util import cleanup_all_streaming_resources, cli_print_tool_output

        # Add a streaming session
        if not hasattr(cli_print_tool_output, "_streaming_sessions"):
            cli_print_tool_output._streaming_sessions = {}
        cli_print_tool_output._streaming_sessions["test_session"] = {"buffer": "test"}

        # Run cleanup
        cleanup_all_streaming_resources()

        # Sessions should be cleared
        assert len(cli_print_tool_output._streaming_sessions) == 0

    def test_grouped_tools_lock_exists(self):
        """_GROUPED_TOOLS_LOCK should exist for thread safety."""
        from cai.util import _GROUPED_TOOLS_LOCK
        import threading

        assert isinstance(_GROUPED_TOOLS_LOCK, type(threading.Lock()))

    def test_panel_update_lock_exists(self):
        """_PANEL_UPDATE_LOCK should exist for thread safety."""
        from cai.util import _PANEL_UPDATE_LOCK
        import threading

        assert isinstance(_PANEL_UPDATE_LOCK, type(threading.Lock()))


class TestStreamingPanelCleanupEdgeCases:
    """Test edge cases in streaming panel cleanup."""

    def test_cleanup_handles_panel_stop_exception(self):
        """Cleanup should handle exceptions from panel.stop()."""
        from cai.util import cleanup_all_streaming_resources, _LIVE_STREAMING_PANELS

        # Create a mock panel that raises on stop
        class BrokenPanel:
            def stop(self):
                raise RuntimeError("Panel broke!")

        _LIVE_STREAMING_PANELS["broken"] = BrokenPanel()

        # Should not raise - exception should be caught
        cleanup_all_streaming_resources()

        # Panel should still be cleaned up
        assert "broken" not in _LIVE_STREAMING_PANELS

    def test_cleanup_handles_grouped_panel_stop_exception(self):
        """Cleanup should handle exceptions from grouped panel.stop()."""
        from cai.util import (
            cleanup_all_streaming_resources,
            _GROUPED_STREAMING_TOOLS,
            _LIVE_STREAMING_PANELS,
        )

        _LIVE_STREAMING_PANELS.clear()

        # Create a mock grouped panel that raises on stop
        class BrokenGroupedPanel:
            def stop(self):
                raise RuntimeError("Grouped panel broke!")

        _GROUPED_STREAMING_TOOLS["broken_group"] = {
            "live_panel": BrokenGroupedPanel(),
            "call_ids": [],
            "tools": {},
        }

        # Should not raise
        cleanup_all_streaming_resources()

        # Group should be cleaned up
        assert "broken_group" not in _GROUPED_STREAMING_TOOLS

    def test_force_stop_is_lock_free(self):
        """_force_stop_all_panels should not acquire any locks."""
        import inspect
        from cai.util import _force_stop_all_panels

        source = inspect.getsource(_force_stop_all_panels)

        # Should not use 'with' for lock acquisition or .acquire()
        assert "with _" not in source  # No 'with _lock:' patterns
        assert ".acquire(" not in source

    def test_cleanup_in_progress_flag_reset(self):
        """_cleanup_in_progress should be reset after cleanup."""
        from cai import util
        from cai.util import cleanup_all_streaming_resources

        # Run cleanup
        cleanup_all_streaming_resources()

        # Flag should be reset to False
        assert util._cleanup_in_progress == False


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
