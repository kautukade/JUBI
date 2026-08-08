"""Plain tick logs only when CAI_CONTINUOUS_OPS_SYSTEMD_PLAIN_LOG is set (systemd unit)."""

from __future__ import annotations

import os

import pytest

from cai.continuous_ops import loop_runner


@pytest.mark.parametrize("flag_val, expect_plain", [("1", True), ("true", True), ("0", False), ("", False)])
def test_cai_subprocess_env_plain_log_only_with_flag(
    monkeypatch: pytest.MonkeyPatch, flag_val: str, expect_plain: bool
) -> None:
    for k in ("CAI_CONTINUOUS_OPS_SYSTEMD_PLAIN_LOG", "NO_COLOR", "FORCE_COLOR", "CLICOLOR"):
        monkeypatch.delenv(k, raising=False)
    if flag_val != "":
        monkeypatch.setenv("CAI_CONTINUOUS_OPS_SYSTEMD_PLAIN_LOG", flag_val)
    env = loop_runner._cai_subprocess_env_from_os_environ()
    if expect_plain:
        assert env.get("NO_COLOR") == "1"
        assert env.get("FORCE_COLOR") == "0"
        assert env.get("CLICOLOR") == "0"
    else:
        assert env.get("NO_COLOR") != "1"
        assert env.get("FORCE_COLOR") != "0"
