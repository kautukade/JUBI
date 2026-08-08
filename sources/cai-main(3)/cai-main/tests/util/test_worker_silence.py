"""Unit tests for :mod:`cai.util._worker_silence`.

The ContextVar gate is the foundation for keeping sub-agent (worker)
output out of the user-facing transcript when the orchestration agent
calls them as tools. Verifying ``set / reset / nested`` semantics here
is cheap and prevents regressions in the orchestration UI contract.
"""

from __future__ import annotations

import asyncio

import pytest

from cai.util._worker_silence import (
    silence_worker_display,
    worker_display_silenced,
)


def test_default_is_false_outside_context() -> None:
    assert worker_display_silenced() is False


def test_active_inside_context_and_restored_on_exit() -> None:
    assert worker_display_silenced() is False
    with silence_worker_display():
        assert worker_display_silenced() is True
    assert worker_display_silenced() is False


def test_nested_context_keeps_silenced_until_outer_exits() -> None:
    """Nested ``silence_worker_display`` must not flip back to ``False`` early."""
    with silence_worker_display():
        assert worker_display_silenced() is True
        with silence_worker_display():
            assert worker_display_silenced() is True
        # Inner exit must NOT drop us back to False — outer context still owns the silence.
        assert worker_display_silenced() is True
    assert worker_display_silenced() is False


def test_exception_inside_context_still_restores() -> None:
    with pytest.raises(RuntimeError):
        with silence_worker_display():
            assert worker_display_silenced() is True
            raise RuntimeError("boom")
    assert worker_display_silenced() is False


@pytest.mark.asyncio
async def test_silence_propagates_into_awaited_coroutine() -> None:
    """``ContextVar`` values flow through ``await`` boundaries in the same task."""
    seen: list[bool] = []

    async def inner() -> None:
        seen.append(worker_display_silenced())
        await asyncio.sleep(0)
        seen.append(worker_display_silenced())

    with silence_worker_display():
        await inner()

    assert seen == [True, True]
    assert worker_display_silenced() is False


@pytest.mark.asyncio
async def test_silence_isolated_between_concurrent_tasks() -> None:
    """Two ``asyncio.gather`` tasks must not bleed silence into each other."""

    async def silent_branch() -> bool:
        with silence_worker_display():
            await asyncio.sleep(0)
            return worker_display_silenced()

    async def loud_branch() -> bool:
        await asyncio.sleep(0)
        return worker_display_silenced()

    silent, loud = await asyncio.gather(silent_branch(), loud_branch())
    assert silent is True
    assert loud is False
    assert worker_display_silenced() is False
