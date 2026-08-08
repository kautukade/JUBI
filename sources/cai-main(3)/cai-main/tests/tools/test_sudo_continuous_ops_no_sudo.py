"""CAI_CONTINUOUS_OPS_NO_SUDO must never open interactive sudo elevation in workers."""

from __future__ import annotations

import os
from unittest.mock import patch

from cai.util.user_prompts import ensure_sudo_credentials, prompt_sudo_elevation


def test_prompt_sudo_elevation_returns_none_when_continuous_ops_no_sudo():
    with patch.dict(os.environ, {"CAI_CONTINUOUS_OPS_NO_SUDO": "true"}, clear=False):
        assert prompt_sudo_elevation("ls /root", "/tmp") is None


def test_ensure_sudo_credentials_returns_message_without_prompt_when_continuous_ops_no_sudo():
    with patch.dict(os.environ, {"CAI_CONTINUOUS_OPS_NO_SUDO": "true"}, clear=False):
        out = ensure_sudo_credentials("sudo ls /root", "/tmp", timeout=5, max_attempts=1)
    assert isinstance(out, str)
    assert "CAI_CONTINUOUS_OPS_NO_SUDO" in out
    assert "ls /root" in out
