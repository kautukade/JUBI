"""Mission planner HTTP retries and MissionPlan diagnostics."""

from __future__ import annotations

from unittest.mock import MagicMock

import pytest

from cai.continuous_ops.model_parse import parse_mission_with_planner


def test_planner_retries_504_then_succeeds(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("ALIAS_API_KEY", "sk-test-planner")
    monkeypatch.setenv("CAI_MISSION_PLANNER_MAX_ATTEMPTS", "3")
    monkeypatch.setattr("time.sleep", lambda *_a, **_k: None)

    good_json = (
        '{"tasks":["check disk"],"tasks_markdown":"- check disk",'
        '"tick_seconds": null, "use_tmux": null, "auth_required": null,'
        '"estimated_tokens_per_iteration": 800, "refined_tick_prompt": "run checks"}'
    )

    class Resp504:
        status_code = 504
        text = "<title>Error</title>"
        headers = {}

    class Resp200:
        status_code = 200
        text = ""
        headers = {}

        def json(self) -> dict:
            return {"choices": [{"message": {"content": good_json}}]}

    seq = [Resp504(), Resp200()]
    post = MagicMock(side_effect=seq)
    monkeypatch.setattr("httpx.post", post)

    plan = parse_mission_with_planner("monitor host")
    assert plan.planner_origin == "planner_api"
    assert post.call_count == 2


def test_planner_all_504_sets_http_status(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("ALIAS_API_KEY", "sk-test-planner")
    monkeypatch.setenv("CAI_MISSION_PLANNER_MAX_ATTEMPTS", "2")
    monkeypatch.setattr("time.sleep", lambda *_a, **_k: None)

    class Resp504:
        status_code = 504
        text = "timeout"
        headers = {}

    monkeypatch.setattr("httpx.post", MagicMock(return_value=Resp504()))

    plan = parse_mission_with_planner("x")
    assert plan.planner_origin != "planner_api"
    assert plan.planner_http_status == 504
    assert plan.planner_failure_summary is not None
