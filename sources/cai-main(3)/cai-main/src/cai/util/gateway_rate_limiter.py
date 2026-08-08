"""Client-side pacing for the shared Alias gateway.

The Alias gateway enforces per-API-key TPM (tokens-per-minute) and RPM
(requests-per-minute) caps. Without client-side pacing, a moderately busy CAI
session burns through both budgets within seconds (a single ~30K-token request
can consume 6% of the pro-tier per-minute token budget) and the gateway
responds with HTTP 429 or empty completions — both surface to the user as
silent lag while CAI reactively retries.

This module enforces those caps **before** each outbound request, queueing
calls until the sliding 60s window has capacity.

Limits source of truth: ``cai.continuous_ops.rate_plan.get_rate_limits()``,
resolved against the ``CAI_ALIAS_RATE_TIER`` env (``pro`` → 500K TPM / 60 RPM,
``edu`` → 150K TPM / 20 RPM). Tier is the only user-facing knob — exposing
TPM/RPM separately would be misleading because the gateway throttles per the
contract bound to the API key, not per local override.

If the user changes ``CAI_ALIAS_RATE_TIER`` mid-session (e.g. via ``/env``),
call :func:`reset_gateway_rate_limiter` so the next ``acquire`` rebuilds the
singleton with the new tier.
"""

from __future__ import annotations

import asyncio
import threading
import time
from collections import deque
from contextlib import asynccontextmanager
from typing import AsyncIterator, Callable, Deque, Tuple

from cai.errors import LLMContextOverflow

_WINDOW_S: float = 60.0

# Pre-flight token buffer added to ``estimated_input_tokens`` before pacing.
# Covers the unknown completion size the gateway will charge against TPM but
# we cannot estimate before the call. After the response arrives, callers
# replace the projection with actual ``prompt + completion`` via
# :meth:`Reservation.update_actual`, so this only matters for the first
# acquire and on streaming (where usage arrives at end-of-stream).
COMPLETION_BUDGET_TOKENS: int = 1024

# Pace requests against this fraction of the nominal TPM/RPM caps. The Alias
# gateway does NOT expose ``x-ratelimit-*`` response headers, so the only
# ground truth we have is the per-request ``usage`` returned in the body.
# Tiktoken estimates run ~5-10% under real ``prompt_tokens`` and the gateway
# accounts for completion + system overhead we cannot predict in advance,
# so pacing at 100% of nominal hits 429 from drift. 0.85 absorbs that drift
# with single-digit % throughput cost.
DEFAULT_SAFETY_MARGIN: float = 0.85

# Type alias for a deque entry. Kept module-level so ``Reservation`` and the
# limiter use the same shape.
_Entry = Tuple[float, int]


class Reservation:
    """Handle for a slot reserved by :meth:`GatewayRateLimiter.acquire`.

    Callers that detect the request never actually reached the gateway (e.g.
    ``KeyboardInterrupt`` between acquire and fetch, or a known client-side
    connection error) MAY call :meth:`release` to free the slot before the
    60s window expires. For errors where the gateway did receive the request
    (429, timeout, HTTP error response, empty completion), keep the slot —
    the gateway counted those tokens against the budget anyway.

    ``release()`` is idempotent and safe to call after the entry has expired
    naturally.
    """

    __slots__ = ("_limiter", "_entry", "_released", "waited")

    def __init__(self, limiter: "GatewayRateLimiter", entry: _Entry, waited: float) -> None:
        self._limiter = limiter
        self._entry = entry
        self._released = False
        # Exposed so callers that previously used the ``float`` return value
        # of ``acquire`` can still read how long they paced.
        self.waited = waited

    def release(self) -> None:
        """Free the reserved slot.

        Safe to call from any coroutine on the same event loop: asyncio is
        single-threaded so this method cannot interleave with another
        coroutine's ``append`` or ``remove`` between Python instructions —
        the deque is only mutated between explicit ``await`` points. If the
        entry has already expired naturally (``_prune``) or been released
        previously, this is a silent no-op.

        Do NOT call from a thread other than the one running the event loop;
        that would require a separate sync lock the limiter does not own.
        """
        if self._released:
            return
        self._released = True
        self._limiter._remove_entry(self._entry)

    def update_actual(self, actual_total_tokens: int) -> None:
        """Replace the pre-flight token estimate with the gateway's real count.

        The gateway charges the per-minute TPM budget for ``prompt_tokens +
        completion_tokens``, not just the input tiktoken estimate we used at
        acquire time. Calling this after the response arrives with
        ``response.usage.prompt_tokens + completion_tokens`` keeps the
        deque aligned with the gateway's authoritative accounting, so the
        next acquire's budget check matches reality.

        No-op if the reservation was already released or has expired out of
        the deque.
        """
        if self._released:
            return
        new_count = max(0, int(actual_total_tokens))
        new_entry: _Entry = (self._entry[0], new_count)
        try:
            idx = self._limiter._events.index(self._entry)
            self._limiter._events[idx] = new_entry
            self._entry = new_entry
        except ValueError:
            # Already pruned by the 60s window — nothing to update.
            pass


class GatewayRateLimiter:
    """Sliding-window TPM/RPM limiter shared across all coroutines.

    Concurrent agents share a single instance because the gateway budget is
    per API-key, not per agent. ``window_s`` is parametrised so tests can use
    short windows without sleeping for real minutes.
    """

    def __init__(
        self,
        tpm: int,
        rpm: int,
        window_s: float = _WINDOW_S,
        safety_margin: float = DEFAULT_SAFETY_MARGIN,
    ) -> None:
        # Nominal caps (used to detect "request too large to ever fit").
        self._tpm = tpm
        self._rpm = rpm
        # Effective caps for pacing — see ``DEFAULT_SAFETY_MARGIN``. We
        # never let the local sum cross these so the gateway, whose
        # accounting drifts a few % from ours, has headroom before 429ing.
        margin = max(0.01, min(1.0, float(safety_margin)))
        self._tpm_pace = max(1, int(tpm * margin))
        self._rpm_pace = max(1, int(rpm * margin))
        self._window_s = window_s
        # Each entry: (effective_timestamp, projected_tokens). The timestamp
        # is the moment the request will actually go out (now + planned wait),
        # so concurrent acquires immediately see the reservation.
        self._events: Deque[_Entry] = deque()
        self._lock = asyncio.Lock()

    def _prune(self, now: float) -> None:
        cutoff = now - self._window_s
        while self._events and self._events[0][0] < cutoff:
            self._events.popleft()

    def _remove_entry(self, entry: _Entry) -> None:
        """Best-effort removal — silent no-op if already pruned or released."""
        try:
            self._events.remove(entry)
        except ValueError:
            pass

    def _budget_until(self, now: float, projected_tokens: int) -> float:
        """Seconds the caller must wait before a request of ``projected_tokens``
        can be sent without breaching TPM or RPM. Zero if it can go now.

        Raises :class:`LLMContextOverflow` if a single request exceeds the TPM
        cap — pacing cannot help, the body itself must shrink.
        """
        # A single request larger than the per-minute token budget can never
        # be paced through: even after every existing reservation expires, the
        # request alone breaches the cap. Surface as ``LLMContextOverflow`` so
        # the REPL can guide the user to ``/compact`` or ``/flush``. The
        # ``details`` keys (``projected_tokens``/``tpm_limit``) tell the panel
        # renderer this is a TPM-budget overflow, not an HTTP 413 from the
        # gateway proxy.
        if projected_tokens > self._tpm:
            raise LLMContextOverflow(
                f"Request projects {projected_tokens} tokens, exceeds gateway "
                f"TPM budget of {self._tpm}. Use /compact or /flush to shrink "
                "history.",
                details={
                    "projected_tokens": projected_tokens,
                    "tpm_limit": self._tpm,
                    "origin": "client_rate_limiter",
                },
            )

        self._prune(now)
        if not self._events:
            return 0.0
        used_tokens = sum(t for _, t in self._events)
        used_requests = len(self._events)

        token_wait = 0.0
        # Pace against the safety-margined effective cap, not the nominal cap.
        if used_tokens + projected_tokens > self._tpm_pace:
            # Find the earliest entry whose expiry frees enough tokens to fit
            # the new request.
            need = used_tokens + projected_tokens - self._tpm_pace
            running = 0
            for ts, t in self._events:
                running += t
                if running >= need:
                    token_wait = max(0.0, ts + self._window_s - now)
                    break

        req_wait = 0.0
        if used_requests + 1 > self._rpm_pace:
            req_wait = max(0.0, self._events[0][0] + self._window_s - now)

        return max(token_wait, req_wait)

    async def acquire(
        self,
        projected_tokens: int,
        *,
        on_pace: Callable[[float], None] | None = None,
    ) -> Reservation:
        """Reserve a slot, sleeping if needed. Returns a :class:`Reservation`.

        ``on_pace(seconds)`` is invoked just before the sleep when a wait is
        required, so callers can surface "pacing for Ns…" to the user via the
        wait-hint overlay.

        Concurrency: the reservation is appended with ``effective_ts = now +
        wait`` **before** the lock is released, so concurrent ``acquire``
        calls see the slot reserved and compute their own wait correctly. No
        re-validation after ``asyncio.sleep`` by design — the future-timestamp
        pattern already handles the common case, and an extra lock + recompute
        round costs more than it saves.

        Cancellation: if the pacing ``asyncio.sleep`` is cancelled (e.g.
        ``KeyboardInterrupt`` while waiting), the reservation is auto-released
        before the exception propagates — the request never left the client.

        Raises :class:`LLMContextOverflow` if ``projected_tokens`` exceeds the
        TPM cap by itself (no amount of pacing can let it through). The
        exception is raised BEFORE any reservation is appended.
        """
        async with self._lock:
            now = time.monotonic()
            wait = self._budget_until(now, projected_tokens)
            effective_ts = now + max(0.0, wait)
            entry: _Entry = (effective_ts, max(0, int(projected_tokens)))
            self._events.append(entry)

        if wait > 0:
            if on_pace is not None:
                try:
                    on_pace(wait)
                except Exception:
                    pass
            try:
                await asyncio.sleep(wait)
            except BaseException:
                # Pacing cancelled — the request never went out. Free the
                # slot so subsequent acquires don't over-pace.
                self._remove_entry(entry)
                raise

        return Reservation(self, entry, waited=wait)

    @asynccontextmanager
    async def alias_gateway_slot(
        self,
        projected_tokens: int,
        *,
        on_pace: Callable[[float], None] | None = None,
    ) -> AsyncIterator[Reservation]:
        """Acquire a slot, yield the reservation, auto-release on pre-gateway errors.

        Pre-gateway errors mean the request demonstrably did not reach the
        gateway (couldn't connect, write failed mid-send, user Ctrl-C'd before
        the HTTP send finished). In those cases the gateway didn't count the
        request, so we free the reservation; otherwise the limiter would
        over-pace for the next 60s after a burst of connection failures.

        Errors NOT released (gateway likely counted the request):
            * HTTP 4xx/5xx (``httpx.HTTPStatusError``) — gateway processed
            * ``httpx.ReadError`` / ``ReadTimeout`` — request sent, response
              never arrived (ambiguous, conservative keep)
            * ``litellm.exceptions.RateLimitError`` / ``Timeout`` — same
            * Any other exception not in the pre-gateway set
        """
        reservation = await self.acquire(projected_tokens, on_pace=on_pace)
        try:
            yield reservation
        except BaseException as exc:
            if _is_pre_gateway_exception(exc):
                reservation.release()
            raise


def _is_pre_gateway_exception(exc: BaseException) -> bool:
    """True when ``exc`` indicates the HTTP request did not finish writing to
    the gateway. Imports of httpx/litellm are lazy so this module stays
    importable in environments where they are not installed (tests, tooling)."""
    if isinstance(exc, KeyboardInterrupt):
        return True
    try:
        import httpx

        # ``ConnectError`` / ``ConnectTimeout`` / ``PoolTimeout``: socket
        # never established. ``WriteError`` / ``WriteTimeout``: request body
        # could not be fully sent.
        if isinstance(
            exc,
            (
                httpx.ConnectError,
                httpx.ConnectTimeout,
                httpx.PoolTimeout,
                httpx.WriteError,
                httpx.WriteTimeout,
            ),
        ):
            return True
    except ImportError:
        pass
    try:
        import litellm.exceptions as _le

        # LiteLLM's wrapper for the same underlying connection failures.
        if isinstance(exc, _le.APIConnectionError):
            return True
    except (ImportError, AttributeError):
        pass
    return False


_GATEWAY_RATE_LIMITER: GatewayRateLimiter | None = None
_LIMITER_INIT_LOCK = threading.Lock()


def get_gateway_rate_limiter() -> GatewayRateLimiter:
    """Process-wide singleton (one budget per API key).

    Lazily instantiated so ``CAI_ALIAS_RATE_TIER`` is read from the environment
    that's actually live when the first request fires (not at import time,
    when ``.env`` may not yet be loaded).
    """
    global _GATEWAY_RATE_LIMITER
    if _GATEWAY_RATE_LIMITER is None:
        with _LIMITER_INIT_LOCK:
            if _GATEWAY_RATE_LIMITER is None:
                from cai.continuous_ops.rate_plan import (
                    get_rate_limits,
                    resolve_rate_tier,
                )

                tpm, rpm = get_rate_limits(resolve_rate_tier())
                _GATEWAY_RATE_LIMITER = GatewayRateLimiter(tpm=tpm, rpm=rpm)
    return _GATEWAY_RATE_LIMITER


def reset_gateway_rate_limiter() -> None:
    """Discard the cached singleton so the next ``get_gateway_rate_limiter()``
    rebuilds it with the current ``CAI_ALIAS_RATE_TIER``.

    Intended for code paths that change the tier mid-session (e.g. an ``/env``
    handler). In-flight ``Reservation`` handles tied to the previous limiter
    keep working but operate on a deque no one else reads, which is fine —
    they will be garbage-collected after the caller finishes.
    """
    global _GATEWAY_RATE_LIMITER
    with _LIMITER_INIT_LOCK:
        _GATEWAY_RATE_LIMITER = None


def make_pace_overlay_callback() -> Callable[[float], None]:
    """Shared ``on_pace`` callback used by both streaming and non-streaming
    paths: surfaces "Rate budget reached, pacing for Ns…" via the model wait
    hint overlay. Returned as a fresh closure each call so callers can pass
    it directly to ``acquire(on_pace=...)``."""
    # Import locally to avoid a circular import at module load time
    # (cai.util.wait_hints imports nothing from this module, but the broader
    # cai.util package has cross-module touches — keep this defensive).
    from cai.util.wait_hints import set_model_wait_retry_overlay

    def _on_pace(seconds: float) -> None:
        set_model_wait_retry_overlay(
            f"Rate budget reached, pacing for {seconds:.0f}s…"
        )

    return _on_pace
