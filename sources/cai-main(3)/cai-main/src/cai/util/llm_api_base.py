"""Resolve OpenAI-compatible ``api_base`` for the Alias gateway and custom endpoints."""

from __future__ import annotations

import os
import urllib.parse

DEFAULT_ALIAS_LLM_API_BASE = "https://api.aliasrobotics.com:666/"

# Model ids for which ``CSI_CUSTOM_ENDPOINT`` / ``ALIAS_API_URL`` may apply (prefix match, case-insensitive).
_ALIAS_API_URL_MODEL_PREFIXES: tuple[str, ...] = ("cai", "alias", "csi")


def cai_model_uses_alias_mini_url_pattern(model: str | None) -> bool:
    """Legacy helper: true when ``model`` looks like ``alias…`` + ``-mini`` (case-insensitive).

    Kept for callers/tests; gateway resolution uses `model_qualifies_for_alias_api_url` instead.
    """
    m = (model or "").strip()
    if not m:
        return False
    ml = m.lower()
    return ml.startswith("alias") and ml.endswith("-mini")


def model_qualifies_for_alias_api_url(model: str | None) -> bool:
    """True when ``CSI_CUSTOM_ENDPOINT`` / ``ALIAS_API_URL`` may apply (cai, alias, or csi prefix).

    For ``provider/model`` ids, only the provider segment (before the first ``/``) is checked.
    """
    raw = (model or "").strip().lower()
    if not raw:
        return False
    base = raw.split("/", 1)[0]
    return any(base.startswith(p) for p in _ALIAS_API_URL_MODEL_PREFIXES)


def _normalize_api_base_url(url: str) -> str:
    u = (url or "").strip()
    if not u:
        return DEFAULT_ALIAS_LLM_API_BASE
    parsed = urllib.parse.urlparse(u if "://" in u else f"https://{u}")
    if not parsed.scheme:
        u = "https://" + u.lstrip("/")
        parsed = urllib.parse.urlparse(u)
    path = parsed.path or "/"
    if not path.endswith("/"):
        path = path + "/"
    netloc = parsed.netloc or ""
    if not netloc:
        return DEFAULT_ALIAS_LLM_API_BASE
    return urllib.parse.urlunparse((parsed.scheme or "https", netloc, path, "", "", ""))


def explicit_custom_llm_api_base_configured(model: str | None = None) -> bool:
    """True if a custom OpenAI-compatible base is in effect for this model."""
    effective = (model if model is not None else os.getenv("CAI_MODEL")) or ""
    if model_qualifies_for_alias_api_url(effective):
        if (os.getenv("CSI_CUSTOM_ENDPOINT") or "").strip():
            return True
        if (os.getenv("ALIAS_API_URL") or "").strip():
            return True
    if (os.getenv("OPENAI_API_BASE") or "").strip():
        return True
    return False


def resolve_llm_openai_compatible_base(model: str | None = None) -> str:
    """Effective API base URL for ``/chat/completions`` (always ends with ``/``).

    When ``model_qualifies_for_alias_api_url``:

    1. ``CSI_CUSTOM_ENDPOINT`` if non-empty (e.g. CSI + CAI backend).
    2. ``ALIAS_API_URL`` if non-empty.
    3. ``OPENAI_API_BASE`` if non-empty.
    4. Built-in Alias gateway default.

    If the model does not qualify, only steps 3–4 apply.

    ``model`` defaults to ``CAI_MODEL`` from the environment when omitted.
    """
    effective = (model if model is not None else os.getenv("CAI_MODEL")) or ""
    if model_qualifies_for_alias_api_url(effective):
        csi_url = (os.getenv("CSI_CUSTOM_ENDPOINT") or "").strip()
        if csi_url:
            return _normalize_api_base_url(csi_url)
        alias_url = (os.getenv("ALIAS_API_URL") or "").strip()
        if alias_url:
            return _normalize_api_base_url(alias_url)
    legacy = (os.getenv("OPENAI_API_BASE") or "").strip()
    if legacy:
        return _normalize_api_base_url(legacy)
    return DEFAULT_ALIAS_LLM_API_BASE


def resolve_llm_openai_compatible_api_key(model: str | None = None) -> str:
    """Resolve API key for OpenAI-compatible clients.

    Rules:
    - Alias-family models (``cai*``, ``alias*``, ``csi*``) MUST use ``ALIAS_API_KEY``.
      They must never fall back to ``OPENAI_API_KEY`` (prevents accidental 401s when
      OPENAI_API_KEY is a placeholder and the model is routed via the Alias gateway).
    - Non-alias models fall back to ``OPENAI_API_KEY``.
    """
    effective = (model if model is not None else os.getenv("CAI_MODEL")) or ""
    if model_qualifies_for_alias_api_url(effective):
        return (os.getenv("ALIAS_API_KEY") or "").strip()
    return (os.getenv("OPENAI_API_KEY") or "").strip()
