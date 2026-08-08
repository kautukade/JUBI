"""
Metadata for the environment reference (tables under ``/help``; detail via ``/help var NAME``): when each
variable takes effect, value constraints, and extras.

Descriptions in ``env_catalog.ENV_VARS`` are the short summaries; this module adds
English guidance on runtime vs restart and allowed shapes.
"""

from __future__ import annotations

from typing import Any, Dict, List, Optional, Tuple

# Map ENV_VARS numeric key ranges to section titles (must match env_catalog.py groupings).
_CATEGORY_BY_NUM: List[Tuple[int, Optional[int], str]] = [
    (1, 8, "CTF (capture-the-flag)"),
    (9, 16, "Core agent & model"),
    (17, 21, "Streaming & debug output"),
    (22, 27, "Parallelization & queue"),
    (28, 33, "Execution limits & timeouts"),
    (34, 41, "Memory & context"),
    (42, 45, "Workspace & containers"),
    (46, 50, "Support & meta agent"),
    (51, 58, "CTR / G-CTR"),
    (59, 62, "Tracing & telemetry"),
    (63, 64, "Security & planning"),
    (65, 69, "Pricing & cost"),
    (70, 71, "Reporting & continuation"),
    (72, 80, "HTTP API server"),
    (81, 85, "Authentication service"),
    (86, 89, "MCP (Model Context Protocol)"),
    (90, 94, "TUI"),
    (95, 106, "Advanced / misc"),
]

# English blurbs and optional dependency rules for environment reference category panels.
# dependency_id is resolved in repl.commands.environment_reference (pentestperf = cai.caibench importable).
CATEGORY_DISPLAY: Dict[str, Dict[str, Any]] = {
    "Core agent & model": {
        "overview": "[dim]Defaults for which [bold]model[/bold] and [bold]agent type[/bold] run ([bold]orchestration_agent[/bold] when unset, or e.g. selection_agent, redteam_agent, one_tool_agent), optional [bold]CAI_ORCHESTRATION_*[/bold] tuning when the entry agent spawns specialist workers, sampling (temperature / top_p), debug verbosity, and output shaping. These affect most interactive and headless sessions.[/dim]",
    },
    "CTF (capture-the-flag)": {
        "overview": "[dim]Docker-backed benchmark challenges (pentestperf-style images): challenge selection, container networking, and whether tools execute inside the target.[/dim]",
        "dependency_id": "pentestperf",
        "missing_dependency_note": (
            "[#9aa0a6]The [bold]caibench[/bold] package ([bold]cai.caibench[/bold]) is not available in this environment. "
            "The published wheel often excludes [bold]src/cai/caibench/[/bold] (see [bold]pyproject.toml[/bold] hatch excludes); "
            "install from a [bold]full source[/bold] tree with [bold]pip install -e .[/bold], or use a build that ships caibench. "
            "You also need [bold]Docker[/bold] for challenge containers. "
            "Until then, [bold]CTF_*[/bold] variables have no effect here—the table is omitted. "
            "They remain documented for CI/benchmark installs.[/]"
        ),
        "present_dependency_note": (
            "[#9aa0a6][bold]caibench[/bold] is loaded. If you do not run CTF or benchmark flows, consider a minimal CAI install "
            "(wheel without caibench) so [bold]CTF_*[/bold] stay inert and the attack surface stays smaller.[/]"
        ),
        "omit_table_without_dependency": True,
    },
    "Streaming & debug output": {
        "overview": "[dim]Control LLM and tool streaming to the terminal, cache visibility, and low-level debug flags for tools and context.[/dim]",
    },
    "Parallelization & queue": {
        "overview": "[dim]Parallel agent workers, auto-run behaviour, and queued command files for batch or multi-terminal setups.[/dim]",
    },
    "Execution limits & timeouts": {
        "overview": "[dim]Turn and interaction caps, price ceilings, and timeouts for tools, idle sessions, and code execution.[/dim]",
    },
    "Memory & context": {
        "overview": "[dim]Optional memory backends (episodic/semantic), context truncation, and how much tool output is shown.[/dim]",
    },
    "Workspace & containers": {
        "overview": "[dim]Named workspace, directories on disk, and which Docker container is considered active for tools.[/dim]",
    },
    "Support & meta agent": {
        "overview": "[dim]Background support and meta agents: models, intervals, and auto-close timing.[/dim]",
    },
    "CTR / G-CTR": {
        "overview": "[dim]Control-the-Rope style digest pipelines: modes, models, output paths, and G-CTR iteration counts.[/dim]",
    },
    "Tracing & telemetry": {
        "overview": "[dim]OpenTelemetry tracing, product telemetry, and opt-outs for session recording or usage tracking.[/dim]",
    },
    "Security & planning": {
        "overview": "[dim]Guardrails and planning-mode toggles for safer or more structured agent behaviour.[/dim]",
    },
    "Pricing & cost": {
        "overview": "[dim]Cost display, async pricing fetch, debug of pricing math, and paths to pricing data files.[/dim]",
    },
    "Reporting & continuation": {
        "overview": "[dim]Report mode presets and fallback models when continuation needs a different endpoint.[/dim]",
    },
    "HTTP API server": {
        "overview": "[dim]Optional HTTP API: bind address, CORS, logging, workers, reload, and auth header naming.[/dim]",
    },
    "Authentication service": {
        "overview": "[dim]Device / OAuth-style auth helper: base URL, public host and ports, session TTL.[/dim]",
    },
    "MCP (Model Context Protocol)": {
        "overview": "[dim]MCP server tokens and SSE timeouts for Model Context Protocol integrations.[/dim]",
    },
    "TUI": {
        "overview": "[dim]Textual UI: enablement, startup YAML, shared prompt, scrollback and render throttling.[/dim]",
    },
    "Advanced / misc": {
        "overview": "[dim]Version string, themes, network checks, auto-compaction thresholds, patterns, and other advanced knobs.[/dim]",
    },
    "Provider keys & runtime": {
        "overview": "[dim]Provider API keys and bases, Ollama routing, parallel merge digests, sensitive-command guards, and other runtime toggles merged from the former “Additional” reference.[/dim]",
    },
    "Per-agent model overrides": {
        "overview": "[dim]Per-agent model overrides generated from registered agents (and per-instance slots when running more than one parallel worker).[/dim]",
    },
}


def category_title_for_number(num: int) -> str:
    try:
        from cai.repl.commands import env_catalog as _ec

        r = _ec.EXTRA_CATALOG_RANGE
        if r and r[0] <= num <= r[1]:
            return "Provider keys & runtime"
    except Exception:  # pylint: disable=broad-except
        pass
    for lo, hi, title in _CATEGORY_BY_NUM:
        if hi is not None and lo <= num <= hi:
            return title
        if hi is None and num >= lo:
            return title
    return "Per-agent model overrides"


# Subsystems that typically read these only once per process or TUI session.
_RESTART_RECOMMENDED = frozenset(
    {
        "CAI_TUI_MODE",
        "CAI_TUI",
        "CAI_TUI_STARTUP_YAML",
        "CAI_TUI_SHARED_PROMPT",
        "CAI_TUI_MAX_LINES",
        "CAI_TUI_MAX_RERENDERS_PER_SEC",
        "CAI_THEME",
        "CAI_API_HOST",
        "CAI_API_PORT",
        "CAI_API_WORKERS",
        "CAI_API_RELOAD",
        "CAI_API_CORS",
        "CAI_TRACING",
        "CAI_TELEMETRY",
        "CAI_VERSION",
        "CAI_AUTO_UPDATE",
        "CAI_COMPACT_REPL",
    }
)

# Often read from os.environ on each tool / stream invocation.
_RUNTIME_FRIENDLY = frozenset(
    {
        "CAI_STREAM",
        "CAI_TOOL_STREAM",
        "CAI_SHOW_CACHE",
        "CAI_DEBUG_TOOLS_VIZ",
        "CAI_DEBUG_STREAMING",
        "CAI_DEBUG_PRICING",
        "CAI_VERBOSE_LLM_RETRY",
        "CAI_HTTP_ERROR_BODY",
        "CAI_TOOL_TIMEOUT",
        "CAI_DISPLAY_MAX_OUTPUT",
        "CAI_CTX_TRUNC",
        "CAI_GUARDRAILS",
        "CAI_PLAN",
        "CAI_SENSITIVE_GUARD",
        "CAI_AVOID_SUDO",
        "CAI_YOLO",
        "CAI_UNRESTRICTED",
        "CAI_UNRESTRICTED_LOG",
        "CAI_TOOL_LIVE_SHOW_PRICING",
        "CAI_DISABLE_TOOL_WAIT_HINTS",
        "CAI_TOOL_OUTPUT_MARKDOWN",
        "CAI_PATTERN_DESCRIPTION",
        "CAI_PARALLEL_EXEC_MODE",
        "CAI_PARALLEL_EXTERNAL_TIMEOUT",
        "CAI_TASK_RESET_PENDING",
        "CAI_SKIP_UPDATE_CHECK",
        "CAI_ACTIVE_COMMAND_TERMINAL",
        "CAI_COST_DISPLAYED",
        "CAI_MERGE_SUMMARIZE_PER_WORKER",
        "CAI_MERGE_SUMMARIZE_MIN_MESSAGES",
        "SSH_USER",
        "SSH_HOST",
    }
)


def effective_label(var_name: str) -> str:
    """Single-word timing label; details are in INTRO_MARKUP *When changes apply*."""
    if var_name in _RESTART_RECOMMENDED:
        return "Restart"
    if var_name in _RUNTIME_FRIENDLY:
        return "Runtime"
    if var_name.startswith("CAI_") and var_name.endswith("_MODEL"):
        return "Mixed"
    return "Mixed"


def is_restart_required(var_name: str) -> bool:
    """True when ``var_name`` is read once at startup; runtime mutation is a no-op.

    Consumers (e.g. ``/env set`` / ``/env default``) should refuse to mutate
    these variables and instruct the user to export them before launching CAI.
    """
    return var_name in _RESTART_RECOMMENDED


def is_secret(var_name: str) -> bool:
    """True when the variable holds a credential (constraint label ``secret``).

    ``/env default`` must refuse to mutate these — popping them from ``os.environ``
    silently breaks authentication (ALIAS_API_KEY, OPENAI_API_KEY, OLLAMA_API_KEY,
    CAI_MCP_TOKEN, CAI_MCP_AUTH_TOKEN) and the user has no in-session way to
    restore them.
    """
    return _CONSTRAINT_LABEL_BY_VAR.get(var_name) == "secret"


# Per-variable type/range for environment reference tables (intro explains bool, string, int, float, etc.).
_CONSTRAINT_LABEL_BY_VAR: Dict[str, str] = {
    "CTF_NAME": "string",
    "CTF_CHALLENGE": "string",
    "CTF_SUBNET": "string",
    "CTF_IP": "string",
    "CTF_INSIDE": "bool",
    "CTF_MODEL": "string",
    "CTF_CONTAINER_NAME": "string",
    "CTF_INSTANCE_ID": "string",
    "CAI_MODEL": "string",
    "CAI_AGENT_TYPE": "string",
    "CAI_TEMPERATURE": "float 0.0–2.0",
    "CAI_TOP_P": "float 0.0–1.0",
    "CAI_DEBUG": "int 0–2",
    "CAI_BRIEF": "bool",
    "CAI_STATE": "bool",
    "CAI_DEFAULT_AGENT": "string",
    "CAI_STREAM": "bool",
    "CAI_TOOL_STREAM": "bool",
    "CAI_SHOW_CACHE": "bool",
    "CAI_DEBUG_TOOLS_VIZ": "bool",
    "CAI_DEBUG_STREAMING": "bool",
    "CAI_COMPACT_REPL": "bool",
    "CAI_PARALLEL": "int 1–20",
    "CAI_PARALLEL_AGENTS": "string",
    "CAI_AUTO_RUN_PARALLEL": "bool",
    "CAI_AUTO_RUN_QUEUE": "bool",
    "CAI_QUEUE_FILE": "string",
    "CAI_VERBOSE_LLM_RETRY": "bool",
    "CAI_MAX_TURNS": "int ≥1",
    "CAI_ORCHESTRATION_WORKER_MAX_TURNS": "int 1–32",
    "CAI_ORCHESTRATION_MAS_HINT": "bool",
    "CAI_MAX_INTERACTIONS": "int ≥1",
    "CAI_PRICE_LIMIT": "float ≥0",
    "CAI_TOOL_TIMEOUT": "int (s)",
    "CAI_IDLE_TIMEOUT": "int (s)",
    "CAI_CODE_TIMEOUT": "int (s)",
    "CAI_COMPACTED_MEMORY": "bool",
    "CAI_ENV_CONTEXT": "bool",
    "CAI_CTX_TRUNC": "bool",
    "CAI_DISPLAY_MAX_OUTPUT": "bool",
    "CAI_WORKSPACE": "string",
    "CAI_WORKSPACE_DIR": "string",
    "CAI_ACTIVE_CONTAINER": "string",
    "CAI_ACTIVE_CONTAINER_DEFAULT": "string",
    "CAI_SUPPORT_MODEL": "string",
    "CAI_SUPPORT_INTERVAL": "int",
    "CAI_META_AGENT": "bool",
    "CAI_META_MODEL": "string",
    "CAI_META_AUTOCLOSE_GRACE": "float (s)",
    "CAI_CTR_DIGEST_MODE": "string",
    "CAI_CTR_DIGEST_MODEL": "string",
    "CAI_CTR_OUTPUT_DIR": "string",
    "CAI_CTR_DEFAULT_OUTPUT_DIR": "string",
    "CAI_CTR_DEFAULT_RUN": "string",
    "CAI_CTR_IS_CTF": "bool",
    "CAI_CTR_DISTANCE_HEURISTIC": "string",
    "CAI_GCTR_NITERATIONS": "int",
    "CAI_TRACING": "bool",
    "CAI_TELEMETRY": "bool",
    "CAI_DISABLE_SESSION_RECORDING": "bool",
    "CAI_DISABLE_USAGE_TRACKING": "bool",
    "CAI_GUARDRAILS": "bool",
    "CAI_PLAN": "bool",
    "CAI_COST_DISPLAYED": "bool",
    "CAI_ENABLE_PRICING_FETCH": "bool",
    "CAI_DEBUG_PRICING": "bool",
    "CAI_PRICING_FILE": "string",
    "CAI_PRICINGS_DIR": "string",
    "CAI_REPORT": "string",
    "CAI_CONTINUATION_FALLBACK_MODEL": "string",
    "CAI_API_HOST": "string",
    "CAI_API_PORT": "int",
    "CAI_API_CORS": "string",
    "CAI_API_KEY_HEADER": "string",
    "CAI_API_LOG_AUTH": "bool",
    "CAI_API_LOG_REQUESTS": "bool",
    "CAI_API_LOG_LEVEL": "string",
    "CAI_API_RELOAD": "bool",
    "CAI_API_WORKERS": "int",
    "CAI_AUTH_BASE_URL": "string",
    "CAI_AUTH_DEVICE_PORT": "int",
    "CAI_AUTH_PUBLIC_HOST": "string",
    "CAI_AUTH_PUBLIC_PORT": "int",
    "CAI_AUTH_SESSION_TTL_SECONDS": "int (s)",
    "CAI_MCP_TOKEN": "secret",
    "CAI_MCP_AUTH_TOKEN": "secret",
    "CAI_MCP_SSE_TIMEOUT": "int (s)",
    "CAI_MCP_SSE_READ_TIMEOUT": "int (s)",
    "CAI_TUI_MODE": "bool",
    "CAI_TUI_STARTUP_YAML": "string",
    "CAI_TUI_SHARED_PROMPT": "string",
    "CAI_TUI_MAX_LINES": "int",
    "CAI_TUI_MAX_RERENDERS_PER_SEC": "int",
    "CAI_VERSION": "string",
    "CAI_THEME": "string",
    "CAI_SKIP_NETWORK_CHECK": "bool",
    "CAI_AUTO_COMPACT": "bool",
    "CAI_AUTO_COMPACT_THRESHOLD": "float 0.0–0.8",
    "CAI_WARN_UNATTRIBUTED": "bool",
    "CAI_UNATTRIBUTED_LOG": "string",
    "CAI_PATTERN_DESCRIPTION": "string",
    "CAI_MODEL_LIST": "string",
    "CAI_CONTEXT_USAGE": "string",
    "CAI_SESSION_INPUT_WAIT": "float (s)",
    "CAI_BROADCAST_MODE": "string",
    "CAI_MERGE_SUMMARIZE_PER_WORKER": "int 0–1",
    "CAI_MERGE_SUMMARIZE_MIN_MESSAGES": "int ≥1",
    "CAI_YOLO": "bool",
    "CAI_SENSITIVE_GUARD": "bool",
    "CAI_UNRESTRICTED": "bool",
    "CAI_UNRESTRICTED_LOG": "bool",
    "CAI_TOOL_LIVE_SHOW_PRICING": "bool",
    "CAI_DISABLE_TOOL_WAIT_HINTS": "bool",
    "CAI_TOOL_OUTPUT_MARKDOWN": "bool",
    "CAI_PARALLEL_EXEC_MODE": "string",
    "CAI_PARALLEL_EXTERNAL_TIMEOUT": "float (s)",
    "CAI_TASK_RESET_PENDING": "int 0–1",
    "CAI_SKIP_UPDATE_CHECK": "bool",
    "CAI_AUTO_UPDATE": "bool",
    "CAI_ACTIVE_COMMAND_TERMINAL": "string",
    "CAI_VERBOSE_HTTP_RETRY": "bool",
    "CAI_HTTP_ERROR_BODY": "bool",
    "ALIAS_API_KEY": "secret",
    "ALIAS_API_URL": "string (URL)",
    "CSI_CUSTOM_ENDPOINT": "string (URL)",
    "OPENAI_API_KEY": "secret",
    "OPENAI_API_BASE": "string",
    "OLLAMA": "string",
    "OLLAMA_API_BASE": "string",
    "OLLAMA_API_KEY": "secret",
}


def constraints_line(var_name: str, description: str) -> str:
    """Compact type (and range if needed) for env reference tables; see INTRO_MARKUP for full semantics."""
    _ = description  # reserved if we add heuristics for unknown vars later
    return _CONSTRAINT_LABEL_BY_VAR.get(var_name, "string")


# Merged into ``env_catalog.ENV_VARS`` at import time. ``constraints``/``effective`` come
# from ``_CONSTRAINT_LABEL_BY_VAR`` and ``_RESTART_RECOMMENDED``/``_RUNTIME_FRIENDLY`` above
# (single source of truth; use ``constraints_line`` / ``effective_label`` to read them).
EXTRA_ENV_VARS: List[Dict[str, Any]] = [
    {
        "name": "CAI_ORCHESTRATION_WORKER_MAX_TURNS",
        "default": "6",
        "description": (
            "Max Runner turns per specialist worker spawned by orchestration_agent tools "
            "(run_specialist, run_dual_approach_contest, run_parallel_specialists). Clamped 1–32."
        ),
    },
    {
        "name": "CAI_ORCHESTRATION_MAS_HINT",
        "default": "true",
        "description": (
            "When true, orchestration_agent may receive one synthetic user-line nudge per Runner "
            "run if the user message looks multi-front but only run_specialist was used—suggesting "
            "parallel or contest tools."
        ),
    },
    {
        "name": "CAI_MERGE_SUMMARIZE_PER_WORKER",
        "default": "1",
        "description": "When 1, enable per-worker merge digests in parallel multi-agent flows.",
    },
    {
        "name": "CAI_MERGE_SUMMARIZE_MIN_MESSAGES",
        "default": "20",
        "description": "Minimum messages in a worker before per-worker digest runs (when merge per worker is on).",
    },
    {
        "name": "CAI_YOLO",
        "default": "unset (off)",
        "description": "When true, skips interactive sensitive-command approval (equivalent to CLI --yolo). Unsafe on untrusted prompts.",
    },
    {
        "name": "CAI_AVOID_SUDO",
        "default": "unset (off)",
        "description": "When true, never run sudo/su/pkexec/doas via generic_linux_command (hard block even with YOLO) and add a system-prompt policy to prefer non-privileged alternatives.",
    },
    {
        "name": "CAI_SENSITIVE_GUARD",
        "default": "true",
        "description": "Master switch for sensitive-command detection in CLI headless mode. Set to false to disable prompts (still prefer CAI_YOLO only when you understand the risk). Prompts are interactive and need a real TTY after streaming output; use YOLO or automation only when you accept non-interactive runs.",
    },
    {
        "name": "CAI_UNRESTRICTED",
        "default": "false",
        "description": "Relaxes some logging / content filters in model paths (developer-oriented).",
    },
    {
        "name": "CAI_UNRESTRICTED_LOG",
        "default": "unset",
        "description": "Additional logging when CAI_UNRESTRICTED is active.",
    },
    {
        "name": "CAI_TOOL_LIVE_SHOW_PRICING",
        "default": "unset (off)",
        "description": "If true, tool Live panels stack the pricing footer and wait hint again (legacy layout).",
    },
    {
        "name": "CAI_DISABLE_TOOL_WAIT_HINTS",
        "default": "unset (off)",
        "description": "Disables tool-batch wait hints (Result-rail messages and footer updates).",
    },
    {
        "name": "CAI_TOOL_OUTPUT_MARKDOWN",
        "default": "true",
        "description": "Render markdown-like tool stdout under Result/captured when heuristics match.",
    },
    {
        "name": "CAI_PARALLEL_EXEC_MODE",
        "default": "external",
        "description": "How parallel agent workers are launched (e.g. external terminals vs embedded).",
    },
    {
        "name": "CAI_PARALLEL_EXTERNAL_TIMEOUT",
        "default": "900",
        "description": "Seconds to wait for external parallel workers.",
    },
    {
        "name": "CAI_TASK_RESET_PENDING",
        "default": "unset",
        "description": "Internal flag used by the headless loop for task-reset signalling.",
    },
    {
        "name": "CAI_SKIP_UPDATE_CHECK",
        "default": "unset",
        "description": "Skip startup update / connectivity checks.",
    },
    {
        "name": "CAI_AUTO_UPDATE",
        "default": "unset (off)",
        "description": "When true and this variable is present in the environment, install cai-framework updates at startup (or with cai --update) without prompting. Unset = always ask.",
    },
    {
        "name": "CAI_ACTIVE_COMMAND_TERMINAL",
        "default": "unset",
        "description": "Tracks which command terminal is active in multi-terminal flows.",
    },
    {
        "name": "CAI_VERBOSE_HTTP_RETRY",
        "default": "unset",
        "description": "Alias accepted by HTTP client; same idea as CAI_VERBOSE_LLM_RETRY.",
    },
    {
        "name": "CAI_HTTP_ERROR_BODY",
        "default": "unset",
        "description": "Include HTTP error bodies in verbose retry / debug output.",
    },
    {
        "name": "ALIAS_API_KEY",
        "default": "unset",
        "description": "Primary API key for Alias Robotics / CAI gateway (also check OPENAI_API_KEY fallbacks).",
    },
    {
        "name": "CSI_CUSTOM_ENDPOINT",
        "default": "unset",
        "description": (
            "When set (non-empty) and the model qualifies, highest-priority OpenAI-compatible base "
            "(typically injected by CSI with CAI backend). Expect ``/chat/completions`` compatibility."
        ),
    },
    {
        "name": "ALIAS_API_URL",
        "default": "unset",
        "description": (
            "When set (non-empty) and the model qualifies (``cai`` / ``alias`` / ``csi`` prefix rule), "
            "OpenAI-compatible chat uses this base after ``CSI_CUSTOM_ENDPOINT`` and before ``OPENAI_API_BASE``."
        ),
    },
    {
        "name": "OPENAI_API_KEY",
        "default": "unset",
        "description": "Fallback API key used by several providers and compatibility layers.",
    },
    {
        "name": "OPENAI_API_BASE",
        "default": "https://api.aliasrobotics.com:666/",
        "description": (
            "Base URL when ``CSI_CUSTOM_ENDPOINT`` / ``ALIAS_API_URL`` do not apply (model prefix) "
            "or are unset/empty."
        ),
    },
    {
        "name": "OLLAMA",
        "default": "unset",
        "description": "Set to enable or point at Ollama-style routing in chat-completions layer.",
    },
    {
        "name": "OLLAMA_API_BASE",
        "default": "https://ollama.com",
        "description": "Ollama API base when using Ollama-backed models.",
    },
    {
        "name": "OLLAMA_API_KEY",
        "default": "unset",
        "description": "API key for hosted Ollama if required.",
    },
]

INTRO_MARKUP = """Variables are normal [bold #00ff9d]process environment[/bold #00ff9d] values. CAI also loads a project [bold #00ff9d].env[/bold #00ff9d] file when present.

[bold]How to set them[/bold]
• [dim white]Before launch:[/dim white] [bold #00ff9d]export VAR=value[/bold #00ff9d] in your shell, or add a line to [bold #00ff9d].env[/bold #00ff9d], then start CAI.
• [dim white]During a session:[/dim white] [bold #00ff9d]/env set <#|NAME> <value...>[/bold #00ff9d], or Python [bold #00ff9d]os.environ["VAR"]="value"[/bold #00ff9d] from extensions.

[bold]When changes apply[/bold]
• [italic]Runtime[/italic] — code that calls [bold]os.getenv[/bold] on each use picks up new values immediately (streaming flags, many tool options, debug toggles).
• [italic]Restart / new session[/italic] — TUI mode, API worker count, some telemetry switches, or anything read only at process startup. In tables this appears as [bold]Restart[/bold].
• [italic]Mixed[/italic] — values are in [bold]os.environ[/bold], but parts of CAI cache a [bold]CAIConfig[/bold] snapshot or model client until you start a new inference turn, switch agents with [bold]/agent[/bold], or restart. When in doubt, restart CAI after changing core model or agent settings.

The [italic]When[/italic] column uses only [bold]Runtime[/bold], [bold]Restart[/bold], or [bold]Mixed[/bold], matching the three categories above.

[bold]Allowed value types[/bold]
• [italic]bool[/italic] — truthy/falsy forms such as [bold]true[/bold]/[bold]false[/bold], [bold]1[/bold]/[bold]0[/bold], [bold]yes[/bold]/[bold]no[/bold] where documented as boolean.
• [italic]string[/italic] — free text, paths, model names, mode labels, etc.; the [italic]Description[/italic] column explains each variable.
• [italic]int[/italic] / [italic]float[/italic] — numeric parsing; a suffix like [bold](s)[/bold] means seconds. Ranges in the table (e.g. [bold]0.0–2.0[/bold], [bold]1–20[/bold]) are the usual bounds CAI enforces or documents.
• [italic]secret[/italic] — same storage as string; never commit real credentials.
• [italic]bool|int[/italic] — boolean or a positive integer, depending on the variable (see description).

The [italic]Values[/italic] column lists one of these labels plus an optional range. Model providers may still reject out-of-range temperatures or token limits even if CAI accepts the string."""
