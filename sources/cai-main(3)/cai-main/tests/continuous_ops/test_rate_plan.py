"""Unit tests for continuous-ops rate / tick planning."""

from cai.continuous_ops.rate_plan import (
    compute_base_tick_seconds,
    get_rate_limits,
    min_allowed_tick_seconds,
)


def test_get_rate_limits_pro_edu():
    tpm, rpm = get_rate_limits("pro")
    assert tpm == 500_000 and rpm == 60
    tpm_e, rpm_e = get_rate_limits("edu")
    assert tpm_e == 150_000 and rpm_e == 20


def test_min_tick_scales_with_estimated_tokens():
    lo = min_allowed_tick_seconds(2_000, tier="pro")
    hi = min_allowed_tick_seconds(100_000, tier="pro")
    assert hi > lo


def test_edu_stricter_than_pro_for_same_tokens():
    est = 50_000
    assert min_allowed_tick_seconds(est, tier="edu") >= min_allowed_tick_seconds(est, tier="pro")


def test_min_is_175_times_base():
    est = 10_000
    tier = "pro"
    base = compute_base_tick_seconds(est, tier=tier)
    assert abs(min_allowed_tick_seconds(est, tier=tier) - 1.75 * base) < 1e-6
