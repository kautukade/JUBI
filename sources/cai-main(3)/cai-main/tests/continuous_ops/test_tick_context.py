"""Tests for continuous-ops tick-to-tick context markers."""

from __future__ import annotations

from pathlib import Path

from cai.continuous_ops.tick_context import (
    extract_tick_context_block,
    extract_tick_context_from_file,
    persist_tick_context,
    rolling_context_for_prompt,
)


def test_extract_tick_context_block_multiline() -> None:
    log = """some noise
<<<COPS_TICK_CONTEXT>>>
line one
line two
<<<END_COPS_TICK_CONTEXT>>>
tail
"""
    got = extract_tick_context_block(log)
    assert got == "line one\nline two"


def test_extract_tick_context_block_missing() -> None:
    assert extract_tick_context_block("no markers") is None


def test_persist_and_rolling_roundtrip(tmp_path: Path) -> None:
    run = tmp_path / "run0"
    run.mkdir()
    persist_tick_context(run, "first summary")
    persist_tick_context(run, "second summary")
    roll = rolling_context_for_prompt(run)
    assert "first summary" in roll
    assert "second summary" in roll


def test_extract_from_file(tmp_path: Path) -> None:
    p = tmp_path / "iter.log"
    p.write_text(
        "x\n<<<COPS_TICK_CONTEXT>>>\nfrom file\n<<<END_COPS_TICK_CONTEXT>>>\n",
        encoding="utf-8",
    )
    assert extract_tick_context_from_file(p) == "from file"
