"""Conservative compatibility for local models that print a tool call as JSON.

Some Ollama models advertise native tool support yet occasionally return a
well-formed tool request inside assistant ``content`` instead of ``tool_calls``.
Jubi may promote that text to a tool call only when the requested tool is
already present in the exact allowlisted schema sent for this turn. This does
not grant any new capability; downstream Jubi/Hermes dispatch policy still
validates and executes the tool.
"""
from __future__ import annotations

import json
from typing import Any


def _allowed_tool_names(tools: list[dict] | None) -> set[str]:
    names: set[str] = set()
    for tool in tools or []:
        if not isinstance(tool, dict):
            continue
        function = tool.get('function')
        if not isinstance(function, dict):
            continue
        name = function.get('name')
        if isinstance(name, str) and name:
            names.add(name)
    return names


def extract_text_tool_call(content: Any, tools: list[dict] | None) -> dict | None:
    """Return one exact allowlisted text-encoded tool call, otherwise ``None``.

    Accepted shape::

        {"name": "tool_name", "arguments": {...}}

    The JSON object may be preceded by explanatory prose, but only trailing
    whitespace or a closing Markdown fence may follow it. Additional keys,
    unknown tools, non-object arguments, malformed JSON and multiple ambiguous
    candidates are rejected.
    """
    if not isinstance(content, str) or not content.strip():
        return None
    allowed = _allowed_tool_names(tools)
    if not allowed:
        return None

    decoder = json.JSONDecoder()
    candidates: list[dict] = []
    for index, char in enumerate(content):
        if char != '{':
            continue
        try:
            value, consumed = decoder.raw_decode(content[index:])
        except (ValueError, TypeError):
            continue
        remainder = content[index + consumed:].strip()
        if remainder not in {'', '```'}:
            continue
        if not isinstance(value, dict) or set(value) != {'name', 'arguments'}:
            continue
        name, arguments = value.get('name'), value.get('arguments')
        if name not in allowed or not isinstance(arguments, dict):
            continue
        candidates.append({'name': name, 'arguments': arguments})

    if len(candidates) != 1:
        return None
    return candidates[0]


def promote_text_tool_call(message: dict, tools: list[dict] | None) -> bool:
    """Promote one safe compatibility call into Ollama's native message shape."""
    if not isinstance(message, dict) or message.get('tool_calls'):
        return False
    call = extract_text_tool_call(message.get('content'), tools)
    if call is None:
        return False
    message['tool_calls'] = [{'function': call}]
    return True
