"""Worker agent type defaults for continuous_ops ticks."""

from __future__ import annotations

import json
from pathlib import Path

from cai.continuous_ops.loop_runner import LoopConfig


def test_loop_config_defaults_worker_agent_to_blueteam(tmp_path: Path) -> None:
    (tmp_path / "run_loop_config.json").write_text(
        json.dumps(
            {
                "tick_seconds": 60,
                "tick_prompt": "x",
                "privileged": False,
                "cai_argv": ["cai"],
                "python_interpreter": "/usr/bin/python3",
                "log_full_days": 7,
                "log_delete_after_days": 15,
                "entry_script": "run_loop.py",
            }
        ),
        encoding="utf-8",
    )
    cfg = LoopConfig.load(tmp_path)
    assert cfg.worker_agent_type == "blueteam_agent"


def test_loop_config_respects_worker_agent_override(tmp_path: Path) -> None:
    (tmp_path / "run_loop_config.json").write_text(
        json.dumps(
            {
                "tick_seconds": 60,
                "tick_prompt": "x",
                "privileged": False,
                "cai_argv": ["cai"],
                "python_interpreter": "/usr/bin/python3",
                "log_full_days": 7,
                "log_delete_after_days": 15,
                "entry_script": "run_loop.py",
                "worker_agent_type": "selection_agent",
            }
        ),
        encoding="utf-8",
    )
    cfg = LoopConfig.load(tmp_path)
    assert cfg.worker_agent_type == "selection_agent"
