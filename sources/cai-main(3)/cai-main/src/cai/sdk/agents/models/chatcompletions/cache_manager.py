"""Prompt caching / cache_control logic for Anthropic and Gemini models.

Centralizes the repeated cache normalization and application patterns
used across get_response, stream_response, and _fetch_response.
"""

from __future__ import annotations

import hashlib
import json
import os
from typing import Any

from ...logger import logger


def normalize_messages_for_cache(converted_messages: list[dict[str, Any]]) -> None:
    """Normalize messages to block format for consistent cache_control support.

    Mutates messages in-place. All non-tool messages get their string content
    converted to ``[{"type": "text", "text": ...}]`` block format, and any
    existing ``cache_control`` annotations are stripped so fresh ones can be
    applied.

    Args:
        converted_messages: List of message dicts to normalize in-place.
    """
    for msg in converted_messages:
        role = msg.get("role")
        content = msg.get("content")

        # Skip messages without content (e.g., assistant with only tool_calls)
        if content is None:
            continue

        # Normalize ALL messages to block format
        if isinstance(content, str):
            msg["content"] = [{"type": "text", "text": content}]
        elif isinstance(content, list):
            normalized = []
            for block in content:
                if isinstance(block, str):
                    normalized.append({"type": "text", "text": block})
                elif isinstance(block, dict):
                    # Remove any existing cache_control -- we'll add fresh ones
                    block_copy = {k: v for k, v in block.items() if k != "cache_control"}
                    normalized.append(block_copy)
                else:
                    normalized.append(block)
            msg["content"] = normalized

        # Remove message-level cache_control
        if "cache_control" in msg:
            del msg["cache_control"]


def _can_have_cache_control(msg: dict[str, Any]) -> bool:
    """Check if a message can have cache_control applied."""
    content = msg.get("content")
    # Assistant with only tool_calls -- no content to add cache_control
    if content is None and msg.get("tool_calls"):
        return False
    # Must have list content (normalized block format)
    return isinstance(content, list) and len(content) > 0


def apply_cache_control(converted_messages: list[dict[str, Any]]) -> list[int]:
    """Determine and apply cache breakpoints to messages.

    Strategy (from Anthropic docs):
    1. Always mark the system message for cache rebuild after expiry.
    2. Mark the last cacheable message for incremental caching.

    Args:
        converted_messages: Normalized message list (mutated in-place).

    Returns:
        List of indices where cache_control was applied.
    """
    cache_indices: list[int] = []

    # 1. Find and mark system message
    for i, msg in enumerate(converted_messages):
        if msg.get("role") == "system":
            cache_indices.append(i)
            break

    # 2. Find the last cacheable message
    for i in range(len(converted_messages) - 1, -1, -1):
        if _can_have_cache_control(converted_messages[i]):
            if i not in cache_indices:
                cache_indices.append(i)
            break

    # Apply cache_control to breakpoint messages
    for idx in cache_indices:
        msg = converted_messages[idx]
        content = msg.get("content")
        if isinstance(content, list) and content:
            last_block = content[-1]
            if isinstance(last_block, dict):
                last_block["cache_control"] = {"type": "ephemeral"}

    return cache_indices


def normalize_and_apply_cache(
    converted_messages: list[dict[str, Any]],
    model_str: str,
) -> None:
    """Full cache pipeline: normalize messages then apply cache_control.

    Only applies to Claude and Gemini models.

    Args:
        converted_messages: Message list to process (mutated in-place).
        model_str: Lowercased model string for provider detection.
    """
    if ("claude" not in model_str and "gemini" not in model_str) or not converted_messages:
        return

    normalize_messages_for_cache(converted_messages)
    cache_indices = apply_cache_control(converted_messages)

    logger.debug(
        f"[CACHE] Applied cache_control to indices: {cache_indices}, "
        f"total messages: {len(converted_messages)}"
    )


def has_cache_control(messages: list[dict[str, Any]]) -> bool:
    """Check if any message has cache_control (at message level or in content blocks)."""
    for msg in messages:
        if msg.get("cache_control"):
            return True
        content = msg.get("content")
        if isinstance(content, list):
            for block in content:
                if isinstance(block, dict) and block.get("cache_control"):
                    return True
    return False


def debug_cache_messages(
    converted_messages: list[dict[str, Any]],
    cache_indices: list[int],
    previous_turn_hashes: list[str],
) -> list[str]:
    """Print detailed cache debug info and return current turn hashes.

    Only executes when CAI_SHOW_CACHE env var is set.

    Args:
        converted_messages: Messages after cache processing.
        cache_indices: Indices where cache_control was applied.
        previous_turn_hashes: Hashes from the previous turn for comparison.

    Returns:
        Current turn's message hashes for next-turn comparison.
    """
    if os.getenv("CAI_SHOW_CACHE", "").lower() not in ("true", "1", "yes"):
        return previous_turn_hashes

    print(f"[CACHE-DEBUG] Applied cache_control to indices: {cache_indices}, "
          f"total messages: {len(converted_messages)}")

    current_turn_hashes: list[str] = []

    for i, msg in enumerate(converted_messages):
        role = msg.get("role", "?")
        content = msg.get("content")

        # Compute hash excluding cache_control for comparison
        msg_for_hash = msg.copy()
        if isinstance(msg_for_hash.get("content"), list):
            clean_content = []
            for block in msg_for_hash["content"]:
                if isinstance(block, dict):
                    clean_block = {k: v for k, v in block.items() if k != "cache_control"}
                    clean_content.append(clean_block)
                else:
                    clean_content.append(block)
            msg_for_hash["content"] = clean_content
        msg_hash = hashlib.md5(
            json.dumps(msg_for_hash, sort_keys=True, default=str).encode()
        ).hexdigest()[:8]
        current_turn_hashes.append(msg_hash)

        # Check match with previous turn
        match_marker = ""
        if i < len(previous_turn_hashes):
            if previous_turn_hashes[i] == msg_hash:
                match_marker = " MATCH"
            else:
                match_marker = f" CHANGED (was {previous_turn_hashes[i]})"

        if isinstance(content, list) and content:
            last_block = content[-1]
            has_cc = isinstance(last_block, dict) and "cache_control" in last_block
            cc_marker = " CC" if has_cc else ""
            text_preview = ""
            for block in content:
                if isinstance(block, dict) and block.get("type") == "text":
                    text = block.get("text", "")[:40].replace("\n", " ")
                    text_preview = (
                        f" '{text}...'"
                        if len(block.get("text", "")) > 40
                        else f" '{text}'"
                    )
                    break
            print(
                f"  [{i}] {role} [hash:{msg_hash}]: "
                f"list({len(content)} blocks){cc_marker}{match_marker}{text_preview}"
            )
        elif isinstance(content, str):
            content_preview = (
                content[:30].replace("\n", " ") + "..."
                if len(content) > 30
                else content.replace("\n", " ")
            )
            print(
                f"  [{i}] {role} [hash:{msg_hash}]: "
                f"string({len(content)} chars) - SHOULD BE LIST!{match_marker} '{content_preview}'"
            )
        elif content is None:
            has_tc = "tool_calls" in msg
            tc_info = ""
            if has_tc:
                tc_list = msg.get("tool_calls", [])
                tc_ids = [tc.get("id", "?")[:12] for tc in tc_list]
                tc_info = f", tool_calls={len(tc_list)} ids={tc_ids}"
            print(f"  [{i}] {role} [hash:{msg_hash}]: None{tc_info}{match_marker}")

    # Summary of matches
    if previous_turn_hashes:
        common_len = min(len(current_turn_hashes), len(previous_turn_hashes))
        matches = sum(
            1
            for i in range(common_len)
            if current_turn_hashes[i] == previous_turn_hashes[i]
        )
        print(
            f"[CACHE-DEBUG] PREFIX MATCH: {matches}/{common_len} "
            f"messages match previous turn (cache needs prefix match)"
        )
        if matches < common_len:
            for i in range(common_len):
                if current_turn_hashes[i] != previous_turn_hashes[i]:
                    print(
                        f"[CACHE-DEBUG] FIRST MISMATCH at index {i}: "
                        f"messages diverge here, cache breaks"
                    )
                    msg_json = json.dumps(
                        converted_messages[i], indent=2, default=str
                    )[:500]
                    print(
                        f"[CACHE-DEBUG] Current message[{i}] JSON (truncated):\n{msg_json}"
                    )
                    break

    print(
        f"[CACHE-DEBUG] Stored {len(current_turn_hashes)} "
        f"message hashes for next turn comparison"
    )
    return current_turn_hashes
