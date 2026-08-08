"""Token counting utilities using tiktoken.

Provides consistent token counting for messages and text,
plus reasoning compatibility checks for Claude models.
"""

from __future__ import annotations

import tiktoken


def _check_reasoning_compatibility(messages):
    """
    Check if message history is compatible with Claude reasoning/thinking.

    According to Claude 4 docs, when reasoning is enabled, the final assistant
    message must start with a thinking block. If there are assistant messages
    with regular text content, reasoning should be disabled.

    Args:
        messages: List of message dictionaries

    Returns:
        bool: True if compatible with reasoning, False otherwise
    """
    if not messages:
        return True  # Empty messages are compatible

    # Find the last assistant message
    last_assistant_msg = None
    for msg in reversed(messages):
        if msg.get("role") == "assistant":
            last_assistant_msg = msg
            break

    if not last_assistant_msg:
        return True  # No assistant messages, compatible

    # Check if the last assistant message has regular text content
    content = last_assistant_msg.get("content")
    if content:
        # If it's a string with text content, not compatible
        if isinstance(content, str) and content.strip():
            return False
        # If it's a list, check for text content blocks
        elif isinstance(content, list):
            for block in content:
                if isinstance(block, dict):
                    if block.get("type") == "text" and block.get("text", "").strip():
                        return False

    # Check if message has tool_calls (these are compatible)
    if last_assistant_msg.get("tool_calls"):
        return True

    # If no content or only thinking blocks, it's compatible
    return True


def count_tokens_with_tiktoken(text_or_messages):
    """
    Count tokens consistently using tiktoken library.
    Works with both strings and message lists.
    Returns a tuple of (input_tokens, reasoning_tokens).
    """
    if not text_or_messages:
        return 0, 0

    try:
        # Try to use cl100k_base encoding (used by GPT-4 and GPT-3.5-turbo)
        encoding = tiktoken.get_encoding("cl100k_base")
    except Exception:
        # Fall back to GPT-2 encoding if cl100k is not available
        try:
            encoding = tiktoken.get_encoding("gpt2")
        except Exception:
            # If tiktoken fails, fall back to character estimate
            if isinstance(text_or_messages, str):
                return len(text_or_messages) // 4, 0
            elif isinstance(text_or_messages, list):
                total_len = 0
                for msg in text_or_messages:
                    if isinstance(msg, dict) and "content" in msg:
                        if isinstance(msg["content"], str):
                            total_len += len(msg["content"])
                return total_len // 4, 0
            else:
                return 0, 0

    # Process different input types
    if isinstance(text_or_messages, str):
        token_count = len(encoding.encode(text_or_messages))
        return token_count, 0
    elif isinstance(text_or_messages, list):
        total_tokens = 0
        reasoning_tokens = 0

        # Add tokens for the messages format (ChatML format overhead)
        # Each message has a base overhead (usually ~4 tokens)
        total_tokens += len(text_or_messages) * 4

        for msg in text_or_messages:
            if isinstance(msg, dict):
                # Add tokens for role
                if "role" in msg:
                    total_tokens += len(encoding.encode(msg["role"]))

                # Count content tokens
                if "content" in msg and msg["content"]:
                    if isinstance(msg["content"], str):
                        content_tokens = len(encoding.encode(msg["content"]))
                        total_tokens += content_tokens

                        # Count tokens in assistant messages as reasoning tokens
                        if msg.get("role") == "assistant":
                            reasoning_tokens += content_tokens
                    elif isinstance(msg["content"], list):
                        for content_part in msg["content"]:
                            if isinstance(content_part, dict) and "text" in content_part:
                                part_tokens = len(encoding.encode(content_part["text"]))
                                total_tokens += part_tokens
                                if msg.get("role") == "assistant":
                                    reasoning_tokens += part_tokens

        return total_tokens, reasoning_tokens
    else:
        return 0, 0
