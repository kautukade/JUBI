"""Removed ``--resume`` / ``--logpath`` CLI flags surface a clear message."""

import os

# Avoid session recorder touching ~/.cai during heavy ``cai.cli`` import chain.
os.environ["CAI_DISABLE_SESSION_RECORDING"] = "true"

import sys

import pytest

from cai.cli import _exit_if_removed_resume_cli_flags


def test_removed_resume_flag_exits(monkeypatch):
    def fake_exit(code: int) -> None:
        raise SystemExit(code)

    monkeypatch.setattr(sys, "exit", fake_exit)
    with pytest.raises(SystemExit) as exc:
        _exit_if_removed_resume_cli_flags(["--resume", "last"])
    assert exc.value.code == 2


def test_removed_logpath_flag_exits(monkeypatch):
    def fake_exit(code: int) -> None:
        raise SystemExit(code)

    monkeypatch.setattr(sys, "exit", fake_exit)
    with pytest.raises(SystemExit) as exc:
        _exit_if_removed_resume_cli_flags(["--logpath", "/tmp"])
    assert exc.value.code == 2


def test_no_removed_flags_returns():
    _exit_if_removed_resume_cli_flags(["--version"])
