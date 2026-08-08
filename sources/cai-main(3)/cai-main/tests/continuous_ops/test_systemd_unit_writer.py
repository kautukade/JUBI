"""Wizard-generated systemd user unit for continuous ops."""

from __future__ import annotations

import sys
from pathlib import Path

from cai.continuous_ops.scriptgen import render_loop_script
from cai.continuous_ops.systemd_unit import _systemd_extra_env_lines, write_systemd_user_unit


def test_write_systemd_user_unit_contains_execstart(tmp_path: Path) -> None:
    render_loop_script(
        run_dir=tmp_path,
        tick_seconds=30,
        tick_prompt="x",
        privileged=False,
        cai_argv=["cai"],
    )
    p = write_systemd_user_unit(
        run_dir=tmp_path, service_stem="cai-cops-run_test", python_bin=sys.executable
    )
    assert p.is_file()
    txt = p.read_text(encoding="utf-8")
    assert "ExecStart=" in txt
    assert "loop_runner" in txt
    assert "Restart=always" in txt
    assert str(tmp_path.resolve()) in txt
    assert "CAI_AVOID_SUDO" not in txt
    assert "Environment=CAI_CONTINUOUS_OPS_SYSTEMD_PLAIN_LOG=1" in txt

    p2 = write_systemd_user_unit(
        run_dir=tmp_path,
        service_stem="cai-cops-run_avoid",
        python_bin=sys.executable,
        avoid_sudo=True,
    )
    p2_txt = p2.read_text(encoding="utf-8")
    assert "Environment=CAI_AVOID_SUDO=1" in p2_txt
    assert "Environment=CAI_CONTINUOUS_OPS_SYSTEMD_PLAIN_LOG=1" in p2_txt


def test_systemd_extra_env_lines_combined() -> None:
    txt = _systemd_extra_env_lines(venv_root="/tmp/fakevenv", pythonpath_extra="/tmp/proj/src")
    assert "VIRTUAL_ENV=" in txt
    assert "PYTHONPATH=" in txt
    assert "/tmp/proj/src" in txt


def test_write_systemd_user_unit_embeds_pythonpath(tmp_path: Path) -> None:
    render_loop_script(
        run_dir=tmp_path,
        tick_seconds=30,
        tick_prompt="x",
        privileged=False,
        cai_argv=["cai"],
    )
    p = write_systemd_user_unit(
        run_dir=tmp_path,
        service_stem="cai-cops-run_pp",
        python_bin=sys.executable,
        avoid_sudo=True,
        venv_root=None,
        pythonpath_extra="/home/dev/cai/src",
    )
    body = p.read_text(encoding="utf-8")
    assert "Environment=PYTHONPATH=" in body
    assert "/home/dev/cai/src" in body
