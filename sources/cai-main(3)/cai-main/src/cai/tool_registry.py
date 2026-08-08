"""CAI Tool Registry.

Central registry for tool discovery and dispatch.
Inspired by Codex's ToolRegistry (HashMap) + ToolRouter pattern.

Created in Day 0 as shared contract between 3 refactoring streams.
- Stream 1 (Core Engine): implements registry, tools auto-register here
- Stream 2 (Foundation): migrates agents/ to consume from registry

Enhanced to support:
- Agent-type to category mapping [E]
- API-key gated tools (requires_key field) [E]
- Category listing for introspection [E]
"""

from __future__ import annotations

import logging
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from cai.sdk.agents.tool import FunctionTool

_logger = logging.getLogger(__name__)

# Agent type -> tool category mapping [E]
# Maps each agent type to the categories of tools it should have access to.
AGENT_TOOL_CATEGORIES: dict[str, list[str]] = {
    "redteam": [
        "recon", "exploitation", "lateral_movement", "privesc",
        "web", "c2", "network", "misc",
    ],
    "blueteam": ["recon", "defensive", "monitoring", "web", "c2", "misc"],
    "bugbounty": ["recon", "web", "exploitation", "network", "misc"],
    "purple": ["recon", "exploitation", "defensive", "web", "network", "misc"],
    "dfir": ["recon", "defensive", "forensics", "network", "misc"],
}

# Tools that require specific API keys to be available [E]
# Maps tool name -> CAIConfig attribute that must be truthy
TOOL_REQUIRES_KEY: dict[str, str] = {
    "query_perplexity": "perplexity_api_key",
    "make_web_search_with_explanation": "perplexity_api_key",
    "c99": "c99_api_key",
    "c99_subdomain_enum": "c99_api_key",
    "shodan_search": "shodan_api_key",
    "shodan_host_info": "shodan_api_key",
    "make_google_search": "google_search_api_key",
    "google_search": "google_search_api_key",
}


# Alias map for agent variable names that don't match AGENT_TOOL_CATEGORIES keys.
# e.g. "bug_bounter" -> "bugbounty", "web_pentester" -> "redteam"
_AGENT_ALIASES: dict[str, str] = {
    "bug_bounter": "bugbounty",
    "web_pentester": "redteam",
    "apt": "redteam",
    "network_traffic_analyzer": "redteam",
    "replay_attack": "redteam",
    "retester": "bugbounty",
}


def _normalize_agent_type(raw: str) -> str:
    """Normalize agent variable names to AGENT_TOOL_CATEGORIES keys.

    Examples:
        'redteam_agent'       -> 'redteam'
        'bug_bounter_agent'   -> 'bugbounty'
        'red_teamer_gctr'     -> 'redteam'  (via alias 'red_teamer' isn't needed; 'redteam' matches)
    """
    normalized = raw.removesuffix("_agent").removesuffix("_gctr")
    return _AGENT_ALIASES.get(normalized, normalized)


class ToolRegistry:
    """Central tool registry replacing hardcoded tool lists in agents."""

    def __init__(self) -> None:
        self._tools: dict[str, "FunctionTool"] = {}
        self._categories: dict[str, list[str]] = {}

    def register(
        self,
        name: str,
        tool: "FunctionTool",
        categories: list[str] | None = None,
    ) -> None:
        """Register a tool with optional categories.

        If *tool* is a plain callable (not a FunctionTool), it is
        silently skipped and a debug log is emitted. This prevents
        raw functions from reaching the agent loop where .name is required.
        """
        if not hasattr(tool, "name"):
            _logger.debug(
                "Skipping registration of %s: not a FunctionTool (got %s)",
                name, type(tool).__name__,
            )
            return
        self._tools[name] = tool
        self._categories[name] = categories or ["misc"]

    def get(self, name: str) -> "FunctionTool":
        """Get tool by name. Raises ToolNotFound if missing."""
        if name not in self._tools:
            from cai.errors import ToolNotFound

            raise ToolNotFound(f"Tool '{name}' not registered")
        return self._tools[name]

    def list_for_category(self, category: str) -> list["FunctionTool"]:
        """Get all tools in a category."""
        return [
            self._tools[name]
            for name, cats in self._categories.items()
            if category in cats
        ]

    def list_for_agent(self, agent_type: str) -> list["FunctionTool"]:
        """Get all tools appropriate for an agent type (no API-key filtering)."""
        resolved = _normalize_agent_type(agent_type)
        categories = AGENT_TOOL_CATEGORIES.get(resolved, ["misc"])
        seen: set[str] = set()
        tools: list["FunctionTool"] = []
        for cat in categories:
            for tool in self.list_for_category(cat):
                tname = getattr(tool, "name", str(tool))
                if tname not in seen:
                    seen.add(tname)
                    tools.append(tool)
        return tools

    def list_for_agent_filtered(self, agent_type: str) -> list["FunctionTool"]:
        """Get tools for an agent type, filtered by available API keys [E].

        Tools in TOOL_REQUIRES_KEY are only included if the corresponding
        CAIConfig field is truthy (i.e., the API key is set).
        """
        from cai.config import get_config
        cfg = get_config()
        all_tools = self.list_for_agent(agent_type)
        result = []
        for tool in all_tools:
            tname = getattr(tool, "name", str(tool))
            required_key_attr = TOOL_REQUIRES_KEY.get(tname)
            if required_key_attr:
                if not getattr(cfg, required_key_attr, None):
                    _logger.debug("Skipping tool %s: %s not set", tname, required_key_attr)
                    continue
            result.append(tool)
        return result

    def all(self) -> list["FunctionTool"]:
        """Get all registered tools."""
        return list(self._tools.values())

    def categories(self) -> list[str]:
        """Get all registered categories [E]."""
        cats: set[str] = set()
        for tool_cats in self._categories.values():
            cats.update(tool_cats)
        return sorted(cats)

    @property
    def count(self) -> int:
        return len(self._tools)


# Singleton
TOOL_REGISTRY = ToolRegistry()
