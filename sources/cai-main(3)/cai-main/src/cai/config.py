"""CAI Centralized Configuration.

Single source of truth for all configuration.
Replaces 936 scattered os.getenv() calls across 137 files.

Created in Day 0 as shared contract between 3 refactoring streams.
- Stream 2 (Foundation): implements from_env() and validate()
- Stream 1 (Core Engine): consumes for LLM/tool settings
- Stream 3 (Interface): consumes for TUI/REPL settings
"""

from __future__ import annotations

import os
import warnings
from dataclasses import dataclass
from typing import Final

_LEGACY_COMPACTED_MEMORY_WARNED = False

# Auto-compact never waits beyond this fraction of the model context window,
# even if CAI_AUTO_COMPACT_THRESHOLD is set higher (users can still set lower).
AUTO_COMPACT_THRESHOLD_MAX: Final[float] = 0.8

DEFAULT_AGENT_TYPE: Final[str] = "selection_agent"
ORCHESTRATION_AGENT_TYPE: Final[str] = "orchestration_agent"


def _parse_inf(value: str) -> int | float:
    if value.lower() in ("inf", "infinite", "infinity", "unlimited"):
        return float("inf")
    return int(value)


def _parse_bool(value: str) -> bool:
    return value.lower() in ("true", "1", "yes")


def compacted_memory_env_enabled() -> bool:
    """Whether REPL /compact summaries are injected into agent system prompts.

    Reads ``CAI_COMPACTED_MEMORY`` when set; otherwise falls back to legacy
    ``CAI_MEMORY`` (deprecated) for one release.
    """
    global _LEGACY_COMPACTED_MEMORY_WARNED  # pylint: disable=global-statement
    if "CAI_COMPACTED_MEMORY" in os.environ:
        return _parse_bool(os.getenv("CAI_COMPACTED_MEMORY", "false"))
    legacy = os.getenv("CAI_MEMORY", "").strip().lower()
    if legacy in ("true", "1", "yes", "episodic", "semantic", "all"):
        if not _LEGACY_COMPACTED_MEMORY_WARNED:
            warnings.warn(
                "CAI_MEMORY is deprecated for compacted-session memory; set "
                "CAI_COMPACTED_MEMORY=true. Legacy CAI_MEMORY support will be removed "
                "in a future release.",
                DeprecationWarning,
                stacklevel=2,
            )
            _LEGACY_COMPACTED_MEMORY_WARNED = True
        return True
    return False


@dataclass
class CAIConfig:
    """Complete CAI configuration, loaded once at startup."""

    # --- Model & Agent ---
    model: str = "alias1"
    agent_type: str = DEFAULT_AGENT_TYPE
    temperature: float = 0.7
    top_p: float = 1.0
    max_tokens: int | None = None
    reasoning_effort: str | None = None

    # --- Limits ---
    max_turns: int | float = float("inf")
    max_interactions: int | float = float("inf")
    price_limit: float = 1.0

    # --- Streaming ---
    stream: bool = False
    tool_stream: bool = True

    # --- Parallel ---
    parallel: int = 1

    # --- Compacted session memory (/compact) ---
    compacted_memory: bool = False

    # --- Debug & Logging ---
    debug: int = 1
    debug_pricing: bool = False
    tracing: bool = True
    telemetry: bool = True

    # --- Auto-compaction ---
    auto_compact: bool = True
    # Compact when context exceeds this fraction of the model window.
    auto_compact_threshold: float = 0.8

    # --- Security ---
    guardrails: bool = False
    tool_timeout: int = 120

    # --- TUI ---
    tui_enabled: bool = True
    tui_mode: str = "default"
    tui_theme: str = "tokyo-night"
    tui_startup_yaml: str | None = None
    tui_shared_prompt: str | None = None

    # --- CTF ---
    ctf_name: str | None = None
    ctf_challenge: str | None = None

    # --- Planning ---
    plan_enabled: bool = False

    # --- Orchestration (workers spawned by orchestration_agent tools) ---
    orchestration_worker_max_turns: int = 6
    orchestration_mas_hint: bool = True

    # --- Tool Registry ---
    # When True, the ToolRegistry auto-supplements agent tools based on
    # agent-type category mapping.  Disabled by default to keep token usage
    # minimal (each extra tool schema costs ~120-150 prompt tokens per turn).
    tool_registry_auto: bool = False

    # --- Continuation ---
    continuation_fallback_model: str | None = None

    # --- Search ---
    google_search_api_key: str | None = None
    google_search_cx: str | None = None

    # --- Web fetch (fetch_url tool) ---
    # SSRF policy: by default the fetch_url tool blocks loopback, RFC1918,
    # link-local and cloud-metadata hosts to prevent server-side request
    # forgery via prompt injection. Set CAI_FETCH_ALLOW_INTERNAL=true to allow
    # internal targets (e.g. when pentesting an internal network).
    fetch_allow_internal: bool = False
    fetch_user_agent: str | None = None  # CAI_FETCH_USER_AGENT (OPSEC override)
    fetch_max_bytes: int = 5_242_880  # CAI_FETCH_MAX_BYTES (5 MB response cap)
    fetch_timeout: int = 20  # CAI_FETCH_TIMEOUT (seconds)

    # --- Workspace ---
    workspace_dir: str | None = None  # CAI_WORKSPACE_DIR override
    workspace_name: str | None = None  # CAI_WORKSPACE named workspace

    # --- API Keys (not logged) ---
    alias_api_key: str | None = None
    openai_api_key: str | None = None
    anthropic_api_key: str | None = None
    openrouter_api_key: str | None = None
    perplexity_api_key: str | None = None
    c99_api_key: str | None = None
    shodan_api_key: str | None = None

    # --- Virtualization ---
    active_container: str | None = None
    default_docker_image: str = "kalilinux/kali-rolling"

    # --- SSH ---
    ssh_user: str | None = None
    ssh_host: str | None = None

    # --- CTF runtime ---
    ctf_inside: bool = True  # CTF_INSIDE: whether tool runs inside CTF container

    # --- Session ---
    session_input_wait: float = 5.0  # CAI_SESSION_INPUT_WAIT: seconds to wait for input

    # --- LiteLLM bypass ---
    force_httpx: bool = False  # When True, ALL OpenAI-compat models use httpx directly

    # --- Ollama ---
    ollama_url: str | None = None

    # --- API Server ---
    api_host: str = "127.0.0.1"
    api_port: int = 8000
    api_reload: bool = False
    api_workers: int = 1

    # --- Broadcast (parallel TUI) ---
    broadcast_mode: bool = False

    # --- Automation ---
    auto_run_queue: bool = False
    auto_run_parallel: bool = False
    queue_file: str | None = None
    pattern_description: str = ""

    @classmethod
    def from_env(cls) -> CAIConfig:
        """Load all configuration from environment variables. Called ONCE."""
        return cls(
            model=os.getenv("CAI_MODEL", "alias1"),
            agent_type=os.getenv("CAI_AGENT_TYPE", DEFAULT_AGENT_TYPE),
            temperature=float(os.getenv("CAI_TEMPERATURE", "0.7")),
            top_p=float(os.getenv("CAI_TOP_P", "1.0")),
            max_tokens=(
                int(os.getenv("CAI_MAX_TOKENS"))
                if os.getenv("CAI_MAX_TOKENS")
                else None
            ),
            reasoning_effort=os.getenv("CAI_REASONING_EFFORT"),
            max_turns=_parse_inf(os.getenv("CAI_MAX_TURNS", "inf")),
            max_interactions=_parse_inf(os.getenv("CAI_MAX_INTERACTIONS", "inf")),
            price_limit=float(os.getenv("CAI_PRICE_LIMIT", "1")),
            auto_compact=_parse_bool(os.getenv("CAI_AUTO_COMPACT", "true")),
            auto_compact_threshold=min(
                float(os.getenv("CAI_AUTO_COMPACT_THRESHOLD", "0.8")),
                AUTO_COMPACT_THRESHOLD_MAX,
            ),
            stream=_parse_bool(os.getenv("CAI_STREAM", "false")),
            tool_stream=_parse_bool(os.getenv("CAI_TOOL_STREAM", "true")),
            parallel=int(os.getenv("CAI_PARALLEL", "1")),
            compacted_memory=compacted_memory_env_enabled(),
            debug=int(os.getenv("CAI_DEBUG", "1")),
            debug_pricing=os.getenv("CAI_DEBUG_PRICING", "0") == "1",
            tracing=_parse_bool(os.getenv("CAI_TRACING", "true")),
            telemetry=os.getenv("CAI_TELEMETRY", "true").lower() != "false",
            guardrails=_parse_bool(os.getenv("CAI_GUARDRAILS", "false")),
            tool_timeout=int(os.getenv("CAI_TOOL_TIMEOUT", "120")),
            tui_enabled=_parse_bool(os.getenv("CAI_TUI", "true")),
            tui_mode=os.getenv("CAI_TUI_MODE", "default"),
            tui_theme=os.getenv("CAI_THEME", "tokyo-night"),
            tui_startup_yaml=os.getenv("CAI_TUI_STARTUP_YAML"),
            tui_shared_prompt=os.getenv("CAI_TUI_SHARED_PROMPT"),
            ctf_name=os.getenv("CTF_NAME"),
            ctf_challenge=os.getenv("CTF_CHALLENGE"),
            plan_enabled=_parse_bool(os.getenv("CAI_PLAN", "false")),
            orchestration_worker_max_turns=int(
                os.getenv("CAI_ORCHESTRATION_WORKER_MAX_TURNS", "6")
            ),
            orchestration_mas_hint=_parse_bool(os.getenv("CAI_ORCHESTRATION_MAS_HINT", "true")),
            tool_registry_auto=_parse_bool(os.getenv("CAI_TOOL_REGISTRY_AUTO", "false")),
            continuation_fallback_model=os.getenv("CAI_CONTINUATION_FALLBACK_MODEL"),
            google_search_api_key=os.getenv("GOOGLE_SEARCH_API_KEY"),
            google_search_cx=os.getenv("GOOGLE_SEARCH_CX"),
            fetch_allow_internal=_parse_bool(
                os.getenv("CAI_FETCH_ALLOW_INTERNAL", "false")
            ),
            fetch_user_agent=os.getenv("CAI_FETCH_USER_AGENT"),
            fetch_max_bytes=int(os.getenv("CAI_FETCH_MAX_BYTES", "5242880")),
            fetch_timeout=int(os.getenv("CAI_FETCH_TIMEOUT", "20")),
            workspace_dir=os.getenv("CAI_WORKSPACE_DIR"),
            workspace_name=os.getenv("CAI_WORKSPACE"),
            alias_api_key=os.getenv("ALIAS_API_KEY"),
            openai_api_key=os.getenv("OPENAI_API_KEY"),
            anthropic_api_key=os.getenv("ANTHROPIC_API_KEY"),
            openrouter_api_key=os.getenv("OPENROUTER_API_KEY"),
            perplexity_api_key=os.getenv("PERPLEXITY_API_KEY"),
            c99_api_key=os.getenv("C99_API_KEY"),
            shodan_api_key=os.getenv("SHODAN_API_KEY"),
            active_container=os.getenv("CAI_ACTIVE_CONTAINER"),
            default_docker_image=os.getenv(
                "CAI_DOCKER_IMAGE", "kalilinux/kali-rolling"
            ),
            ssh_user=os.getenv("SSH_USER"),
            ssh_host=os.getenv("SSH_HOST"),
            ctf_inside=_parse_bool(os.getenv("CTF_INSIDE", "true")),
            session_input_wait=float(os.getenv("CAI_SESSION_INPUT_WAIT", "5.0")),
            force_httpx=_parse_bool(os.getenv("CAI_FORCE_HTTPX", "false")),
            ollama_url=os.getenv("CAI_OLLAMA_URL"),
            api_host=os.getenv("CAI_API_HOST", "127.0.0.1"),
            api_port=int(os.getenv("CAI_API_PORT", "8000")),
            api_reload=os.getenv("CAI_API_RELOAD", "false").lower() == "true",
            api_workers=int(os.getenv("CAI_API_WORKERS", "1")),
            broadcast_mode=_parse_bool(os.getenv("CAI_BROADCAST_MODE", "false")),
            auto_run_queue=os.getenv("CAI_AUTO_RUN_QUEUE") == "1",
            auto_run_parallel=os.getenv("CAI_AUTO_RUN_PARALLEL") == "1",
            queue_file=os.getenv("CAI_QUEUE_FILE"),
            pattern_description=os.getenv("CAI_PATTERN_DESCRIPTION", ""),
        )

    def validate(self) -> list[str]:
        """Return list of validation warnings. Empty = all OK."""
        warnings = []
        if self.price_limit <= 0:
            warnings.append("CAI_PRICE_LIMIT must be > 0")
        if not (0 <= self.temperature <= 2):
            warnings.append("CAI_TEMPERATURE must be between 0 and 2")
        if not (0 <= self.top_p <= 1):
            warnings.append("CAI_TOP_P must be between 0 and 1")
        if self.parallel < 1:
            warnings.append("CAI_PARALLEL must be >= 1")
        if not (1 <= self.orchestration_worker_max_turns <= 32):
            warnings.append("CAI_ORCHESTRATION_WORKER_MAX_TURNS must be between 1 and 32")
        if self.tool_timeout < 1:
            warnings.append("CAI_TOOL_TIMEOUT must be >= 1")
        if self.debug not in (0, 1, 2):
            warnings.append("CAI_DEBUG must be 0, 1, or 2")
        _ac_env = os.getenv("CAI_AUTO_COMPACT_THRESHOLD")
        if _ac_env is not None:
            try:
                if float(_ac_env) > AUTO_COMPACT_THRESHOLD_MAX + 1e-9:
                    cap = f"{AUTO_COMPACT_THRESHOLD_MAX:.0%}"
                    warnings.append(
                        f"CAI_AUTO_COMPACT_THRESHOLD above {cap} is capped at {cap}; "
                        "auto-compact will not defer past that."
                    )
            except ValueError:
                pass
        return warnings


# ---------------------------------------------------------------------------
# Module-level singleton — loaded once, available everywhere via:
#   from cai.config import get_config
# ---------------------------------------------------------------------------
_CONFIG: CAIConfig | None = None


def get_config() -> CAIConfig:
    """Return the global CAIConfig singleton (lazy-loaded from env on first call)."""
    global _CONFIG
    if _CONFIG is None:
        _CONFIG = CAIConfig.from_env()
    return _CONFIG


def reset_config() -> None:
    """Force reload from environment. Useful after tests or dynamic env changes."""
    global _CONFIG
    _CONFIG = None
