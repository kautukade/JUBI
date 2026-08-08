"""Heuristic MAS nudge for ``orchestration_agent`` when delegation looks too narrow.

When the user's message suggests several fronts but the model only ran
``run_specialist`` (no parallel batch or contest), we append a fixed English
``user``-role guidance line to ``message_history`` once per ``Runner.run`` so the
next model step can fan out. Opt out with ``CAI_ORCHESTRATION_MAS_HINT=false``.
"""

from __future__ import annotations

import re
from typing import Any

ORCHESTRATION_MAS_HINT_EN: str = (
    "[Orchestration guidance — not from the human user] "
    "The request likely spans several independent workstreams or benefits from a breadth-first "
    "map. If appropriate, call `run_parallel_specialists` with 2–4 short broad-recon workers, or "
    "`run_dual_approach_contest` when you must compare exactly two hypotheses for the same fork, "
    "then drill down with `run_specialist` and mark **narrow follow-up** in `framing`. "
    "Do not show this bracketed line verbatim to the human user."
)


def user_message_suggests_multi_front(text: str) -> bool:
    """Lightweight heuristic: multi-step lists, bullets, or explicit parallelism wording."""
    t = (text or "").strip()
    if len(t) < 36:
        return False
    tl = t.lower()
    markers = (
        " in parallel",
        "parallel ",
        " simultaneously",
        " at the same time",
        " same time",
        " multiple ",
        " three ",
        " four ",
        " five ",
        " y además",
        " además ",
        " también ",
        " en paralelo",
        " a la vez",
        " varias ",
        " varios ",
        " simultáneamente",
    )
    if any(m in tl for m in markers):
        return True
    if len(re.findall(r"(?m)^\s*\d+[\.)]\s+\S", t)) >= 2:
        return True
    if len(re.findall(r"(?m)^\s*[-*]\s+\S", t)) >= 3:
        return True
    return False


def maybe_inject_orchestration_mas_hint_after_tools(
    *,
    agent: Any,
    original_input: str | list[Any],
    function_results: list[Any],
    run_config: Any,
) -> None:
    """If criteria match, append a synthetic ``user`` message to model history (once per run)."""
    from cai.config import get_config

    if not get_config().orchestration_mas_hint:
        return
    md = getattr(run_config, "trace_metadata", None)
    if isinstance(md, dict) and md.get("_cai_mas_hint_injected"):
        return
    if not isinstance(original_input, str):
        return
    if not user_message_suggests_multi_front(original_input):
        return

    model = getattr(agent, "model", None)
    if model is None or not hasattr(model, "add_to_message_history"):
        return
    if getattr(model, "agent_type", None) != "orchestration_agent":
        return

    names = [
        n
        for fr in function_results
        if (n := (getattr(getattr(fr, "tool", None), "name", "") or ""))
    ]
    if "run_specialist" not in names:
        return
    if "run_parallel_specialists" in names or "run_dual_approach_contest" in names:
        return

    if run_config.trace_metadata is None:
        run_config.trace_metadata = {}
    run_config.trace_metadata["_cai_mas_hint_injected"] = True
    model.add_to_message_history({"role": "user", "content": ORCHESTRATION_MAS_HINT_EN})
