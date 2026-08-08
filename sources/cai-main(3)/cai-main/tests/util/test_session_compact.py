"""Tests for session-wide compaction and agent handoff."""

import os

import pytest

from cai.util.session_compact import (
    ENV_HANDOFF,
    ENV_SESSION_SUMMARY,
    extract_compact_block,
    extract_recent_findings,
    get_keep_recent_messages,
    prepare_agent_handoff,
    record_compaction_result,
    set_session_compact_summary,
    get_session_compact_summary,
    consume_agent_handoff,
)


def test_get_keep_recent_messages_default(monkeypatch):
    monkeypatch.delenv("CAI_COMPACT_KEEP_RECENT", raising=False)
    assert get_keep_recent_messages() == 10


def test_get_keep_recent_messages_clamped(monkeypatch):
    monkeypatch.setenv("CAI_COMPACT_KEEP_RECENT", "99")
    assert get_keep_recent_messages() == 16
    monkeypatch.setenv("CAI_COMPACT_KEEP_RECENT", "2")
    assert get_keep_recent_messages() == 4


def test_extract_compact_block():
    si = """foo
<compacted_context>
IP 10.0.0.1 found
</compacted_context>
bar"""
    assert "10.0.0.1" in (extract_compact_block(si) or "")


def test_extract_recent_findings_ips():
    history = [
        {"role": "assistant", "content": "Vuln 4: SQLi on 192.168.1.10 port 443"},
        {"role": "user", "content": "continue"},
    ]
    findings = extract_recent_findings(history, n=3)
    assert any("192.168.1.10" in f for f in findings)


def test_session_summary_env(monkeypatch):
    monkeypatch.delenv(ENV_SESSION_SUMMARY, raising=False)
    set_session_compact_summary("## Targets\n10.0.0.5", agent_name="Red Team Agent")
    assert "10.0.0.5" in (get_session_compact_summary() or "")


def test_prepare_and_consume_handoff(monkeypatch):
    monkeypatch.delenv(ENV_HANDOFF, raising=False)
    record_compaction_result(
        "## Vulnerabilities\n1. XSS",
        "Red Team Agent",
        [{"role": "assistant", "content": "pending: run nmap on 10.0.0.2"}],
    )
    handoff = prepare_agent_handoff(
        "Red Team Agent",
        [{"role": "assistant", "content": "Vuln 5: IDOR on 10.0.0.3"}],
        '<compacted_context>\n## IPs\n10.0.0.3\n</compacted_context>',
        to_agent_name="Web App Pentester",
    )
    assert "<agent_handoff>" in handoff
    assert "10.0.0.3" in handoff
    consumed = consume_agent_handoff()
    assert consumed
    assert not os.getenv(ENV_HANDOFF)
