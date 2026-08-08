"""CAI Error Hierarchy.

Typed errors replacing bare except: blocks and string-based error returns.
Inspired by Codex's CodexErr enum with 150+ variants.

Created in Day 0 as shared contract between 3 refactoring streams.
- Stream 1 (Core Engine): populates LLM and Tool errors
- Stream 2 (Foundation): populates Config errors
- Stream 3 (Interface): consumes all error types for display
"""


class CAIError(Exception):
    """Base for all CAI errors."""

    def __init__(self, message: str = "", details: dict | None = None):
        super().__init__(message)
        self.details = details or {}


# --- LLM Errors (Stream 1 owns) ---


class LLMError(CAIError):
    """Errors communicating with LLM providers."""
    pass


class LLMTimeout(LLMError):
    """LLM call exceeded timeout."""
    pass


class LLMAuthError(LLMError):
    """Authentication/authorization failure."""
    pass


class LLMRateLimited(LLMError):
    """Rate limit hit, includes retry-after if available."""

    def __init__(self, message: str = "", retry_after: float | None = None):
        super().__init__(message)
        self.retry_after = retry_after


class LLMContextOverflow(LLMError):
    """Context window exceeded."""
    pass


class LLMProviderUnavailable(LLMError):
    """Provider endpoint unreachable."""
    pass


class LLMEmptyAssistantError(LLMProviderUnavailable):
    """Gateway returned consecutive empty assistant completions (no text, no tools)."""

    pass


# --- Tool Errors (Stream 1 owns) ---


class ToolError(CAIError):
    """Errors during tool execution."""
    pass


class ToolTimeout(ToolError):
    """Tool execution exceeded timeout."""

    def __init__(self, message: str = "", timeout_seconds: int = 0):
        super().__init__(message)
        self.timeout_seconds = timeout_seconds


class ToolNotFound(ToolError):
    """Requested tool not in registry."""
    pass


class ToolExecutionFailed(ToolError):
    """Tool process exited with error."""

    def __init__(self, message: str = "", exit_code: int = -1):
        super().__init__(message)
        self.exit_code = exit_code


# --- Config Errors (Stream 2 owns) ---


class ConfigError(CAIError):
    """Configuration loading/validation errors."""
    pass


class ConfigValidationError(ConfigError):
    """Config values out of expected range."""
    pass


class ConfigMissingError(ConfigError):
    """Required configuration not provided."""
    pass


# --- Session Errors (Stream 3 owns) ---


class SessionError(CAIError):
    """Session persistence errors."""
    pass


class SessionCorrupted(SessionError):
    """Session file unreadable or corrupted."""
    pass
