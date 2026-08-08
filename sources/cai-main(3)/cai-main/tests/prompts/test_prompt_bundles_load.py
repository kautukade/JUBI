"""Ensure packaged prompt markdown loads through `load_prompt_template` (Mako render succeeds)."""

from __future__ import annotations

import pytest

from cai.util import load_prompt_template
from cai.util.prompts import _MICRO_PROFILE_PATHS

# Primary agent system templates (short list for CI time).
_SYSTEM_PROMPT_RELPATHS = (
    "prompts/system_orchestration_agent.md",
    "prompts/system_selection_agent.md",
    "prompts/system_red_team_agent.md",
    "prompts/system_blue_team_agent.md",
    "prompts/system_bug_bounter.md",
    "prompts/system_web_pentester.md",
    "prompts/system_ctf_agent.md",
    "prompts/system_compliance_agent.md",
    "prompts/system_use_cases.md",
    "prompts/memory_analysis_agent.md",
    "prompts/reverse_engineering_agent.md",
    "prompts/subghz_agent.md",
    "prompts/wifi_security_agent.md",
    "prompts/system_dfir_agent.md",
    "prompts/system_network_analyzer.md",
    "prompts/system_replay_attack_agent.md",
)


def test_system_templates_load(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("CAI_CYBER_PROFILE", raising=False)
    for rel in _SYSTEM_PROMPT_RELPATHS:
        text = load_prompt_template(rel)
        assert isinstance(text, str)
        assert len(text) > 80, f"prompt unexpectedly short: {rel}"


def test_micro_profile_templates_load(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("CAI_CYBER_PROFILE", raising=False)
    for rel in sorted(set(_MICRO_PROFILE_PATHS.values())):
        text = load_prompt_template(rel)
        assert isinstance(text, str)
        assert len(text) > 40, f"micro prompt unexpectedly short: {rel}"
        assert "MICRO-PROFILE" in text.upper(), rel
