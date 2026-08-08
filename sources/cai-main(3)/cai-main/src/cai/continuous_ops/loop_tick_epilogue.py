"""Extra turns + snapshot export at end of a continuous-ops tick (``cai`` subprocess)."""

from __future__ import annotations

import os
from pathlib import Path

_TICK_DONE = "[TICK_COMPLETE]"


def _max_turns_per_tick() -> int:
    try:
        v = int((os.getenv("CAI_COPS_MAX_TURNS_PER_TICK") or "20").strip())
    except ValueError:
        v = 20
    return max(1, min(100, v))


def _tick_complete_in_history(agent) -> bool:
    hist = getattr(getattr(agent, "model", None), "message_history", None) or []
    for msg in reversed(hist[-24:]):
        if msg.get("role") != "assistant":
            continue
        c = msg.get("content")
        if isinstance(c, str) and _TICK_DONE in c:
            return True
    return False


def run_continuous_ops_extra_turns(agent, console, force_until_flag, ctf_global) -> None:
    """After the first model turn of this subprocess, optionally run follow-up turns."""
    from cai.cli_headless import _run_single_agent

    max_n = _max_turns_per_tick()
    follow = (
        "You are still inside the same continuous-ops tick session. Continue executing any "
        "remaining work implied by the operator instructions and prior tool results. "
        "When everything for this tick is finished, end your reply with a line containing only "
        f"{_TICK_DONE} (and keep any required status line such as [STATUS: OK] / [STATUS: INCIDENT])."
    )
    rounds = 1
    while rounds < max_n:
        if _tick_complete_in_history(agent):
            break
        _run_single_agent(agent, follow, console, force_until_flag, ctf_global)
        rounds += 1


def export_loop_child_snapshot(agent) -> None:
    out = (os.getenv("CAI_COPS_SNAPSHOT_OUT") or "").strip()
    if not out:
        return
    path = Path(out).expanduser()
    hist = getattr(getattr(agent, "model", None), "message_history", None)
    if not hist:
        return
    try:
        from cai.continuous_ops.session_snapshot import export_snapshot

        export_snapshot(path, hist)
    except Exception:
        pass


def maybe_import_snapshot_before_cli_loop(agent, history_key: str) -> None:
    """Load prior tick snapshot into the active agent (call after ``switch_to_single_agent``)."""
    inp = (os.getenv("CAI_COPS_SNAPSHOT_IN") or "").strip()
    if not inp or agent is None:
        return
    path = Path(inp).expanduser()
    if not path.is_file():
        return
    try:
        from cai.continuous_ops.session_snapshot import apply_snapshot_to_agent, load_snapshot_messages

        msgs = load_snapshot_messages(path)
        if msgs:
            apply_snapshot_to_agent(agent, msgs, history_key=history_key)
    except Exception:
        pass
