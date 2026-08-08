"""
Token counting, model info, and token display utilities for CAI.
"""

import os
from typing import Optional

from rich.text import Text


def get_model_name(model):
    """
    Extract a string model name from various model inputs.
    Centralizes model name standardization to avoid inconsistencies (e.g. avoid passing model object instead of string name).
    Args:
        model: String model name or model object

    Returns:
        str: Standardized model name string
    """
    if isinstance(model, str):
        return model
    # If not a string, use environment variable
    return os.environ.get("CAI_MODEL", "qwen2.5:72b")


def get_model_input_tokens(model):
    """
    Get the maximum input tokens for a given model (context window capacity).

    Preference order:
    1) pricings/pricing.json (user overrides)
    2) pricings/native_pricing.json (cached LiteLLM pricing)
    3) Fallback heuristic map by model family
    """
    try:
        import json
        import pathlib

        model_name = get_model_name(model)

        # 1) Prefer local custom pricing only in ./pricings/pricing.json
        custom_path = pathlib.Path("pricings") / "pricing.json"
        if custom_path.exists():
            with open(custom_path, encoding="utf-8") as f:
                pricing_data = json.load(f)
                model_info = pricing_data.get(model_name, {})
                if model_info and isinstance(model_info, dict):
                    tokens = model_info.get("max_input_tokens")
                    if isinstance(tokens, int) and tokens > 0:
                        return tokens

        # 2) Fallback to cached native LiteLLM pricing: ./pricings/native_pricing.json
        native_path = pathlib.Path("pricings") / "native_pricing.json"
        if native_path.exists():
            with open(native_path, encoding="utf-8") as f:
                native_data = json.load(f)
                model_info = native_data.get(model_name, {})
                if model_info and isinstance(model_info, dict):
                    tokens = model_info.get("max_input_tokens")
                    if isinstance(tokens, int) and tokens > 0:
                        return tokens
    except Exception:
        # Ignore pricing file errors and fall through to heuristic map
        pass

    # 3) Heuristic by model family as a last resort
    # Updated December 2025 based on LiteLLM pricing data
    # Order matters: more specific patterns should come first
    model_lower = str(model).lower()
    model_tokens_specific = {
        # OpenAI GPT-5.x series (newest, up to 400K context)
        "gpt-5.2": 400_000,
        "gpt-5.1": 272_000,
        "gpt-5-pro": 400_000,
        "gpt-5": 272_000,
        # OpenAI GPT-4.1 series (1M context!)
        "gpt-4.1": 1_047_576,
        # OpenAI O-series reasoning models (200K context)
        "o4-mini": 200_000,
        "o3-pro": 200_000,
        "o3-mini": 200_000,
        "o3": 200_000,
        "o1-pro": 200_000,
        "o1-mini": 128_000,
        "o1": 200_000,
        # OpenAI GPT-4o series (128K context)
        "gpt-4o": 128_000,
        "gpt-4-turbo": 128_000,
        "gpt-4-32k": 32_768,
        "gpt-4": 8_192,
        # Claude 4.x series (200K-1M context)
        "claude-sonnet-4-20250514": 1_000_000,  # Special: 1M context!
        "claude-sonnet-4": 1_000_000,
        "claude-opus-4.5": 200_000,
        "claude-opus-4-5": 200_000,
        "claude-opus-4.1": 200_000,
        "claude-opus-4-1": 200_000,
        "claude-opus-4": 200_000,
        "claude-haiku-4.5": 200_000,
        "claude-haiku-4-5": 200_000,
        "claude-sonnet-4.5": 200_000,
        "claude-sonnet-4-5": 200_000,
        # Claude 3.x series (200K context)
        "claude-3-7": 200_000,
        "claude-3.7": 200_000,
        "claude-3-5": 200_000,
        "claude-3.5": 200_000,
        "claude-3": 200_000,
        # Gemini 3.x series (1M context)
        "gemini-3": 1_048_576,
        # Gemini 2.5 series (1M-2M context)
        "gemini-2.5-pro": 1_048_576,
        "gemini-2.5": 1_048_576,
        # Gemini 2.0 series (1M context)
        "gemini-2.0": 1_048_576,
        "gemini-2": 1_048_576,
        # Gemini 1.5 series (1M-2M context)
        "gemini-1.5-pro": 2_097_152,
        "gemini-1.5": 1_048_576,
        # DeepSeek models (128K-164K context)
        "deepseek-v3.2": 163_840,
        "deepseek-v3": 128_000,
        "deepseek-r1": 128_000,
        "deepseek-chat": 131_072,
        "deepseek-reasoner": 131_072,
        # Qwen models
        "qwen3": 131_072,
        "qwen2.5": 131_072,
        "qwen": 32_000,
        # Llama models
        "llama3.3": 131_072,
        "llama3.2": 128_000,
        "llama3.1": 128_000,
        "llama3": 8_192,
        "llama": 4_096,
    }
    # Check specific patterns first (order-dependent matching)
    for pattern, tokens in model_tokens_specific.items():
        if pattern in model_lower:
            return tokens

    # Fallback for generic model families
    model_tokens_generic = {
        "gpt": 128_000,
        "o1": 200_000,
        "o3": 200_000,
        "o4": 200_000,
        "claude": 200_000,
        "gemini": 1_000_000,
        "deepseek": 128_000,
        "qwen": 32_000,
        "llama": 8_192,
        "mistral": 32_000,
        "mixtral": 32_000,
    }
    for model_type, tokens in model_tokens_generic.items():
        if model_type in model_lower:
            return tokens
    return 128_000  # Default fallback


def _create_token_display(
    interaction_input_tokens,
    interaction_output_tokens,
    interaction_reasoning_tokens,
    total_input_tokens,
    total_output_tokens,
    total_reasoning_tokens,
    model,
    interaction_cost=None,
    interaction_input_cost=None,
    interaction_output_cost=None,
    total_cost=None,
    total_input_cost=None,
    total_output_cost=None,
    # New cache params (read/creation separation)
    cache_read_tokens: Optional[int] = None,
    cache_creation_tokens: Optional[int] = None,
    cache_read_savings: Optional[float] = None,
    cache_creation_extra: Optional[float] = None,
    # Legacy params (backward compat)
    cached_tokens: Optional[int] = None,
    cached_cost: Optional[float] = None,
    cache_savings: Optional[float] = None,
) -> Text:
    # Lazy imports to avoid circular dependencies
    from cai.util.pricing import COST_TRACKER

    # Debug: Print cache metrics when CAI_SHOW_CACHE is enabled
    if os.getenv("CAI_SHOW_CACHE", "").lower() in ("true", "1", "yes"):
        print(f"[CACHE-DEBUG] _create_token_display received: CR={cache_read_tokens}, CW={cache_creation_tokens}")

    # Standardize model name
    model_name = get_model_name(model)

    # Use the provided costs directly if available, otherwise use the last tracked values
    # DO NOT process costs here - this function is called multiple times for display
    if interaction_cost is not None:
        current_cost = float(interaction_cost)
    else:
        # Use the last recorded interaction cost
        current_cost = COST_TRACKER.last_interaction_cost

    if total_cost is not None:
        total_cost_value = float(total_cost)
    else:
        # Use the last recorded total cost
        total_cost_value = COST_TRACKER.last_total_cost

    # Create display text with improved cost breakdown
    tokens_text = Text(justify="left")
    tokens_text.append("\n", style="bold")

    # Current interaction tokens with individual costs (include reasoning tokens explicitly)
    tokens_text.append("  ", style="cyan")
    tokens_text.append("Interaction: ", style="bold cyan")
    tokens_text.append(f"In: {interaction_input_tokens}", style="green")
    if interaction_input_cost and interaction_input_cost > 0:
        tokens_text.append(f" -> (${interaction_input_cost:.6f})", style="dim green")
    tokens_text.append(" ")

    tokens_text.append(f"Out: {interaction_output_tokens}", style="yellow")
    if interaction_output_cost and interaction_output_cost > 0:
        tokens_text.append(f" -> (${interaction_output_cost:.6f})", style="dim yellow")
    tokens_text.append(" ")

    # Always show reasoning tokens explicitly (even if zero, keep label consistent across panels)
    tokens_text.append(f"R: {interaction_reasoning_tokens}", style="magenta")
    if (
        current_cost > 0
        and (interaction_input_tokens + interaction_output_tokens + interaction_reasoning_tokens) > 0
        and interaction_reasoning_tokens > 0
    ):
        reasoning_cost = current_cost * (
            interaction_reasoning_tokens
            / (interaction_input_tokens + interaction_output_tokens + interaction_reasoning_tokens)
        )
        tokens_text.append(f" (${reasoning_cost:.5f})", style="dim magenta")

    # Show cache info (read = savings, creation = extra cost)
    # Use new params if available, fall back to legacy
    read_tokens = cache_read_tokens if cache_read_tokens else (cached_tokens or 0)
    creation_tokens = cache_creation_tokens or 0
    read_savings = cache_read_savings if cache_read_savings else (cache_savings or 0.0)
    creation_extra = cache_creation_extra or 0.0

    # CAI_SHOW_CACHE: Always show cache metrics for debugging
    show_cache_always = os.getenv("CAI_SHOW_CACHE", "").lower() in ("true", "1", "yes")

    # Show cache read tokens (savings) - green because it saves money
    if read_tokens and int(read_tokens) > 0:
        tokens_text.append(" ")
        tokens_text.append(f"CR: {int(read_tokens)}", style="green")
        if read_savings > 0:
            tokens_text.append(f" (-${read_savings:.4f})", style="bold green")
    elif show_cache_always:
        tokens_text.append(" ")
        tokens_text.append(f"CR: 0", style="dim green")

    # Show cache creation tokens (extra cost) - yellow because it costs more
    if creation_tokens and int(creation_tokens) > 0:
        tokens_text.append(" ")
        tokens_text.append(f"CW: {int(creation_tokens)}", style="yellow")
        if creation_extra > 0:
            tokens_text.append(f" (+${creation_extra:.4f})", style="dim yellow")
    elif show_cache_always:
        tokens_text.append(" ")
        tokens_text.append(f"CW: 0", style="dim yellow")

    interaction_total_tokens = (
        interaction_input_tokens + interaction_output_tokens + interaction_reasoning_tokens
    )
    tokens_text.append(
        f" | Total: {interaction_total_tokens} -> (${current_cost:.4f})",
        style="bold white",
    )

    # Session total
    session_total = getattr(COST_TRACKER, 'session_total_cost', total_cost_value)
    tokens_text.append("\n  ", style="blue")
    tokens_text.append("Session Total: ", style="bold blue")
    tokens_text.append(f"${session_total:.4f}", style="bold blue")
    tokens_text.append(" (all agents) ", style="dim")

    # Session total across all agents
    tokens_text.append("Session: ", style="bold magenta")
    tokens_text.append(f"${COST_TRACKER.session_total_cost:.4f}", style="bold magenta")

    # Context usage (show current interaction input vs model capacity)
    tokens_text.append(" | ", style="dim")
    context_pct = 0.0
    try:
        max_tokens = get_model_input_tokens(model_name)
        if max_tokens > 0:
            # May exceed 100% if provider token count > our capacity estimate (same as pricing footer).
            context_pct = (interaction_input_tokens / max_tokens) * 100.0
    except Exception:
        pass
    if context_pct > 80:
        indicator = "!!!"
        indicator_style = "bold red"
    elif context_pct > 50:
        indicator = "!!"
        indicator_style = "bold yellow"
    else:
        indicator = ""
        indicator_style = "green"
    tokens_text.append(f"Context: {context_pct:.1f}%", style=indicator_style)
    if indicator:
        tokens_text.append(f" {indicator}", style=indicator_style)

    return tokens_text


def _create_token_info_display(token_info=None):
    """Create a compact token info display for tool panels."""
    if not token_info:
        return None

    from rich.text import Text

    # Lazy import to avoid circular dependencies
    from cai.util.pricing import COST_TRACKER

    model = token_info.get("model", os.environ.get("CAI_MODEL", ""))
    interaction_input = token_info.get("interaction_input_tokens", 0)
    interaction_output = token_info.get("interaction_output_tokens", 0)
    interaction_reasoning = token_info.get("interaction_reasoning_tokens", 0)
    interaction_cost = token_info.get("interaction_cost", 0)

    # Skip display if no meaningful data
    if interaction_input == 0 and interaction_output == 0 and not interaction_cost:
        return None

    # Create token display text
    token_text = _create_token_display(
        interaction_input_tokens=interaction_input,
        interaction_output_tokens=interaction_output,
        interaction_reasoning_tokens=interaction_reasoning,
        total_input_tokens=token_info.get("total_input_tokens", 0),
        total_output_tokens=token_info.get("total_output_tokens", 0),
        total_reasoning_tokens=token_info.get("total_reasoning_tokens", 0),
        model=model,
        interaction_cost=interaction_cost,
        interaction_input_cost=token_info.get("interaction_input_cost"),
        interaction_output_cost=token_info.get("interaction_output_cost"),
        total_cost=token_info.get("total_cost"),
        total_input_cost=token_info.get("total_input_cost"),
        total_output_cost=token_info.get("total_output_cost"),
        cache_read_tokens=token_info.get("cache_read_tokens"),
        cache_creation_tokens=token_info.get("cache_creation_tokens"),
        cache_read_savings=token_info.get("cache_read_savings"),
        cache_creation_extra=token_info.get("cache_creation_extra"),
        cached_tokens=token_info.get("cached_tokens"),
        cached_cost=token_info.get("cached_cost"),
    )

    return token_text


def _get_timing_info(execution_info=None):
    """Extract and format timing information from execution_info."""
    if not execution_info:
        return None

    from cai.util.terminal import format_time

    tool_time = execution_info.get("tool_time")
    if tool_time:
        return format_time(tool_time)
    return None
