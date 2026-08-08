"""
Generic agent factory module for creating agent instances dynamically.

Uses CAIConfig singleton for configuration instead of scattered os.getenv() calls [S].
"""

import importlib
import os
from typing import Callable, Dict

from openai import AsyncOpenAI

from cai.config import get_config
from cai.sdk.agents import Agent, OpenAIChatCompletionsModel
from cai.sdk.agents.logger import logger


def create_generic_agent_factory(
    agent_module_path: str, agent_var_name: str
) -> Callable[[str | None, str | None], Agent]:
    """
    Create a generic factory function for any agent.

    Args:
        agent_module_path: Full module path to the agent (e.g., 'cai.agents.one_tool')
        agent_var_name: Name of the agent variable in the module (e.g., 'one_tool_agent')

    Returns:
        A factory function that creates new instances of the agent
    """

    def factory(
        model_override: str | None = None,
        custom_name: str | None = None,
        agent_id: str | None = None,
    ):
        # Import the module
        module = importlib.import_module(agent_module_path)

        # Get the original agent instance
        original_agent = getattr(module, agent_var_name)

        # Get model configuration from CAIConfig singleton [S]
        cfg = get_config()
        model_name = model_override  # First priority: explicit override

        if not model_name:
            # Second priority: agent-specific environment variable
            # (kept as os.getenv because these are per-agent overrides)
            agent_key = agent_var_name.upper()
            model_name = os.getenv(f"CAI_{agent_key}_MODEL")

        if not model_name:
            # Third priority: global CAI_MODEL via CAIConfig
            model_name = cfg.model

        api_key = cfg.openai_api_key or "sk-placeholder-key-for-local-models"

        # Create a new model instance with the original agent name
        # Custom name is only for display purposes, not for the model
        new_model = OpenAIChatCompletionsModel(
            model=model_name,
            openai_client=AsyncOpenAI(api_key=api_key),
            agent_name=original_agent.name,  # Always use original agent name
            agent_id=agent_id,
            agent_type=agent_var_name,  # Pass the agent type for registry
        )

        # Mark as parallel agent if running in parallel mode [S]
        parallel_count = cfg.parallel
        if parallel_count > 1 and agent_id and agent_id.startswith("P"):
            new_model._is_parallel_agent = True

        # Clone the agent with the new model
        cloned_agent = original_agent.clone(model=new_model)

        # Update agent name if custom name was provided
        if custom_name:
            cloned_agent.name = custom_name
            
        # Apply compacted memory to agent if available
        from cai.util import apply_compacted_memory_to_agent
        apply_compacted_memory_to_agent(cloned_agent)

        # [R] Supplement agent tools from TOOL_REGISTRY ──────────────────
        # Only when explicitly enabled via CAI_TOOL_REGISTRY_AUTO=true.
        # Disabled by default to keep token usage minimal: each extra tool
        # schema costs ~120-150 prompt tokens per turn, which compounds
        # quadratically across conversation turns.
        if cfg.tool_registry_auto:
            try:
                from cai.tool_registry import TOOL_REGISTRY

                if TOOL_REGISTRY.count > 0:
                    existing_names = {
                        getattr(t, "name", str(t))
                        for t in (cloned_agent.tools or [])
                    }
                    extra_tools = TOOL_REGISTRY.list_for_agent_filtered(agent_var_name)
                    for tool in extra_tools:
                        tname = getattr(tool, "name", str(tool))
                        if tname not in existing_names:
                            if not hasattr(cloned_agent, "tools") or cloned_agent.tools is None:
                                cloned_agent.tools = []
                            cloned_agent.tools.append(tool)
                            existing_names.add(tname)
                            logger.debug("[R] Auto-added tool %s to agent %s", tname, agent_var_name)
            except Exception as exc:
                logger.debug("[R] ToolRegistry supplement skipped: %s", exc)

        # Check if this agent has any MCP tools configured
        try:
            from cai.repl.commands.mcp import get_mcp_tools_for_agent

            # Get MCP tools for this agent and add them
            mcp_tools = get_mcp_tools_for_agent(agent_var_name)
            if mcp_tools:
                # Ensure the agent has tools list
                if not hasattr(cloned_agent, "tools"):
                    cloned_agent.tools = []

                # Remove any existing tools with the same names to avoid duplicates
                existing_tool_names = {t.name for t in mcp_tools}
                cloned_agent.tools = [
                    t for t in cloned_agent.tools if t.name not in existing_tool_names
                ]

                # Add the MCP tools
                cloned_agent.tools.extend(mcp_tools)

        except ImportError:
            # MCP command not available, skip
            pass

        return cloned_agent

    return factory


def discover_agent_factories() -> Dict[str, Callable[[], Agent]]:
    """
    Dynamically discover all agents and create factories for them.

    Returns:
        Dictionary mapping agent names to factory functions
    """
    import pkgutil
    import os

    import cai.agents

    agent_factories = {}

    # Scan the agents module for all agent definitions
    for importer, modname, ispkg in pkgutil.iter_modules(
        cai.agents.__path__, cai.agents.__name__ + "."
    ):
        if ispkg:
            continue  # Skip packages like 'patterns' and 'meta'

        try:
            # Import the module
            module = importlib.import_module(modname)

            # Look for Agent instances
            for attr_name in dir(module):
                if attr_name.startswith("_"):
                    continue

                attr = getattr(module, attr_name)
                if isinstance(attr, Agent):
                    # Create a factory for this agent
                    agent_name = attr_name.lower()
                    agent_factories[agent_name] = create_generic_agent_factory(modname, attr_name)

        except Exception:
            # Skip modules that fail to import
            continue

    # Also scan patterns subdirectory
    patterns_path = os.path.join(os.path.dirname(cai.agents.__file__), "patterns")
    if os.path.exists(patterns_path):
        for importer, modname, ispkg in pkgutil.iter_modules(
            [patterns_path], cai.agents.__name__ + ".patterns."
        ):
            if ispkg:
                continue

            try:
                module = importlib.import_module(modname)

                for attr_name in dir(module):
                    if attr_name.startswith("_"):
                        continue

                    attr = getattr(module, attr_name)
                    if isinstance(attr, Agent):
                        agent_name = attr_name.lower()
                        agent_factories[agent_name] = create_generic_agent_factory(
                            modname, attr_name
                        )

            except Exception:
                continue
    
    # Also scan personal subdirectory
    personal_path = os.path.join(os.path.dirname(cai.agents.__file__), "personal")
    if os.path.exists(personal_path):
        for importer, modname, ispkg in pkgutil.iter_modules(
            [personal_path], cai.agents.__name__ + ".personal."
        ):
            if ispkg:
                continue

            try:
                module = importlib.import_module(modname)
                for attr_name in dir(module):
                    if attr_name.startswith("_"):
                        continue
                    attr = getattr(module, attr_name)
                    if isinstance(attr, Agent):
                        agent_name = attr_name.lower()
                        agent_factories[agent_name] = create_generic_agent_factory(
                            modname, attr_name
                        )

            except Exception:
                continue

    return agent_factories


# Global registry of agent factories
AGENT_FACTORIES = None


def get_agent_factory(agent_name: str) -> Callable[[], Agent]:
    """
    Get a factory function for creating instances of the specified agent.

    Args:
        agent_name: Name of the agent

    Returns:
        Factory function that creates new agent instances

    Raises:
        ValueError: If agent not found
    """
    global AGENT_FACTORIES

    # Lazy initialization
    if AGENT_FACTORIES is None:
        AGENT_FACTORIES = discover_agent_factories()

    agent_name_lower = agent_name.lower()

    if agent_name_lower not in AGENT_FACTORIES:
        raise ValueError(
            f"Agent '{agent_name}' not found. Available agents: {list(AGENT_FACTORIES.keys())}"
        )

    return AGENT_FACTORIES[agent_name_lower]
