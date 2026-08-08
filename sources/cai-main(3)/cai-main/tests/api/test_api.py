import types
from typing import List

import pytest
from fastapi.testclient import TestClient

from cai.api.app import create_cai_api_app
from cai.api.commands import CommandExecutionResult, CommandMetadata
from cai.api.sessions import SessionManager
from cai.sdk.agents.result import RunResult


class DummyCommandExecutor:
    def describe_commands(self) -> List[CommandMetadata]:
        return [
            CommandMetadata(name="/help", description="display help", aliases=["/h"], subcommands=[]),
            CommandMetadata(name="/memory", description="memory ops", aliases=[], subcommands=["show"]),
        ]

    def run(self, command_name: str, args=None, auto_correct=True) -> CommandExecutionResult:  # noqa: D401
        return CommandExecutionResult(
            handled=True,
            suggested_command=None,
            stdout=f"executed {command_name}",
            stderr="",
            exit_code=None,
        )


def _fake_agent_factory(agent_name: str, model_name: str, agent_id: str):
    return types.SimpleNamespace(name=f"{agent_name}-{agent_id}", model=types.SimpleNamespace(model=model_name))


@pytest.fixture()
def client(monkeypatch: pytest.MonkeyPatch) -> TestClient:
    async def fake_runner(starting_agent, input, context=None, max_turns=None, hooks=None, run_config=None):
        return RunResult(
            input=input,
            new_items=[],
            raw_responses=[],
            final_output={"echo": input[-1]["content"] if input else None},
            input_guardrail_results=[],
            output_guardrail_results=[],
            _last_agent=starting_agent,
        )

    monkeypatch.setattr("cai.api.sessions.Runner.run", fake_runner)
    monkeypatch.setenv("ALIAS_API_KEY", "test-key")
    manager = SessionManager(agent_factory=_fake_agent_factory)
    app = create_cai_api_app(session_manager=manager, command_executor=DummyCommandExecutor())
    return TestClient(app)


@pytest.fixture()
def auth_headers() -> dict:
    return {"X-CAI-API-Key": "test-key"}


def test_create_and_list_sessions(client: TestClient, auth_headers: dict):
    response = client.post("/api/v1/sessions", headers=auth_headers, json={"agent": "test_agent"})
    assert response.status_code == 201
    data = response.json()
    assert data["agent"] == "test_agent"

    list_response = client.get("/api/v1/sessions", headers=auth_headers)
    assert list_response.status_code == 200
    payload = list_response.json()
    assert isinstance(payload["sessions"], list)
    assert payload["sessions"][0]["agent"] == "test_agent"


def test_inference_returns_history(client: TestClient, auth_headers: dict):
    session = client.post("/api/v1/sessions", headers=auth_headers, json={"agent": "test_agent"}).json()
    session_id = session["id"]
    infer = client.post(
        f"/api/v1/sessions/{session_id}/messages",
        headers=auth_headers,
        json={"input": "hola"},
    )
    assert infer.status_code == 200
    payload = infer.json()
    assert payload["session"]["id"] == session_id
    assert payload["result"]["history"][-1]["content"] == "hola"


def test_command_route(client: TestClient, auth_headers: dict):
    response = client.post("/api/v1/commands/help", headers=auth_headers, json={})
    assert response.status_code == 200
    assert response.json()["stdout"] == "executed help"
