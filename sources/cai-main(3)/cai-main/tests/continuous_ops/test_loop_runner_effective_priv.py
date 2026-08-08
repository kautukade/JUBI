"""effective_privileged_worker respects CAI_AVOID_SUDO."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from cai.continuous_ops.loop_runner import LoopConfig, effective_privileged_worker


def _cfg(tmp: Path, *, privileged: bool) -> LoopConfig:
    (tmp / "run_loop_config.json").write_text(
        json.dumps(
            {
                "tick_seconds": 60,
                "tick_prompt": "x",
                "privileged": privileged,
                "cai_argv": ["cai"],
                "python_interpreter": "/usr/bin/python3",
                "log_full_days": 7,
                "log_delete_after_days": 15,
                "entry_script": "run_loop.py",
            }
        ),
        encoding="utf-8",
    )
    return LoopConfig.load(tmp)


@pytest.mark.parametrize("truthy", ["1", "true", "yes", "on"])
def test_avoid_sudo_disables_effective_priv(monkeypatch, tmp_path: Path, truthy: str) -> None:
    monkeypatch.setenv("CAI_AVOID_SUDO", truthy)
    cfg = _cfg(tmp_path, privileged=True)
    assert effective_privileged_worker(cfg) is False


def test_no_avoid_sudo_keeps_priv(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.delenv("CAI_AVOID_SUDO", raising=False)
    cfg = _cfg(tmp_path, privileged=True)
    assert effective_privileged_worker(cfg) is True


def test_privileged_false_stays_false(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.setenv("CAI_AVOID_SUDO", "true")
    cfg = _cfg(tmp_path, privileged=False)
    assert effective_privileged_worker(cfg) is False
