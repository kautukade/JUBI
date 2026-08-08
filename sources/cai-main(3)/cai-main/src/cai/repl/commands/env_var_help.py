"""
Long-form help for a single environment variable (``/help var NAME``).

Builds on ``ENV_VARS`` / ``EXTRA_ENV_VARS`` plus optional example overrides.
Also owns the shared Rich markup helpers for the parent command (used by
``help.py`` and ``environment_reference.py``).
"""

from __future__ import annotations

from difflib import get_close_matches
from typing import Dict, List, Optional, Tuple

from cai.repl.commands.env_catalog import ENV_VARS
from cai.repl.commands.env_info_catalog import (
    EXTRA_ENV_VARS,
    category_title_for_number,
    constraints_line,
    effective_label,
)

ENV_VAR_DETAIL_COMMAND = "/help"
ENV_VAR_DETAIL_SUBCOMMAND = "var"


def usage_markup_bold() -> str:
    """Rich markup (use literal NAME — avoid angle brackets; Rich treats ``<...>`` as tags)."""
    return f"[bold]{ENV_VAR_DETAIL_COMMAND} {ENV_VAR_DETAIL_SUBCOMMAND} NAME[/bold]"


def example_cyan_line(var_name: str = "CAI_MODEL") -> str:
    """One bullet for docs (CAI green + bold, same as env_var_help snippets)."""
    return (
        f"• [bold #00ff9d]{ENV_VAR_DETAIL_COMMAND} {ENV_VAR_DETAIL_SUBCOMMAND} "
        f"{var_name}[/bold #00ff9d]"
    )

# Snippets in “How to set” / “Examples” use CAI green + bold (#00ff9d), not cyan (reads blue in many themes).

# Extra example lines (shell / REPL) keyed by canonical variable name.
_VAR_EXAMPLES: Dict[str, List[str]] = {
    "CAI_MODEL": [
        "[bold #00ff9d]export CAI_MODEL=alias1[/bold #00ff9d]",
        "[bold #00ff9d]/env set 9 gpt-4o[/bold #00ff9d]",
        "[bold #00ff9d]CAI_MODEL=o3-mini cai[/bold #00ff9d]  [dim]# one process only[/dim]",
    ],
    "CAI_AGENT_TYPE": [
        "[bold #00ff9d]export CAI_AGENT_TYPE=orchestration_agent[/bold #00ff9d]  "
        "[dim]# default: breadth-first + specialist tools (parallel / contest / single)[/dim]",
        "[bold #00ff9d]export CAI_AGENT_TYPE=selection_agent[/bold #00ff9d]  [dim]# handoff-only router[/dim]",
        "[bold #00ff9d]/agent select redteam_agent[/bold #00ff9d]  [dim]# also updates this env[/dim]",
        "[bold #00ff9d]/env set 10 one_tool_agent[/bold #00ff9d]",
    ],
    "CAI_ORCHESTRATION_WORKER_MAX_TURNS": [
        "[bold #00ff9d]export CAI_ORCHESTRATION_WORKER_MAX_TURNS=8[/bold #00ff9d]  "
        "[dim]# per specialist worker Runner cap (1–32)[/dim]",
        "[bold #00ff9d]/env set CAI_ORCHESTRATION_WORKER_MAX_TURNS 4[/bold #00ff9d]",
    ],
    "CAI_ORCHESTRATION_MAS_HINT": [
        "[bold #00ff9d]export CAI_ORCHESTRATION_MAS_HINT=false[/bold #00ff9d]  "
        "[dim]# disable synthetic multi-front nudge for orchestration_agent[/dim]",
        "[bold #00ff9d]/env set CAI_ORCHESTRATION_MAS_HINT true[/bold #00ff9d]",
    ],
    "CAI_TEMPERATURE": [
        "[bold #00ff9d]export CAI_TEMPERATURE=0.3[/bold #00ff9d]  [dim]# steadier answers[/dim]",
        "[bold #00ff9d]export CAI_TEMPERATURE=1.0[/bold #00ff9d]  [dim]# more variety[/dim]",
        "[bold #00ff9d]/temperature 0.5[/bold #00ff9d]  [dim]# REPL: env + active agent model_settings[/dim]",
        "[bold #00ff9d]/env set 11 0.7[/bold #00ff9d]",
    ],
    "CAI_TOP_P": [
        "[bold #00ff9d]export CAI_TOP_P=0.95[/bold #00ff9d]  [dim]# slightly tighter nucleus[/dim]",
        "[bold #00ff9d]export CAI_TOP_P=1.0[/bold #00ff9d]  [dim]# default, broad sampling[/dim]",
        "[bold #00ff9d]/env set 12 1.0[/bold #00ff9d]",
    ],
    "CAI_DEBUG": [
        "[bold #00ff9d]export CAI_DEBUG=0[/bold #00ff9d]  [dim]# quiet[/dim]",
        "[bold #00ff9d]export CAI_DEBUG=1[/bold #00ff9d]  [dim]# default[/dim]",
        "[bold #00ff9d]export CAI_DEBUG=2[/bold #00ff9d]  [dim]# tracebacks on errors[/dim]",
        "[bold #00ff9d]/env set 13 2[/bold #00ff9d]",
    ],
    "CAI_STREAM": [
        "[bold #00ff9d]export CAI_STREAM=true[/bold #00ff9d]  [dim]# stream LLM tokens[/dim]",
        "[bold #00ff9d]/env set 17 true[/bold #00ff9d]",
    ],
    "CAI_PARALLEL": [
        "[bold #00ff9d]export CAI_PARALLEL=3[/bold #00ff9d]",
        "[bold #00ff9d]/env set 22 2[/bold #00ff9d]",
    ],
    "CAI_MAX_TURNS": [
        "[bold #00ff9d]export CAI_MAX_TURNS=50[/bold #00ff9d]",
        "[bold #00ff9d]export CAI_MAX_TURNS=inf[/bold #00ff9d]",
        "[bold #00ff9d]/env set 28 100[/bold #00ff9d]",
    ],
    "CTF_NAME": [
        "[bold #00ff9d]export CTF_NAME=my_challenge_image_tag[/bold #00ff9d]  [dim]# pentestperf / caibench[/dim]",
        "[bold #00ff9d]/env set 1 kiddoctf[/bold #00ff9d]",
    ],
    "CSI_CUSTOM_ENDPOINT": [
        "[bold #00ff9d]export CSI_CUSTOM_ENDPOINT=http://127.0.0.1:8080/v1[/bold #00ff9d]  [dim]# e.g. CSI + CAI[/dim]",
    ],
    "ALIAS_API_URL": [
        "[bold #00ff9d]export ALIAS_API_URL=https://your-gateway.example/v1/[/bold #00ff9d]",
        "[bold #00ff9d]/env set ALIAS_API_URL https://your-gateway.example/v1/[/bold #00ff9d]",
    ],
}

# Optional long “usage notes” prepended after the catalog description (English).
_VAR_NOTES: Dict[str, str] = {
    "CAI_MODEL": (
        "This is the LiteLLM / provider-facing model id CAI passes for most agents unless "
        "overridden by a per-agent [bold]CAI_<AGENT>_MODEL[/bold]. Alias names like "
        "[bold]alias1[/bold] are resolved inside CAI."
    ),
    "CAI_AGENT_TYPE": (
        "This is the registered agent key (see [bold]/agent list[/bold]). "
        "[bold]orchestration_agent[/bold] is the usual default: it can delegate with "
        "[bold]run_specialist[/bold], [bold]run_dual_approach_contest[/bold], and "
        "[bold]run_parallel_specialists[/bold], then synthesize for the user; worker subprocess "
        "turn budgets follow [bold]CAI_ORCHESTRATION_WORKER_MAX_TURNS[/bold]. "
        "[bold]selection_agent[/bold] is a slimmer handoff-only router (without those tools). "
        "Pin a specialist when you know exactly which toolkit you need."
    ),
    "CAI_ORCHESTRATION_WORKER_MAX_TURNS": (
        "Applies only to specialist workers spawned by [bold]orchestration_agent[/bold] tools "
        "([bold]run_specialist[/bold], [bold]run_dual_approach_contest[/bold], "
        "[bold]run_parallel_specialists[/bold]); clamped to 1–32."
    ),
    "CAI_ORCHESTRATION_MAS_HINT": (
        "When [bold]true[/bold], [bold]orchestration_agent[/bold] may receive at most one synthetic "
        "English [bold]user[/bold] line per [bold]Runner[/bold] run if the prompt looks multi-front "
        "but only [bold]run_specialist[/bold] ran—suggesting parallel or contest tools. Set "
        "[bold]false[/bold] to disable."
    ),
    "CAI_DEFAULT_AGENT": (
        "Used mainly by the [bold]TUI[/bold] when a new terminal has no agent yet. "
        "The main headless/REPL session still follows [bold]CAI_AGENT_TYPE[/bold]."
    ),
    "CSI_CUSTOM_ENDPOINT": (
        "When non-empty and the model qualifies (same [bold]cai[/bold] / [bold]alias[/bold] / [bold]csi[/bold] "
        "prefix rule as [bold]ALIAS_API_URL[/bold]), this OpenAI-compatible base wins over [bold]ALIAS_API_URL[/bold]. "
        "Usually set by CSI when launching CAI."
    ),
    "ALIAS_API_URL": (
        "When non-empty and the model qualifies, CAI uses this base for [bold]/chat/completions[/bold] after "
        "[bold]CSI_CUSTOM_ENDPOINT[/bold] and before [bold]OPENAI_API_BASE[/bold]. Other models ignore it."
    ),
}


def _all_config_var_names() -> List[str]:
    return [v["name"] for v in ENV_VARS.values()]


def _all_extra_var_names() -> List[str]:
    return [e["name"] for e in EXTRA_ENV_VARS]


def _resolve_name(raw: str) -> Optional[str]:
    """Return canonical env var name or None."""
    key = raw.strip()
    if not key:
        return None
    # Allow accidental $VAR or ${VAR}
    if key.startswith("${") and key.endswith("}"):
        key = key[2:-1]
    if key.startswith("$"):
        key = key[1:]
    upper = key.upper()
    for name in _all_config_var_names():
        if name.upper() == upper:
            return name
    for name in _all_extra_var_names():
        if name.upper() == upper:
            return name
    return None


def _find_config_entry(canonical: str) -> Tuple[Optional[int], Optional[dict]]:
    for num, info in ENV_VARS.items():
        if info["name"] == canonical:
            return int(num), info
    return None, None


def _find_extra_entry(canonical: str) -> Optional[dict]:
    for row in EXTRA_ENV_VARS:
        if row["name"] == canonical:
            return row
    return None


def _default_examples(canonical: str, num: Optional[int], default: Optional[str]) -> List[str]:
    lines: List[str] = [
        f"[bold #00ff9d]export {canonical}=<value>[/bold #00ff9d]",
    ]
    if num is not None:
        lines.append(f"[bold #00ff9d]/env set {num} <value>[/bold #00ff9d]")
    lines.append(f"[bold #00ff9d]/env set {canonical} <value>[/bold #00ff9d]")
    if default is not None and str(default).strip():
        lines.append(f"[dim]# catalog default: {default}[/dim]")
    return lines


def render_variable_help(raw: str) -> Tuple[bool, str, str]:
    """
    Returns (ok, canonical_name, rich_markup_body) for a Panel body.
    """
    canonical = _resolve_name(raw)
    if not canonical:
        names = _all_config_var_names() + _all_extra_var_names()
        by_upper = {n.upper(): n for n in names}
        needle = raw.strip().upper()
        close = get_close_matches(needle, list(by_upper.keys()), n=5, cutoff=0.45)
        suggest = ""
        if close:
            resolved = [by_upper[c] for c in close]
            suggest = "\n[dim]Did you mean: " + ", ".join(resolved) + "?[/dim]"
        return False, raw.strip().upper(), f"[red]Unknown environment variable.[/red]{suggest}"

    num, cfg_entry = _find_config_entry(canonical)
    extra = _find_extra_entry(canonical) if cfg_entry is None else None

    if cfg_entry is not None:
        desc = (cfg_entry.get("description") or "").strip()
        default = cfg_entry.get("default")
        default_s = "—" if default is None else str(default)
        values = constraints_line(canonical, desc)
        when = effective_label(canonical)
        category = category_title_for_number(int(num)) if num is not None else "—"
        lines: List[str] = [
            f"[bold #00ff9d]{canonical}[/bold #00ff9d]  [dim](/env list #{num})[/dim]",
            f"[dim]Category:[/dim] {category}",
            f"[dim]Values column:[/dim] [bold]{values}[/bold]  ·  [dim]When:[/dim] [bold]{when}[/bold]",
            "",
            "[bold]What it does[/bold]",
            desc,
        ]
        note = _VAR_NOTES.get(canonical)
        if note:
            lines.extend(["", note])
        lines.extend(
            [
                "",
                "[bold]Default in catalog[/bold]",
                default_s,
                "",
                "[bold]How to set[/bold]",
                "• Before launch: shell [bold #00ff9d]export[/bold #00ff9d] or a line in [bold #00ff9d].env[/bold #00ff9d] next to the project.",
                "• In session: [bold #00ff9d]/env set <#|NAME> <value...>[/bold #00ff9d].",
                "• From code: [bold #00ff9d]os.environ[\"VAR\"] = \"...\"[/bold #00ff9d]",
                "",
                "[bold]Examples[/bold]",
            ]
        )
        for ex in _VAR_EXAMPLES.get(canonical) or _default_examples(canonical, num, default):
            lines.append(f"• {ex}")
        lines.extend(
            [
                "",
                "[dim]Full tables: scroll [bold]/help[/bold] (below the quick guide). Live values: [bold]/env list[/bold].[/dim]",
            ]
        )
        return True, canonical, "\n".join(lines)

    # EXTRA_ENV_VARS only
    assert extra is not None
    desc = (extra.get("description") or "").strip()
    default = extra.get("default", "—")
    values = constraints_line(canonical, desc)
    when = effective_label(canonical)
    lines = [
        f"[bold #00ff9d]{canonical}[/bold #00ff9d]  [dim](extra — not merged into catalog)[/dim]",
        f"[dim]Values:[/dim] [bold]{values}[/bold]  ·  [dim]When:[/dim] [bold]{when}[/bold]",
        "",
        "[bold]What it does[/bold]",
        desc,
        "",
        "[bold]Default (documentation)[/bold]",
        str(default),
        "",
        "[bold]How to set[/bold]",
        "• [bold #00ff9d]export VAR=value[/bold #00ff9d] before starting CAI, or edit [bold #00ff9d].env[/bold #00ff9d].",
        "• If missing from [bold]/env list[/bold], set via shell [bold #00ff9d]export[/bold #00ff9d] or [bold].env[/bold].",
        "",
        "[bold]Examples[/bold]",
    ]
    for ex in _VAR_EXAMPLES.get(canonical) or [
        f"[bold #00ff9d]export {canonical}=<value>[/bold #00ff9d]",
    ]:
        lines.append(f"• {ex}")
    lines.append("")
    lines.append(
        "[dim]See [bold]/help[/bold] environment reference if this variable is not in the catalog.[/dim]"
    )
    return True, canonical, "\n".join(lines)
