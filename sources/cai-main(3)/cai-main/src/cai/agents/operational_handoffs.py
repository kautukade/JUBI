"""Operational specialist handoffs shared by routing agents (selection, orchestration)."""

from __future__ import annotations

import importlib

from cai.sdk.agents.extensions.handoff_filters import remove_all_tools
from cai.sdk.agents.handoffs import handoff


def operational_agent_factory_keys() -> list[str]:
    """Factory keys for specialists; contest workers use the same names."""
    return [attr for _, attr, _ in operational_agent_specs()]


def operational_agent_specs() -> list[tuple[str, str, str]]:
    """Module path, agent variable name, routing description."""
    return [
        (
            "cai.agents.red_teamer",
            "redteam_agent",
            "Use for broad offensive security: pentests, exploitation, privilege escalation, "
            "shell/CLI recon, and general attack-chain work (not single-app web-only scopes).",
        ),
        (
            "cai.agents.blue_teamer",
            "blueteam_agent",
            "Use for defensive work: detection engineering, IR playbooks, hardening, SOC-style "
            "triage, log/rule tuning, and blue-team exercises.",
        ),
        (
            "cai.agents.bug_bounter",
            "bug_bounter_agent",
            "Use for bug-bounty style hunting: scoped web/API/mobile app testing, PoCs, "
            "responsible disclosure - prefer web pentester only for formal web-app pentest "
            "engagements.",
        ),
        (
            "cai.agents.dfir",
            "dfir_agent",
            "Use for DFIR: disk/memory artifacts, timelines, malware triage in an incident, "
            "evidence handling, and post-breach investigation.",
        ),
        (
            "cai.agents.reverse_engineering_agent",
            "reverse_engineering_agent",
            "Use for static/dynamic RE: binaries, firmware, malware families, unpacking, "
            "and low-level behavior analysis (not generic coding tasks).",
        ),
        (
            "cai.agents.network_traffic_analyzer",
            "network_security_analyzer_agent",
            "Use when the core artifact is network data: PCAP/pcapng, flows, protocols, "
            "packet-level analysis, and traffic baselines.",
        ),
        (
            "cai.agents.wifi_security_tester",
            "wifi_security_agent",
            "Use for wireless-specific work: Wi-Fi assessment, RF/wireless protocols, "
            "and radio-layer security (not general IP pentesting).",
        ),
        (
            "cai.agents.memory_analysis_agent",
            "memory_analysis_agent",
            "Use when the user supplies or discusses memory dumps, process memory, "
            "or runtime-only artifacts (e.g. Volatility-style analysis).",
        ),
        (
            "cai.agents.reporter",
            "reporting_agent",
            "Use for polished deliverables: formal reports, executive summaries, "
            "structured write-ups, and stakeholder-facing documentation.",
        ),
        (
            "cai.agents.one_tool",
            "one_tool_agent",
            "Use for CTF-style puzzles, single-step shell commands, and very light tooling "
            "- not sustained software development.",
        ),
        (
            "cai.agents.retester",
            "retester_agent",
            "Use to validate or re-test findings: false-positive reduction, repro checks, "
            "and regression verification after fixes.",
        ),
        (
            "cai.agents.web_pentester",
            "web_pentester_agent",
            "Use for focused web application/API penetration testing and structured "
            "app security assessment (engagement-style), distinct from opportunistic "
            "bounty hunting.",
        ),
        (
            "cai.agents.apt_agent",
            "apt_agent",
            "Use for adversary simulation narratives, targeted campaign-style offensive stories, "
            "and purple/red scenarios where APT framing is explicit (within authorized scope).",
        ),
        (
            "cai.agents.usecase",
            "use_case_agent",
            "Use when the user wants a structured, scenario-driven security walkthrough "
            "or use-case template rather than ad-hoc tooling.",
        ),
        (
            "cai.agents.compliance_agent",
            "compliance_agent",
            "Use for GRC and compliance mapping: NIS2, CRA, ISO 27001, IEC 62443, controls, "
            "evidence packs, and gap analysis (Risk & Compliance specialist).",
        ),
        (
            "cai.agents.codeagent",
            "codeagent",
            "Use for substantial code: multi-file projects, refactors, test harnesses, "
            "and iterative implementation - not quick one-off shell snippets.",
        ),
        (
            "cai.agents.continuous_ops_agent",
            "continuous_ops_agent",
            "Use when the operator wants periodic / long-running monitoring or triage loops "
            "with explicit tick intervals, tmux-friendly background execution, and "
            "API-rate-aware scheduling.",
        ),
    ]


def build_operational_handoffs() -> list:
    """Handoffs to operational specialists (lazy import per module to avoid cycles)."""
    from cai.sdk.agents import Agent as _Agent

    out: list = []
    for mod_path, attr, desc in operational_agent_specs():
        try:
            mod = importlib.import_module(mod_path)
            ag = getattr(mod, attr, None)
            if isinstance(ag, _Agent):
                display = getattr(ag, "name", attr)
                out.append(
                    handoff(
                        ag,
                        tool_description_override=(
                            f"Hand off to {display} for this user request. {desc}"
                        ),
                        input_filter=remove_all_tools,
                    )
                )
        except Exception:
            continue
    return out
