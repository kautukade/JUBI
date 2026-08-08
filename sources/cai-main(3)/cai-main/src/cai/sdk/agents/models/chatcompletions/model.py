"""Core OpenAIChatCompletionsModel class.

This module contains the main model class that orchestrates LLM interactions.
It delegates to submodules for token counting, message building, caching,
streaming state, and usage tracking.

NOTE: The class itself remains large because its methods (get_response,
stream_response, _fetch_response) are deeply intertwined with streaming state,
caching, cost tracking, and CLI output.  The extraction of *reusable*
utilities into sibling modules still yields significant improvements in
navigability and testability.
"""

# Re-export everything the old monolith exposed so that
# ``from cai.sdk.agents.models.chatcompletions.model import X`` works
# for any X that used to live in openai_chatcompletions.py.

# --- Standard library -------------------------------------------------
from __future__ import annotations

import asyncio
import contextvars
import dataclasses
import hashlib
import inspect
import json
import os
import re
import sys
import time
import uuid
import weakref
from collections.abc import AsyncIterator, Iterable
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, Literal, Optional, cast, overload

# --- Third-party ------------------------------------------------------
import litellm
import tiktoken
from openai import NOT_GIVEN, AsyncOpenAI, AsyncStream, NotGiven
from openai._models import BaseModel
from openai.types import ChatModel
from openai.types.chat import (
    ChatCompletion,
    ChatCompletionAssistantMessageParam,
    ChatCompletionChunk,
    ChatCompletionContentPartImageParam,
    ChatCompletionContentPartParam,
    ChatCompletionContentPartTextParam,
    ChatCompletionDeveloperMessageParam,
    ChatCompletionMessage,
    ChatCompletionMessageParam,
    ChatCompletionMessageToolCallParam,
    ChatCompletionSystemMessageParam,
    ChatCompletionToolChoiceOptionParam,
    ChatCompletionToolMessageParam,
    ChatCompletionUserMessageParam,
)
from openai.types.chat.chat_completion_tool_param import ChatCompletionToolParam
from openai.types.chat.completion_create_params import ResponseFormat
from openai.types.completion_usage import CompletionUsage
from openai.types.responses import (
    EasyInputMessageParam,
    Response,
    ResponseCompletedEvent,
    ResponseContentPartAddedEvent,
    ResponseContentPartDoneEvent,
    ResponseCreatedEvent,
    ResponseFileSearchToolCallParam,
    ResponseFunctionCallArgumentsDeltaEvent,
    ResponseFunctionToolCall,
    ResponseFunctionToolCallParam,
    ResponseInputContentParam,
    ResponseInputImageParam,
    ResponseInputTextParam,
    ResponseOutputItem,
    ResponseOutputItemAddedEvent,
    ResponseOutputItemDoneEvent,
    ResponseOutputMessage,
    ResponseOutputMessageParam,
    ResponseOutputRefusal,
    ResponseOutputText,
    ResponseRefusalDeltaEvent,
    ResponseTextDeltaEvent,
    ResponseUsage,
)
from openai.types.responses.response_input_param import FunctionCallOutput, ItemReference, Message
from openai.types.responses.response_usage import OutputTokensDetails
from wasabi import color

# --- CAI internal imports ---------------------------------------------
from cai.sdk.agents.simple_agent_manager import SimpleAgentManager, AGENT_MANAGER
from cai.sdk.agents.parallel_isolation import PARALLEL_ISOLATION
from cai.sdk.agents.run_to_jsonl import get_session_recorder
from cai.sdk.agents.global_usage_tracker import GLOBAL_USAGE_TRACKER
from cai.util import (
    _LIVE_STREAMING_PANELS,
    COST_TRACKER,
    calculate_model_cost,
    cli_print_agent_messages,
    cli_print_tool_output,
    create_agent_streaming_context,
    finish_agent_streaming,
    get_ollama_api_base,
    start_active_timer,
    start_claude_thinking_if_applicable,
    start_idle_timer,
    stop_active_timer,
    stop_idle_timer,
    update_agent_streaming_content,
)
from cai.internal.components.metrics import process_intermediate_logs

# --- SDK relative imports ---------------------------------------------
from ... import _debug
from ...agent_output import AgentOutputSchema
from ...exceptions import AgentsException, UserError
from ...handoffs import Handoff
from ...items import ModelResponse, TResponseInputItem, TResponseOutputItem, TResponseStreamEvent
from ...logger import logger
from ...tool import FunctionTool, Tool
from ...tracing import generation_span
from ...tracing.span_data import GenerationSpanData
from ...tracing.spans import Span
from ...usage import Usage
from ...version import __version__
from ..fake_id import FAKE_RESPONSES_ID
from ..interface import Model, ModelTracing

# --- Submodule imports (the refactored pieces) -----------------------
from .token_counter import count_tokens_with_tiktoken, _check_reasoning_compatibility
from .usage_tracker import InputTokensDetails, CustomResponseUsage
from .cache_manager import (
    normalize_and_apply_cache,
    normalize_messages_for_cache,
    apply_cache_control,
    has_cache_control as _has_cache_control,
    debug_cache_messages,
)
from .stream_handler import StreamingState
from .message_builder import Converter as _Converter, ToolConverter

if TYPE_CHECKING:
    from ...model_settings import ModelSettings

# --- Module-level setup -----------------------------------------------

# Suppress debug info from litellm
litellm.suppress_debug_info = True

if os.getenv("CAI_MODEL") == "o3-mini" or os.getenv("CAI_MODEL") == "gemini-1.5-pro":
    litellm.drop_params = True

_USER_AGENT = f"Agents/Python {__version__}"
_HEADERS = {"User-Agent": _USER_AGENT}

# Global registry to track active model instances
# DEPRECATED: Use AGENT_REGISTRY instead
ACTIVE_MODEL_INSTANCES = {}

# Persistent message history store for agents without active instances
PERSISTENT_MESSAGE_HISTORIES = {}

# Debug: Store previous turn's message hashes for cache comparison
_PREVIOUS_TURN_MSG_HASHES = []

# Flag: auto-compaction in progress (blocks nested compact). Cleared in auto_compactor
# try/finally; if a hard crash/kill happens mid-compact, restart the CLI to reset.
_compaction_in_progress = False

# Context variable to track the current active model per async context
_current_model_context = contextvars.ContextVar('current_model', default=None)


def set_current_active_model(model):
    """Set the current active model for tool execution context."""
    _current_model_context.set(weakref.ref(model) if model else None)


def get_current_active_model():
    """Get the current active model."""
    model_ref = _current_model_context.get()
    if model_ref:
        return model_ref()
    return None


def get_agent_message_history(agent_name: str) -> list:
    """Get message history for a specific agent."""
    if "[" in agent_name and agent_name.endswith("]"):
        base_name = agent_name.rsplit("[", 1)[0].strip()
    else:
        base_name = agent_name
    return AGENT_MANAGER.get_message_history(base_name)


def get_all_agent_histories() -> dict:
    """Get all agent message histories."""
    return AGENT_MANAGER.get_all_histories()


def clear_agent_history(agent_name: str):
    """Clear history for a specific agent."""
    if "[" in agent_name and agent_name.endswith("]"):
        base_name = agent_name.rsplit("[", 1)[0].strip()
    else:
        base_name = agent_name
    AGENT_MANAGER.clear_history(base_name)
    active_agent = AGENT_MANAGER.get_active_agent()
    if active_agent and hasattr(active_agent, 'message_history'):
        if hasattr(active_agent, 'agent_name') and active_agent.agent_name == base_name:
            active_agent.message_history.clear()
            os.environ['CAI_CONTEXT_USAGE'] = '0.0'


def clear_all_histories():
    """Clear all agent histories."""
    AGENT_MANAGER.clear_all_histories()
    active_agent = AGENT_MANAGER.get_active_agent()
    if active_agent and hasattr(active_agent, 'message_history'):
        active_agent.message_history.clear()
    PERSISTENT_MESSAGE_HISTORIES.clear()
    os.environ['CAI_CONTEXT_USAGE'] = '0.0'


# Keep _StreamingState as an alias for backward compatibility
_StreamingState = StreamingState
