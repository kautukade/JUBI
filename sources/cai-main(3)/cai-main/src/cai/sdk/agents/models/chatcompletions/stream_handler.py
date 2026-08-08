"""Async streaming processing for chat completions.

Contains the _StreamingState dataclass used to track state during
streamed responses.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from openai.types.responses import (
    ResponseFunctionToolCall,
    ResponseOutputRefusal,
    ResponseOutputText,
)


@dataclass
class StreamingState:
    """Mutable state tracked while consuming a streamed response."""

    started: bool = False
    sequence_number: int = 0
    text_content_index_and_output: tuple[int, ResponseOutputText] | None = None
    refusal_content_index_and_output: tuple[int, ResponseOutputRefusal] | None = None
    function_calls: dict[int, ResponseFunctionToolCall] = field(default_factory=dict)

    def next_sequence_number(self) -> int:
        """Return the current sequence number and increment it."""
        seq = self.sequence_number
        self.sequence_number += 1
        return seq
