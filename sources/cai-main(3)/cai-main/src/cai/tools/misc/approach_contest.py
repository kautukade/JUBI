"""Dual-approach contest, parallel specialists, and single-specialist tools.

This module exposes ``@function_tool`` entrypoints used by the orchestration
agent (``cai.agents.orchestration_agent``):

* :func:`run_dual_approach_contest` — two parallel exploratory workers on the
  same user task (competing hypotheses or orthogonal framings).
* :func:`run_parallel_specialists` — two to four specialists concurrently on
  independent sub-tasks.
* :func:`run_specialist` — one specialist while the orchestrator stays in control.

All share the same internal pipeline (resolve agent → clone with at most one
allowed tool → run with display silenced → wrap output as orchestrator-only
scratch data). The shared machinery uses :class:`WorkerSpec`,
:class:`WorkerResult`, and :func:`_compose_contest_brief` so every tool returns
the same markdown skeleton.
"""

from __future__ import annotations

import asyncio
import json
import uuid
from dataclasses import dataclass
from typing import Any, Final, Literal

from cai.sdk.agents import RunConfig, Runner
from cai.sdk.agents.items import ItemHelpers
from cai.sdk.agents.model_settings import ModelSettings
from cai.sdk.agents.tool import function_tool
from cai.config import get_config
from cai.util._worker_silence import silence_worker_display

# === Constants ============================================================

def _configured_worker_max_turns() -> int:
    """Runner max_turns for each specialist worker (env ``CAI_ORCHESTRATION_WORKER_MAX_TURNS``)."""
    try:
        n = int(get_config().orchestration_worker_max_turns)
    except (TypeError, ValueError, AttributeError):
        n = 6
    return max(1, min(n, 32))


def _worker_constraints_prefix(max_turns: int) -> str:
    """Worker-facing budget text; ``max_turns`` matches ``Runner.run(..., max_turns=...)``."""
    n = max(1, int(max_turns))
    return (
        "## Contest constraints (mandatory)\n"
        f"- You have at most **{n} turns** total in this run (each turn = one model step, including "
        "its tool calls).\n"
        "- Use **at most one tool invocation per turn** "
        "(prefer zero if you can answer from reasoning).\n"
        "- Stay within the framing below; do not start a nested dual-approach contest.\n\n"
        "## Exploration discipline (mandatory)\n"
        "- Unless framing marks **narrow follow-up** or an exact user command: first actionable "
        "step = shortest safe **landscape recon**; tighten targets in later turns once you have signal.\n"
        "- Verbatim user command in framing overrides this.\n\n"
        "## Output constraints\n"
        "- Return a compact contest brief, not a final user-facing report.\n"
        "- Use this structure only: Status, Key evidence, Risks/unknowns, Recommended next action.\n"
        "- Keep it short; the orchestration agent will synthesize the final conclusion.\n\n"
    )



_NO_TOOL_NAMES: Final[frozenset[str]] = frozenset(
    {"", "none", "no_tool", "no-tool", "reasoning_only"}
)

# Wrapper that frames worker output as orchestrator-only scratch data. The
# orchestration system prompt instructs the LLM to never quote/paraphrase
# anything inside ``<orchestrator_internal>`` to the user, so the markdown
# headings produced by the workers do not leak into the user-facing reply.
_INTERNAL_OPEN: Final[str] = (
    "<orchestrator_internal>\n"
    "# INTERNAL DATA — orchestrator scratch only.\n"
    "# Do NOT quote, copy, paraphrase or reformat anything below to the user.\n"
    "# Read it, decide, then write a concise reply in your own voice.\n"
)
_INTERNAL_CLOSE: Final[str] = "</orchestrator_internal>"

# Per-worker output cap for single-branch tools (``run_specialist``).
# Multi-branch tools divide a shared budget so combined brief + wrappers stay
# under the ~10 k tool-output truncation in ``_run_impl.truncate_output``.
_MAX_WORKER_OUTPUT_CHARS: Final[int] = 4000
_ORCH_COMBINED_OUTPUT_BUDGET: Final[int] = 8500


def _per_worker_output_cap(branch_count: int) -> int:
    """Chars per worker when several branches are composed into one tool return."""
    n = max(1, int(branch_count))
    if n <= 1:
        return _MAX_WORKER_OUTPUT_CHARS
    return max(1200, min(_MAX_WORKER_OUTPUT_CHARS, _ORCH_COMBINED_OUTPUT_BUDGET // n))

# === Pre-baked decision paragraphs ========================================
# Hoisted out of ``run_dual_approach_contest`` so the long English prose stays
# visible at module level, easy to diff and to lint without scrolling through
# control flow.

_DECISION_BOTH_FAILED: Final[str] = (
    "Both branches failed. The orchestration agent should explain the blocker briefly "
    "and choose the next concrete recovery step."
)
_DECISION_READY: Final[str] = (
    "The orchestration agent compares evidence, coverage, and risk, then continues with "
    "`run_specialist` for concrete execution unless the next decision is again volatile "
    "enough to justify another contest. The final user-facing conclusion comes from the "
    "orchestration agent."
)
_DECISION_SPECIALIST: Final[str] = (
    "The orchestration agent uses the worker brief above as scratch data, decides on the "
    "next concrete action, and either calls another tool or writes the final synthesis."
)
_DECISION_PARALLEL: Final[str] = (
    "The orchestration agent merges the worker briefs, then continues in the same user turn "
    "with more tool calls until the user goal is met, or writes one final synthesis."
)
_DECISION_PARALLEL_ALL_FAILED: Final[str] = (
    "All parallel workers failed. The orchestration agent should explain the blocker briefly "
    "and choose the next concrete recovery step."
)

_RATIONALE_SPECIALIST: Final[str] = "single-specialist execution selected by the orchestrator"

_DEFAULT_MAX_TURNS: Final[int] = 2


# === Data classes =========================================================


@dataclass(frozen=True, slots=True)
class WorkerSpec:
    """Inputs for one worker run.

    Each worker invocation (contest branch A/B or single specialist) becomes a
    ``WorkerSpec`` instance. Keeps :func:`_run_worker` declarative — no eight
    keyword arguments per call site, no positional vs keyword foot-guns.
    """

    label: str
    agent_type: str
    framing: str
    user_task: str
    rationale: str
    allowed_tool_name: str
    max_turns: int = _DEFAULT_MAX_TURNS
    # If set, caps this worker's brief before merging (multi-branch tools).
    max_output_chars: int | None = None
    group_id: str = ""
    workflow_prefix: str = "Contest"


@dataclass(frozen=True, slots=True)
class WorkerResult:
    """Typed outcome of one worker run.

    Replaces the previous "stringly-typed" convention where errors were carried
    in the worker output prefixed with ``[error] …``. Callers now check
    :attr:`failed` directly and the concrete error message is preserved
    separately from the worker brief that gets shown in the contest summary.
    """

    label: str
    agent_name: str
    allowed_tool_name: str
    output: str
    error: str | None = None

    @property
    def failed(self) -> bool:
        return self.error is not None

    @property
    def status(self) -> Literal["completed", "failed"]:
        return "failed" if self.failed else "completed"


# === Helpers ==============================================================


def _wrap_internal(body: str) -> str:
    """Frame ``body`` so the orchestrator reads it as private scratch data."""
    return f"{_INTERNAL_OPEN}\n{body}\n{_INTERNAL_CLOSE}"


def _truncate_worker_output(text: str, max_chars: int | None = None) -> str:
    """Cap a single worker brief; keep head + tail with a clear marker."""
    cap = _MAX_WORKER_OUTPUT_CHARS if max_chars is None else max(256, int(max_chars))
    if len(text) <= cap:
        return text
    half = cap // 2
    head = text[:half]
    tail = text[-half:]
    return (
        f"{head}\n\n"
        f"... [truncated by orchestrator: {len(text) - cap} chars] ...\n\n"
        f"{tail}"
    )


def _display_tool_name(tool_name: str) -> str:
    requested = (tool_name or "").strip()
    if requested.lower() in _NO_TOOL_NAMES:
        return "none"
    return requested


def _new_group_id(kind: str) -> str:
    """Stable, collision-resistant trace group id.

    The previous implementation used ``id(asyncio.current_task())`` which is a
    process-local memory address that can be reused once the originating task
    is collected — fine for a single run but unreliable as a tracing key when
    the orchestrator fires many contests back-to-back. ``uuid4`` removes that
    coupling entirely.
    """
    return f"{kind}:{uuid.uuid4().hex[:12]}"


def _resolve_worker_tool(agent: Any, allowed_tool_name: str) -> tuple[list[Any], str | None]:
    """Resolve the tool(s) a worker is allowed to use.

    Accepts either a single tool name or a comma-separated list (e.g.
    ``"fetch_url,generic_linux_command"``) so the orchestrator can grant a
    worker a small toolbox in a single delegation, avoiding the 1-tool /
    1-delegation amplification (see debug session ab1027).

    Returns the list of resolved tools — empty when the caller passed
    ``none`` / ``""`` for reasoning-only workers. Returns ``(tools, None)``
    on success or ``([], error_message)`` when ANY requested name is
    unknown for ``agent``.
    """
    requested = (allowed_tool_name or "").strip()
    if requested.lower() in _NO_TOOL_NAMES:
        return [], None

    available = list(agent.tools or [])
    by_name = {getattr(t, "name", ""): t for t in available}

    requested_names = [n.strip() for n in requested.split(",") if n.strip()]
    if not requested_names:
        return [], None

    resolved: list[Any] = []
    missing: list[str] = []
    for name in requested_names:
        tool = by_name.get(name)
        if tool is None:
            missing.append(name)
        elif tool not in resolved:
            resolved.append(tool)

    if missing:
        avail = ", ".join(sorted(by_name.keys()))
        joined_missing = ", ".join(f"`{m}`" for m in missing)
        return (
            [],
            f"Tool(s) {joined_missing} not available for `{agent.name}`. Available: {avail}",
        )
    return resolved, None


def _contest_worker(agent: Any, allowed_tool_name: str) -> tuple[Any | None, str | None]:
    """Return a worker clone that cannot hand off and exposes the chosen tool(s).

    ``allowed_tool_name`` may be a single tool or a comma-separated list, in
    which case the worker is given the full small toolbox at once.
    """
    tools, error = _resolve_worker_tool(agent, allowed_tool_name)
    if error:
        return None, error

    base_settings = agent.model_settings or ModelSettings()
    # Allow parallel tool calls only when the worker actually has >1 tool;
    # otherwise keep the original sequential behaviour.
    parallel = len(tools) > 1
    model_settings = base_settings.resolve(ModelSettings(parallel_tool_calls=parallel))
    return (
        agent.clone(
            tools=tools,
            handoffs=[],
            model_settings=model_settings,
        ),
        None,
    )


def _resolve_agent(agent_type: str, label: str) -> tuple[Any | None, str | None]:
    """Look up a specialist agent factory; return ``(agent, error_message)``.

    On failure (typo, removed agent, capitalisation) the error string includes
    the list of available factory keys so the orchestration agent can self-
    correct without an extra round-trip — mirrors how :func:`_resolve_worker_tool`
    already advertises available tool names.
    """
    from cai.agents import get_agent_by_name

    key = agent_type.strip()
    try:
        return get_agent_by_name(key, agent_id=f"O{label}"), None
    except ValueError as exc:
        try:
            from cai.agents import get_available_agents

            available = ", ".join(sorted(get_available_agents().keys()))
        except Exception:  # pragma: no cover — defensive: discovery rarely fails
            available = ""
        suggestion = f" Available: {available}." if available else ""
        return None, f"Invalid agent_type `{key}`: {exc}.{suggestion}"


def _build_worker_input(spec: WorkerSpec) -> str:
    """Compose the user prompt fed to one worker.

    Kept as a pure function so the test suite can assert against deterministic
    headings (``## Approach framing (A)``, ``## Allowed worker tool``, …)
    without booting the whole orchestration pipeline.
    """
    return (
        f"{_worker_constraints_prefix(spec.max_turns)}"
        f"## Approach framing ({spec.label})\n{spec.framing}\n\n"
        f"## Shared user task\n{spec.user_task}\n\n"
        f"## Allowed worker tool\n{spec.allowed_tool_name or 'none'}\n\n"
        f"## Contest rationale (from orchestrator)\n{spec.rationale}\n"
    )


# === Worker runner ========================================================


async def _run_worker(spec: WorkerSpec) -> WorkerResult:
    """Execute one worker according to ``spec`` and return a typed ``WorkerResult``."""
    base_agent, agent_error = _resolve_agent(spec.agent_type, spec.label)
    if agent_error or base_agent is None:
        msg = agent_error or "Unknown agent resolution error"
        return WorkerResult(
            label=spec.label,
            agent_name=spec.agent_type,
            allowed_tool_name=spec.allowed_tool_name,
            output=msg,
            error=msg,
        )

    agent, setup_error = _contest_worker(base_agent, spec.allowed_tool_name)
    display_name = getattr(agent or base_agent, "name", None) or spec.agent_type
    if setup_error:
        return WorkerResult(
            label=spec.label,
            agent_name=display_name,
            allowed_tool_name=spec.allowed_tool_name,
            output=setup_error,
            error=setup_error,
        )

    user_input = _build_worker_input(spec)
    trace_meta: dict[str, str] = {"contest_group": spec.group_id}
    if spec.workflow_prefix == "Parallel":
        trace_meta["parallel_branch"] = str(spec.label)
    else:
        trace_meta["contest_branch"] = f"approach_{spec.label.lower()}"
    rc = RunConfig(
        workflow_name=f"{spec.workflow_prefix} branch {spec.label}",
        group_id=spec.group_id,
        trace_metadata=trace_meta,
    )

    try:
        # ``silence_worker_display`` suppresses the worker's user-facing markdown
        # panels and Rich streaming panels (the "● Red Team Agent ─ <conclusion>"
        # boxes); only the orchestration agent's final synthesis is meant to
        # reach the user. Live-block tool rows still render so the user sees
        # progress for the worker's individual tool calls.
        with silence_worker_display():
            result = await Runner.run(
                agent,
                user_input,
                max_turns=spec.max_turns,
                run_config=rc,
            )
    except Exception as exc:  # pylint: disable=broad-except
        msg = f"{type(exc).__name__}: {exc}"
        return WorkerResult(
            label=spec.label,
            agent_name=display_name,
            allowed_tool_name=spec.allowed_tool_name,
            output=msg,
            error=msg,
        )

    out = ItemHelpers.text_message_outputs(result.new_items)
    if not out.strip():
        out = "(no textual output captured)"
    out_cap = spec.max_output_chars
    return WorkerResult(
        label=spec.label,
        agent_name=display_name,
        allowed_tool_name=spec.allowed_tool_name,
        output=_truncate_worker_output(out, out_cap),
    )


# === Brief composition ====================================================


def _format_branch_section(result: WorkerResult) -> list[str]:
    return [
        f"### Approach {result.label}",
        f"- Agent: `{result.agent_name}`",
        f"- Tool: `{_display_tool_name(result.allowed_tool_name)}`",
        f"- Status: `{result.status}`",
        "",
        "#### Worker brief",
        result.output,
        "",
    ]


def _compose_contest_brief(
    *,
    title: str,
    overall_status: str,
    rationale: str,
    results: tuple[WorkerResult, ...],
    decision_text: str,
    extra_header_lines: tuple[str, ...] = (),
) -> str:
    """Render the canonical brief shared by both contest and single-specialist tools.

    Both tools used to render a different markdown shape, which forced the LLM
    (and any downstream parser) to learn two formats. With a single skeleton
    here, ``run_specialist`` and ``run_dual_approach_contest`` differ only on
    the title, the number of branches, and the closing decision paragraph.
    """
    lines: list[str] = [
        f"## {title}",
        "",
        f"- Overall status: `{overall_status}`",
        f"- Rationale: {rationale}",
    ]
    lines.extend(extra_header_lines)
    lines.append("")
    for result in results:
        lines.extend(_format_branch_section(result))
    lines.extend(["### Next Decision", decision_text])
    return "\n".join(lines)


# === Public tools =========================================================


@function_tool
async def run_dual_approach_contest(
    agent_type_for_approach_a: str,
    agent_type_for_approach_b: str,
    allowed_tool_for_approach_a: str,
    allowed_tool_for_approach_b: str,
    approach_a_framing: str,
    approach_b_framing: str,
    shared_user_task: str,
    contest_rationale: str,
) -> str:
    """Run two parallel exploratory approaches on the same user task (max 2 agents; worker turn budget from ``CAI_ORCHESTRATION_WORKER_MAX_TURNS``).

    Use only when comparing orthogonal methodologies, competing hypotheses, volatile evidence,
    or a high-risk fork before committing CAI to a single path. Tactical follow-up actions should
    normally use ``run_specialist`` instead.

    ``agent_type_*`` must be factory keys (e.g. ``redteam_agent``, ``blueteam_agent``).
    Workers run with no handoffs and at most one selected tool name (``none`` = reasoning only).

    After this tool returns, you (Orchestration Agent) remain in control: compare outputs, pick a
    winner, plan the **next** step, and either call this tool again for a new genuinely volatile
    decision, call ``run_specialist`` for concrete follow-up work, or continue reasoning until the
    user's goal is met.

    Args:
        agent_type_for_approach_a: Factory key for worker A.
        agent_type_for_approach_b: Factory key for worker B (may equal A).
        allowed_tool_for_approach_a: Exact tool name A may use, or ``none`` for reasoning-only.
        allowed_tool_for_approach_b: Exact tool name B may use, or ``none`` for reasoning-only.
        approach_a_framing: How A should tackle the task (tools/strategy are directed here).
        approach_b_framing: How B should tackle it (**orthogonal** to A when possible).
        shared_user_task: The concrete user request both workers must address.
        contest_rationale: Short justification for running a contest now.
    """
    group_id = _new_group_id("contest")
    per_cap = _per_worker_output_cap(2)
    specs = (
        WorkerSpec(
            label="A",
            agent_type=agent_type_for_approach_a,
            framing=approach_a_framing,
            user_task=shared_user_task,
            rationale=contest_rationale,
            allowed_tool_name=allowed_tool_for_approach_a,
            group_id=group_id,
            workflow_prefix="Contest",
            max_turns=_configured_worker_max_turns(),
            max_output_chars=per_cap,
        ),
        WorkerSpec(
            label="B",
            agent_type=agent_type_for_approach_b,
            framing=approach_b_framing,
            user_task=shared_user_task,
            rationale=contest_rationale,
            allowed_tool_name=allowed_tool_for_approach_b,
            group_id=group_id,
            workflow_prefix="Contest",
            max_turns=_configured_worker_max_turns(),
            max_output_chars=per_cap,
        ),
    )
    results: tuple[WorkerResult, ...] = tuple(
        await asyncio.gather(*(_run_worker(spec) for spec in specs))
    )

    if all(r.failed for r in results):
        return _wrap_internal(
            _compose_contest_brief(
                title="Dual-Approach Contest",
                overall_status="both branches failed",
                rationale=contest_rationale,
                results=results,
                decision_text=_DECISION_BOTH_FAILED,
            )
        )
    return _wrap_internal(
        _compose_contest_brief(
            title="Dual-Approach Contest",
            overall_status="ready for orchestration decision",
            rationale=contest_rationale,
            extra_header_lines=(
                "- Structure: worker briefs are shown for transparency; final conclusion follows "
                "after orchestration.",
            ),
            results=results,
            decision_text=_DECISION_READY,
        )
    )


@function_tool
async def run_specialist(
    agent_type: str,
    allowed_tool_name: str,
    task: str,
    framing: str,
) -> str:
    """Run one specialist while keeping the orchestration agent in control.

    Use this for the winning path after a contest, for **narrow follow-up** drill-down, or when
    only one lane is appropriate. Prefer ``run_parallel_specialists`` for wave-1 parallel broad
    recon across orthogonal fronts. The worker cannot hand off and exposes at most one tool name.

    Args:
        agent_type: Factory key for the specialist (e.g. ``redteam_agent``).
        allowed_tool_name: Tool name the worker may use, OR a comma-separated list
            of names (e.g. ``"fetch_url,generic_linux_command"``) to grant a small
            toolbox in one delegation and avoid 1-tool/1-delegation fan-out, OR
            ``none`` for reasoning-only.
        task: Short concrete work request (avoid pasting the entire user brief verbatim).
        framing: Strategy and constraints; include **broad recon** vs **narrow follow-up** so the
            worker applies breadth-first discipline or skips it when you need exact execution.
    """
    spec = WorkerSpec(
        label="S",
        agent_type=agent_type,
        framing=framing,
        user_task=task,
        rationale=_RATIONALE_SPECIALIST,
        allowed_tool_name=allowed_tool_name,
        group_id=_new_group_id("specialist"),
        workflow_prefix="Specialist",
        max_turns=_configured_worker_max_turns(),
    )
    result = await _run_worker(spec)
    overall_status = "failed" if result.failed else "completed"
    return _wrap_internal(
        _compose_contest_brief(
            title="Specialist Brief",
            overall_status=overall_status,
            rationale=_RATIONALE_SPECIALIST,
            results=(result,),
            decision_text=_DECISION_SPECIALIST,
        )
    )

@function_tool
async def run_parallel_specialists(workers_json: str, parallel_rationale: str) -> str:
    """Run 2–4 specialists concurrently on independent sub-tasks while you keep control.

    Primary tool for **wave-1 MAS**: parallel **broad** scouts on orthogonal fronts (short ``task``
    strings, recon-oriented ``framing``). Also use when the user names multiple workstreams.

    Prefer ``run_dual_approach_contest`` when comparing two hypotheses for the **same** fork;
    prefer ``run_specialist`` for a single **narrow** follow-up after you have signal.

    ``workers_json`` must be a JSON array of 2–4 objects. Each object requires keys:
    ``agent_type``, ``allowed_tool_name``, ``task``, ``framing`` (same contract as
    ``run_specialist``).

    Args:
        workers_json: JSON array of worker specs (2–4 items).
        parallel_rationale: Why parallel execution is appropriate now (e.g. wave-1 landscape map).
    """
    raw = (workers_json or "").strip()
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as exc:
        return f"Invalid workers_json (not valid JSON): {exc}"

    if not isinstance(data, list):
        return "workers_json must be a JSON array."

    if len(data) < 2:
        return "Provide at least 2 workers for parallel execution, or use run_specialist for one."

    if len(data) > 4:
        return "At most 4 parallel workers are allowed; split extra work across subsequent tool calls."

    required = ("agent_type", "allowed_tool_name", "task", "framing")
    for i, w in enumerate(data, start=1):
        if not isinstance(w, dict):
            return f"Worker {i} must be a JSON object."
        for key in required:
            if key not in w:
                return f"Worker {i} missing required key `{key}`."

    max_t = _configured_worker_max_turns()
    per_cap = _per_worker_output_cap(len(data))
    group_id = _new_group_id("parallel")
    specs: list[WorkerSpec] = []
    for i, w in enumerate(data, start=1):
        specs.append(
            WorkerSpec(
                label=f"P{i}",
                agent_type=str(w["agent_type"]),
                framing=str(w["framing"]),
                user_task=str(w["task"]),
                rationale=parallel_rationale,
                allowed_tool_name=str(w["allowed_tool_name"]),
                max_turns=max_t,
                max_output_chars=per_cap,
                group_id=group_id,
                workflow_prefix="Parallel",
            )
        )

    results: tuple[WorkerResult, ...] = tuple(
        await asyncio.gather(*(_run_worker(spec) for spec in specs))
    )

    if all(r.failed for r in results):
        return _wrap_internal(
            _compose_contest_brief(
                title="Parallel Specialists",
                overall_status="all branches failed",
                rationale=parallel_rationale,
                results=results,
                decision_text=_DECISION_PARALLEL_ALL_FAILED,
            )
        )

    overall = "partial completion" if any(r.failed for r in results) else "completed"
    return _wrap_internal(
        _compose_contest_brief(
            title="Parallel Specialists",
            overall_status=overall,
            rationale=parallel_rationale,
            results=results,
            decision_text=_DECISION_PARALLEL,
        )
    )

