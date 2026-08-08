"""Context-aware environment overrides for parallel agent execution."""
from __future__ import annotations

import asyncio
import contextvars
import os
import subprocess
from collections.abc import Mapping
from contextlib import asynccontextmanager, contextmanager
from typing import Any, Iterable

_ENV_OVERRIDES: contextvars.ContextVar[dict[str, str] | None] = contextvars.ContextVar(
    "cai_env_overrides", default=None
)

_PATCHED = False


def _normalize_overrides(raw: Mapping[str, Any]) -> dict[str, str]:
    """Return a normalized string dictionary."""
    normalized: dict[str, str] = {}
    for key, value in raw.items():
        if value is None:
            continue
        normalized[str(key)] = str(value)
    return normalized


def _get_overrides() -> dict[str, str] | None:
    """Return overrides for the current context."""
    overrides = _ENV_OVERRIDES.get()
    if overrides:
        return overrides
    return None


def _merge_env(env: Mapping[str, str] | None, overrides: Mapping[str, str]) -> dict[str, str]:
    """Merge overrides into an environment mapping."""
    if env is None:
        merged: dict[str, str] = dict(os.environ.copy())
    else:
        merged = dict(env)
    merged.update(overrides)
    return merged


def _ensure_patched() -> None:
    """Patch stdlib helpers exactly once."""
    global _PATCHED
    if _PATCHED:
        return

    _PATCHED = True

    original_getitem = os._Environ.__getitem__
    original_get = os._Environ.get
    original_iter = os._Environ.__iter__
    original_copy = os._Environ.copy

    def _context_getitem(self, key):  # type: ignore[override]
        overrides = _get_overrides()
        if overrides and key in overrides:
            return overrides[key]
        return original_getitem(self, key)

    def _context_get(self, key, default=None):  # type: ignore[override]
        overrides = _get_overrides()
        if overrides and key in overrides:
            return overrides[key]
        return original_get(self, key, default)

    def _context_iter(self) -> Iterable[str]:  # type: ignore[override]
        overrides = _get_overrides()
        yielded = set()
        for key in original_iter(self):
            yielded.add(key)
            yield key
        if overrides:
            for key in overrides:
                if key not in yielded:
                    yield key

    def _context_copy(self):  # type: ignore[override]
        data = original_copy(self)
        overrides = _get_overrides()
        if overrides:
            data.update(overrides)
        return data

    os._Environ.__getitem__ = _context_getitem  # type: ignore[assignment]
    os._Environ.get = _context_get  # type: ignore[assignment]
    os._Environ.__iter__ = _context_iter  # type: ignore[assignment]
    os._Environ.copy = _context_copy  # type: ignore[assignment]

    original_popen = subprocess.Popen

    class _PatchedPopen(original_popen):  # type: ignore[misc]
        def __init__(self, *args, **kwargs):
            overrides = _get_overrides()
            if overrides:
                kwargs["env"] = _merge_env(kwargs.get("env"), overrides)
            super().__init__(*args, **kwargs)

    subprocess.Popen = _PatchedPopen  # type: ignore[assignment]

    original_create_exec = asyncio.create_subprocess_exec
    original_create_shell = asyncio.create_subprocess_shell

    async def _patched_create_exec(*args, env=None, **kwargs):  # type: ignore[override]
        overrides = _get_overrides()
        if overrides:
            env = _merge_env(env, overrides)
        return await original_create_exec(*args, env=env, **kwargs)

    async def _patched_create_shell(*args, env=None, **kwargs):  # type: ignore[override]
        overrides = _get_overrides()
        if overrides:
            env = _merge_env(env, overrides)
        return await original_create_shell(*args, env=env, **kwargs)

    asyncio.create_subprocess_exec = _patched_create_exec  # type: ignore[assignment]
    asyncio.create_subprocess_shell = _patched_create_shell  # type: ignore[assignment]


@contextmanager
def environment_override(overrides: Mapping[str, Any] | None):
    """Synchronously apply environment overrides."""
    if not overrides:
        yield
        return

    _ensure_patched()

    normalized = _normalize_overrides(overrides)
    if not normalized:
        yield
        return

    current = _ENV_OVERRIDES.get()
    combined = dict(current) if current else {}
    combined.update(normalized)
    token = _ENV_OVERRIDES.set(combined)
    try:
        yield
    finally:
        _ENV_OVERRIDES.reset(token)


@asynccontextmanager
async def async_environment_override(overrides: Mapping[str, Any] | None):
    """Async helper that delegates to environment_override."""
    with environment_override(overrides):
        yield


__all__ = ["environment_override", "async_environment_override"]
