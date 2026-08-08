"""Conservative tick intervals from Alias-style TPM/RPM tiers (no key fingerprinting)."""

from __future__ import annotations

import os


def resolve_rate_tier() -> str:
    """Return ``pro`` or ``edu`` from ``CAI_ALIAS_RATE_TIER`` (default: ``pro``)."""
    raw = (os.getenv("CAI_ALIAS_RATE_TIER") or "pro").strip().lower()
    if raw in ("edu", "education", "educational", "student"):
        return "edu"
    return "pro"


def get_rate_limits(tier: str | None = None) -> tuple[int, int]:
    """Return ``(tokens_per_minute, requests_per_minute)`` for the tier."""
    t = (tier or resolve_rate_tier()).lower()
    if t == "edu":
        return 150_000, 20
    return 500_000, 60


def compute_base_tick_seconds(
    estimated_tokens_per_iteration: int,
    tier: str | None = None,
) -> float:
    """Lower bound on seconds between iterations to reduce 429 risk (heuristic).

    Uses the stricter of:
    - spacing implied by RPM (with headroom),
    - spacing implied by TPM vs estimated tokens per iteration (with headroom).
    """
    tpm, rpm = get_rate_limits(tier)
    est = max(int(estimated_tokens_per_iteration), 256)
    from_rpm = (60.0 / float(max(rpm, 1))) * 1.25
    from_tpm = (float(est) / float(max(tpm, 1))) * 60.0 * 1.25
    return max(1.0, from_rpm, from_tpm)


def min_allowed_tick_seconds(
    estimated_tokens_per_iteration: int,
    tier: str | None = None,
) -> float:
    """Minimum user-facing tick per product rule ``1.75 * base_time``."""
    return 1.75 * compute_base_tick_seconds(estimated_tokens_per_iteration, tier=tier)
