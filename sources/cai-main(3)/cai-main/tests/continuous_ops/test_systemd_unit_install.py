"""Tests for systemd --user install helpers (mocked subprocess)."""

from __future__ import annotations

from pathlib import Path

from cai.continuous_ops.systemd_unit import enable_linger_for_session_user, install_user_unit


def test_install_user_unit_success(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.setenv("HOME", str(tmp_path))
    unit = tmp_path / "cai-cops-test.service"
    unit.write_text("[Unit]\nDescription=x\n[Service]\nExecStart=/bin/true\n", encoding="utf-8")
    recorded: list[list[str]] = []

    def fake_run(cmd: list[str], **_kwargs):
        recorded.append(list(cmd))

        class Proc:
            returncode = 0
            stderr = ""
            stdout = ""

        return Proc()

    monkeypatch.setattr("cai.continuous_ops.systemd_unit.subprocess.run", fake_run)
    ok, err = install_user_unit(unit)
    assert ok is True
    assert err == ""
    assert (tmp_path / ".config" / "systemd" / "user" / unit.name).is_file()
    assert ["systemctl", "--user", "daemon-reload"] in recorded
    assert ["systemctl", "--user", "enable", "--now", unit.name] in recorded


def test_install_user_unit_systemctl_failure(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.setenv("HOME", str(tmp_path))
    unit = tmp_path / "cai-cops-bad.service"
    unit.write_text("[Unit]\nDescription=x\n[Service]\nExecStart=/bin/true\n", encoding="utf-8")

    def fake_run(cmd: list[str], **_kwargs):
        class Proc:
            returncode = 1 if "enable" in cmd else 0
            stderr = "no user bus"
            stdout = ""

        return Proc()

    monkeypatch.setattr("cai.continuous_ops.systemd_unit.subprocess.run", fake_run)
    ok, err = install_user_unit(unit)
    assert ok is False
    assert "no user bus" in err


def test_enable_linger_for_session_user(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.setenv("USER", "tester")

    def fake_run(cmd: list[str], **_kwargs):
        assert cmd == ["loginctl", "enable-linger", "tester"]

        class Proc:
            returncode = 0
            stderr = ""
            stdout = ""

        return Proc()

    monkeypatch.setattr("cai.continuous_ops.systemd_unit.subprocess.run", fake_run)
    ok, err = enable_linger_for_session_user()
    assert ok is True
    assert err == ""


def test_enable_linger_no_user(monkeypatch) -> None:
    monkeypatch.delenv("USER", raising=False)
    monkeypatch.delenv("LOGNAME", raising=False)
    ok, err = enable_linger_for_session_user()
    assert ok is False
    assert "unset" in err.lower()
