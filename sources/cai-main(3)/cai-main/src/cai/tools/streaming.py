"""Output streaming utilities for tool execution.

Provides helpers to check streaming state and gather agent token
information for display in streaming panels.
"""

import os

from cai.util import enrich_token_info_for_pricing


def _get_idle_timeout() -> int:
    """Get the idle timeout from CAI_IDLE_TIMEOUT env var, default 100 seconds."""
    try:
        return int(os.getenv("CAI_IDLE_TIMEOUT", "100"))
    except ValueError:
        return 100


def is_tool_streaming_enabled() -> bool:
    """
    Check if tool output streaming is enabled.

    CAI_TOOL_STREAM controls tool output streaming (default: true)
    CAI_STREAM is ONLY for LLM inference streaming - does NOT affect tools.

    Tools stream by default. Only CAI_TOOL_STREAM=false disables it.
    """
    tool_stream_env = os.getenv("CAI_TOOL_STREAM")
    if tool_stream_env is not None:
        return tool_stream_env.lower() != "false"
    return True  # Default: streaming enabled for tools


def _get_agent_token_info():
    """Get current agent's token information from the active model instance."""
    # Try to get agent info from the current execution context
    try:
        from cai.sdk.agents.models.openai_chatcompletions import get_current_active_model

        # First try to get the current active model (set during execution)
        model = get_current_active_model()

        if model:
            # Get display name with ID (e.g., "Red Team Agent [P1]")
            if hasattr(model, "get_full_display_name"):
                display_name = model.get_full_display_name()
            elif hasattr(model, "agent_name"):
                # Include [P1] only if we have a valid agent_id
                if hasattr(model, "agent_id") and model.agent_id:
                    display_name = f"{model.agent_name} [{model.agent_id}]"
                else:
                    # In single agent mode, just show the agent name without [P1]
                    display_name = model.agent_name
            else:
                display_name = "Agent"

            token_info = {
                "agent_name": display_name,  # This now includes the ID
                "agent_id": getattr(model, "agent_id", None),
                "interaction_counter": getattr(model, "interaction_counter", 0),
                "total_input_tokens": getattr(model, "total_input_tokens", 0),
                "total_output_tokens": getattr(model, "total_output_tokens", 0),
                "total_reasoning_tokens": getattr(model, "total_reasoning_tokens", 0),
                "total_cost": getattr(model, "total_cost", 0.0),
                "model": str(
                    getattr(
                        model,
                        "_current_request_model",
                        getattr(model, "model", os.environ.get("CAI_MODEL", "")),
                    )
                ),
            }

            # Add current interaction-level tokens from COST_TRACKER so tool panels reflect this iteration
            try:
                from cai.util import COST_TRACKER
                token_info["interaction_input_tokens"] = getattr(COST_TRACKER, "interaction_input_tokens", 0)
                token_info["interaction_output_tokens"] = getattr(COST_TRACKER, "interaction_output_tokens", 0)
                token_info["interaction_reasoning_tokens"] = getattr(COST_TRACKER, "interaction_reasoning_tokens", 0)
                token_info["interaction_cost"] = float(getattr(COST_TRACKER, "interaction_cost", 0.0))
                token_info["interaction_input_cost"] = float(getattr(COST_TRACKER, "interaction_input_cost", 0.0))
                token_info["interaction_output_cost"] = float(getattr(COST_TRACKER, "interaction_output_cost", 0.0))
            except Exception:
                pass

            # Add terminal_id from streaming context if available
            if hasattr(model, "_streaming_context") and model._streaming_context:
                streaming_ctx = model._streaming_context
                if isinstance(streaming_ctx, dict) and "terminal_id" in streaming_ctx:
                    token_info["terminal_id"] = streaming_ctx["terminal_id"]
                    # Try to extract terminal number from terminal_id
                    terminal_id = streaming_ctx["terminal_id"]
                    if terminal_id and terminal_id.startswith("terminal-") and terminal_id[9:].isdigit():
                        token_info["terminal_number"] = int(terminal_id[9:])

            # If no terminal_id from streaming context, try to get from current context
            if "terminal_id" not in token_info:
                try:
                    # Try async context first
                    from cai.tui.core.execution_context import get_terminal_id_context
                    terminal_id = get_terminal_id_context()
                    if terminal_id:
                        token_info["terminal_id"] = terminal_id
                        if terminal_id.startswith("terminal-") and terminal_id[9:].isdigit():
                            token_info["terminal_number"] = int(terminal_id[9:])
                except ImportError:
                    pass

                # Try thread-local context as fallback
                if "terminal_id" not in token_info:
                    try:
                        from cai.tui.core.terminal_tracking import get_current_terminal_id
                        terminal_id = get_current_terminal_id()
                        if terminal_id:
                            token_info["terminal_id"] = terminal_id
                            if terminal_id.startswith("terminal-") and terminal_id[9:].isdigit():
                                token_info["terminal_number"] = int(terminal_id[9:])
                    except ImportError:
                        pass

            return enrich_token_info_for_pricing(token_info)

        # Fallback: Try to get from the most recent instance in the registry
        from cai.sdk.agents.models.openai_chatcompletions import ACTIVE_MODEL_INSTANCES

        if ACTIVE_MODEL_INSTANCES:
            # Get the most recent instance (highest instance ID)
            latest_key = max(ACTIVE_MODEL_INSTANCES.keys(), key=lambda x: x[1])
            model_ref = ACTIVE_MODEL_INSTANCES[latest_key]
            model = model_ref() if model_ref else None

            if model:
                # Get display name with ID
                if hasattr(model, "get_full_display_name"):
                    display_name = model.get_full_display_name()
                elif hasattr(model, "agent_name"):
                    # Include [P1] only if we have a valid agent_id
                    if hasattr(model, "agent_id") and model.agent_id:
                        display_name = f"{model.agent_name} [{model.agent_id}]"
                    else:
                        # In single agent mode, just show the agent name without [P1]
                        display_name = model.agent_name
                else:
                    display_name = "Agent"

                token_info = {
                    "agent_name": display_name,  # This now includes the ID
                    "agent_id": getattr(model, "agent_id", None),
                    "interaction_counter": getattr(model, "interaction_counter", 0),
                    "total_input_tokens": getattr(model, "total_input_tokens", 0),
                    "total_output_tokens": getattr(model, "total_output_tokens", 0),
                    "total_reasoning_tokens": getattr(model, "total_reasoning_tokens", 0),
                    "total_cost": getattr(model, "total_cost", 0.0),
                    "model": str(
                        getattr(
                            model,
                            "_current_request_model",
                            getattr(model, "model", os.environ.get("CAI_MODEL", "")),
                        )
                    ),
                }

                # Add current interaction-level tokens from COST_TRACKER in fallback path
                try:
                    from cai.util import COST_TRACKER
                    token_info["interaction_input_tokens"] = getattr(COST_TRACKER, "interaction_input_tokens", 0)
                    token_info["interaction_output_tokens"] = getattr(COST_TRACKER, "interaction_output_tokens", 0)
                    token_info["interaction_reasoning_tokens"] = getattr(COST_TRACKER, "interaction_reasoning_tokens", 0)
                    token_info["interaction_cost"] = float(getattr(COST_TRACKER, "interaction_cost", 0.0))
                    token_info["interaction_input_cost"] = float(getattr(COST_TRACKER, "interaction_input_cost", 0.0))
                    token_info["interaction_output_cost"] = float(getattr(COST_TRACKER, "interaction_output_cost", 0.0))
                except Exception:
                    pass

                # Add terminal_id from streaming context if available
                if hasattr(model, "_streaming_context") and model._streaming_context:
                    streaming_ctx = model._streaming_context
                    if isinstance(streaming_ctx, dict) and "terminal_id" in streaming_ctx:
                        token_info["terminal_id"] = streaming_ctx["terminal_id"]
                        # Try to extract terminal number from terminal_id
                        terminal_id = streaming_ctx["terminal_id"]
                        if terminal_id and terminal_id.startswith("terminal-") and terminal_id[9:].isdigit():
                            token_info["terminal_number"] = int(terminal_id[9:])

                # If no terminal_id from streaming context, try to get from current context
                if "terminal_id" not in token_info:
                    try:
                        # Try async context first
                        from cai.tui.core.execution_context import get_terminal_id_context
                        terminal_id = get_terminal_id_context()
                        if terminal_id:
                            token_info["terminal_id"] = terminal_id
                            if terminal_id.startswith("terminal-") and terminal_id[9:].isdigit():
                                token_info["terminal_number"] = int(terminal_id[9:])
                    except ImportError:
                        pass

                    # Try thread-local context as fallback
                    if "terminal_id" not in token_info:
                        try:
                            from cai.tui.core.terminal_tracking import get_current_terminal_id
                            terminal_id = get_current_terminal_id()
                            if terminal_id:
                                token_info["terminal_id"] = terminal_id
                                if terminal_id.startswith("terminal-") and terminal_id[9:].isdigit():
                                    token_info["terminal_number"] = int(terminal_id[9:])
                        except ImportError:
                            pass

                return enrich_token_info_for_pricing(token_info)
    except Exception:
        pass

    # Return default values if we can't get agent info
    return enrich_token_info_for_pricing(
        {
            "agent_name": "Agent",
            "agent_id": None,
            "interaction_counter": 0,
            "interaction_input_tokens": 0,
            "interaction_output_tokens": 0,
            "interaction_reasoning_tokens": 0,
            "total_input_tokens": 0,
            "total_output_tokens": 0,
            "total_reasoning_tokens": 0,
            "total_cost": 0.0,
            "model": os.environ.get("CAI_MODEL", ""),
        }
    )
