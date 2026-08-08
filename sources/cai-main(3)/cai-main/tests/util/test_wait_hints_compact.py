from io import StringIO
from types import SimpleNamespace

import pytest
from rich.console import Console
from rich.text import Text

import cai.util.streaming as streaming
import cai.util.wait_hints as wait_hints
from cai.output import TaskRecord
from cai.repl.ui.compact_renderer import CompactCLIHandler, _row_for_record


def test_compact_owner_suppresses_legacy_footer_refresh(monkeypatch):
    calls = []

    def fake_refresh():
        calls.append("refresh")

    monkeypatch.setattr(
        "cai.util.streaming.refresh_tool_wait_displays",
        fake_refresh,
    )

    wait_hints.set_compact_live_owner(True)
    try:
        wait_hints._request_footer_ui_refresh()
    finally:
        wait_hints.set_compact_live_owner(False)

    assert calls == []


def test_clear_wait_hints_removes_published_body():
    wait_hints._set_model_wait_body("model wait")
    wait_hints._set_tool_wait_body("tool wait")

    assert wait_hints.get_current_wait_hint_body()

    wait_hints.clear_wait_hints()

    assert wait_hints.get_current_wait_hint_body() is None


@pytest.mark.asyncio
async def test_tool_wait_loop_under_compact_owner_only_publishes_body(monkeypatch):
    monkeypatch.setattr(wait_hints, "tool_wait_hints_enabled", lambda: True)

    wait_hints.set_compact_live_owner(True)
    loop = wait_hints._WaitHintLoop(
        mode="tool",
        tool_label="generic_linux_command",
        exec_summary="sleep 10",
    )
    try:
        await loop.start()

        assert wait_hints.get_current_wait_hint_body()
        assert wait_hints.get_tool_wait_footer_renderable() is None
    finally:
        await loop.stop()
        wait_hints.set_compact_live_owner(False)


def test_compact_final_dismiss_releases_ownership_on_flush(monkeypatch):
    owner_changes = []

    def fake_set_owner(active):
        owner_changes.append(active)

    monkeypatch.setattr("cai.util.wait_hints.set_compact_live_owner", fake_set_owner)

    handler = CompactCLIHandler(
        Console(file=StringIO(), force_terminal=True, width=80)
    )
    handler._owns_wait_hints = True

    handler.dismiss_for_final_output()
    assert owner_changes == []

    handler.flush()
    assert owner_changes == [False]


def test_finish_agent_streaming_clears_wait_ui_before_final(monkeypatch):
    calls = []

    def fake_prepare():
        calls.append("prepare")

    monkeypatch.setattr(streaming, "_prepare_terminal_for_final_agent_output", fake_prepare)
    monkeypatch.setattr(streaming, "_print_pricing_footer", lambda *args, **kwargs: None)

    context = {
        "content": Text("final answer"),
        "is_started": False,
        "context_key": "test",
        "header": Text("Agent"),
        "live": None,
    }
    streaming.create_agent_streaming_context._active_streaming = {"test": context}

    assert streaming.finish_agent_streaming(context, {"has_tool_calls": False}) is True
    assert calls == ["prepare"]


def test_cli_print_agent_messages_clears_wait_ui_before_final(monkeypatch):
    calls = []

    def fake_prepare():
        calls.append("prepare")

    monkeypatch.setattr(streaming, "_prepare_terminal_for_final_agent_output", fake_prepare)
    monkeypatch.setattr(streaming, "_print_pricing_footer", lambda *args, **kwargs: None)

    streaming.cli_print_agent_messages(
        agent_name="Agent",
        message=SimpleNamespace(content="final answer", tool_calls=None),
        counter=1,
        model="test-model",
        debug=False,
        suppress_empty=True,
    )

    assert calls == ["prepare"]


def test_compact_row_hides_primary_agent_id_and_agent_label():
    row = _row_for_record(
        TaskRecord(
            task_id="task-1",
            turn_id="turn-1",
            agent_name="Red Team Agent",
            agent_id="P0",
            tool_name="generic_linux_command",
            label="nmap -sV 127.0.0.1",
            started_at=0.0,
            call_id="call-1",
        ),
        now=1.0,
        tick=0,
    )

    assert "Red Team Agent ─ nmap" in row.plain
    assert "[P0]" not in row.plain
    assert " AGENT " not in row.plain


def test_compact_row_keeps_parallel_agent_id():
    row = _row_for_record(
        TaskRecord(
            task_id="task-1",
            turn_id="turn-1",
            agent_name="Red Team Agent",
            agent_id="P1",
            tool_name="generic_linux_command",
            label="nmap -sV 127.0.0.1",
            started_at=0.0,
            call_id="call-1",
        ),
        now=1.0,
        tick=0,
    )

    assert "Red Team Agent [P1] ─ nmap" in row.plain
    assert " AGENT " not in row.plain


def test_set_model_wait_retry_overlay_overrides_model_body():
    """``set_model_wait_retry_overlay`` shadows the default body and ``None`` clears it."""
    try:
        wait_hints.set_model_wait_retry_overlay("Rate budget reached, pacing for 5s…")
        assert (
            wait_hints._model_body(0.0, {})
            == "Rate budget reached, pacing for 5s…"
        )
    finally:
        wait_hints.set_model_wait_retry_overlay(None)
    assert wait_hints._model_body(0.0, {}) != "Rate budget reached, pacing for 5s…"
