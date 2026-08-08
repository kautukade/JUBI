"""Shared web-intel tools and prompt hardening for specialist agents.

Single import surface for web-intelligence tools that should be made
available to multiple specialist agents (``red_teamer``, ``blue_teamer``,
``dfir``, ``bug_bounter``, ``web_pentester``, ``apt_agent``,
``network_traffic_analyzer``, ``reverse_engineering_agent``,
``compliance_agent``).

Centralising the import keeps the per-agent ``tools = [..., *WEB_INTEL_TOOLS]``
line short, makes future additions a one-file change, and guarantees a
single ``TOOL_REGISTRY`` registration of ``fetch_url`` (the registration
runs as a side effect of the first import below).

Each specialist that opts into ``WEB_INTEL_TOOLS`` must also append
``WEB_INTEL_PROMPT_HARDENING`` to its base system prompt before handing
it to ``create_system_prompt_renderer``. Without that block the agent's
own system prompt does not bias the LLM towards ``fetch_url`` and the
model regresses to ``curl`` via ``generic_linux_command`` for read-the-web
tasks (observed pattern on direct ``/a redteam_agent`` invocations).

The orchestrator and ``one_tool``/``codeagent``/``reporter`` agents do not
import this module on purpose — see ``orchestration_agent.py`` for the
delegation rationale.
"""

from __future__ import annotations

from typing import Any

from cai.tools.web.fetch_url import fetch_url

WEB_INTEL_TOOLS: tuple[Any, ...] = (fetch_url,)

# Markdown block appended to the *base* system prompt of every specialist
# that exposes ``fetch_url``. Lives next to the tool tuple so that adding
# the tool and the guidance is a single, hard-to-forget change. The
# leading newlines preserve a clean separator from the specialist's own
# prompt; the section heading mirrors the style used elsewhere in
# ``src/cai/prompts/``.
WEB_INTEL_PROMPT_HARDENING: str = """

## Web-intelligence tool selection (MANDATORY)

For tasks that reduce to **reading the content of a URL for intel** \
(NVD / MITRE / GHSA / vendor advisories / CVE write-ups / JSON APIs / \
RSS / PDFs / HTML pages, or any "look up", "what are the latest \u2026", \
"summarise this URL" request), you **MUST** use ``fetch_url``.

Do NOT shell out to ``curl``, ``wget``, ``http``, ``lynx`` or any \
``generic_linux_command`` variant for the same job. ``fetch_url`` already \
handles SSRF, redirects, anti-bot fallback, prompt-injection sanitisation \
and returns clean Markdown / pretty JSON / extracted PDF text \u2014 no \
``jq``+``python`` shell pipelines needed. A ``curl`` fallback for the same \
fetch is a regression.

If ``fetch_url`` returns a *"JavaScript required"* notice with a JSON-API \
or static-mirror hint, follow that hint with **another** ``fetch_url`` \
call \u2014 never with ``curl``.

Reserve ``generic_linux_command`` / ``execute_code`` for things \
``fetch_url`` cannot do:
- Active web testing / fuzzing (``ffuf``, ``gobuster``, ``nuclei``, ``wfuzz``).
- POST / PUT / DELETE bodies, custom auth headers, deliberate verb fuzzing.
- Network-level probes (``nmap``, ``masscan``, ``rustscan``).
- Local file / shell ops that do not fetch a URL.
"""

__all__ = ("WEB_INTEL_TOOLS", "WEB_INTEL_PROMPT_HARDENING")
