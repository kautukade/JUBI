"""Tests for `_compose_cyber_layered_prompt` and micro-profile registry.

No template bundle I/O beyond compose.
"""

from __future__ import annotations

import pytest

from cai.util.prompts import (
    _MICRO_PROFILE_PATHS,
    _compose_cyber_layered_prompt,
    _load_micro_profile_text,
)


def test_compose_cyber_layering_disabled_returns_base_only(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("CAI_CYBER_PROFILE", "false")
    try:
        out = _compose_cyber_layered_prompt(
            "BASE_ONLY_MARKER",
            None,
            unrestricted=False,
            cyber_micro_profile_key="redteam",
        )
        assert out == "BASE_ONLY_MARKER"
    finally:
        monkeypatch.delenv("CAI_CYBER_PROFILE", raising=False)


def test_compose_full_includes_baseline_and_micro(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("CAI_CYBER_PROFILE", "true")
    monkeypatch.setenv("CAI_CYBER_PROFILE_MODE", "full")
    monkeypatch.delenv("CAI_UNRESTRICTED", raising=False)
    try:
        out = _compose_cyber_layered_prompt(
            "TAIL_BASE_MARKER",
            None,
            unrestricted=False,
            cyber_micro_profile_key="selection",
        )
        assert "CAI CYBER BASELINE" in out
        assert "AGENT MICRO-PROFILE: SELECTION" in out.upper()
        assert "TAIL_BASE_MARKER" in out
    finally:
        monkeypatch.delenv("CAI_CYBER_PROFILE", raising=False)
        monkeypatch.delenv("CAI_CYBER_PROFILE_MODE", raising=False)


def test_compose_lite_uses_lite_baseline(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("CAI_CYBER_PROFILE", "true")
    monkeypatch.setenv("CAI_CYBER_PROFILE_MODE", "lite")
    try:
        out = _compose_cyber_layered_prompt(
            "X",
            None,
            unrestricted=False,
            cyber_micro_profile_key="ctf",
        )
        assert "CAI CYBER BASELINE (LITE)" in out
        assert "AGENT MICRO-PROFILE: CTF" in out.upper()
    finally:
        monkeypatch.delenv("CAI_CYBER_PROFILE", raising=False)
        monkeypatch.delenv("CAI_CYBER_PROFILE_MODE", raising=False)


def test_compose_mode_off_env_value(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("CAI_CYBER_PROFILE", "true")
    monkeypatch.setenv("CAI_CYBER_PROFILE_MODE", "off")
    try:
        out = _compose_cyber_layered_prompt(
            "PLAIN",
            None,
            unrestricted=False,
            cyber_micro_profile_key="web",
        )
        assert out == "PLAIN"
    finally:
        monkeypatch.delenv("CAI_CYBER_PROFILE", raising=False)
        monkeypatch.delenv("CAI_CYBER_PROFILE_MODE", raising=False)


@pytest.mark.parametrize(
    "key",
    ("redteam", "blueteam", "guardrail", "compliance"),
)
def test_compose_representative_micro_keys(
    key: str,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("CAI_CYBER_PROFILE", "true")
    monkeypatch.setenv("CAI_CYBER_PROFILE_MODE", "full")
    try:
        out = _compose_cyber_layered_prompt(
            f"MARKER_{key}",
            None,
            unrestricted=False,
            cyber_micro_profile_key=key,
        )
        assert f"MARKER_{key}" in out
        assert "CAI CYBER BASELINE" in out
        assert "MICRO-PROFILE" in out.upper()
    finally:
        monkeypatch.delenv("CAI_CYBER_PROFILE", raising=False)
        monkeypatch.delenv("CAI_CYBER_PROFILE_MODE", raising=False)


def test_every_micro_profile_registry_key_composes(monkeypatch: pytest.MonkeyPatch) -> None:
    """Each `_MICRO_PROFILE_PATHS` entry must load and layer with the full baseline."""
    monkeypatch.setenv("CAI_CYBER_PROFILE", "true")
    monkeypatch.setenv("CAI_CYBER_PROFILE_MODE", "full")
    try:
        for profile_key in sorted(_MICRO_PROFILE_PATHS):
            text = _load_micro_profile_text(profile_key)
            assert text.strip(), f"empty micro profile: {profile_key}"
            composed = _compose_cyber_layered_prompt(
                "END_MARKER",
                None,
                unrestricted=False,
                cyber_micro_profile_key=profile_key,
            )
            assert "END_MARKER" in composed
            assert "CAI CYBER BASELINE" in composed
            assert "MICRO-PROFILE" in composed.upper()
    finally:
        monkeypatch.delenv("CAI_CYBER_PROFILE", raising=False)
        monkeypatch.delenv("CAI_CYBER_PROFILE_MODE", raising=False)
