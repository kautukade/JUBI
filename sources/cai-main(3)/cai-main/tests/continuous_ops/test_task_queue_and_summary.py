"""Task queue cadence and mechanical summary."""

from __future__ import annotations

import json
from pathlib import Path

from cai.continuous_ops import mechanical_summary, task_queue
from cai.continuous_ops.model_parse import MissionPlan


def test_pick_next_rotates_by_repeat_after(tmp_path: Path) -> None:
    run_dir = tmp_path / "run1"
    run_dir.mkdir()
    now = task_queue._utcnow()
    t1 = task_queue.TaskItem("a", "task a", 10, None, now)
    t2 = task_queue.TaskItem("b", "task b", 10, now, now)
    task_queue.save_queue(
        run_dir,
        {"schema_version": 1, "tasks": [t1.to_dict(), t2.to_dict()], "cursor": 0},
    )
    id1, text1 = task_queue.pick_next_task(run_dir)
    assert text1 in ("task a", "task b")
    task_queue.mark_task_run(run_dir, id1)
    _id2, text2 = task_queue.pick_next_task(run_dir)
    assert {text1, text2} == {"task a", "task b"}


def test_merge_appends_from_log(tmp_path: Path) -> None:
    run_dir = tmp_path / "run2"
    run_dir.mkdir()
    task_queue.ensure_task_queue_bootstrapped(run_dir, "bootstrap only")
    log = """hello
<<<COPS_TASK_APPEND>>>
[{"id": "x1", "text": "New task", "repeat_after_seconds": 120}]
<<<END_COPS_TASK_APPEND>>>
tail
"""
    n = task_queue.merge_appends_from_log(log, run_dir)
    assert n == 1
    raw = task_queue.load_queue(run_dir)
    assert raw is not None
    ids = {t["id"] for t in raw["tasks"]}
    assert "x1" in ids


def test_mechanical_summary_keeps_status_lines() -> None:
    log = "noise\n[STATUS: OK]\nmore\n### Step 1 done\n" + "\n".join(f"line{i}" for i in range(200))
    s = mechanical_summary.build_mechanical_summary(log)
    assert "[STATUS:" in s or "[status:" in s.lower()


def test_initialize_from_plan_structured(tmp_path: Path) -> None:
    run_dir = tmp_path / "run3"
    run_dir.mkdir()
    plan = MissionPlan(
        tasks_markdown="- a\n- b",
        tick_seconds=60,
        use_tmux=None,
        auth_required=None,
        estimated_tokens_per_iteration=1000,
        refined_tick_prompt="ref",
        tier="pro",
        structured_tasks=("one", "two"),
    )
    task_queue.initialize_from_plan(run_dir, plan)
    raw = task_queue.load_queue(run_dir)
    assert raw and len(raw["tasks"]) == 2


def test_initialize_from_plan_fallback_markdown(tmp_path: Path) -> None:
    run_dir = tmp_path / "run4"
    run_dir.mkdir()
    plan = MissionPlan(
        tasks_markdown="- alpha\n- beta",
        tick_seconds=60,
        use_tmux=None,
        auth_required=None,
        estimated_tokens_per_iteration=1000,
        refined_tick_prompt="",
        tier="pro",
        structured_tasks=None,
    )
    task_queue.initialize_from_plan(run_dir, plan)
    raw = task_queue.load_queue(run_dir)
    assert raw and len(raw["tasks"]) >= 2
