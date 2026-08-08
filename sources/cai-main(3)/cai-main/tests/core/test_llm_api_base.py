"""Tests for ``cai.util.llm_api_base`` resolution."""

from __future__ import annotations

import pytest

from cai.util import llm_api_base as m


def test_mini_pattern() -> None:
    assert m.cai_model_uses_alias_mini_url_pattern("alias2-mini") is True
    assert m.cai_model_uses_alias_mini_url_pattern("Alias3.1-MINI") is True
    assert m.cai_model_uses_alias_mini_url_pattern("alias1") is False
    assert m.cai_model_uses_alias_mini_url_pattern("") is False


@pytest.mark.parametrize(
    "model,expect",
    [
        ("alias1", True),
        ("Alias2-mini", True),
        ("csi-local", True),
        ("CAI-test", True),
        ("openai/gpt-4o", False),
        ("gpt-4o", False),
        ("claude-3-5-sonnet", False),
        ("", False),
    ],
)
def test_model_qualifies_for_alias_api_url(model: str, expect: bool) -> None:
    assert m.model_qualifies_for_alias_api_url(model) is expect


@pytest.mark.parametrize(
    "model,csi_url,alias_url,openai_base,expect_substr",
    [
        (
            "alias1",
            "https://csi.example/v1",
            "https://mini.example/v1",
            "https://custom.example/",
            "csi.example",
        ),
        ("alias1", "", "https://mini.example/v1", "https://custom.example/", "mini.example"),
        ("alias2-mini", "http://127.0.0.1:9999", "", "", "127.0.0.1:9999"),
        ("cai-dev", "", "https://cai-gw.example/", "https://ignored.example/", "cai-gw.example"),
        (
            "gpt-4o",
            "https://ignored-csi.example/",
            "https://ignored-alias.example/",
            "https://openai-compat.example/",
            "openai-compat.example",
        ),
        (
            "gpt-4o",
            "https://csi-ignored.example/",
            "https://alias-ignored.example/",
            "",
            "aliasrobotics.com",
        ),
        ("alias1", "", "", "https://custom.example/", "custom.example"),
        ("gpt-4o", "", "", "", "aliasrobotics.com"),
    ],
)
def test_resolve_base(
    monkeypatch: pytest.MonkeyPatch,
    model: str,
    csi_url: str,
    alias_url: str,
    openai_base: str,
    expect_substr: str,
) -> None:
    monkeypatch.setenv("CAI_MODEL", model)
    if csi_url:
        monkeypatch.setenv("CSI_CUSTOM_ENDPOINT", csi_url)
    else:
        monkeypatch.delenv("CSI_CUSTOM_ENDPOINT", raising=False)
    if alias_url:
        monkeypatch.setenv("ALIAS_API_URL", alias_url)
    else:
        monkeypatch.delenv("ALIAS_API_URL", raising=False)
    if openai_base:
        monkeypatch.setenv("OPENAI_API_BASE", openai_base)
    else:
        monkeypatch.delenv("OPENAI_API_BASE", raising=False)
    out = m.resolve_llm_openai_compatible_base(model)
    assert expect_substr in out


def test_explicit_custom_alias_model_and_url(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("CSI_CUSTOM_ENDPOINT", raising=False)
    monkeypatch.setenv("ALIAS_API_URL", "https://x/")
    assert m.explicit_custom_llm_api_base_configured("alias1") is True


def test_explicit_custom_csi_only(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("CSI_CUSTOM_ENDPOINT", "https://csi-only.example/")
    monkeypatch.delenv("ALIAS_API_URL", raising=False)
    monkeypatch.delenv("OPENAI_API_BASE", raising=False)
    assert m.explicit_custom_llm_api_base_configured("alias1") is True


def test_explicit_custom_non_alias_model_alias_url_only(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("ALIAS_API_URL", "https://x/")
    monkeypatch.setenv("CSI_CUSTOM_ENDPOINT", "https://csi.example/")
    monkeypatch.delenv("OPENAI_API_BASE", raising=False)
    assert m.explicit_custom_llm_api_base_configured("gpt-4o") is False


def test_explicit_custom_openai_only(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("ALIAS_API_URL", raising=False)
    monkeypatch.delenv("CSI_CUSTOM_ENDPOINT", raising=False)
    monkeypatch.setenv("OPENAI_API_BASE", "https://y/")
    assert m.explicit_custom_llm_api_base_configured("gpt-4o") is True


def test_no_explicit_custom_default(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("CAI_MODEL", "alias3-mini")
    monkeypatch.delenv("ALIAS_API_URL", raising=False)
    monkeypatch.delenv("CSI_CUSTOM_ENDPOINT", raising=False)
    monkeypatch.delenv("OPENAI_API_BASE", raising=False)
    assert m.explicit_custom_llm_api_base_configured("alias1") is False
