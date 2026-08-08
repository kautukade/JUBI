"""Configuration defaults for the orchestration agent."""

from __future__ import annotations

from cai.config import CAIConfig, reset_config


def test_default_agent_type_is_selection_agent(monkeypatch):
    monkeypatch.delenv("CAI_AGENT_TYPE", raising=False)
    reset_config()

    cfg = CAIConfig.from_env()

    assert cfg.agent_type == "selection_agent"


def test_orchestration_worker_max_turns_default(monkeypatch):
    monkeypatch.delenv("CAI_ORCHESTRATION_WORKER_MAX_TURNS", raising=False)
    reset_config()
    assert CAIConfig.from_env().orchestration_worker_max_turns == 6


def test_orchestration_worker_max_turns_env(monkeypatch):
    monkeypatch.setenv("CAI_ORCHESTRATION_WORKER_MAX_TURNS", "12")
    reset_config()
    assert CAIConfig.from_env().orchestration_worker_max_turns == 12


def test_orchestration_mas_hint_default(monkeypatch):
    monkeypatch.delenv("CAI_ORCHESTRATION_MAS_HINT", raising=False)
    reset_config()
    assert CAIConfig.from_env().orchestration_mas_hint is True


def test_orchestration_mas_hint_false(monkeypatch):
    monkeypatch.setenv("CAI_ORCHESTRATION_MAS_HINT", "false")
    reset_config()
    assert CAIConfig.from_env().orchestration_mas_hint is False
