"""Regression: mission planner responses are often markdown-fenced JSON."""

from __future__ import annotations

from cai.continuous_ops.model_parse import _extract_json_object, _strip_markdown_json_fence


def test_strip_markdown_json_fence() -> None:
    raw = '```json\n{"tasks": ["a"], "tasks_markdown": "- a"}\n```'
    assert '"tasks"' in _strip_markdown_json_fence(raw)


def test_extract_json_plain() -> None:
    assert _extract_json_object('{"tick_seconds": 30, "tasks": ["x"]}') == {
        "tick_seconds": 30,
        "tasks": ["x"],
    }


def test_extract_json_fenced() -> None:
    body = '```json\n{\n  "tasks": ["check disk", "check load"],\n  "tasks_markdown": "- a\\n- b"\n}\n```'
    data = _extract_json_object(body)
    assert data is not None
    assert data["tasks"] == ["check disk", "check load"]


def test_extract_json_preamble() -> None:
    body = 'Here is the plan:\n```json\n{"tasks": ["t1"], "tasks_markdown": "- t1"}\n```\nHope this helps.'
    data = _extract_json_object(body)
    assert data is not None
    assert data["tasks"] == ["t1"]


def test_extract_json_trailing_after_brace() -> None:
    body = '{"tasks": ["only"]}\n\n(Some notes)'
    data = _extract_json_object(body)
    assert data is not None
    assert data["tasks"] == ["only"]
