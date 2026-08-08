"""Unit tests for continuous-ops mission task parsing."""

from __future__ import annotations

from cai.continuous_ops.model_parse import (
    MissionPlan,
    needs_task_collection,
    normalized_tasks,
    parse_task_lines_from_markdown,
    summary_iteration_tasks,
)


def test_parse_bullets():
    md = "- First task\n- Second task"
    assert parse_task_lines_from_markdown(md) == ["First task", "Second task"]


def test_parse_ordered():
    md = "1. Alpha\n2. Beta"
    assert parse_task_lines_from_markdown(md) == ["Alpha", "Beta"]


def test_parse_single_block_without_bullets():
    md = "Monitor the host for anomalies."
    assert parse_task_lines_from_markdown(md) == ["Monitor the host for anomalies."]


def test_normalized_prefers_structured_tasks():
    plan = MissionPlan(
        tasks_markdown="- ignored",
        tick_seconds=None,
        use_tmux=None,
        auth_required=None,
        estimated_tokens_per_iteration=1000,
        refined_tick_prompt="x",
        tier="pro",
        structured_tasks=("A", "B"),
    )
    assert normalized_tasks(plan) == ["A", "B"]


def test_needs_task_collection_empty():
    plan = MissionPlan(
        tasks_markdown="",
        tick_seconds=None,
        use_tmux=None,
        auth_required=None,
        estimated_tokens_per_iteration=1000,
        refined_tick_prompt="",
        tier="pro",
        structured_tasks=None,
    )
    assert needs_task_collection(plan) is True


def test_needs_task_collection_too_short_only():
    plan = MissionPlan(
        tasks_markdown="a",
        tick_seconds=None,
        use_tmux=None,
        auth_required=None,
        estimated_tokens_per_iteration=1000,
        refined_tick_prompt="a",
        tier="pro",
        structured_tasks=("x",),
    )
    assert needs_task_collection(plan) is True


def test_needs_task_collection_ok():
    plan = MissionPlan(
        tasks_markdown="- Check disk usage",
        tick_seconds=None,
        use_tmux=None,
        auth_required=None,
        estimated_tokens_per_iteration=1000,
        refined_tick_prompt="x",
        tier="pro",
        structured_tasks=None,
    )
    assert needs_task_collection(plan) is False


def test_summary_iteration_tasks_prefers_refined_tick_prompt():
    """Final summary should list expanded checklist, not a lone vague structured task."""
    plan = MissionPlan(
        tasks_markdown="- Monitoriza el host",
        tick_seconds=30,
        use_tmux=True,
        auth_required=False,
        estimated_tokens_per_iteration=1000,
        refined_tick_prompt="- OS/kernel identity and uptime\n- Disk space and load\n- Unprivileged listening ports",
        tier="pro",
        structured_tasks=("Monitoriza el host",),
    )
    assert normalized_tasks(plan) == ["Monitoriza el host"]
    assert summary_iteration_tasks(plan) == [
        "OS/kernel identity and uptime",
        "Disk space and load",
        "Unprivileged listening ports",
    ]


def test_summary_iteration_tasks_prefers_rich_tasks_markdown_over_single_structured():
    """When structured_tasks is one vague line but tasks_markdown lists many bullets, summary uses markdown."""
    plan = MissionPlan(
        tasks_markdown=(
            "- Check kernel and uptime\n"
            "- Disk and load averages\n"
            "- Listening ports (unprivileged)\n"
            "- User-readable auth logs if present"
        ),
        tick_seconds=30,
        use_tmux=True,
        auth_required=False,
        estimated_tokens_per_iteration=1000,
        refined_tick_prompt="Monitor the host",
        tier="pro",
        structured_tasks=("Monitor the host",),
    )
    assert normalized_tasks(plan) == ["Monitor the host"]
    out = summary_iteration_tasks(plan)
    assert len(out) == 4
    assert "Listening ports" in out[2]
