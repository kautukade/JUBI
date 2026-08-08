"""Pre-loop summary stub for operator watch commands."""

from __future__ import annotations

import time
from pathlib import Path

from cai.continuous_ops.loop_runner import _ensure_latest_log_placeholder, _write_preloop_summary_stub


def test_preloop_summary_creates_file(tmp_path: Path) -> None:
    start = time.time()
    _write_preloop_summary_stub(tmp_path, start, "2026-01-01T00:00:00", 60, 120.0)
    p = tmp_path / "logs" / "summary" / "summary.txt"
    assert p.is_file()
    txt = p.read_text(encoding="utf-8")
    assert "iterations_total: 0" in txt
    assert "last_exit_code: -1" in txt
    assert "first CAI subprocess tick" in txt


def test_latest_log_placeholder(tmp_path: Path) -> None:
    _ensure_latest_log_placeholder(tmp_path)
    latest = tmp_path / "logs" / "full" / "latest.log"
    assert latest.exists() or latest.is_symlink()
    target = tmp_path / "logs" / "full" / "_bootstrap_placeholder.log"
    assert target.is_file()
