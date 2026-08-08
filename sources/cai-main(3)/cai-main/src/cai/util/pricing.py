"""
Cost tracking, pricing lookup, and financial utilities for CAI.
"""

import atexit
import json
import logging
import os
import pathlib
import sys
import threading
import time
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional

from cai.util.tokens import get_model_name, get_model_input_tokens
from cai.util.config_utils import get_pricings_dir, _seed_pricings_dir
from cai.util.interaction import get_interaction_counter
from cai.util.session import is_parallel_session

# ============== PRICING DEBUG LOGGER ==============
# Set CAI_DEBUG_PRICING=1 to enable runtime pricing debug logs
_PRICING_DEBUG_FILE = None
_PRICING_DEBUG_LOCK = threading.Lock()
_PRICING_DEBUG_INTERACTION = 0

# ============== PENDING CACHE INFO ==============
# Cache info that hasn't been displayed yet (for tool-only responses)
_PENDING_CACHE_INFO = None
_PENDING_CACHE_LOCK = threading.Lock()


def set_pending_cache_info(cache_info: Optional[Dict] = None):
    """Store cache info to be displayed with the next tool output."""
    global _PENDING_CACHE_INFO
    with _PENDING_CACHE_LOCK:
        _PENDING_CACHE_INFO = cache_info


def get_and_clear_pending_cache_info() -> Optional[Dict]:
    """Get pending cache info and clear it (one-time display)."""
    global _PENDING_CACHE_INFO
    with _PENDING_CACHE_LOCK:
        info = _PENDING_CACHE_INFO
        _PENDING_CACHE_INFO = None
        return info


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


def _pricing_debug_log(step: str, **kwargs):
    """Log pricing debug information to debug_pricing.txt in real-time."""
    if os.getenv("CAI_DEBUG_PRICING", "0") != "1":
        return

    global _PRICING_DEBUG_FILE, _PRICING_DEBUG_INTERACTION
    with _PRICING_DEBUG_LOCK:
        try:
            if _PRICING_DEBUG_FILE is None:
                debug_path = Path.cwd() / "debug_pricing.txt"
                _PRICING_DEBUG_FILE = open(debug_path, "a", encoding="utf-8")
                _PRICING_DEBUG_FILE.write(f"\n{'='*80}\n")
                _PRICING_DEBUG_FILE.write(f"CAI PRICING DEBUG SESSION - {datetime.now().isoformat()}\n")
                _PRICING_DEBUG_FILE.write(f"{'='*80}\n\n")

            timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
            _PRICING_DEBUG_FILE.write(f"[{timestamp}] [INT#{_PRICING_DEBUG_INTERACTION}] {step}\n")
            for key, value in kwargs.items():
                _PRICING_DEBUG_FILE.write(f"    {key}: {value}\n")
            _PRICING_DEBUG_FILE.write("\n")
            _PRICING_DEBUG_FILE.flush()
        except Exception as e:
            print(f"[PRICING DEBUG ERROR] {e}", file=sys.stderr)

def _pricing_debug_new_interaction():
    """Increment the interaction counter for debug logging."""
    global _PRICING_DEBUG_INTERACTION
    with _PRICING_DEBUG_LOCK:
        _PRICING_DEBUG_INTERACTION += 1
        return _PRICING_DEBUG_INTERACTION

def _close_pricing_debug():
    """Close the pricing debug file on exit."""
    global _PRICING_DEBUG_FILE
    if _PRICING_DEBUG_FILE:
        try:
            _PRICING_DEBUG_FILE.write(f"\n{'='*80}\n")
            _PRICING_DEBUG_FILE.write(f"SESSION ENDED - {datetime.now().isoformat()}\n")
            _PRICING_DEBUG_FILE.write(f"{'='*80}\n")
            _PRICING_DEBUG_FILE.close()
        except:
            pass
        _PRICING_DEBUG_FILE = None

atexit.register(_close_pricing_debug)
def _save_native_pricing_cache(data: dict) -> None:
    """Persist the upstream pricing JSON as ./pricings/native_pricing.json"""
    try:
        pricings_dir = get_pricings_dir()
        target = pricings_dir / "native_pricing.json"
        with open(target, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
    except Exception:
        # Best-effort cache; never raise
        pass


def _load_native_pricing_cache() -> Optional[dict]:
    """Load cached upstream pricing from ./pricings/native_pricing.json if present."""
    try:
        pricings_dir = get_pricings_dir()
        source = pricings_dir / "native_pricing.json"
        if source.exists():
            with open(source, encoding="utf-8") as f:
                return json.load(f)
    except Exception:
        pass
    return None


_PRICING_PREFETCH_DATA: Optional[dict] = None
_PRICING_PREFETCH_ERROR: Optional[str] = None
_PRICING_PREFETCH_EVENT = threading.Event()
_PRICING_PREFETCH_LOCK = threading.Lock()
_PRICING_PREFETCH_THREAD: Optional[threading.Thread] = None


def _fetch_remote_pricing_sync() -> Optional[dict]:
    """Fetch pricing data synchronously from the LiteLLM source."""

    LITELLM_URL = (
        "https://raw.githubusercontent.com/BerriAI/litellm/main/"
        "model_prices_and_context_window.json"
    )

    try:
        import requests

        response = requests.get(LITELLM_URL, timeout=5)
        if response.status_code == 200:
            data = response.json()
            _save_native_pricing_cache(data)
            return data
    except Exception:
        pass
    return None


def _prefetch_remote_pricing_worker() -> None:
    """Background worker that prefetches remote pricing without blocking startup."""

    global _PRICING_PREFETCH_DATA, _PRICING_PREFETCH_ERROR

    try:
        data = _fetch_remote_pricing_sync()
        if not data:
            cached = _load_native_pricing_cache()
            if isinstance(cached, dict):
                data = cached

        if data:
            _PRICING_PREFETCH_DATA = data
            _PRICING_PREFETCH_ERROR = None
        else:
            _PRICING_PREFETCH_ERROR = "pricing data unavailable"
    except Exception as exc:  # pragma: no cover - defensive guard
        _PRICING_PREFETCH_ERROR = str(exc)
    finally:
        _PRICING_PREFETCH_EVENT.set()


def _ensure_pricing_prefetch_started() -> None:
    """Ensure the background pricing fetcher is running (non-blocking)."""

    global _PRICING_PREFETCH_THREAD

    # Only prefetch if explicitly enabled
    if os.getenv("CAI_ENABLE_PRICING_FETCH", "0").lower() not in ("1", "true", "yes"):
        return

    if _PRICING_PREFETCH_EVENT.is_set():
        return

    with _PRICING_PREFETCH_LOCK:
        if _PRICING_PREFETCH_THREAD and _PRICING_PREFETCH_THREAD.is_alive():
            return

        thread = threading.Thread(
            target=_prefetch_remote_pricing_worker,
            name="cai-pricing-prefetch",
            daemon=True,
        )
        thread.start()
        _PRICING_PREFETCH_THREAD = thread


def _get_prefetched_pricing_for_model(
    model_name: str,
    *,
    wait: bool = False,
    timeout: float = 0.0,
) -> Optional[tuple]:
    """Return prefetched pricing for a model when available."""

    if wait:
        _PRICING_PREFETCH_EVENT.wait(timeout)
    elif not _PRICING_PREFETCH_EVENT.is_set():
        return None

    if not _PRICING_PREFETCH_EVENT.is_set():
        return None

    data = _PRICING_PREFETCH_DATA
    if not isinstance(data, dict):
        return None

    pricing_info = data.get(model_name)
    if not isinstance(pricing_info, dict):
        return None

    input_cost_per_token = pricing_info.get("input_cost_per_token", 0)
    output_cost_per_token = pricing_info.get("output_cost_per_token", 0)
    return input_cost_per_token, output_cost_per_token


def _pricing_tuple_from_mapping(mapping: Any, model_name: str) -> Optional[tuple]:
    """Extract pricing tuple from a mapping, returning None when not present.

    Supports partial name matching: if an exact match is not found, the function
    will try to find a key that contains the model_name or vice versa. This allows
    users to use model name variations (e.g., "claude-sonnet-4" matches
    "claude-sonnet-4-20250514").

    Updated December 2025 to support flexible model name matching.
    """

    if not isinstance(mapping, dict):
        return None

    # 1. Try exact match first (fastest)
    pricing_info = mapping.get(model_name)
    if isinstance(pricing_info, dict):
        input_cost_per_token = pricing_info.get("input_cost_per_token", 0)
        output_cost_per_token = pricing_info.get("output_cost_per_token", 0)
        return input_cost_per_token, output_cost_per_token

    # 2. Try partial matching (model_name is contained in a key)
    # This handles cases like "gpt-4o" matching "gpt-4o-2024-11-20"
    model_lower = model_name.lower()
    best_match = None
    best_match_len = 0

    for key in mapping.keys():
        key_lower = key.lower()
        # Check if user's model name is contained in the key
        if model_lower in key_lower:
            # Prefer shorter keys (more specific match)
            # e.g., "claude-sonnet-4" should match "claude-sonnet-4-20250514"
            # over "openrouter/anthropic/claude-sonnet-4"
            if best_match is None or len(key) < best_match_len:
                pricing_info = mapping.get(key)
                if isinstance(pricing_info, dict):
                    best_match = key
                    best_match_len = len(key)
        # Also check if key is contained in model name
        # e.g., user types "anthropic/claude-sonnet-4" and key is "claude-sonnet-4"
        elif key_lower in model_lower:
            if best_match is None or len(key) > best_match_len:
                pricing_info = mapping.get(key)
                if isinstance(pricing_info, dict):
                    best_match = key
                    best_match_len = len(key)

    if best_match:
        pricing_info = mapping.get(best_match)
        if isinstance(pricing_info, dict):
            input_cost_per_token = pricing_info.get("input_cost_per_token", 0)
            output_cost_per_token = pricing_info.get("output_cost_per_token", 0)
            return input_cost_per_token, output_cost_per_token

    return None

# Shared stats tracking object to maintain consistent costs across calls
class CostTracker:
    # Session-level stats
    session_total_cost: float = 0.0

    # Current agent stats
    current_agent_total_cost: float = 0.0
    current_agent_input_tokens: int = 0
    current_agent_output_tokens: int = 0
    current_agent_reasoning_tokens: int = 0

    # Current interaction stats
    interaction_input_tokens: int = 0
    interaction_output_tokens: int = 0
    interaction_reasoning_tokens: int = 0
    interaction_cost: float = 0.0

    # Cache token stats (for Anthropic prompt caching)
    cache_read_tokens: int = 0
    cache_creation_tokens: int = 0

    # Calculation cache
    model_pricing_cache: Dict[str, tuple]
    calculated_costs_cache: Dict[str, float]

    # Aggregated visualisation caches
    agent_costs: Dict[str, float]
    terminal_costs: Dict[str, float]
    agent_cost_states: Dict[str, Dict[str, Any]]

    # Track the last calculation to debug inconsistencies
    last_interaction_cost: float = 0.0
    last_total_cost: float = 0.0
    # Internal flags
    pricing_fetch_warned: bool = False

    def __init__(self) -> None:
        self.session_total_cost = 0.0

        self.current_agent_total_cost = 0.0
        self.current_agent_input_tokens = 0
        self.current_agent_output_tokens = 0
        self.current_agent_reasoning_tokens = 0

        self.interaction_input_tokens = 0
        self.interaction_output_tokens = 0
        self.interaction_reasoning_tokens = 0
        self.interaction_cost = 0.0

        # Cache token tracking for Anthropic prompt caching
        self.cache_read_tokens = 0
        self.cache_creation_tokens = 0

        self.model_pricing_cache = {}
        self.calculated_costs_cache = {}

        self.agent_costs = {}
        self.terminal_costs = {}
        self.agent_cost_states = {}

        self.last_interaction_cost = 0.0
        self.last_total_cost = 0.0

        self.pricing_fetch_warned = False

        _ensure_pricing_prefetch_started()

    def _warn_unattributed(self, message: str, **context: Any) -> None:
        """Emit a warning about unattributed cost and persist stack trace.

        Note: Warnings are disabled by default. Set CAI_WARN_UNATTRIBUTED=1 to enable.
        """
        # Warnings disabled - return early
        if not os.environ.get("CAI_WARN_UNATTRIBUTED"):
            return

        logger = logging.getLogger("CostTracker")
        try:
            import traceback

            stack = "\n".join(traceback.format_stack(limit=12))
        except Exception:
            stack = "<unable to capture stack>"

        if logger.hasHandlers():
            logger.warning(message, extra=context)
            logger.warning("Call stack for unattributed cost:\n%s", stack)
        else:
            print(f"[CostTracker] WARNING: {message} | context={context}", flush=True)
            print(f"[CostTracker] Call stack for unattributed cost:\n{stack}", flush=True)

        # Always append to a dedicated log for offline inspection
        try:
            from pathlib import Path

            log_path = Path(os.environ.get("CAI_UNATTRIBUTED_LOG", Path.home() / ".cai_unattributed.log"))
            log_path.parent.mkdir(parents=True, exist_ok=True)
            with log_path.open("a", encoding="utf-8") as fh:
                fh.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {message} | context={context}\n")
                fh.write(f"{stack}\n\n")
        except Exception:
            pass


    def remove_agent_tracking(
        self,
        *,
        agent_id: Optional[str] = None,
        agent_name: Optional[str] = None,
        terminal_id: Optional[str] = None,
    ) -> None:
        """Remove tracking data associated with an agent or terminal."""

        normalized_keys = set()
        display_keys_to_delete = set()

        if agent_id or agent_name:
            normalized = self._normalize_agent_key(None, agent_id, agent_name)
            if normalized:
                normalized_keys.add(normalized)
            # Collect any other keys referencing the same agent metadata
            for key, state in list(self.agent_cost_states.items()):
                state_id = state.get("agent_id")
                state_name = state.get("agent_name")
                if (agent_id and state_id == agent_id) or (
                    agent_name and state_name == agent_name
                ):
                    normalized_keys.add(key)
        else:
            # If only terminal is provided, drop states without metadata that map to that terminal
            for key, state in list(self.agent_cost_states.items()):
                if terminal_id and state.get("terminal_id") == terminal_id:
                    normalized_keys.add(key)

        for key in normalized_keys:
            state = self.agent_cost_states.pop(key, None)
            if state and state.get("display_name"):
                display_keys_to_delete.add(state["display_name"])

        if agent_name or agent_id:
            display_keys_to_delete.add(
                self._build_agent_display_name(agent_name, agent_id)
            )

        for display_key in display_keys_to_delete:
            self.agent_costs.pop(display_key, None)

        if terminal_id:
            self.terminal_costs.pop(terminal_id, None)
            if terminal_id.startswith("terminal-"):
                _, _, number = terminal_id.partition("-")
                if number:
                    self.terminal_costs.pop(f"T{number}", None)


    def check_price_limit(self, new_cost: float) -> None:
        """Check if adding the new cost would exceed the price limit."""
        import os

        from cai.sdk.agents.exceptions import PriceLimitExceeded

        price_limit_env = os.getenv("CAI_PRICE_LIMIT")
        try:
            price_limit = float(price_limit_env) if price_limit_env is not None else float("inf")
        except ValueError:
            price_limit = float("inf")

        if price_limit != float("inf"):
            total_cost = self.session_total_cost + new_cost
            if total_cost > price_limit:
                raise PriceLimitExceeded(total_cost, price_limit)

    def update_session_cost(self, new_cost: float) -> None:
        """Add cost to session total and log the update"""
        # Check price limit before updating
        self.check_price_limit(new_cost)

        old_total = self.session_total_cost
        self.session_total_cost += new_cost

        # Also update the global usage tracker when session cost changes
        # This ensures consistency between COST_TRACKER and GLOBAL_USAGE_TRACKER
        try:
            from cai.sdk.agents.global_usage_tracker import GLOBAL_USAGE_TRACKER
            # We don't have model/token details here, so just update the cost
            # The tokens should have been tracked separately
            # This is just a safety net to ensure costs are consistent
        except ImportError:
            pass

    def add_interaction_cost(self, new_cost: float) -> None:
        """
        Add an interaction cost to the session total and check price limit.
        This is a convenience method that combines check_price_limit and update_session_cost.
        """
        # Skip updating costs if the cost is zero (common with local models)
        if new_cost <= 0:
            self.last_interaction_cost = 0.0
            return

        # Check price limit first
        self.check_price_limit(new_cost)

        # Then update the session cost
        self.session_total_cost += new_cost

        # Update the last interaction cost for tracking
        self.last_interaction_cost = new_cost

    def reset_cost_for_local_model(self, model_name: str) -> bool:
        """
        Reset interaction cost tracking when switching to a local model.
        Returns True if the model was identified as local and cost was reset.
        """
        # Check if this is a local/free model by getting its pricing (non-blocking)
        input_cost, output_cost = self.get_model_pricing(model_name, allow_async=True)

        # If both costs are zero, it's a free/local model
        if input_cost == 0.0 and output_cost == 0.0:
            # Reset the current interaction costs but keep total session costs
            self.interaction_cost = 0.0
            self.last_interaction_cost = 0.0
            # Don't reset session_total_cost as that includes previous paid models
            return True

        return False

    def reset_agent_costs(self) -> None:
        """
        Reset costs for a new agent run.
        This should be called when starting a new agent to avoid inheriting previous agent's costs.
        """
        # Reset current agent stats
        self.current_agent_total_cost = 0.0
        self.current_agent_input_tokens = 0
        self.current_agent_output_tokens = 0
        self.current_agent_reasoning_tokens = 0

        # Reset current interaction stats
        self.interaction_input_tokens = 0
        self.interaction_output_tokens = 0
        self.interaction_reasoning_tokens = 0
        self.interaction_cost = 0.0

        # Reset tracking variables
        self.last_interaction_cost = 0.0
        self.last_total_cost = 0.0

        # Reset aggregation caches exposed to TUI/CLI visuals
        self.agent_costs.clear()
        self.terminal_costs.clear()
        self.agent_cost_states.clear()
        _AGENT_PRICING_CACHE.clear()

    def _normalize_agent_key(
        self,
        agent_key: Optional[str] = None,
        agent_id: Optional[str] = None,
        agent_name: Optional[str] = None,
    ) -> Optional[str]:
        if agent_key:
            return agent_key
        if agent_id:
            return f"id:{agent_id}"
        if agent_name:
            return f"name:{agent_name}"
        return "unknown"

    def _build_agent_display_name(
        self,
        agent_name: Optional[str],
        agent_id: Optional[str],
    ) -> str:
        if agent_name and agent_id:
            if agent_id in agent_name:
                return agent_name
            return f"{agent_name} [{agent_id}]"
        if agent_name:
            return agent_name
        if agent_id:
            return f"Agent [{agent_id}]"
        return "Agent"

    def log_final_cost(self) -> None:
        """Display final cost information at exit"""
        # Skip displaying cost if already shown in the session summary
        if os.environ.get("CAI_COST_DISPLAYED", "").lower() == "true":
            return
        print(f"\nTotal CAI Session Cost: ${self.session_total_cost:.6f}")

    def get_model_pricing(self, model_name: str, *, allow_async: bool = True) -> tuple:
        """Get and cache pricing information for a model.

        Enhancements:
        - Busca pricing local en ./pricings/pricing.json (o ruta en CAI_PRICING_FILE).
        - Cachea la tabla nativa (LiteLLM) en ./pricings/native_pricing.json.
        - Si falla la descarga, usa la caché nativa local.
        """
        # Use the centralized function to standardize model names
        model_name = get_model_name(model_name)
        _pricing_debug_log("GET_MODEL_PRICING: START", model_name=model_name, allow_async=allow_async)

        # Check cache first
        if model_name in self.model_pricing_cache:
            cached = self.model_pricing_cache[model_name]
            _pricing_debug_log("GET_MODEL_PRICING: CACHE HIT",
                model_name=model_name,
                input_cost_per_token=cached[0],
                output_cost_per_token=cached[1])
            return cached

        # Ensure local pricings dir exists and is seeded with our pricing.json (best-effort)
        _seed_pricings_dir()

        # Try to load pricing from local files first (env → ./pricings/pricing.json)
        # Only use if the specific model name exists in the file
        try:
            pricing_file_env = os.getenv("CAI_PRICING_FILE")
            candidate_paths: List[pathlib.Path] = []
            if pricing_file_env:
                candidate_paths.append(pathlib.Path(pricing_file_env))
            # Only use pricings/pricing.json as local default
            candidate_paths.append(get_pricings_dir() / "pricing.json")

            for pricing_path in candidate_paths:
                if pricing_path.exists():
                    _pricing_debug_log("GET_MODEL_PRICING: TRYING LOCAL FILE", path=str(pricing_path))
                    with open(pricing_path, encoding="utf-8") as f:
                        local_pricing = json.load(f)
                        pricing_tuple = _pricing_tuple_from_mapping(local_pricing, model_name)
                        if pricing_tuple:
                            self.model_pricing_cache[model_name] = pricing_tuple
                            _pricing_debug_log("GET_MODEL_PRICING: FOUND IN LOCAL FILE",
                                path=str(pricing_path),
                                input_cost_per_token=pricing_tuple[0],
                                output_cost_per_token=pricing_tuple[1])
                            return pricing_tuple
                        else:
                            _pricing_debug_log("GET_MODEL_PRICING: NOT FOUND IN LOCAL FILE", path=str(pricing_path))
        except Exception as e:
            _pricing_debug_log("GET_MODEL_PRICING: LOCAL FILE ERROR", error=str(e))
            print(f"  WARNING: Error loading local pricing.json files: {str(e)}")

        _pricing_debug_log("GET_MODEL_PRICING: TRYING NATIVE CACHE")
        cached_tuple = _pricing_tuple_from_mapping(_load_native_pricing_cache(), model_name)
        if cached_tuple:
            self.model_pricing_cache[model_name] = cached_tuple
            _pricing_debug_log("GET_MODEL_PRICING: FOUND IN NATIVE CACHE",
                input_cost_per_token=cached_tuple[0],
                output_cost_per_token=cached_tuple[1])
            return cached_tuple

        # Skip remote pricing fetch if not explicitly enabled (useful for airgapped / CI)
        if os.getenv("CAI_ENABLE_PRICING_FETCH", "0").lower() not in ("1", "true", "yes"):
            default_pricing = (0.0, 0.0)
            self.model_pricing_cache[model_name] = default_pricing
            _pricing_debug_log("GET_MODEL_PRICING: USING DEFAULT (0.0, 0.0)",
                reason="CAI_ENABLE_PRICING_FETCH not enabled")
            return default_pricing

        _ensure_pricing_prefetch_started()

        if allow_async:
            prefetched = _get_prefetched_pricing_for_model(model_name, wait=False)
            if prefetched:
                self.model_pricing_cache[model_name] = prefetched
                return prefetched

            wait_env = os.getenv("CAI_PRICING_ASYNC_WAIT", "").strip()
            wait_seconds = 0.0
            if wait_env:
                try:
                    wait_seconds = float(wait_env)
                except ValueError:
                    wait_seconds = 0.0

            if wait_seconds > 0:
                prefetched = _get_prefetched_pricing_for_model(
                    model_name, wait=True, timeout=max(wait_seconds, 0.0)
                )
                if prefetched:
                    self.model_pricing_cache[model_name] = prefetched
                    return prefetched

            if not self.pricing_fetch_warned and not _PRICING_PREFETCH_EVENT.is_set():
                # Be quiet by default to avoid interfering with CLI/TUI output
                if os.getenv("CAI_PRICING_VERBOSE", "0").lower() in ("1", "true", "yes"):
                    print(
                        "  INFO: Pricing fetch still running; using cached/default values until it completes."
                    )
                self.pricing_fetch_warned = True

            if cached_tuple:
                return cached_tuple
            return 0.0, 0.0

        # Avoid blocking: if async disabled, still don't wait long
        prefetched = _get_prefetched_pricing_for_model(model_name, wait=True, timeout=0.0)
        if prefetched:
            self.model_pricing_cache[model_name] = prefetched
            return prefetched

        # Skip remote fetch if pricing fetch is disabled (default)
        if os.getenv("CAI_ENABLE_PRICING_FETCH", "0").lower() not in ("1", "true", "yes"):
            # Try to use cached native pricing before falling back to defaults
            cached_tuple = _pricing_tuple_from_mapping(_load_native_pricing_cache(), model_name)
            if cached_tuple:
                self.model_pricing_cache[model_name] = cached_tuple
                return cached_tuple
            default_pricing = (0.0, 0.0)
            self.model_pricing_cache[model_name] = default_pricing
            return default_pricing

        # As a last resort, try a quick synchronous fetch; keep request-level timeout short
        remote_data = _fetch_remote_pricing_sync()
        remote_tuple = _pricing_tuple_from_mapping(remote_data, model_name)
        if remote_tuple:
            self.model_pricing_cache[model_name] = remote_tuple
            return remote_tuple

        cached_tuple = _pricing_tuple_from_mapping(_load_native_pricing_cache(), model_name)
        if cached_tuple:
            self.model_pricing_cache[model_name] = cached_tuple
            return cached_tuple

        default_pricing = (0.0, 0.0)
        self.model_pricing_cache[model_name] = default_pricing
        return default_pricing


    def calculate_cost(
        self,
        model: str,
        input_tokens: int,
        output_tokens: int,
        label: Optional[str] = None,
        force_calculation: bool = False,
    ) -> float:
        """Calculate and cache cost for a given model and token counts.

        This method uses a priority-based approach:
        1. Check cache first (unless force_calculation is True)
        2. Try litellm.completion_cost (most comprehensive pricing database)
        3. Fall back to local pricing.json if litellm fails

        Args:
            model: Model name or object
            input_tokens: Number of input tokens
            output_tokens: Number of output tokens
            label: Optional label for debugging
            force_calculation: If True, bypass cache

        Returns:
            float: Calculated cost in dollars
        """
        # Standardize model name using the central function
        model_name = get_model_name(model)

        # Validate token counts
        input_tokens = max(0, int(input_tokens or 0))
        output_tokens = max(0, int(output_tokens or 0))

        _pricing_debug_log("CALCULATE_COST: START",
            model=model_name,
            input_tokens=input_tokens,
            output_tokens=output_tokens,
            label=label,
            force_calculation=force_calculation)

        # Generate a cache key
        cache_key = f"{model_name}_{input_tokens}_{output_tokens}"

        # Return cached result if available (unless force_calculation is True)
        if cache_key in self.calculated_costs_cache and not force_calculation:
            cached_cost = self.calculated_costs_cache[cache_key]
            _pricing_debug_log("CALCULATE_COST: CACHE HIT", cache_key=cache_key, cached_cost=cached_cost)
            return cached_cost

        total_cost = 0.0

        # First, try to use litellm's completion_cost method
        # This has the most comprehensive and up-to-date pricing database
        try:
            import litellm

            # Create a mock response with usage data for litellm.completion_cost
            mock_response = {
                "model": model_name,
                "usage": {
                    "prompt_tokens": input_tokens,
                    "completion_tokens": output_tokens,
                    "total_tokens": input_tokens + output_tokens,
                },
            }

            _pricing_debug_log("CALCULATE_COST: TRYING LITELLM", model=model_name)
            # Try to get cost from litellm
            litellm_cost = litellm.completion_cost(completion_response=mock_response)

            # Validate the cost is reasonable (not negative, not absurdly high)
            if litellm_cost is not None and litellm_cost >= 0:
                # Sanity check: cost per token should be reasonable
                # Most expensive models are ~$100/1M tokens = $0.0001/token
                total_tokens = input_tokens + output_tokens
                if total_tokens > 0:
                    cost_per_token = litellm_cost / total_tokens
                    # Max reasonable cost: ~$0.001 per token (10x most expensive)
                    if cost_per_token <= 0.001:
                        total_cost = float(litellm_cost)
                        self.calculated_costs_cache[cache_key] = total_cost
                        _pricing_debug_log("CALCULATE_COST: LITELLM SUCCESS",
                            litellm_cost=litellm_cost,
                            cost_per_token=cost_per_token,
                            total_cost=total_cost)
                        return total_cost
                    else:
                        _pricing_debug_log("CALCULATE_COST: LITELLM REJECTED (cost too high)",
                            litellm_cost=litellm_cost,
                            cost_per_token=cost_per_token)
        except Exception as e:
            # If litellm fails or is not available, continue to fallback
            _pricing_debug_log("CALCULATE_COST: LITELLM FAILED", error=str(e))
            pass

        # Fallback to our pricing.json method
        # Get pricing information from local files
        _pricing_debug_log("CALCULATE_COST: FALLBACK TO LOCAL PRICING")
        input_cost_per_token, output_cost_per_token = self.get_model_pricing(
            model_name, allow_async=True
        )

        # Calculate costs - use high precision for calculations
        input_cost = input_tokens * input_cost_per_token
        output_cost = output_tokens * output_cost_per_token
        total_cost = input_cost + output_cost

        _pricing_debug_log("CALCULATE_COST: LOCAL PRICING RESULT",
            input_cost_per_token=input_cost_per_token,
            output_cost_per_token=output_cost_per_token,
            input_cost=input_cost,
            output_cost=output_cost,
            total_cost=total_cost,
            formula=f"({input_tokens} * {input_cost_per_token}) + ({output_tokens} * {output_cost_per_token}) = {total_cost}")

        # Cache the result with full precision
        self.calculated_costs_cache[cache_key] = total_cost

        return total_cost

    def process_interaction_cost(
        self,
        model: str,
        input_tokens: int,
        output_tokens: int,
        reasoning_tokens: int = 0,
        provided_cost: Optional[float] = None,
        agent_key: Optional[str] = None,
        agent_name: Optional[str] = None,
        agent_id: Optional[str] = None,
        terminal_id: Optional[str] = None,
    ) -> float:
        """Process and track costs for a new interaction"""
        # Standardize model name
        model_name = get_model_name(model)

        _pricing_debug_log("PROCESS_INTERACTION_COST: START",
            model=model_name,
            input_tokens=input_tokens,
            output_tokens=output_tokens,
            reasoning_tokens=reasoning_tokens,
            provided_cost=provided_cost,
            agent_name=agent_name,
            agent_id=agent_id,
            terminal_id=terminal_id,
            session_total_before=self.session_total_cost)

        # Update token counts
        self.interaction_input_tokens = input_tokens
        self.interaction_output_tokens = output_tokens
        self.interaction_reasoning_tokens = reasoning_tokens

        # Use provided cost or calculate
        if provided_cost is not None and provided_cost > 0:
            self.interaction_cost = float(provided_cost)
            _pricing_debug_log("PROCESS_INTERACTION_COST: USING PROVIDED COST",
                provided_cost=provided_cost)
        else:
            self.interaction_cost = self.calculate_cost(
                model_name, input_tokens, output_tokens, label="OFFICIAL CALCULATION: Interaction"
            )
            _pricing_debug_log("PROCESS_INTERACTION_COST: CALCULATED COST",
                calculated_cost=self.interaction_cost)

        self.last_interaction_cost = self.interaction_cost

        normalized_key = self._normalize_agent_key(agent_key, agent_id, agent_name)
        if normalized_key == "unknown":
            if not agent_name:
                agent_name = "Unattributed"
            if not terminal_id:
                terminal_id = "unassigned"
            self._warn_unattributed(
                "Tracking cost for interaction without explicit agent metadata; assigning to 'Unattributed'",
                model=model_name,
                provided_cost=provided_cost,
                agent_key=agent_key,
            )
        if normalized_key:
            state = self.agent_cost_states.get(normalized_key, {})
            state.update(
                {
                    "agent_name": agent_name or state.get("agent_name"),
                    "agent_id": agent_id or state.get("agent_id"),
                    "model": model_name,
                    "terminal_id": terminal_id or state.get("terminal_id"),
                    "last_interaction_cost": self.interaction_cost,
                    "last_interaction_input_tokens": input_tokens,
                    "last_interaction_output_tokens": output_tokens,
                    "last_interaction_reasoning_tokens": reasoning_tokens,
                    "updated_at": time.time(),
                }
            )
            state.setdefault("total_cost", state.get("total_cost", 0.0))
            self.agent_cost_states[normalized_key] = state

        _pricing_debug_log("PROCESS_INTERACTION_COST: COMPLETE",
            interaction_cost=self.interaction_cost,
            last_interaction_cost=self.last_interaction_cost,
            normalized_key=normalized_key)

        return self.interaction_cost

    def process_total_cost(
        self,
        model: str,
        total_input_tokens: int,
        total_output_tokens: int,
        total_reasoning_tokens: int = 0,
        provided_cost: Optional[float] = None,
        agent_key: Optional[str] = None,
        agent_name: Optional[str] = None,
        agent_id: Optional[str] = None,
        terminal_id: Optional[str] = None,
    ) -> float:
        """Process and track costs for total (cumulative) usage"""
        # Standardize model name
        model_name = get_model_name(model)

        _pricing_debug_log("PROCESS_TOTAL_COST: START",
            model=model_name,
            total_input_tokens=total_input_tokens,
            total_output_tokens=total_output_tokens,
            total_reasoning_tokens=total_reasoning_tokens,
            provided_cost=provided_cost,
            agent_name=agent_name,
            agent_id=agent_id,
            terminal_id=terminal_id,
            session_total_before=self.session_total_cost,
            current_agent_total_before=self.current_agent_total_cost)

        # Update token counts
        self.current_agent_input_tokens = total_input_tokens
        self.current_agent_output_tokens = total_output_tokens
        self.current_agent_reasoning_tokens = total_reasoning_tokens

        # If a total cost is explicitly provided, use it directly
        if provided_cost is not None and provided_cost > 0:
            new_total_cost = float(provided_cost)
            _pricing_debug_log("PROCESS_TOTAL_COST: USING PROVIDED COST",
                provided_cost=provided_cost)
        else:
            # Calculate the total cost from all tokens
            new_total_cost = self.calculate_cost(
                model_name, total_input_tokens, total_output_tokens, label="TOTAL COST CALCULATION"
            )
            _pricing_debug_log("PROCESS_TOTAL_COST: CALCULATED COST",
                calculated_cost=new_total_cost)

        normalized_key = self._normalize_agent_key(agent_key, agent_id, agent_name)
        if normalized_key == "unknown":
            if not agent_name:
                agent_name = "Unattributed"
            if not terminal_id:
                terminal_id = "unassigned"
            self._warn_unattributed(
                "Accumulating total cost without agent metadata; assigning to 'Unattributed'",
                model=model_name,
                provided_cost=provided_cost,
                agent_key=agent_key,
            )
        if normalized_key:
            previous_total = self.agent_cost_states.get(normalized_key, {}).get("total_cost", 0.0)
        else:
            previous_total = self.current_agent_total_cost
        cost_diff = new_total_cost - previous_total

        _pricing_debug_log("PROCESS_TOTAL_COST: COST DIFF CALCULATION",
            new_total_cost=new_total_cost,
            previous_total=previous_total,
            cost_diff=cost_diff,
            will_update_session=(cost_diff > 0))

        # Only add to session total if there's genuinely new cost (and it's positive)
        if cost_diff > 0:
            self.update_session_cost(cost_diff)
            _pricing_debug_log("PROCESS_TOTAL_COST: SESSION UPDATED",
                cost_diff_added=cost_diff,
                new_session_total=self.session_total_cost)

        # Update the current agent's total cost and trackers
        self.current_agent_total_cost = new_total_cost
        self.last_total_cost = new_total_cost

        if normalized_key:
            state = self.agent_cost_states.get(normalized_key, {})
            display_name = state.get(
                "display_name",
                self._build_agent_display_name(agent_name, agent_id),
            )
            state.update(
                {
                    "agent_name": agent_name or state.get("agent_name"),
                    "agent_id": agent_id or state.get("agent_id"),
                    "display_name": display_name,
                    "model": model_name,
                    "terminal_id": terminal_id or state.get("terminal_id"),
                    "total_cost": new_total_cost,
                    "total_input_tokens": total_input_tokens,
                    "total_output_tokens": total_output_tokens,
                    "total_reasoning_tokens": total_reasoning_tokens,
                    "updated_at": time.time(),
                }
            )
            if "last_interaction_cost" not in state:
                state["last_interaction_cost"] = self.last_interaction_cost
            self.agent_cost_states[normalized_key] = state

            if new_total_cost > 0:
                self.agent_costs[display_name] = new_total_cost
            elif display_name not in self.agent_costs:
                pass
            else:
                self.agent_costs.pop(display_name, None)

            if terminal_id:
                if new_total_cost > 0:
                    self.terminal_costs[terminal_id] = new_total_cost
                elif terminal_id not in self.terminal_costs:
                    pass
                else:
                    self.terminal_costs.pop(terminal_id, None)

        _pricing_debug_log("PROCESS_TOTAL_COST: COMPLETE",
            new_total_cost=new_total_cost,
            current_agent_total_cost=self.current_agent_total_cost,
            session_total_cost=self.session_total_cost,
            normalized_key=normalized_key)

        # Return the updated total cost for caller convenience
        return new_total_cost


# Initialize the global cost tracker
COST_TRACKER = CostTracker()

# Cache per-agent pricing snapshots to avoid cross-terminal bleed when multiple
# agents/terminals render concurrently. Keyed by agent_id when available and
# falls back to agent_name for single-agent runs.
_AGENT_PRICING_CACHE: Dict[str, Dict[str, Any]] = {}


def _build_agent_pricing_key(token_info: Optional[Dict[str, Any]]) -> Optional[str]:
    """Generate a cache key for agent pricing snapshots."""
    if not token_info:
        return None

    agent_id = token_info.get("agent_id")
    if agent_id:
        return f"id:{agent_id}"

    agent_name = token_info.get("agent_name")
    if agent_name:
        return f"name:{agent_name}"

    return None


def enrich_token_info_for_pricing(
    token_info: Optional[Dict[str, Any]],
    *,
    default_model: Optional[str] = None,
) -> Dict[str, Any]:
    """Ensure token_info dictionaries carry explicit pricing details.

    This normalises token and cost fields so downstream renderers (CLI/TUI) can
    display consistent pricing summaries without falling back to global session
    state that may belong to a different agent/terminal.

    IMPORTANT: In multi-agent mode (TUI or parallel), we MUST NOT fall back to
    global COST_TRACKER values as they may belong to a different agent. Instead,
    we only use agent-specific state from agent_cost_states when available.
    """

    def _to_int(value: Any) -> int:
        try:
            return int(value or 0)
        except (TypeError, ValueError):
            return 0

    def _to_float(value: Any) -> float:
        try:
            return float(value or 0.0)
        except (TypeError, ValueError):
            return 0.0

    enriched: Dict[str, Any] = dict(token_info or {})
    agent_key = _build_agent_pricing_key(enriched)

    # Determine if we're in multi-agent mode (TUI or parallel)
    is_multi_agent = os.getenv("CAI_TUI_MODE") == "true" or is_parallel_session()

    # Hydrate missing values from previous snapshot when available
    if agent_key and agent_key in _AGENT_PRICING_CACHE:
        cached_snapshot = _AGENT_PRICING_CACHE[agent_key]
        for field, cached_value in cached_snapshot.items():
            if field not in enriched or enriched[field] in (None, 0, 0.0, ""):
                enriched[field] = cached_value

    # In multi-agent mode, try to get agent-specific state from COST_TRACKER
    agent_specific_state = None
    if agent_key and hasattr(COST_TRACKER, "agent_cost_states"):
        agent_specific_state = COST_TRACKER.agent_cost_states.get(agent_key, {})

    # Extract current totals
    # CRITICAL: In multi-agent mode, only use agent-specific state, not global COST_TRACKER values
    interaction_input = _to_int(enriched.get("interaction_input_tokens"))
    interaction_output = _to_int(enriched.get("interaction_output_tokens"))
    interaction_reasoning = _to_int(enriched.get("interaction_reasoning_tokens"))

    if interaction_input == 0:
        if agent_specific_state:
            interaction_input = _to_int(agent_specific_state.get("last_interaction_input_tokens", 0))
        # CRITICAL FIX: Always fall back to COST_TRACKER if still 0, even in multi-agent mode
        # The previous logic skipped this fallback in multi-agent mode, causing "In: 0 Out: 0" displays
        if interaction_input == 0:
            interaction_input = _to_int(getattr(COST_TRACKER, "interaction_input_tokens", 0))
        enriched["interaction_input_tokens"] = interaction_input

    if interaction_output == 0:
        if agent_specific_state:
            interaction_output = _to_int(agent_specific_state.get("last_interaction_output_tokens", 0))
        # CRITICAL FIX: Always fall back to COST_TRACKER if still 0
        if interaction_output == 0:
            interaction_output = _to_int(getattr(COST_TRACKER, "interaction_output_tokens", 0))
        enriched["interaction_output_tokens"] = interaction_output

    if interaction_reasoning == 0:
        if agent_specific_state:
            interaction_reasoning = _to_int(agent_specific_state.get("last_interaction_reasoning_tokens", 0))
        # CRITICAL FIX: Always fall back to COST_TRACKER if still 0
        if interaction_reasoning == 0:
            interaction_reasoning = _to_int(getattr(COST_TRACKER, "interaction_reasoning_tokens", 0))
        enriched["interaction_reasoning_tokens"] = interaction_reasoning

    total_input = _to_int(enriched.get("total_input_tokens"))
    total_output = _to_int(enriched.get("total_output_tokens"))
    total_reasoning = _to_int(enriched.get("total_reasoning_tokens"))

    if total_input == 0:
        if agent_specific_state:
            total_input = _to_int(agent_specific_state.get("total_input_tokens", 0))
        # CRITICAL FIX: Always fall back to COST_TRACKER if still 0
        if total_input == 0:
            total_input = _to_int(getattr(COST_TRACKER, "current_agent_input_tokens", 0))
        enriched["total_input_tokens"] = total_input

    if total_output == 0:
        if agent_specific_state:
            total_output = _to_int(agent_specific_state.get("total_output_tokens", 0))
        # CRITICAL FIX: Always fall back to COST_TRACKER if still 0
        if total_output == 0:
            total_output = _to_int(getattr(COST_TRACKER, "current_agent_output_tokens", 0))
        enriched["total_output_tokens"] = total_output

    if total_reasoning == 0:
        if agent_specific_state:
            total_reasoning = _to_int(agent_specific_state.get("total_reasoning_tokens", 0))
        # CRITICAL FIX: Always fall back to COST_TRACKER if still 0
        if total_reasoning == 0:
            total_reasoning = _to_int(getattr(COST_TRACKER, "current_agent_reasoning_tokens", 0))
        enriched["total_reasoning_tokens"] = total_reasoning

    # Normalise terminal metadata for downstream consumers
    terminal_id = enriched.get("terminal_id")
    if not terminal_id:
        terminal_number = enriched.get("terminal_number")
        if terminal_number:
            terminal_id = f"terminal-{terminal_number}"
            enriched["terminal_id"] = terminal_id

    # Derive a stable agent_id from terminal_id when not explicitly provided
    # This prevents collisions across parallel agents that share the same base name
    if not enriched.get("agent_id") and terminal_id and isinstance(terminal_id, str):
        try:
            if terminal_id.startswith("terminal-"):
                number = terminal_id.split("-", 1)[1]
                if number.isdigit():
                    enriched["agent_id"] = f"P{number}"
        except Exception:
            pass

    # Normalise model name before pricing lookups
    model_name = get_model_name(
        enriched.get("model") or default_model or os.environ.get("CAI_MODEL", "")
    )
    enriched["model"] = model_name

    input_rate, output_rate = COST_TRACKER.get_model_pricing(
        model_name, allow_async=True
    )

    # Compute interaction costs when not provided or obviously stale
    interaction_input_cost = _to_float(enriched.get("interaction_input_cost"))
    calculated_input_cost = input_rate * interaction_input
    if interaction_input_cost == 0.0 or interaction_input_cost != calculated_input_cost:
        interaction_input_cost = calculated_input_cost
        enriched["interaction_input_cost"] = interaction_input_cost

    interaction_output_cost = _to_float(enriched.get("interaction_output_cost"))
    calculated_output_cost = output_rate * interaction_output
    if interaction_output_cost == 0.0 or interaction_output_cost != calculated_output_cost:
        interaction_output_cost = calculated_output_cost
        enriched["interaction_output_cost"] = interaction_output_cost

    interaction_cost = _to_float(enriched.get("interaction_cost"))
    calculated_interaction_cost = interaction_input_cost + interaction_output_cost
    if interaction_cost == 0.0 or abs(interaction_cost - calculated_interaction_cost) > 1e-9:
        if calculated_interaction_cost > 0:
            interaction_cost = calculated_interaction_cost
        elif agent_specific_state:
            interaction_cost = _to_float(agent_specific_state.get("last_interaction_cost", 0.0))
        elif not is_multi_agent:
            interaction_cost = _to_float(getattr(COST_TRACKER, "last_interaction_cost", 0.0))
        enriched["interaction_cost"] = interaction_cost

    # Compute totals
    total_input_cost = _to_float(enriched.get("total_input_cost"))
    calculated_total_input_cost = input_rate * total_input
    if total_input_cost == 0.0 or total_input_cost != calculated_total_input_cost:
        total_input_cost = calculated_total_input_cost
        enriched["total_input_cost"] = total_input_cost

    total_output_cost = _to_float(enriched.get("total_output_cost"))
    calculated_total_output_cost = output_rate * total_output
    if total_output_cost == 0.0 or total_output_cost != calculated_total_output_cost:
        total_output_cost = calculated_total_output_cost
        enriched["total_output_cost"] = total_output_cost

    total_cost = _to_float(enriched.get("total_cost"))
    calculated_total_cost = total_input_cost + total_output_cost
    if total_cost == 0.0 or abs(total_cost - calculated_total_cost) > 1e-6:
        if calculated_total_cost > 0:
            total_cost = calculated_total_cost
        elif agent_specific_state:
            total_cost = _to_float(agent_specific_state.get("total_cost", 0.0))
        elif not is_multi_agent:
            total_cost = _to_float(getattr(COST_TRACKER, "current_agent_total_cost", 0.0)) or _to_float(
                getattr(COST_TRACKER, "last_total_cost", 0.0)
            )
        enriched["total_cost"] = total_cost

    enriched["session_total_cost"] = _to_float(getattr(COST_TRACKER, "session_total_cost", 0.0))

    # Context usage inference for visual alerts
    context_pct = _to_float(enriched.get("context_percentage") or enriched.get("context_usage_pct"))
    if context_pct == 0.0 and interaction_input > 0:
        max_tokens = get_model_input_tokens(model_name)
        if max_tokens:
            context_pct = min((interaction_input / max_tokens) * 100, 100.0)
            enriched["context_percentage"] = context_pct

    # Ensure cached token metadata is present for consistent display
    if "cached_tokens" in enriched and enriched.get("cached_tokens") is None:
        enriched["cached_tokens"] = 0
    if "cached_cost" not in enriched or enriched.get("cached_cost") is None:
        enriched["cached_cost"] = 0.0

    # Enrich cache_read_tokens and cache_creation_tokens from COST_TRACKER if missing
    # These are needed for OpenAI/Anthropic prompt caching display
    cache_read = _to_int(enriched.get("cache_read_tokens"))
    cache_creation = _to_int(enriched.get("cache_creation_tokens"))

    if cache_read == 0:
        if agent_specific_state:
            cache_read = _to_int(agent_specific_state.get("cache_read_tokens", 0))
        elif not is_multi_agent:
            cache_read = _to_int(getattr(COST_TRACKER, "cache_read_tokens", 0))
        enriched["cache_read_tokens"] = cache_read

    if cache_creation == 0:
        if agent_specific_state:
            cache_creation = _to_int(agent_specific_state.get("cache_creation_tokens", 0))
        elif not is_multi_agent:
            cache_creation = _to_int(getattr(COST_TRACKER, "cache_creation_tokens", 0))
        enriched["cache_creation_tokens"] = cache_creation

    # Calculate cache savings and extra costs if we have cache tokens
    if cache_read > 0 or cache_creation > 0:
        cache_read_cost, cache_read_savings, cache_creation_cost, cache_creation_extra = (
            calculate_cached_token_costs(model_name, cache_read, cache_creation)
        )
        enriched["cache_read_savings"] = cache_read_savings
        enriched["cache_creation_extra"] = cache_creation_extra

    # Track interaction counter fallbacks for renderers that rely on it
    if not enriched.get("interaction_counter"):
        enriched["interaction_counter"] = get_interaction_counter()

    # Persist snapshot for future renders tied to the same agent/terminal
    if agent_key:
        fields_to_cache = {
            "interaction_input_tokens": interaction_input,
            "interaction_output_tokens": interaction_output,
            "interaction_reasoning_tokens": interaction_reasoning,
            "interaction_cost": interaction_cost,
            "interaction_input_cost": interaction_input_cost,
            "interaction_output_cost": interaction_output_cost,
            "total_input_tokens": total_input,
            "total_output_tokens": total_output,
            "total_reasoning_tokens": total_reasoning,
            "total_cost": total_cost,
            "total_input_cost": total_input_cost,
            "total_output_cost": total_output_cost,
            "session_total_cost": enriched["session_total_cost"],
            "context_percentage": enriched.get("context_percentage", context_pct),
        }
        _AGENT_PRICING_CACHE[agent_key] = fields_to_cache

    # Update CostTracker aggregation maps for sidebar/CLI summaries
    raw_agent_name = enriched.get("agent_name") or "Agent"
    agent_name_trimmed = raw_agent_name
    if "[" in agent_name_trimmed and "]" in agent_name_trimmed:
        agent_name_trimmed = agent_name_trimmed.split("[")[0].strip()
    agent_id_value = enriched.get("agent_id")
    agent_id_str = str(agent_id_value) if agent_id_value not in (None, "") else ""

    meaningful_total = max(
        total_cost,
        interaction_cost,
        total_input_cost,
        total_output_cost,
    )

    stored_total = total_cost if total_cost > 0 else meaningful_total

    normalized_key = COST_TRACKER._normalize_agent_key(
        None,
        agent_id_str or None,
        agent_name_trimmed,
    )
    display_name = raw_agent_name or COST_TRACKER._build_agent_display_name(
        agent_name_trimmed, agent_id_str
    )

    if normalized_key:
        state = COST_TRACKER.agent_cost_states.get(normalized_key, {})
        state.update(
            {
                "agent_name": agent_name_trimmed,
                "agent_id": agent_id_str or state.get("agent_id"),
                "display_name": display_name,
                "model": model_name,
                "terminal_id": terminal_id or state.get("terminal_id"),
                "total_cost": stored_total,
                "total_input_tokens": total_input,
                "total_output_tokens": total_output,
                "total_reasoning_tokens": total_reasoning,
                "last_interaction_cost": interaction_cost,
                "last_interaction_input_tokens": interaction_input,
                "last_interaction_output_tokens": interaction_output,
                "last_interaction_reasoning_tokens": interaction_reasoning,
                "updated_at": time.time(),
            }
        )
        COST_TRACKER.agent_cost_states[normalized_key] = state

    if display_name:
        if stored_total > 0:
            COST_TRACKER.agent_costs[display_name] = stored_total
        elif display_name in COST_TRACKER.agent_costs:
            COST_TRACKER.agent_costs.pop(display_name, None)

    if terminal_id:
        if stored_total > 0:
            COST_TRACKER.terminal_costs[terminal_id] = stored_total
        elif terminal_id in COST_TRACKER.terminal_costs:
            COST_TRACKER.terminal_costs.pop(terminal_id, None)

    if terminal_id and terminal_id.startswith("terminal-"):
        try:
            terminal_number = terminal_id.split("-", 1)[1]
        except Exception:
            terminal_number = None
        if terminal_number:
            predictable_id = f"T{terminal_number}"
            if stored_total > 0:
                COST_TRACKER.terminal_costs[predictable_id] = stored_total
            elif predictable_id in COST_TRACKER.terminal_costs:
                COST_TRACKER.terminal_costs.pop(predictable_id, None)

    return enriched

# Register exit handler for final cost display
atexit.register(COST_TRACKER.log_final_cost)

def get_model_pricing(model_name, *, allow_async: bool = True):
    """
    Get pricing information for a model, using the CostTracker's implementation.
    This is a global helper that delegates to the CostTracker instance.

    Args:
        model_name: String name of the model

    Returns:
        tuple: (input_cost_per_token, output_cost_per_token)
    """
    # Standardize model name
    model_name = get_model_name(model_name)

    # Use the CostTracker's implementation to maintain consistency and use its cache
    return COST_TRACKER.get_model_pricing(model_name, allow_async=allow_async)


def calculate_model_cost(model, input_tokens, output_tokens):
    """
    Calculate the cost for a given model based on token usage.

    Args:
        model: The model name or object
        input_tokens: Number of input tokens used
        output_tokens: Number of output tokens used

    Returns:
        float: The calculated cost in dollars
    """
    # Use the CostTracker to handle duplicates
    return COST_TRACKER.calculate_cost(
        model,
        input_tokens,
        output_tokens,
        label="COST CALCULATION",
        force_calculation=False,  # Let it use the cache for duplicates
    )


def calculate_cached_token_costs(
    model,
    cache_read_tokens: int = 0,
    cache_creation_tokens: int = 0
) -> tuple[float, float, float, float]:
    """
    Calculate costs and savings from cached tokens based on the provider's cache pricing.

    Different providers have different cache pricing:
    - Anthropic/Claude:
        - cache_read costs 10% of input price (90% savings)
        - cache_creation costs 125% of input price (25% extra cost)
    - OpenAI: cache reads cost 50% of input price (50% savings)
    - Gemini: cached tokens are typically free or very cheap
    - DeepSeek: similar to Anthropic

    Args:
        model: The model name
        cache_read_tokens: Number of tokens read from cache (savings)
        cache_creation_tokens: Number of tokens written to cache (extra cost)

    Returns:
        tuple: (cache_read_cost, cache_read_savings, cache_creation_cost, cache_creation_extra)
    """
    if cache_read_tokens <= 0 and cache_creation_tokens <= 0:
        return 0.0, 0.0, 0.0, 0.0

    model_lower = str(model).lower()

    # Get the input price per token for this model
    input_price, _ = get_model_pricing(model)
    if input_price <= 0:
        return 0.0, 0.0, 0.0, 0.0

    # Determine cache rates based on provider
    # Reference: https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching
    if any(p in model_lower for p in ["claude", "anthropic"]):
        # Anthropic: reads cost 10%, writes cost 125%
        read_rate = 0.10
        write_rate = 1.25
    elif any(p in model_lower for p in ["gpt-", "openai", "o1", "o3"]):
        # OpenAI: reads cost 50%, writes are normal (no extra)
        read_rate = 0.50
        write_rate = 1.0
    elif "gemini" in model_lower:
        # Gemini: reads are cheap, writes are normal
        read_rate = 0.25
        write_rate = 1.0
    elif "deepseek" in model_lower:
        # DeepSeek: similar to Anthropic
        read_rate = 0.10
        write_rate = 1.25
    else:
        # Default
        read_rate = 0.50
        write_rate = 1.0

    # Calculate cache read costs and savings
    cache_read_full_price = cache_read_tokens * input_price
    cache_read_cost = cache_read_full_price * read_rate
    cache_read_savings = cache_read_full_price - cache_read_cost

    # Calculate cache creation costs (extra cost for writing to cache)
    cache_creation_normal_price = cache_creation_tokens * input_price
    cache_creation_cost = cache_creation_normal_price * write_rate
    cache_creation_extra = cache_creation_cost - cache_creation_normal_price

    _pricing_debug_log("CACHED_TOKEN_COSTS",
        model=model,
        cache_read_tokens=cache_read_tokens,
        cache_creation_tokens=cache_creation_tokens,
        input_price_per_token=input_price,
        read_rate=read_rate,
        write_rate=write_rate,
        cache_read_cost=cache_read_cost,
        cache_read_savings=cache_read_savings,
        cache_creation_cost=cache_creation_cost,
        cache_creation_extra=cache_creation_extra)

    return cache_read_cost, cache_read_savings, cache_creation_cost, cache_creation_extra


def calculate_cached_token_savings(model, cached_tokens: int) -> tuple[float, float]:
    """
    Legacy wrapper for calculate_cached_token_costs.
    Only handles cache_read tokens for backward compatibility.
    """
    cache_read_cost, cache_read_savings, _, _ = calculate_cached_token_costs(
        model, cache_read_tokens=cached_tokens, cache_creation_tokens=0
    )
    return cache_read_cost, cache_read_savings
