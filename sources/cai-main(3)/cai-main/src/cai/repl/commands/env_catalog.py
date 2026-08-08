"""REPL environment catalog: ``ENV_VARS`` and list/get/set/default handlers."""

from __future__ import annotations

import os
from typing import Dict, List, Optional, Tuple

from rich.box import SIMPLE_HEAD
from rich.console import Console
from rich.panel import Panel
from rich.table import Table
from rich.text import Text

from cai.repl.commands.env_catalog_validate import (
    resolve_catalog_spec,
    validate_catalog_value,
)
from cai.repl.commands.env_info_catalog import (
    constraints_line,
    effective_label,
    is_restart_required,
    is_secret,
)
from cai.repl.ui.banner import _CAI_GREEN, _GREY, _quick_guide_subpanel_title

console = Console()

# Same inner table chrome as ``environment_reference._category_vars_table`` (bare /help env tables).
HELP_REFERENCE_MATCH_TABLE_KWARGS: Dict[str, object] = {
    "show_header": True,
    "header_style": "bold white",
    "box": SIMPLE_HEAD,
    "show_edge": False,
    "show_lines": False,
    "pad_edge": False,
    "padding": (0, 1),
    "expand": True,
    "border_style": _CAI_GREEN,
}

# (min_num, max_num) for rows merged from ``EXTRA_ENV_VARS``; set in ``_merge_extra_catalog_entries``.
EXTRA_CATALOG_RANGE: Optional[Tuple[int, int]] = None

# Catalog indices are contiguous integers starting at 1. Static rows occupy 1..107; merged
# ``EXTRA_ENV_VARS`` and dynamic per-agent keys append at the end (still contiguous).
ENV_VARS: Dict[int, Dict[str, object]] = {
    1: {"name": "CTF_NAME", "description": "Name of the CTF challenge to run", "default": None},
    2: {"name": "CTF_CHALLENGE", "description": "Specific challenge name within the CTF", "default": None},
    3: {"name": "CTF_SUBNET", "description": "Network subnet for CTF container", "default": "192.168.3.0/24"},
    4: {"name": "CTF_IP", "description": "IP address for CTF container", "default": "192.168.3.100"},
    5: {"name": "CTF_INSIDE", "description": "Conquer CTF from within container", "default": "true"},
    6: {"name": "CTF_MODEL", "description": "Model override for CTF challenges", "default": None},
    7: {"name": "CTF_CONTAINER_NAME", "description": "Docker container name for CTF", "default": None},
    8: {"name": "CTF_INSTANCE_ID", "description": "Instance ID for CTF tracking", "default": ""},
    9: {"name": "CAI_MODEL", "description": "Model to use for agents", "default": "alias1"},
    10: {
        "name": "CAI_AGENT_TYPE",
        "description": "Registered agent key (e.g. selection_agent, orchestration_agent, redteam_agent)",
        "default": "selection_agent",
    },
    11: {
        "name": "CAI_TEMPERATURE",
        "description": "Model temperature (0.0-2.0); REPL /temperature also updates the active agent",
        "default": "0.7",
    },
    12: {"name": "CAI_TOP_P", "description": "Nucleus sampling top_p (0.0-1.0)", "default": "1.0"},
    13: {"name": "CAI_DEBUG", "description": "Debug level (0: tool only, 1: verbose, 2: CLI)", "default": "1"},
    14: {"name": "CAI_BRIEF", "description": "Enable brief output mode", "default": "false"},
    15: {"name": "CAI_STATE", "description": "Enable stateful mode", "default": "false"},
    16: {"name": "CAI_DEFAULT_AGENT", "description": "Default agent type", "default": "redteam_agent"},
    17: {"name": "CAI_STREAM", "description": "Enable LLM inference streaming", "default": "false"},
    18: {"name": "CAI_TOOL_STREAM", "description": "Enable tool output streaming", "default": "true"},
    19: {"name": "CAI_SHOW_CACHE", "description": "Show cache info and message history", "default": "false"},
    20: {"name": "CAI_DEBUG_TOOLS_VIZ", "description": "Debug tool visualization", "default": "false"},
    21: {"name": "CAI_DEBUG_STREAMING", "description": "Debug streaming output", "default": "false"},
    22: {"name": "CAI_PARALLEL", "description": "Number of parallel agents (1-20)", "default": "1"},
    23: {"name": "CAI_PARALLEL_AGENTS", "description": "Comma-separated agent names for parallel", "default": None},
    24: {"name": "CAI_AUTO_RUN_PARALLEL", "description": "Auto-run parallel agents on startup", "default": "false"},
    25: {"name": "CAI_AUTO_RUN_QUEUE", "description": "Auto-run queued commands", "default": "false"},
    26: {"name": "CAI_QUEUE_FILE", "description": "Path to command queue file", "default": None},
    27: {
        "name": "CAI_VERBOSE_LLM_RETRY",
        "description": "Print HTTP/LiteLLM retry and timeout messages to console",
        "default": "false",
    },
    28: {"name": "CAI_MAX_TURNS", "description": "Maximum turns for agent interactions", "default": "inf"},
    29: {"name": "CAI_MAX_INTERACTIONS", "description": "Maximum interactions in session", "default": "inf"},
    30: {"name": "CAI_PRICE_LIMIT", "description": "Price limit in dollars", "default": "1"},
    31: {"name": "CAI_TOOL_TIMEOUT", "description": "Tool execution timeout (seconds)", "default": None},
    32: {"name": "CAI_IDLE_TIMEOUT", "description": "Idle timeout before cleanup", "default": "100"},
    33: {"name": "CAI_CODE_TIMEOUT", "description": "Code execution timeout", "default": "30"},
    34: {
        "name": "CAI_COMPACTED_MEMORY",
        "description": "Inject /compact conversation summaries into agent system prompts (true/false)",
        "default": "false",
    },
    35: {"name": "CAI_ENV_CONTEXT", "description": "Add environment context to LLM", "default": "true"},
    36: {"name": "CAI_CTX_TRUNC", "description": "Enable context truncation", "default": "false"},
    37: {"name": "CAI_DISPLAY_MAX_OUTPUT", "description": "Show full output (no truncation)", "default": "false"},
    38: {"name": "CAI_WORKSPACE", "description": "Current workspace name", "default": None},
    39: {"name": "CAI_WORKSPACE_DIR", "description": "Workspace directory path", "default": None},
    40: {"name": "CAI_ACTIVE_CONTAINER", "description": "Active Docker container ID", "default": ""},
    41: {"name": "CAI_ACTIVE_CONTAINER_DEFAULT", "description": "Default container", "default": ""},
    42: {"name": "CAI_SUPPORT_MODEL", "description": "Model for support agent", "default": "o3-mini"},
    43: {"name": "CAI_SUPPORT_INTERVAL", "description": "Turns between support executions", "default": "5"},
    44: {"name": "CAI_META_AGENT", "description": "Enable meta agent", "default": "false"},
    45: {"name": "CAI_META_MODEL", "description": "Model for meta agent", "default": None},
    46: {"name": "CAI_META_AUTOCLOSE_GRACE", "description": "Meta agent auto-close grace (s)", "default": "1.5"},
    47: {"name": "CAI_CTR_DIGEST_MODE", "description": "CTR mode: llm or algorithmic", "default": "llm"},
    48: {"name": "CAI_CTR_DIGEST_MODEL", "description": "Model for LLM-based CTR", "default": "alias1"},
    49: {"name": "CAI_CTR_OUTPUT_DIR", "description": "CTR output directory", "default": None},
    50: {"name": "CAI_CTR_DEFAULT_OUTPUT_DIR", "description": "Default CTR output dir", "default": None},
    51: {"name": "CAI_CTR_DEFAULT_RUN", "description": "Default CTR run identifier", "default": None},
    52: {"name": "CAI_CTR_IS_CTF", "description": "CTR in CTF mode", "default": "false"},
    53: {"name": "CAI_CTR_DISTANCE_HEURISTIC", "description": "CTR graph distance heuristic", "default": None},
    54: {"name": "CAI_GCTR_NITERATIONS", "description": "Tool calls before GCTR analysis", "default": "5"},
    55: {"name": "CAI_TRACING", "description": "Enable OpenTelemetry tracing", "default": "true"},
    56: {"name": "CAI_TELEMETRY", "description": "Enable telemetry collection", "default": "true"},
    57: {"name": "CAI_DISABLE_SESSION_RECORDING", "description": "Disable JSONL recording", "default": "false"},
    58: {"name": "CAI_DISABLE_USAGE_TRACKING", "description": "Disable usage tracking", "default": "false"},
    59: {"name": "CAI_GUARDRAILS", "description": "Enable security guardrails", "default": "false"},
    60: {"name": "CAI_PLAN", "description": "Enable planning mode", "default": "false"},
    61: {"name": "CAI_COST_DISPLAYED", "description": "Show cost display", "default": "false"},
    62: {"name": "CAI_ENABLE_PRICING_FETCH", "description": "Enable async pricing fetch", "default": "false"},
    63: {"name": "CAI_DEBUG_PRICING", "description": "Debug pricing calculations", "default": "false"},
    64: {"name": "CAI_PRICING_FILE", "description": "Custom pricing data file", "default": None},
    65: {"name": "CAI_PRICINGS_DIR", "description": "Pricing data directory", "default": None},
    66: {"name": "CAI_REPORT", "description": "Report mode (ctf, nis2, pentesting)", "default": "ctf"},
    67: {"name": "CAI_CONTINUATION_FALLBACK_MODEL", "description": "Fallback model for continuation", "default": None},
    68: {"name": "CAI_API_HOST", "description": "API server host", "default": "127.0.0.1"},
    69: {"name": "CAI_API_PORT", "description": "API server port", "default": "8000"},
    70: {"name": "CAI_API_CORS", "description": "CORS allowed origins", "default": "*"},
    71: {"name": "CAI_API_KEY_HEADER", "description": "API key header name", "default": "X-CAI-API-Key"},
    72: {"name": "CAI_API_LOG_AUTH", "description": "Log authentication", "default": "false"},
    73: {"name": "CAI_API_LOG_REQUESTS", "description": "Log API requests", "default": "false"},
    74: {"name": "CAI_API_LOG_LEVEL", "description": "API log level", "default": "info"},
    75: {"name": "CAI_API_RELOAD", "description": "API hot-reload mode", "default": "false"},
    76: {"name": "CAI_API_WORKERS", "description": "API worker processes", "default": "1"},
    77: {"name": "CAI_AUTH_BASE_URL", "description": "Auth service base URL", "default": None},
    78: {"name": "CAI_AUTH_DEVICE_PORT", "description": "Device auth port", "default": "10101"},
    79: {"name": "CAI_AUTH_PUBLIC_HOST", "description": "Public auth host", "default": None},
    80: {"name": "CAI_AUTH_PUBLIC_PORT", "description": "Public auth port", "default": None},
    81: {"name": "CAI_AUTH_SESSION_TTL_SECONDS", "description": "Session TTL (seconds)", "default": None},
    82: {"name": "CAI_MCP_TOKEN", "description": "MCP authentication token", "default": None},
    83: {"name": "CAI_MCP_AUTH_TOKEN", "description": "MCP auth token (alt)", "default": None},
    84: {"name": "CAI_MCP_SSE_TIMEOUT", "description": "MCP SSE timeout (s)", "default": "5"},
    85: {"name": "CAI_MCP_SSE_READ_TIMEOUT", "description": "MCP SSE read timeout (s)", "default": "300"},
    86: {"name": "CAI_TUI_MODE", "description": "Enable TUI mode", "default": "false"},
    87: {"name": "CAI_TUI_STARTUP_YAML", "description": "TUI startup config YAML", "default": None},
    88: {"name": "CAI_TUI_SHARED_PROMPT", "description": "Shared TUI prompt", "default": None},
    89: {"name": "CAI_TUI_MAX_LINES", "description": "Max TUI output lines", "default": None},
    90: {"name": "CAI_TUI_MAX_RERENDERS_PER_SEC", "description": "Max TUI re-renders/s", "default": None},
    91: {"name": "CAI_VERSION", "description": "CAI version string", "default": "dev"},
    92: {"name": "CAI_THEME", "description": "UI color theme", "default": None},
    93: {"name": "CAI_SKIP_NETWORK_CHECK", "description": "Skip network checks", "default": "false"},
    94: {"name": "CAI_AUTO_COMPACT", "description": "Enable auto-compaction", "default": None},
    95: {
        "name": "CAI_AUTO_COMPACT_THRESHOLD",
        "description": "Context fraction before auto-compact (default 0.8); max 0.8 — higher values are capped",
        "default": None,
    },
    96: {"name": "CAI_WARN_UNATTRIBUTED", "description": "Warn unattributed content", "default": "false"},
    97: {"name": "CAI_UNATTRIBUTED_LOG", "description": "Unattributed content log", "default": "~/.cai_unattributed.log"},
    98: {"name": "CAI_PATTERN_DESCRIPTION", "description": "Agent pattern description", "default": ""},
    99: {"name": "CAI_MODEL_LIST", "description": "Custom model list", "default": None},
    100: {"name": "CAI_CONTEXT_USAGE", "description": "Context usage tracking", "default": None},
    101: {"name": "CAI_SESSION_INPUT_WAIT", "description": "Session input wait (s)", "default": "5.0"},
    102: {"name": "CAI_BROADCAST_MODE", "description": "Broadcast mode for parallel", "default": None},
    103: {
        "name": "CAI_COMPACT_REPL",
        "description": (
            "Enable compact CLI task UI (1/true/yes/on); use 0/false for legacy verbose scrollback. "
            "Locked at startup — restart CAI for the change to take effect."
        ),
        "default": "true",
    },
    104: {
        "name": "CAI_FETCH_ALLOW_INTERNAL",
        "description": (
            "Permit fetch_url to reach loopback/RFC1918/link-local hosts "
            "(true during internal pentests). Cloud-metadata is always blocked."
        ),
        "default": "false",
    },
    105: {
        "name": "CAI_FETCH_USER_AGENT",
        "description": "Override User-Agent for fetch_url (OPSEC).",
        "default": None,
    },
    106: {
        "name": "CAI_FETCH_MAX_BYTES",
        "description": "Hard cap on fetch_url response body (bytes; default 5 MiB).",
        "default": "5242880",
    },
    107: {
        "name": "CAI_FETCH_TIMEOUT",
        "description": "Per-request timeout for fetch_url (seconds).",
        "default": "20",
    },
}


def _catalog_default_from_extra(entry: Dict[str, object]) -> Optional[str]:
    raw = entry.get("default")
    if raw is None:
        return None
    s = str(raw).strip()
    sl = s.lower()
    if sl in ("unset", "unset (off)", "—", "-", ""):
        return None
    return s


def _merge_extra_catalog_entries() -> None:
    """Append ``env_info_catalog.EXTRA_ENV_VARS`` into ``ENV_VARS`` (same keys as /help reference)."""
    global EXTRA_CATALOG_RANGE  # pylint: disable=global-statement

    from cai.repl.commands.env_info_catalog import EXTRA_ENV_VARS

    existing = {str(v["name"]) for v in ENV_VARS.values()}
    n = max(ENV_VARS.keys()) + 1
    first = n
    for entry in EXTRA_ENV_VARS:
        name = str(entry["name"])
        if name in existing:
            continue
        ENV_VARS[n] = {
            "name": name,
            "description": str(entry.get("description") or ""),
            "default": _catalog_default_from_extra(entry),
        }
        existing.add(name)
        n += 1
    last = n - 1
    if last >= first:
        EXTRA_CATALOG_RANGE = (first, last)


def get_env_var_value(var_name: str) -> str:
    """Return current value or catalog default (or ``Not set``)."""
    for var_info in ENV_VARS.values():
        if var_info["name"] == var_name:
            return os.environ.get(var_name, var_info["default"] or "Not set")
    return "Unknown variable"


def set_env_var(var_name: str, value: str) -> bool:
    os.environ[var_name] = value
    return True


def find_var_num_by_name(var_name: str) -> Optional[int]:
    for num, var_info in ENV_VARS.items():
        if var_info["name"] == var_name:
            return num
    return None


def add_agent_model_vars_to_catalog() -> None:
    """Append CAI_<AGENT>_MODEL entries (and parallel instances) to ENV_VARS."""
    try:
        from cai.agents import get_available_agents

        available_agents = get_available_agents()
        current_var_num = max(ENV_VARS.keys()) + 1

        for agent_key in sorted(available_agents.keys()):
            var_name = f"CAI_{agent_key.upper()}_MODEL"
            agent_obj = available_agents[agent_key]
            agent_display_name = getattr(agent_obj, "name", agent_key)

            ENV_VARS[current_var_num] = {
                "name": var_name,
                "description": f"Model override for {agent_display_name} agent",
                "default": None,
            }
            current_var_num += 1

        parallel_count = int(os.getenv("CAI_PARALLEL", "1"))
        if parallel_count > 1:
            for agent_key in sorted(available_agents.keys()):
                agent_obj = available_agents[agent_key]
                agent_display_name = getattr(agent_obj, "name", agent_key)

                for instance_num in range(1, parallel_count + 1):
                    var_name = f"CAI_{agent_key.upper()}_{instance_num}_MODEL"

                    ENV_VARS[current_var_num] = {
                        "name": var_name,
                        "description": (
                            f"Model override for {agent_display_name} instance #{instance_num}"
                        ),
                        "default": None,
                    }
                    current_var_num += 1
    except Exception:  # pylint: disable=broad-except
        pass


def mask_secret_catalog_display(key: str, value: str) -> str:
    if any(s in key.lower() for s in ("key", "token", "secret", "password")):
        if not value:
            return value
        half = len(value) // 2
        return value[:half] + "*" * (len(value) - half)
    return value


def print_bare_env_session_view() -> bool:
    """``/env`` with no args: only ``CAI_*`` / ``CTF_*`` keys present in ``os.environ`` (legacy behaviour)."""
    env_vars = {k: v for k, v in os.environ.items() if k.startswith(("CAI_", "CTF_"))}

    if not env_vars:
        console.print(
            Panel(
                Text("No CAI_ or CTF_ environment variables in this process.", style="yellow"),
                title=_quick_guide_subpanel_title("Environment variables — session"),
                title_align="left",
                border_style=_CAI_GREEN,
                padding=(1, 1),
            )
        )
        return True

    table = Table(**HELP_REFERENCE_MATCH_TABLE_KWARGS)
    table.add_column("Variable", no_wrap=True, min_width=18)
    table.add_column("Value", ratio=1)

    for idx, (key, value) in enumerate(sorted(env_vars.items())):
        body_style = "white" if idx % 2 == 0 else _GREY
        masked = mask_secret_catalog_display(key, value)
        table.add_row(
            Text(key, style=f"bold {_CAI_GREEN}"),
            Text(masked, style=body_style),
        )

    console.print(
        Panel(
            table,
            title=_quick_guide_subpanel_title("Environment variables — session"),
            title_align="left",
            border_style=_CAI_GREEN,
            padding=(1, 1),
        )
    )
    console.print(
        "[dim]Tip: [bold]/env list[/bold] for the full numbered catalog (all variables).[/dim]"
    )
    return True


def handle_env_catalog_list(_: Optional[List[str]] = None) -> bool:
    """Print every catalog row; table chrome matches ``/help`` environment reference tables."""
    table = Table(**HELP_REFERENCE_MATCH_TABLE_KWARGS)
    table.add_column("#", justify="right", width=4, no_wrap=True)
    table.add_column("Variable", no_wrap=True, min_width=16)
    table.add_column("Current", min_width=8, ratio=1)
    table.add_column("Default", min_width=8, max_width=22, no_wrap=True)
    table.add_column("Values", min_width=10, ratio=2)
    table.add_column("When", min_width=8, no_wrap=True, ratio=1)
    table.add_column("Description", min_width=18, ratio=4)

    for idx, (num, var_info) in enumerate(sorted(ENV_VARS.items(), key=lambda x: x[0])):
        name = str(var_info["name"])
        desc = str(var_info.get("description") or "")
        default = var_info.get("default")
        default_s = "—" if default is None else str(default)
        raw_val = get_env_var_value(name)
        current_value = mask_secret_catalog_display(name, raw_val)
        body_style = "white" if idx % 2 == 0 else _GREY
        table.add_row(
            Text(str(num), style=body_style),
            Text(name, style=f"bold {_CAI_GREEN}"),
            Text(current_value, style=body_style),
            Text(default_s, style=_CAI_GREEN),
            Text(constraints_line(name, desc), style=body_style),
            Text(effective_label(name), style=body_style),
            Text(desc, style=body_style),
        )

    console.print(
        Panel(
            table,
            title=_quick_guide_subpanel_title("Environment variables — catalog (all)"),
            title_align="left",
            border_style=_CAI_GREEN,
            padding=(1, 1),
        )
    )
    console.print(
        "\n[#9aa0a6][CAI] Usage:[/] "
        "[bold #00ff9d]/env set <#|NAME> <value...>[/bold #00ff9d] — "
        "value may contain spaces (no quotes). "
        "[bold #00ff9d]/env default[/bold #00ff9d] restores catalog defaults."
    )
    return True


def handle_env_catalog_get(args: Optional[List[str]] = None) -> bool:
    if not args or not str(args[0]).strip():
        console.print("[yellow]Usage: /env get <number|VARIABLE_NAME>[/yellow]")
        return False

    resolved = resolve_catalog_spec(str(args[0]), ENV_VARS)
    if not resolved:
        console.print(f"[red]Error: Unknown catalog entry '{args[0]}'[/red]")
        console.print("[yellow]Use /env list for numbers and names.[/yellow]")
        return False

    var_num, var_info, var_name = resolved
    raw_val = get_env_var_value(var_name)
    current_value = mask_secret_catalog_display(var_name, raw_val)
    def_disp = var_info["default"] if var_info["default"] is not None else "Not set"
    desc = str(var_info.get("description") or "")

    body = (
        f"[bold #00ff9d]{var_name}[/bold #00ff9d]  [dim](#{var_num})[/dim]\n\n"
        f"[#9aa0a6]Current:[/] [white]{current_value}[/white]\n"
        f"[#9aa0a6]Default:[/] [white]{def_disp}[/white]\n\n"
        f"[dim]{desc}[/dim]"
    )
    console.print(
        Panel(
            body,
            title="[bold #00ff9d]Catalog variable[/bold #00ff9d]",
            title_align="left",
            border_style="#00ff9d",
            padding=(1, 1),
        )
    )
    return True


def handle_env_catalog_set(args: Optional[List[str]] = None) -> bool:
    if not args or len(args) < 2:
        console.print("[yellow]Usage: /env set <number|VARIABLE_NAME> <value...>[/yellow]")
        return False

    resolved = resolve_catalog_spec(str(args[0]), ENV_VARS)
    if not resolved:
        console.print(f"[red]Error: Unknown catalog entry '{args[0]}'[/red]")
        console.print("[yellow]Use /env list for numbers and names.[/yellow]")
        return False

    value = " ".join(args[1:]).strip()
    if not value:
        console.print("[red]Error: value cannot be empty.[/red]")
        return False

    _var_num, var_info, var_name = resolved
    if is_restart_required(var_name):
        console.print(
            f"[red]Error: [bold #00ff9d]{var_name}[/bold #00ff9d] is locked at "
            f"startup; runtime mutation has no effect.[/red]"
        )
        console.print(
            f"[yellow]Export it before launching cai, e.g. "
            f"[white]export {var_name}={value}[/white], or add it to your .env "
            f"and restart.[/yellow]"
        )
        return False

    err = validate_catalog_value(var_name, value, var_info)
    if err:
        console.print(f"[red]{err}[/red]")
        return False

    old_value = get_env_var_value(var_name)
    set_env_var(var_name, value)

    # Tier change must rebuild the gateway rate limiter; otherwise the cached
    # singleton keeps the previous tier's TPM/RPM until process restart.
    if var_name == "CAI_ALIAS_RATE_TIER" and value != old_value:
        try:
            from cai.util.gateway_rate_limiter import reset_gateway_rate_limiter

            reset_gateway_rate_limiter()
        except Exception:
            pass

    console.print(
        f"Set [bold #00ff9d]{var_name}[/bold #00ff9d] to "
        f"[white]'{value}'[/white] [dim](was: '{old_value}')[/dim]"
    )
    return True


def handle_env_catalog_default(args: Optional[List[str]] = None) -> bool:
    if args:
        console.print("[yellow]Usage: /env default[/yellow] (no arguments)")
        return False

    skipped: List[str] = []
    preserved_secrets: List[str] = []
    for _num, var_info in sorted(ENV_VARS.items()):
        name = str(var_info["name"])
        if is_restart_required(name):
            skipped.append(name)
            continue
        if is_secret(name):
            preserved_secrets.append(name)
            continue
        default = var_info["default"]
        if default is None:
            os.environ.pop(name, None)
        else:
            os.environ[name] = str(default)

    console.print(
        "Restored runtime [bold #00ff9d]catalog[/bold #00ff9d] variables "
        "to their registered defaults."
    )
    if preserved_secrets:
        console.print(
            f"[yellow]Preserved {len(preserved_secrets)} credential variable(s) "
            f"to keep authentication intact: {', '.join(preserved_secrets)}. "
            f"Use [white]/env set <NAME> <value>[/white] to mutate them explicitly.[/yellow]"
        )
    if skipped:
        console.print(
            f"[yellow]Skipped {len(skipped)} locked-at-startup variable(s); "
            f"restart cai (or export them) to reset: {', '.join(skipped)}.[/yellow]"
        )
    return True


_merge_extra_catalog_entries()
add_agent_model_vars_to_catalog()
