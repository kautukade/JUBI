"""
CAI utility package.

This package provides backwards-compatible re-exports of all public names
that were previously available from the monolithic ``cai.util`` module.
Existing code such as::

    from cai.util import COST_TRACKER, cli_print_tool_output, load_prompt_template

continues to work without modification.
"""

# ---------------------------------------------------------------------------
# timing.py -- active/idle timers
# ---------------------------------------------------------------------------
from cai.util.timing import (
    START_TIME,
    start_active_timer,
    stop_active_timer,
    start_idle_timer,
    stop_idle_timer,
    get_active_time,
    get_idle_time,
    get_active_time_seconds,
    get_idle_time_seconds,
)

# ---------------------------------------------------------------------------
# tokens.py -- model name helpers, token counts, token display
# ---------------------------------------------------------------------------
from cai.util.tokens import (
    get_model_input_tokens,
    get_model_name,
    _create_token_display,
)


# ---------------------------------------------------------------------------
# config_utils.py -- directories, ollama, litellm patches, CTF setup
# ---------------------------------------------------------------------------
from cai.util.config_utils import (
    get_config_dir,
    get_session_logs_dir,
    get_pricings_dir,
    _seed_pricings_dir,
    get_ollama_api_base,
    get_ollama_auth_headers,
    ensure_litellm_transcription_support,
    ensure_litellm_logging_worker_loop_safety,
    visualize_agent_graph,
    setup_ctf,
    update_agent_models_recursively,
)

# ---------------------------------------------------------------------------
# pricing.py -- CostTracker, pricing lookups, cost calculation
# ---------------------------------------------------------------------------
from cai.util.pricing import (
    CostTracker,
    COST_TRACKER,
    _AGENT_PRICING_CACHE,
    _build_agent_pricing_key,
    enrich_token_info_for_pricing,
    calculate_model_cost,
    calculate_cached_token_costs,
    calculate_cached_token_savings,
    get_model_pricing,
    _pricing_debug_log,
    _pricing_debug_new_interaction,
    _close_pricing_debug,
    _save_native_pricing_cache,
    _load_native_pricing_cache,
    _fetch_remote_pricing_sync,
    _prefetch_remote_pricing_worker,
    _ensure_pricing_prefetch_started,
    _get_prefetched_pricing_for_model,
    _pricing_tuple_from_mapping,
    set_pending_cache_info,
    get_and_clear_pending_cache_info,
    is_tool_streaming_enabled,
)

# ---------------------------------------------------------------------------
# interaction.py -- interaction counter, limit enforcement, signal handler
# ---------------------------------------------------------------------------
from cai.util.interaction import (
    reset_interaction_counter,
    increment_interaction_counter,
    get_interaction_counter,
    MaxInteractionsExceeded,
    check_interaction_limit,
    signal_handler,
    _interrupt_count,
    _last_interrupt_time,
)

# ---------------------------------------------------------------------------
# prompts.py -- template loading, system prompt rendering, memory
# ---------------------------------------------------------------------------
from cai.util.prompts import (
    load_prompt_template,
    create_system_prompt_renderer,
    render_system_prompt,
    append_instructions,
    wrapped_instructions,
    apply_compacted_memory_to_agent,
)

# ---------------------------------------------------------------------------
# terminal.py -- formatting, Rich console/theme, message parsing
# ---------------------------------------------------------------------------
from cai.util.terminal import (
    color,
    theme,
    console,
    format_time,
    _sanitize_output_for_display,
    _format_tool_args,
    _print_simple_tool_output,
    _create_tool_panel_content,
    _create_token_info_display,
    _get_timing_info,
    parse_message_content,
    parse_message_tool_call,
    is_tool_output_message,
    print_message_history,
    get_language_from_code_block,
    check_flag,
    sanitize_message_list,
    fix_message_list,
)

# ---------------------------------------------------------------------------
# streaming.py -- live panels, tool streaming, agent streaming, thinking
# ---------------------------------------------------------------------------
from cai.util.streaming import (
    _LIVE_STREAMING_PANELS,
    _PANEL_UPDATE_LOCK,
    _GROUPED_STREAMING_TOOLS,
    _GROUPED_TOOLS_LOCK,
    close_all_streaming_panels,
    _finalize_live_panel,
    _find_or_create_tool_group,
    _build_grouped_panel_content,
    _update_tool_group,
    _finalize_tool_group,
    _get_group_for_call_id,
    _check_and_finalize_group,
    cli_print_tool_output,
    cli_print_tool_call,
    cli_print_agent_messages,
    create_agent_streaming_context,
    update_agent_streaming_content,
    finish_agent_streaming,
    start_tool_streaming,
    update_tool_streaming,
    finish_tool_streaming,
    cleanup_all_streaming_resources,
    cleanup_agent_streaming_resources,
    _force_stop_all_panels,
    _PARALLEL_EXECUTION_STATE,
    _CLAUDE_THINKING_PANELS,
    create_claude_thinking_context,
    update_claude_thinking_content,
    finish_claude_thinking_display,
    detect_claude_thinking_in_stream,
    print_claude_reasoning_simple,
    start_claude_thinking_if_applicable,
    _cleanup_in_progress,
)

# ---------------------------------------------------------------------------
# Ensure all module-level side-effects are triggered
# (signal handler registration, atexit, idle timer start, etc.)
# These happen on import of the respective submodules above.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Lazy re-exports for names that cause circular imports at module level [A]
# Using __getattr__ to avoid triggering the chatcompletions package __init__
# (which would re-import cai.util → circular).  We load token_counter.py
# directly from disk via importlib.util so the package init is NOT executed.
# ---------------------------------------------------------------------------
_LAZY_REEXPORTS = {
    "count_tokens_with_tiktoken",
    "_check_reasoning_compatibility",
}

_token_counter_mod = None          # cached lazily


def _load_token_counter():
    """Load token_counter.py directly, skipping chatcompletions/__init__.py."""
    global _token_counter_mod
    if _token_counter_mod is not None:
        return _token_counter_mod
    import importlib.util, pathlib
    _here = pathlib.Path(__file__).resolve().parent        # cai/util/
    _file = _here.parent / "sdk" / "agents" / "models" / "chatcompletions" / "token_counter.py"
    spec = importlib.util.spec_from_file_location(
        "cai.sdk.agents.models.chatcompletions.token_counter", str(_file)
    )
    _token_counter_mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(_token_counter_mod)
    return _token_counter_mod


def __getattr__(name: str):
    if name in _LAZY_REEXPORTS:
        mod = _load_token_counter()
        val = getattr(mod, name)
        globals()[name] = val          # cache for fast subsequent access
        return val
    raise AttributeError(f"module 'cai.util' has no attribute {name!r}")
