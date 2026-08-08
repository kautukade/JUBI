"""Tests for CAI_AVOID_SUDO hard blocks on privilege-escalation shell commands."""

from __future__ import annotations

import pytest

from cai.util.user_prompts import avoid_sudo_command_blocked


@pytest.mark.parametrize(
    "command",
    [
        "sudo id",
        "sudo -n true",
        "cd /tmp && sudo ls",
        "pkexec whoami",
        "doas id",
        "su - root",
        "su",
    ],
)
def test_avoid_sudo_blocks_when_enabled(monkeypatch, command: str):
    monkeypatch.setenv("CAI_AVOID_SUDO", "true")
    blocked, msg = avoid_sudo_command_blocked(command)
    assert blocked is True
    assert msg


@pytest.mark.parametrize(
    "command",
    [
        "id",
        "whoami",
        "ls -la /tmp",
        "grep -E 'sudo' README.md",
    ],
)
def test_avoid_sudo_allows_when_enabled(monkeypatch, command: str):
    monkeypatch.setenv("CAI_AVOID_SUDO", "true")
    blocked, _ = avoid_sudo_command_blocked(command)
    assert blocked is False


def test_avoid_sudo_off_by_default(monkeypatch):
    monkeypatch.delenv("CAI_AVOID_SUDO", raising=False)
    blocked, msg = avoid_sudo_command_blocked("sudo id")
    assert blocked is False
    assert msg == ""
