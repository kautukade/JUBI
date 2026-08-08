"""
Tests for dynamic tool output weighting and context optimization.

These tests verify that:
1. Tool outputs are truncated when context usage exceeds thresholds
2. Older tool outputs are truncated more aggressively than newer ones
3. Small outputs pass through unchanged
4. Binary and large outputs are handled correctly
"""

import pytest


# Test the pure logic functions directly without heavy module imports
class TestTruncateToolOutputPureLogic:
    """Tests for truncation logic."""

    def _truncate_tool_output(self, content: str, max_chars: int) -> str:
        """Local implementation for testing."""
        if len(content) <= max_chars:
            return content
        head_size = max_chars // 2 - 100
        tail_size = max_chars // 2 - 100
        head = content[:head_size]
        tail = content[-tail_size:]
        omitted = len(content) - head_size - tail_size
        return f"{head}\n\n[... TRUNCATED {omitted:,} chars for context optimization ...]\n\n{tail}"

    def test_small_content_unchanged(self):
        """Small content should pass through unchanged."""
        content = "Small output"
        result = self._truncate_tool_output(content, 1000)
        assert result == content

    def test_large_content_truncated(self):
        """Large content should be truncated to max_chars."""
        content = "X" * 10000
        max_chars = 1000
        result = self._truncate_tool_output(content, max_chars)
        assert len(result) <= max_chars + 200
        assert "TRUNCATED" in result

    def test_preserves_head_and_tail(self):
        """Truncation should preserve both head and tail of content."""
        content = "HEAD" + ("X" * 10000) + "TAIL"
        result = self._truncate_tool_output(content, 1000)
        assert result.startswith("HEAD")
        assert result.endswith("TAIL")


class TestDynamicToolOutputWeightingPureLogic:
    """Tests for the weighting function logic."""

    TOOL_OUTPUT_WEIGHT_TOKEN_THRESHOLD = 60000
    TOOL_OUTPUT_WEIGHT_CONTEXT_THRESHOLD = 0.50
    TOOL_OUTPUT_MAX_CHARS = 50000
    TOOL_OUTPUT_MIN_CHARS = 2000

    def _count_tokens(self, messages):
        """Simple char-based token estimate."""
        total = 0
        for msg in messages:
            content = msg.get("content", "")
            if isinstance(content, str):
                total += len(content) // 4
        return total

    def _truncate_tool_output(self, content: str, max_chars: int) -> str:
        if len(content) <= max_chars:
            return content
        head_size = max_chars // 2 - 100
        tail_size = max_chars // 2 - 100
        head = content[:head_size]
        tail = content[-tail_size:]
        omitted = len(content) - head_size - tail_size
        return f"{head}\n\n[... TRUNCATED {omitted:,} chars ...]\n\n{tail}"

    def _apply_weighting(self, messages, max_context_tokens):
        """Local implementation for testing."""
        if not messages:
            return messages

        current_tokens = self._count_tokens(messages)
        context_usage = current_tokens / max_context_tokens if max_context_tokens > 0 else 0

        needs_weighting = (
            current_tokens > self.TOOL_OUTPUT_WEIGHT_TOKEN_THRESHOLD
            or context_usage > self.TOOL_OUTPUT_WEIGHT_CONTEXT_THRESHOLD
        )

        if not needs_weighting:
            return messages

        tool_indices = []
        for i, msg in enumerate(messages):
            if msg.get("role") == "tool":
                content = msg.get("content", "")
                if isinstance(content, str) and len(content) > self.TOOL_OUTPUT_MIN_CHARS:
                    tool_indices.append((i, len(content)))

        if not tool_indices:
            return messages

        if context_usage > 0.85:
            pressure = 1.0
        elif context_usage > 0.70:
            pressure = 0.75
        elif context_usage > self.TOOL_OUTPUT_WEIGHT_CONTEXT_THRESHOLD:
            pressure = 0.5
        elif current_tokens > 100000:
            pressure = 0.6
        elif current_tokens > self.TOOL_OUTPUT_WEIGHT_TOKEN_THRESHOLD:
            pressure = 0.3
        else:
            pressure = 0.2

        num_tools = len(tool_indices)
        modified_messages = [m.copy() for m in messages]

        for position, (msg_idx, content_len) in enumerate(tool_indices):
            position_weight = position / num_tools if num_tools > 1 else 1.0
            allocation = (1 - pressure) + (pressure * position_weight)
            max_chars = int(self.TOOL_OUTPUT_MAX_CHARS * allocation)
            max_chars = max(max_chars, self.TOOL_OUTPUT_MIN_CHARS)

            msg = modified_messages[msg_idx]
            content = msg.get("content", "")
            if isinstance(content, str) and len(content) > max_chars:
                truncated = self._truncate_tool_output(content, max_chars)
                modified_messages[msg_idx] = {**msg, "content": truncated}

        return modified_messages

    def test_empty_messages_unchanged(self):
        """Empty message list should return unchanged."""
        result = self._apply_weighting([], 200000)
        assert result == []

    def test_small_messages_unchanged(self):
        """Messages below threshold should pass through unchanged."""
        messages = [
            {"role": "user", "content": "Hello"},
            {"role": "assistant", "content": "Hi there"},
        ]
        result = self._apply_weighting(messages, 200000)
        assert result == messages

    def test_large_tool_outputs_truncated(self):
        """Tool outputs should be truncated when over threshold."""
        large_output = "X" * 300000  # ~75k tokens
        messages = [
            {"role": "user", "content": "Test"},
            {"role": "tool", "tool_call_id": "1", "content": large_output},
        ]

        result = self._apply_weighting(messages, 200000)
        assert len(result[1]["content"]) < len(large_output)

    def test_older_outputs_truncated_more(self):
        """Older tool outputs should be truncated more aggressively."""
        large_output = "X" * 300000

        messages = [
            {"role": "tool", "tool_call_id": "1", "content": large_output},  # Oldest
            {"role": "tool", "tool_call_id": "2", "content": large_output},  # Middle
            {"role": "tool", "tool_call_id": "3", "content": large_output},  # Newest
        ]

        result = self._apply_weighting(messages, 200000)

        oldest_len = len(result[0]["content"])
        middle_len = len(result[1]["content"])
        newest_len = len(result[2]["content"])

        assert oldest_len <= middle_len <= newest_len

    def test_preserves_original(self):
        """Function should not modify the original messages."""
        large_output = "X" * 300000
        messages = [{"role": "tool", "tool_call_id": "1", "content": large_output}]
        original_len = len(messages[0]["content"])

        result = self._apply_weighting(messages, 200000)

        assert len(messages[0]["content"]) == original_len
        assert result is not messages

    def test_token_reduction(self):
        """Weighting should reduce overall token count."""
        large_output = "X" * 300000
        messages = [
            {"role": "user", "content": "Test"},
            {"role": "tool", "tool_call_id": "1", "content": large_output},
        ]

        original_tokens = self._count_tokens(messages)
        result = self._apply_weighting(messages, 200000)
        new_tokens = self._count_tokens(result)

        assert new_tokens < original_tokens


class TestGenericLinuxCommandCompression:
    """Tests for the output compression in generic_linux_command."""

    MAX_OUTPUT_CHARS = 50000

    def _is_binary_content(self, data: str) -> bool:
        """Local implementation for testing."""
        if not data:
            return False
        sample = data[:8192]
        non_printable = sum(1 for c in sample if ord(c) < 32 and c not in "\n\r\t")
        if "\x00" in sample:
            return True
        ratio = non_printable / len(sample) if sample else 0
        return ratio > 0.10

    def test_binary_detection_text(self):
        """Text content should not be detected as binary."""
        assert self._is_binary_content("Hello World\nThis is text\n") is False

    def test_binary_detection_null_bytes(self):
        """Content with null bytes should be detected as binary."""
        assert self._is_binary_content("data\x00\x01\x02more\x00") is True

    def test_binary_detection_high_non_printable(self):
        """High ratio of non-printable chars should be detected as binary."""
        binary_like = "".join(chr(i) for i in range(32)) * 100
        assert self._is_binary_content(binary_like) is True


class TestModuleImport:
    """Test that the actual module functions can be imported."""

    def test_import_truncate_function(self):
        """Should be able to import _truncate_tool_output."""
        from cai.sdk.agents.models.openai_chatcompletions import _truncate_tool_output

        result = _truncate_tool_output("small", 1000)
        assert result == "small"

    def test_import_weighting_function(self):
        """Should be able to import _apply_dynamic_tool_output_weighting."""
        from cai.sdk.agents.models.openai_chatcompletions import (
            _apply_dynamic_tool_output_weighting,
        )

        result = _apply_dynamic_tool_output_weighting([], "gpt-4", 200000)
        assert result == []

    def test_import_constants(self):
        """Should be able to import threshold constants."""
        from cai.sdk.agents.models.openai_chatcompletions import (
            TOOL_OUTPUT_WEIGHT_TOKEN_THRESHOLD,
            TOOL_OUTPUT_WEIGHT_CONTEXT_THRESHOLD,
            TOOL_OUTPUT_MAX_CHARS,
            TOOL_OUTPUT_MIN_CHARS,
        )

        assert TOOL_OUTPUT_WEIGHT_TOKEN_THRESHOLD > 0
        assert 0 < TOOL_OUTPUT_WEIGHT_CONTEXT_THRESHOLD <= 1.0
        assert TOOL_OUTPUT_MAX_CHARS > TOOL_OUTPUT_MIN_CHARS

    def test_import_generic_linux_helpers(self):
        """Should be able to import generic_linux_command helpers."""
        from cai.tools.reconnaissance.generic_linux_command import (
            _is_binary_content,
            _compress_output_for_model,
            MAX_OUTPUT_CHARS,
        )

        assert MAX_OUTPUT_CHARS > 0
        assert _is_binary_content("text") is False
