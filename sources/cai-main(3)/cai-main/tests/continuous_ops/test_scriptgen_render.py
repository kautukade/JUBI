"""Smoke test for generated loop artifacts."""

import json
from pathlib import Path

from cai.continuous_ops.scriptgen import CONFIG_NAME, render_loop_script


def test_render_loop_script_contains_expected_markers(tmp_path: Path) -> None:
    out = render_loop_script(
        run_dir=tmp_path,
        tick_seconds=120,
        tick_prompt="Do one iteration.\nSecond line.",
        privileged=False,
        cai_argv=["cai"],
    )
    assert out.name == "run_loop.py"
    assert out.is_file()
    text = out.read_text(encoding="utf-8")
    assert "cai.continuous_ops.loop_runner" in text
    assert "Do one iteration." not in text  # prompt lives in JSON only
    cfg_path = tmp_path / CONFIG_NAME
    assert cfg_path.is_file()
    cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
    assert cfg["tick_seconds"] == 120
    assert "Do one iteration." in cfg["tick_prompt"]
    assert cfg["privileged"] is False
    assert cfg["cai_argv"] == ["cai"]
    assert cfg.get("worker_agent_type") == "blueteam_agent"


def test_render_privileged_primes_sudo_and_no_sudo_block_marker(tmp_path: Path) -> None:
    render_loop_script(
        run_dir=tmp_path,
        tick_seconds=90,
        tick_prompt="Tick.",
        privileged=True,
        cai_argv=["cai"],
    )
    cfg = json.loads((tmp_path / CONFIG_NAME).read_text(encoding="utf-8"))
    assert cfg["privileged"] is True
