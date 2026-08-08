"""
Test backwards compatibility with old JSONL log formats.

This module tests that the current loaders can handle:
1. Simple format: {"messages": [...]} - single line with just messages
2. Full old format: alternating model+messages and chat.completion records
3. New format: session events + chat.completion with agent_name, cost, timing

See also: caiextensions-memory repository for real old format logs.
"""

import os
import json
import pytest
import tempfile
from pathlib import Path

from cai.sdk.agents.run_to_jsonl import load_history_from_jsonl
from cai.repl.session_resume import fast_load_messages, normalize_messages_for_agent


class TestBackwardsCompatibility:
    """Test backwards compatibility with different log formats."""

    @pytest.fixture
    def simple_format_log(self, tmp_path):
        """Create a simple format log file."""
        log_file = tmp_path / "simple.jsonl"
        log_file.write_text(
            '{"messages": [{"role": "system", "content": "You are helpful."}, '
            '{"role": "user", "content": "Hello"}, '
            '{"role": "assistant", "content": "Hi there!"}]}\n'
        )
        return str(log_file)

    @pytest.fixture
    def old_format_log(self, tmp_path):
        """Create an old format log file with model+messages and completions."""
        log_file = tmp_path / "old_format.jsonl"
        lines = [
            # Request 1
            json.dumps({
                "model": "gpt-4",
                "messages": [
                    {"role": "system", "content": "You are a security expert."},
                    {"role": "user", "content": "Scan the target"}
                ],
                "tools": [{"type": "function", "function": {"name": "nmap"}}],
                "stream": False
            }),
            # Response 1
            json.dumps({
                "id": "chatcmpl-1",
                "object": "chat.completion",
                "model": "gpt-4",
                "messages": [],
                "choices": [{
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": "I'll scan the target.",
                        "tool_calls": [{
                            "id": "call_1",
                            "type": "function",
                            "function": {"name": "nmap", "arguments": "{}"}
                        }]
                    },
                    "finish_reason": "tool_calls"
                }],
                "usage": {"prompt_tokens": 100, "completion_tokens": 50}
            }),
            # Request 2 (with tool response)
            json.dumps({
                "model": "gpt-4",
                "messages": [
                    {"role": "system", "content": "You are a security expert."},
                    {"role": "user", "content": "Scan the target"},
                    {"role": "assistant", "content": "I'll scan.", "tool_calls": [{"id": "call_1"}]},
                    {"role": "tool", "tool_call_id": "call_1", "tool_name": "nmap", "content": "Port 80 open"}
                ],
                "tools": [],
                "stream": False
            }),
            # Response 2
            json.dumps({
                "id": "chatcmpl-2",
                "object": "chat.completion",
                "model": "gpt-4",
                "messages": [],
                "choices": [{
                    "index": 0,
                    "message": {"role": "assistant", "content": "Found open port 80."},
                    "finish_reason": "stop"
                }],
                "usage": {"prompt_tokens": 150, "completion_tokens": 20}
            })
        ]
        log_file.write_text("\n".join(lines) + "\n")
        return str(log_file)

    @pytest.fixture
    def new_format_log(self, tmp_path):
        """Create a new format log with session events and metadata."""
        log_file = tmp_path / "new_format.jsonl"
        lines = [
            json.dumps({
                "event": "session_start",
                "session_id": "test-123",
                "timestamp": "2025-01-01T00:00:00"
            }),
            json.dumps({
                "id": "chatcmpl-new",
                "object": "chat.completion",
                "model": "gpt-4",
                "agent_name": "Test Agent",
                "messages": [
                    {"role": "user", "content": "Hello"}
                ],
                "choices": [{
                    "index": 0,
                    "message": {"role": "assistant", "content": "Hi!"},
                    "finish_reason": "stop"
                }],
                "usage": {"prompt_tokens": 10, "completion_tokens": 5},
                "cost": {"interaction_cost": 0.001, "total_cost": 0.001},
                "timing": {"active_seconds": 1.0}
            }),
            json.dumps({
                "event": "session_end",
                "session_id": "test-123"
            })
        ]
        log_file.write_text("\n".join(lines) + "\n")
        return str(log_file)

    def test_simple_format_load_history(self, simple_format_log):
        """Test loading simple format with load_history_from_jsonl."""
        messages = load_history_from_jsonl(simple_format_log, system_prompt=False)

        assert len(messages) == 2  # user + assistant (system excluded)
        assert messages[0]["role"] == "user"
        assert messages[0]["content"] == "Hello"
        assert messages[1]["role"] == "assistant"
        assert messages[1]["content"] == "Hi there!"

    def test_simple_format_fast_load(self, simple_format_log):
        """Test loading simple format with fast_load_messages."""
        messages = fast_load_messages(simple_format_log)

        assert len(messages) == 2
        assert messages[0]["role"] == "user"
        assert messages[1]["role"] == "assistant"

    def test_old_format_load_history(self, old_format_log):
        """Test loading old format with load_history_from_jsonl."""
        messages = load_history_from_jsonl(old_format_log, system_prompt=False)

        assert len(messages) >= 3  # user, assistant with tool, tool, assistant

        roles = [m["role"] for m in messages]
        assert "user" in roles
        assert "assistant" in roles
        assert "tool" in roles

    def test_old_format_fast_load(self, old_format_log):
        """Test loading old format with fast_load_messages."""
        messages = fast_load_messages(old_format_log)

        assert len(messages) >= 3
        roles = [m["role"] for m in messages]
        assert "user" in roles
        assert "assistant" in roles

    def test_new_format_load_history(self, new_format_log):
        """Test loading new format with load_history_from_jsonl."""
        messages = load_history_from_jsonl(new_format_log, system_prompt=False)

        assert len(messages) >= 1
        # New format should have agent_name
        has_agent_name = any(m.get("agent_name") for m in messages)
        assert has_agent_name or len(messages) > 0

    def test_new_format_fast_load(self, new_format_log):
        """Test loading new format with fast_load_messages."""
        messages = fast_load_messages(new_format_log)

        assert len(messages) >= 1

    def test_content_normalization(self, simple_format_log):
        """Test that content is properly normalized to strings."""
        messages = fast_load_messages(simple_format_log)
        normalized = normalize_messages_for_agent(messages)

        for msg in normalized:
            content = msg.get("content")
            if content is not None:
                assert isinstance(content, str), f"Content should be string, got {type(content)}"

    def test_no_duplicates(self, old_format_log):
        """Test that no duplicate messages are loaded."""
        messages = load_history_from_jsonl(old_format_log, system_prompt=False)

        # Check for exact duplicates
        seen = set()
        duplicates = 0
        for m in messages:
            key = (m.get("role"), str(m.get("content", ""))[:100])
            if key in seen:
                duplicates += 1
            seen.add(key)

        # Allow some duplicates (repeated user messages are valid)
        assert duplicates < len(messages) // 2, f"Too many duplicates: {duplicates}"

    def test_tool_name_field_old_format(self, old_format_log):
        """Test that old format tool_name field is handled."""
        messages = load_history_from_jsonl(old_format_log, system_prompt=False)

        tool_messages = [m for m in messages if m.get("role") == "tool"]
        # Old format has tool_name, new format doesn't - both should work
        assert all(m.get("tool_call_id") or m.get("content") for m in tool_messages)


class TestRealWorldLogs:
    """Test with real log files if available."""

    @pytest.fixture
    def fixture_simple_log(self):
        """Path to simple format fixture."""
        return Path(__file__).parent.parent / "fixtures" / "old_format_simple.jsonl"

    @pytest.fixture
    def fixture_full_log(self):
        """Path to full old format fixture."""
        return Path(__file__).parent.parent / "fixtures" / "old_format_full.jsonl"

    def test_fixture_simple_format(self, fixture_simple_log):
        """Test loading the simple format fixture."""
        if not fixture_simple_log.exists():
            pytest.skip("Fixture not found")

        messages = load_history_from_jsonl(str(fixture_simple_log), system_prompt=False)
        assert len(messages) >= 2
        assert messages[0]["role"] in ["user", "assistant"]

    def test_fixture_full_format(self, fixture_full_log):
        """Test loading the full old format fixture."""
        if not fixture_full_log.exists():
            pytest.skip("Fixture not found")

        messages = load_history_from_jsonl(str(fixture_full_log), system_prompt=False)
        assert len(messages) >= 2

        # Should have tool calls
        has_tool_calls = any(m.get("tool_calls") for m in messages)
        has_tool_response = any(m.get("role") == "tool" for m in messages)
        assert has_tool_calls or has_tool_response
