"""Tests for the client-side gateway rate limiter."""

from __future__ import annotations

import asyncio

import pytest

from cai.errors import LLMContextOverflow
from cai.util.gateway_rate_limiter import (
    COMPLETION_BUDGET_TOKENS,
    DEFAULT_SAFETY_MARGIN,
    GatewayRateLimiter,
    Reservation,
    get_gateway_rate_limiter,
    reset_gateway_rate_limiter,
)


# Tests that assert exact budget arithmetic at boundary conditions disable
# the safety margin so their expectations stay on nominal caps. Production
# always uses the default 0.85 margin (see ``DEFAULT_SAFETY_MARGIN``).
_RAW = {"safety_margin": 1.0}


@pytest.mark.asyncio
async def test_acquire_returns_zero_when_under_budget():
    rl = GatewayRateLimiter(tpm=1000, rpm=10, window_s=60, **_RAW)
    handle = await rl.acquire(100)
    assert handle.waited == 0.0


@pytest.mark.asyncio
async def test_acquire_paces_when_tpm_exceeded(monkeypatch):
    rl = GatewayRateLimiter(tpm=1000, rpm=100, window_s=60, **_RAW)
    slept: list[float] = []

    async def fake_sleep(seconds):
        slept.append(seconds)

    monkeypatch.setattr(asyncio, "sleep", fake_sleep)

    await rl.acquire(800)
    await rl.acquire(800)  # 800 + 800 > 1000 TPM → second call must wait
    assert len(slept) == 1
    assert slept[0] > 0


@pytest.mark.asyncio
async def test_acquire_paces_when_rpm_exceeded(monkeypatch):
    rl = GatewayRateLimiter(tpm=1_000_000, rpm=2, window_s=60, **_RAW)
    slept: list[float] = []

    async def fake_sleep(seconds):
        slept.append(seconds)

    monkeypatch.setattr(asyncio, "sleep", fake_sleep)

    await rl.acquire(1)
    await rl.acquire(1)
    await rl.acquire(1)  # third request exceeds RPM=2 → must wait
    assert len(slept) == 1
    assert slept[0] > 0


@pytest.mark.asyncio
async def test_acquire_raises_when_request_exceeds_tpm():
    """A request that alone breaches the nominal TPM cap cannot be paced —
    raise even with the safety margin in effect (the margin paces *within*
    the cap; oversize-by-itself is a different failure)."""
    rl = GatewayRateLimiter(tpm=1000, rpm=10, window_s=60)
    with pytest.raises(LLMContextOverflow) as exc_info:
        await rl.acquire(2000)
    assert exc_info.value.details["projected_tokens"] == 2000
    assert exc_info.value.details["tpm_limit"] == 1000


@pytest.mark.asyncio
async def test_on_pace_callback_receives_wait_seconds(monkeypatch):
    rl = GatewayRateLimiter(tpm=1000, rpm=100, window_s=60, **_RAW)

    async def fake_sleep(seconds):
        pass

    monkeypatch.setattr(asyncio, "sleep", fake_sleep)

    paced_with: list[float] = []

    await rl.acquire(900)
    await rl.acquire(900, on_pace=lambda s: paced_with.append(s))

    assert paced_with, "on_pace callback should fire when the second call waits"
    assert paced_with[0] > 0


@pytest.mark.asyncio
async def test_acquire_returns_reservation_with_waited_seconds():
    rl = GatewayRateLimiter(tpm=10_000, rpm=100, window_s=60, **_RAW)
    handle = await rl.acquire(100)
    assert isinstance(handle, Reservation)
    assert handle.waited == 0.0


@pytest.mark.asyncio
async def test_reservation_release_frees_slot_and_is_idempotent():
    rl = GatewayRateLimiter(tpm=1000, rpm=10, window_s=60, **_RAW)
    h1 = await rl.acquire(600)
    h2 = await rl.acquire(300)
    assert len(rl._events) == 2

    h1.release()
    assert len(rl._events) == 1
    h1.release()  # idempotent — should not raise or double-remove
    assert len(rl._events) == 1

    h2.release()
    assert len(rl._events) == 0


@pytest.mark.asyncio
async def test_pacing_sleep_cancellation_auto_releases_reservation(monkeypatch):
    """If the pacing ``asyncio.sleep`` is cancelled (e.g. KeyboardInterrupt),
    the entry should be removed before the exception propagates — otherwise
    the limiter over-paces subsequent requests for the next 60s."""
    rl = GatewayRateLimiter(tpm=1000, rpm=100, window_s=60, **_RAW)

    async def fake_sleep(seconds):
        raise asyncio.CancelledError

    monkeypatch.setattr(asyncio, "sleep", fake_sleep)

    await rl.acquire(800)  # fills the budget; this one doesn't sleep
    assert len(rl._events) == 1

    with pytest.raises(asyncio.CancelledError):
        await rl.acquire(800)  # triggers pacing → cancelled

    # The second entry was appended, then removed on cancellation.
    assert len(rl._events) == 1, "cancelled pacing must free its reservation"


def test_singleton_lazy_init_reads_tier_env(monkeypatch):
    reset_gateway_rate_limiter()
    monkeypatch.setenv("CAI_ALIAS_RATE_TIER", "edu")
    rl = get_gateway_rate_limiter()
    assert (rl._tpm, rl._rpm) == (150_000, 20)


def test_reset_rebuilds_singleton_with_new_tier(monkeypatch):
    reset_gateway_rate_limiter()
    monkeypatch.setenv("CAI_ALIAS_RATE_TIER", "pro")
    first = get_gateway_rate_limiter()
    assert (first._tpm, first._rpm) == (500_000, 60)

    monkeypatch.setenv("CAI_ALIAS_RATE_TIER", "edu")
    # Without reset, the singleton stays as 'pro'.
    assert get_gateway_rate_limiter() is first
    assert (first._tpm, first._rpm) == (500_000, 60)

    reset_gateway_rate_limiter()
    second = get_gateway_rate_limiter()
    assert second is not first
    assert (second._tpm, second._rpm) == (150_000, 20)


@pytest.mark.asyncio
async def test_alias_gateway_slot_releases_on_pre_gateway_exception():
    """``httpx.ConnectError`` raised inside the slot frees the reservation —
    the request demonstrably did not reach the gateway."""
    import httpx

    rl = GatewayRateLimiter(tpm=10_000, rpm=100, window_s=60, **_RAW)

    with pytest.raises(httpx.ConnectError):
        async with rl.alias_gateway_slot(500):
            raise httpx.ConnectError("dns failure")

    assert len(rl._events) == 0, "pre-gateway error should release the slot"


@pytest.mark.asyncio
async def test_alias_gateway_slot_keeps_reservation_on_http_status_error():
    """``httpx.HTTPStatusError`` means the gateway processed (and rejected)
    the request — those tokens were counted, so the reservation must stay."""
    import httpx

    rl = GatewayRateLimiter(tpm=10_000, rpm=100, window_s=60, **_RAW)

    with pytest.raises(httpx.HTTPStatusError):
        async with rl.alias_gateway_slot(500):
            raise httpx.HTTPStatusError(
                "429 too many requests",
                request=httpx.Request("POST", "http://x"),
                response=httpx.Response(429),
            )

    assert len(rl._events) == 1, "gateway-side error must keep the slot"


@pytest.mark.asyncio
async def test_alias_gateway_slot_releases_on_keyboard_interrupt():
    rl = GatewayRateLimiter(tpm=10_000, rpm=100, window_s=60, **_RAW)

    with pytest.raises(KeyboardInterrupt):
        async with rl.alias_gateway_slot(500):
            raise KeyboardInterrupt

    assert len(rl._events) == 0


@pytest.mark.asyncio
async def test_alias_gateway_slot_keeps_reservation_on_clean_exit():
    rl = GatewayRateLimiter(tpm=10_000, rpm=100, window_s=60, **_RAW)

    async with rl.alias_gateway_slot(500) as reservation:
        assert reservation.waited == 0.0

    # Successful path: gateway processed the request, reservation persists.
    assert len(rl._events) == 1


def test_completion_budget_constant_exposed():
    assert COMPLETION_BUDGET_TOKENS > 0


@pytest.mark.asyncio
async def test_reservation_update_actual_replaces_token_count():
    """``update_actual`` should overwrite the pre-flight estimate so the deque
    reflects the real prompt+completion charge after the response."""
    rl = GatewayRateLimiter(tpm=10_000, rpm=100, window_s=60, **_RAW)
    handle = await rl.acquire(1500)  # pre-flight projection
    assert sum(t for _, t in rl._events) == 1500

    handle.update_actual(2300)  # gateway charged 2300 tokens
    assert sum(t for _, t in rl._events) == 2300


@pytest.mark.asyncio
async def test_reservation_update_actual_after_release_is_noop():
    rl = GatewayRateLimiter(tpm=10_000, rpm=100, window_s=60, **_RAW)
    handle = await rl.acquire(500)
    handle.release()
    assert len(rl._events) == 0

    # After release, update_actual must not resurrect the entry or raise.
    handle.update_actual(900)
    assert len(rl._events) == 0


# ---------------- Safety margin -------------------------------------------


def test_default_safety_margin_value():
    """The exported default is what production picks up via the singleton."""
    assert DEFAULT_SAFETY_MARGIN == 0.85


def test_safety_margin_caps_effective_tpm_and_rpm():
    rl = GatewayRateLimiter(tpm=500_000, rpm=60, window_s=60)  # default 0.85
    assert rl._tpm == 500_000
    assert rl._rpm == 60
    assert rl._tpm_pace == 425_000
    assert rl._rpm_pace == 51


@pytest.mark.asyncio
async def test_safety_margin_paces_earlier_than_nominal_cap(monkeypatch):
    """With margin=0.5, a 600+600 burst on a 1000-TPM cap must pace at the
    SECOND request — under raw rules it would only pace at the third
    (600+600=1200>1000 still fits under the margin=1.0 path? Actually yes,
    1200>1000 already paces. Use 400+400 to differentiate)."""
    rl = GatewayRateLimiter(tpm=1000, rpm=100, window_s=60, safety_margin=0.5)
    slept: list[float] = []

    async def fake_sleep(seconds):
        slept.append(seconds)

    monkeypatch.setattr(asyncio, "sleep", fake_sleep)

    # tpm_pace = 500; first req of 400 fits; second of 400 → 800 > 500 → pace
    await rl.acquire(400)
    await rl.acquire(400)
    assert len(slept) == 1, "margin should force pacing before nominal cap"


@pytest.mark.asyncio
async def test_safety_margin_does_not_block_oversized_with_overflow_error():
    """A single request projecting > NOMINAL tpm still raises ``LLMContextOverflow``
    regardless of the (smaller) effective cap — the margin paces within the
    nominal cap; oversize-by-itself is a context problem, not a pacing one."""
    rl = GatewayRateLimiter(tpm=1000, rpm=10, window_s=60, safety_margin=0.5)
    # 600 > effective (500) but < nominal (1000) → must pace, NOT raise.
    handle = await rl.acquire(600)
    assert isinstance(handle, Reservation)

    # 2000 > nominal (1000) → must raise.
    with pytest.raises(LLMContextOverflow):
        await rl.acquire(2000)
