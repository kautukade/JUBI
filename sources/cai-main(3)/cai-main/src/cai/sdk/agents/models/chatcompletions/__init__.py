"""chatcompletions package -- refactored from openai_chatcompletions.py.

Re-exports all public names so that existing imports like
``from cai.sdk.agents.models.openai_chatcompletions import OpenAIChatCompletionsModel``
continue to work via the compatibility shim at the old path.
"""

# Core model class (still lives in the original file during this phase)
# We re-export the submodule utilities for direct consumption.
from .token_counter import count_tokens_with_tiktoken, _check_reasoning_compatibility
from .usage_tracker import InputTokensDetails, CustomResponseUsage
from .cache_manager import (
    normalize_and_apply_cache,
    normalize_messages_for_cache,
    apply_cache_control,
    has_cache_control,
    debug_cache_messages,
)
from .stream_handler import StreamingState
from .message_builder import Converter, ToolConverter
from .auto_compactor import auto_compact_if_needed, get_model_max_tokens
from .httpx_client import direct_httpx_completion
from .litellm_adapter import fetch_response_litellm_openai, fetch_response_litellm_ollama

# model.py re-exports module-level helpers and the main class will
# be imported from the original file until full migration completes.
from .model import (
    ACTIVE_MODEL_INSTANCES,
    PERSISTENT_MESSAGE_HISTORIES,
    set_current_active_model,
    get_current_active_model,
    get_agent_message_history,
    get_all_agent_histories,
    clear_agent_history,
    clear_all_histories,
    _StreamingState,
)

__all__ = [
    # Token counting
    "count_tokens_with_tiktoken",
    "_check_reasoning_compatibility",
    # Usage tracking
    "InputTokensDetails",
    "CustomResponseUsage",
    # Cache management
    "normalize_and_apply_cache",
    "normalize_messages_for_cache",
    "apply_cache_control",
    "has_cache_control",
    "debug_cache_messages",
    # Streaming
    "StreamingState",
    "_StreamingState",
    # Message building
    "Converter",
    "ToolConverter",
    # Module-level helpers
    "ACTIVE_MODEL_INSTANCES",
    "PERSISTENT_MESSAGE_HISTORIES",
    "set_current_active_model",
    "get_current_active_model",
    "get_agent_message_history",
    "get_all_agent_histories",
    "clear_agent_history",
    "clear_all_histories",
    # Auto-compaction
    "auto_compact_if_needed",
    "get_model_max_tokens",
    # Direct httpx client (LiteLLM bypass)
    "direct_httpx_completion",
    # LiteLLM adapters
    "fetch_response_litellm_openai",
    "fetch_response_litellm_ollama",
]
