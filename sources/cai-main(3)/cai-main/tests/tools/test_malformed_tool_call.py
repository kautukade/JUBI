"""
Test for malformed tool calls.

Reproduces issues where the LLM returns "tool calls" as plain text instead of
proper function call structures, causing execution to stop immediately.

Evidence from logs:
1. logs/cai_c50b2981-5693-43bd-839b-bb3d6522024d_20260122_060652_alias_darwin_25.1.0_127_0_0_1.jsonl
   Line 55: {"event": "assistant_message", "content": "<tool_call>generic_linuxcommand..."}

2. Bug Bounter session showing:
   <tool_call>generic_linux_command</arg_key><arg_value>curl -s "https://s3-ap-east-1..."
"""
from __future__ import annotations

import pytest

from cai.sdk.agents import Agent, Runner
from cai.sdk.agents.items import ToolCallItem

from tests.fake_model import FakeModel
from tests.core.test_responses import get_function_tool, get_text_message


@pytest.mark.asyncio
async def test_malformed_tool_call_as_text_stops_immediately():
    """
    Case 1: LLM returns tool call with wrong tool name as text.
    From log line 55: <tool_call>generic_linuxcommand<arg_key>command</arg_key>...

    This test demonstrates the PROBLEM: when using FakeModel (which bypasses
    the converter), malformed tool calls in text stop the loop immediately.
    """
    model = FakeModel()
    agent = Agent(
        name="test_agent",
        model=model,
        tools=[get_function_tool("generic_linux_command", "command executed")],
    )

    malformed_tool_call_text = (
        "<tool_call>generic_linuxcommand"
        "<arg_key>command</arg_key><arg_value>host example.com</arg_value>"
        "</tool_call>"
    )
    model.set_next_output([get_text_message(malformed_tool_call_text)])

    result = await Runner.run(agent, input="run a command")

    # Loop ends immediately - malformed text treated as final output
    assert result.final_output == malformed_tool_call_text
    assert len(result.raw_responses) == 1
    assert len([i for i in result.new_items if isinstance(i, ToolCallItem)]) == 0


@pytest.mark.asyncio
async def test_malformed_tool_call_broken_xml_stops_immediately():
    """
    Case 2: LLM returns tool call with broken XML structure as text.
    From Bug Bounter session: <tool_call>generic_linux_command</arg_key><arg_value>curl -s...
    Note: Missing opening <arg_key> tag - goes straight to </arg_key>

    This test demonstrates the PROBLEM: when using FakeModel (which bypasses
    the converter), malformed tool calls in text stop the loop immediately.
    """
    model = FakeModel()
    agent = Agent(
        name="test_agent",
        model=model,
        tools=[get_function_tool("generic_linux_command", "command executed")],
    )

    # Exact format from the Bug Bounter session - broken XML structure
    malformed_tool_call_text = (
        '<tool_call>generic_linux_command</arg_key><arg_value>curl -s '
        '"https://s3-ap-east-1.amazonaws.com/?prefix=examplecorp&max-keys=100" 2>&1 | '
        'grep -o \'[^<]*\' | head -20</arg_value>'
        '<arg_key>interactive</arg_key><arg_value>false</arg_value>'
        '<arg_key>session_id</arg_key><arg_value>null</arg_value>'
        '<arg_key>timeout</arg_key><arg_value>15</arg_value></tool_call>'
    )
    model.set_next_output([get_text_message(malformed_tool_call_text)])

    result = await Runner.run(agent, input="run a command")

    # Loop ends immediately - malformed text treated as final output
    assert result.final_output == malformed_tool_call_text
    assert len(result.raw_responses) == 1
    assert len([i for i in result.new_items if isinstance(i, ToolCallItem)]) == 0


# =============================================================================
# FIX VERIFICATION TESTS
# These tests verify the _parse_malformed_tool_call method in the converter
# correctly detects and parses malformed tool calls from text content.
# =============================================================================

class TestMalformedToolCallParser:
    """Tests for the _parse_malformed_tool_call fix in OpenAIChatCompletionsModel."""

    @pytest.fixture
    def converter(self):
        """Create a converter instance for testing."""
        from cai.sdk.agents.models.openai_chatcompletions import _Converter
        return _Converter()

    def test_parse_standard_malformed_tool_call(self, converter):
        """
        FIX VERIFICATION: Parser correctly extracts tool name and args from
        standard malformed format.
        """
        text = (
            "<tool_call>generic_linuxcommand"
            "<arg_key>command</arg_key><arg_value>host example.com</arg_value>"
            "</tool_call>"
        )
        result = converter._parse_malformed_tool_call(text)

        assert result is not None
        tool_name, args = result
        assert tool_name == "generic_linuxcommand"
        assert args.get("command") == "host example.com"

    def test_parse_broken_xml_malformed_tool_call(self, converter):
        """
        FIX VERIFICATION: Parser correctly extracts tool name and args from
        broken XML format (missing opening <arg_key> tag).
        """
        text = (
            '<tool_call>generic_linux_command</arg_key><arg_value>curl -s '
            '"https://example.com" 2>&1</arg_value>'
            '<arg_key>interactive</arg_key><arg_value>false</arg_value>'
            '<arg_key>timeout</arg_key><arg_value>15</arg_value></tool_call>'
        )
        result = converter._parse_malformed_tool_call(text)

        assert result is not None
        tool_name, args = result
        assert tool_name == "generic_linux_command"
        # Parser should extract the command from the first arg_value
        assert "command" in args or args.get("interactive") is False

    def test_parse_returns_none_for_normal_text(self, converter):
        """Parser returns None for text without malformed tool calls."""
        text = "This is normal text without any tool calls."
        result = converter._parse_malformed_tool_call(text)
        assert result is None

    def test_parse_extracts_boolean_and_null_values(self, converter):
        """Parser correctly converts string 'false', 'true', 'null' to Python types."""
        text = (
            "<tool_call>test_tool"
            "<arg_key>enabled</arg_key><arg_value>true</arg_value>"
            "<arg_key>disabled</arg_key><arg_value>false</arg_value>"
            "<arg_key>optional</arg_key><arg_value>null</arg_value>"
            "<arg_key>count</arg_key><arg_value>42</arg_value>"
            "</tool_call>"
        )
        result = converter._parse_malformed_tool_call(text)

        assert result is not None
        tool_name, args = result
        assert tool_name == "test_tool"
        assert args.get("enabled") is True
        assert args.get("disabled") is False
        assert args.get("optional") is None
        assert args.get("count") == 42

    def test_message_to_output_items_converts_malformed_to_function_call(self, converter):
        """
        FIX VERIFICATION: message_to_output_items converts malformed tool call
        text into a proper ResponseFunctionToolCall, allowing the agent loop
        to continue instead of stopping.
        """
        from openai.types.chat import ChatCompletionMessage
        from openai.types.responses import ResponseFunctionToolCall

        # Simulate an LLM response with malformed tool call as text content
        message = ChatCompletionMessage(
            role="assistant",
            content=(
                "<tool_call>generic_linux_command"
                "<arg_key>command</arg_key><arg_value>ls -la</arg_value>"
                "</tool_call>"
            ),
        )

        items = converter.message_to_output_items(message)

        # Should have both the text message AND a parsed function call
        assert len(items) == 2

        # First item is the text message
        assert items[0].type == "message"

        # Second item should be the parsed function call
        assert isinstance(items[1], ResponseFunctionToolCall)
        assert items[1].name == "generic_linux_command"
        assert "command" in items[1].arguments
        assert "ls -la" in items[1].arguments
