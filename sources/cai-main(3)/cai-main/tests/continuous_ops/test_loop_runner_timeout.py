"""Wall-clock timeout policy for continuous-ops ticks."""

from cai.continuous_ops.loop_runner import CAI_TICK_WALL_RC, tick_wall_timeout_seconds


def test_tick_wall_timeout_minimum_120():
    assert tick_wall_timeout_seconds(1) == 120.0
    assert tick_wall_timeout_seconds(30) == 120.0
    assert tick_wall_timeout_seconds(59) == 120.0


def test_tick_wall_timeout_double_when_above_60s_tick():
    assert tick_wall_timeout_seconds(60) == 120.0
    assert tick_wall_timeout_seconds(90) == 180.0
    assert tick_wall_timeout_seconds(300) == 600.0


def test_timeout_exit_code_constant():
    assert CAI_TICK_WALL_RC == 124
