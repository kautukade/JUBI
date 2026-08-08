"""Assistant content extraction and local expansion when the API planner fails."""

from __future__ import annotations

from cai.continuous_ops.model_parse import (
    _apply_local_mission_expansion_if_needed,
    _completion_choice_assistant_text,
    _fallback_plan,
    _mission_is_short_or_vague,
    MissionPlan,
)


def test_completion_extracts_multimodal_content_list() -> None:
    body = {
        "choices": [
            {
                "message": {
                    "content": [
                        {"type": "text", "text": '{"tasks": ["a"], "tasks_markdown": "- a", "tick_seconds": null}'},
                    ]
                }
            }
        ]
    }
    s = _completion_choice_assistant_text(body)
    assert "tasks" in s


def test_completion_legacy_text_field() -> None:
    body = {"choices": [{"text": '{"tasks": ["x"]}', "message": {}}]}
    s = _completion_choice_assistant_text(body)
    assert "tasks" in s


def test_mission_vague_one_liner() -> None:
    assert _mission_is_short_or_vague("Monitoriza el host") is True
    assert _mission_is_short_or_vague("- a\n- b\n- c\n- d") is False


def test_fallback_plan_expands_vague_mission() -> None:
    plan = _fallback_plan("Monitoriza el host", origin="fallback_exception")
    assert plan.structured_tasks is not None
    assert len(plan.structured_tasks or ()) >= 4
    assert "Monitoriza" in plan.refined_tick_prompt or "host" in plan.tasks_markdown.lower()


def test_planner_api_not_expanded() -> None:
    p = MissionPlan(
        tasks_markdown="- only",
        tick_seconds=30,
        use_tmux=None,
        auth_required=None,
        estimated_tokens_per_iteration=1000,
        refined_tick_prompt="x",
        tier="pro",
        structured_tasks=("a",),
        planner_origin="planner_api",
    )
    assert _apply_local_mission_expansion_if_needed(p, "short") is p
