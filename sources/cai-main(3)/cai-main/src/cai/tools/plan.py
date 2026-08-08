"""
Plan management tool to update per-agent plans (todo list).

Each agent maintains its own in-memory plan stored on the model instance
(`agent.model._current_plan`). This prevents plan mixing across agents
and avoids writing to shared JSON files under ~/.cai.

Usage:
  - Call this tool with a list of todo dictionaries to set/update the plan.
  - Do not mix plan updates with command execution tools.

The tool is a no-op unless `CAI_PLAN=true` is set in the environment.
"""

import os
from cai.sdk.agents.tool import function_tool  # pylint: disable=import-error

# Prefer the per-execution-context active model first
try:
    from cai.sdk.agents.models.openai_chatcompletions import (
        get_current_active_model,
        ACTIVE_MODEL_INSTANCES,
    )
except Exception:  # pragma: no cover - defensive import fallback
    get_current_active_model = lambda: None  # type: ignore
    ACTIVE_MODEL_INSTANCES = {}

# As a secondary fallback, use the SimpleAgentManager active agent
try:
    from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
except Exception:  # pragma: no cover
    AGENT_MANAGER = None  # type: ignore


@function_tool
async def Todo_list(todos: list | None = None) -> str:
    """
    Update the current agent's plan (todo list).

    Args:
        todos: List of todo dicts with fields like:
               {'content': str, 'status': 'pending'|'in_progress'|'completed', 'activeForm': str}

    Behavior:
        - Stores the plan in-memory on the current agent's model instance
          (per-agent, not shared).
        - Requires environment variable CAI_PLAN=true to take effect.

    Returns:
        A confirmation with the formatted <todo_list> block, or an error message.
    """
    from cai.config import get_config
    cfg = get_config()
    if not cfg.plan_enabled:
        return "Plan feature disabled. Set CAI_PLAN=true to enable plan tracking."

    if todos is None:
        return "Error: 'todos' is required (list of todo dicts)"

    if not isinstance(todos, list) or len(todos) == 0:
        return "Error: 'todos' must be a non-empty list"

    if not all(isinstance(item, dict) for item in todos):
        return "Error: every item in 'todos' must be a dict"

    # Resolve the current model instance, prioritizing the execution context
    model_instance = None

    # 1) ContextVar during model generation/tool execution
    try:
        model_instance = get_current_active_model()
    except Exception:
        model_instance = None

    # 2) Active agent from SimpleAgentManager
    if model_instance is None and AGENT_MANAGER is not None:
        try:
            agent = AGENT_MANAGER.get_active_agent()
            if agent and hasattr(agent, "model") and hasattr(agent.model, "_current_plan"):
                model_instance = agent.model
        except Exception:
            pass

    # 3) Most recent model from ACTIVE_MODEL_INSTANCES registry
    if model_instance is None and ACTIVE_MODEL_INSTANCES:
        try:
            latest_key = max(ACTIVE_MODEL_INSTANCES.keys(), key=lambda x: x[1])
            model_ref = ACTIVE_MODEL_INSTANCES.get(latest_key)
            model_instance = model_ref() if model_ref else None
        except Exception:
            model_instance = None

    if model_instance is None or not hasattr(model_instance, "_current_plan"):
        return "Error: could not locate the current agent model to store the plan"

    # Store plan on the model instance (per-agent)
    try:
        model_instance._current_plan = todos  # type: ignore[attr-defined]
    except Exception as e:  # pragma: no cover - defensive
        return f"Error updating plan: {e}"

    # Produce a compact confirmation with the todo list for visibility
    lines = [
        "Plan updated successfully. Keep using the todo list to track progress.",
        "",
        "<todo_list>",
    ]
    for idx, task in enumerate(todos, 1):
        status = task.get("status", "pending")
        content = task.get("content", "N/A")
        lines.append(f"{idx}. [{status}] {content}")
    lines.append("</todo_list>")
    return "\n".join(lines)


# Auto-register in ToolRegistry [E]
from cai.tool_registry import TOOL_REGISTRY  # noqa: E402
TOOL_REGISTRY.register("Todo_list", Todo_list, categories=["misc"])
