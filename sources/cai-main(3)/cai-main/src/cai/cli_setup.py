"""CLI environment bootstrap: .env loading, warning suppression, logging filters, CTF init.

Extracted from cli.py to keep the main CLI module a thin orchestrator.
Every function is called once at startup.
"""

import logging
import os
import sys
import warnings

from dotenv import load_dotenv


# ---------------------------------------------------------------------------
# 1. .env and OPENAI_API_KEY defaults
# ---------------------------------------------------------------------------

def load_dotenv_and_defaults():
    """Load .env from cwd; set OPENAI_API_KEY default if missing."""
    dotenv_path = os.path.join(os.getcwd(), '.env')
    load_dotenv(dotenv_path=dotenv_path, verbose=False)
    if "OPENAI_API_KEY" not in os.environ:
        os.environ["OPENAI_API_KEY"] = ""


# ---------------------------------------------------------------------------
# 2. Warning suppression
# ---------------------------------------------------------------------------

def configure_warnings():
    """Suppress Python warnings except when CAI_DEBUG=2."""
    _original_showwarning = warnings.showwarning

    def _custom_handler(message, category, filename, lineno, file=None, line=None):
        if os.getenv("CAI_DEBUG", "1") == "2":
            _original_showwarning(message, category, filename, lineno, file, line)

    warnings.showwarning = _custom_handler

    if os.getenv("CAI_DEBUG", "1") != "2":
        warnings.filterwarnings("ignore")
        os.environ["PYTHONWARNINGS"] = "ignore"

    # Broad category filters
    warnings.filterwarnings("ignore", category=RuntimeWarning)
    warnings.filterwarnings("ignore", category=ResourceWarning)

    # Specific message patterns
    _patterns = [
        ".*asynchronous generator.*",
        ".*was never awaited.*",
        r".*didn't stop after athrow.*",
        r".*didn\u2019t stop after athrow.*",
        ".*cancel scope.*",
        ".*coroutine.*was never awaited.*",
        r".*generator.*didn't stop.*",
        ".*Task was destroyed.*",
        ".*Event loop is closed.*",
        ".*Unclosed client session.*",
        ".*Unclosed connector.*",
        ".*client_session:.*",
        ".*connector:.*",
        ".*connections:.*",
    ]
    for pat in _patterns:
        warnings.filterwarnings("ignore", message=pat)

    if not sys.warnoptions:
        warnings.simplefilter("ignore", RuntimeWarning)
        warnings.simplefilter("ignore", ResourceWarning)


# ---------------------------------------------------------------------------
# 3. Logging filters
# ---------------------------------------------------------------------------

class ComprehensiveErrorFilter(logging.Filter):
    """Filter to suppress various expected errors and warnings."""

    _SUPPRESS_PATTERNS = [
        "asynchronous generator", "asyncgen", "closedresourceerror",
        "didn't stop after athrow", "didnt stop after athrow",
        "didn\u2019t stop after athrow",
        "generator didn't stop", "generator didn\u2019t stop",
        "cancel scope", "unhandled errors in a taskgroup",
        "error in post_writer", "was never awaited",
        "connection error while setting up", "error closing",
        "anyio._backends", "httpx_sse",
        "connection reset by peer", "broken pipe", "connection aborted",
        "runtime warning", "runtimewarning", "coroutine",
        "task was destroyed", "event loop is closed", "session is closed",
        "unclosed client session", "unclosed connector",
        "client_session:", "connector:", "connections:",
    ]

    def filter(self, record):
        msg = record.getMessage().lower()

        for pattern in self._SUPPRESS_PATTERNS:
            if pattern in msg:
                return False

        if "sse" in msg and any(w in msg for w in ("cleanup", "closing", "shutdown", "closed")):
            return False
        if "error invoking mcp tool" in msg and "closedresourceerror" in msg:
            return False
        if "mcp server session not found" in msg or "successfully reconnected to mcp server" in msg:
            record.levelno = logging.DEBUG
            record.levelname = "DEBUG"

        return True


def configure_loggers():
    """Apply ComprehensiveErrorFilter to relevant loggers."""
    error_filter = ComprehensiveErrorFilter()
    _LOGGERS = [
        "openai.agents", "mcp.client.sse", "httpx", "httpx_sse",
        "mcp", "asyncio", "anyio", "anyio._backends._asyncio",
        "cai.sdk.agents", "aiohttp",
    ]
    for name in _LOGGERS:
        logger = logging.getLogger(name)
        logger.addFilter(error_filter)
        if name in ("asyncio", "anyio", "anyio._backends._asyncio"):
            logger.setLevel(logging.ERROR)
        else:
            logger.setLevel(logging.WARNING)


def suppress_aiohttp_warnings():
    """Suppress aiohttp-specific warnings about unclosed sessions."""
    try:
        import aiohttp as _  # noqa: F401
        for sub in ("aiohttp", "aiohttp.client", "aiohttp.connector"):
            logging.getLogger(sub).setLevel(logging.ERROR)
    except ImportError:
        pass


# ---------------------------------------------------------------------------
# 4. CTF container initialization (lazy, called at runtime)
# ---------------------------------------------------------------------------

# Module-level state for CTF
ctf_global = None
messages_ctf = ""
ctf_init = 1
first_ctf_time = False
previous_ctf_name = os.getenv("CTF_NAME", None)
_ctf_initialized = False


def initialize_ctf_if_needed():
    """Initialize CTF setup when CTF_NAME is set and pentestperf is available.

    Called at runtime (not import time) to avoid issues during test collection.
    """
    global ctf_global, messages_ctf, ctf_init, first_ctf_time, previous_ctf_name, _ctf_initialized

    if _ctf_initialized:
        return
    _ctf_initialized = True

    from cai import is_pentestperf_available
    from cai.util import setup_ctf

    if is_pentestperf_available() and os.getenv("CTF_NAME", None):
        try:
            from cai.caibench.ctf import CTFSetupError as _CTFSetupError

            _ctf_boot_exc: tuple = (ValueError, _CTFSetupError)
        except ImportError:
            _ctf_boot_exc = (ValueError,)
        try:
            ctf, messages_ctf_result = setup_ctf()
            ctf_global = ctf
            messages_ctf = messages_ctf_result
            ctf_init = 0
            first_ctf_time = True
        except _ctf_boot_exc as exc:
            print(f"CTF setup failed: {exc}", file=sys.stderr)
            exl = str(exc).lower()
            if type(exc).__name__ == "CTFSetupError" or any(
                k in exl for k in ("registry", "gitlab", "credential", "authenticate", "pull image")
            ):
                print(
                    "Pista: define CAIBENCH_IMG_REGISTRY_TOKEN (token GitLab con read_registry para "
                    "registry.gitlab.com) en .env o export; prueba: "
                    'echo "$CAIBENCH_IMG_REGISTRY_TOKEN" | docker login registry.gitlab.com '
                    "-u gitlab --password-stdin",
                    file=sys.stderr,
                )
            os.environ.pop("CTF_NAME", None)
            previous_ctf_name = None
            ctf_global = None
            messages_ctf = ""
            ctf_init = 1
            first_ctf_time = False


# ---------------------------------------------------------------------------
# 5. Log symlink helper
# ---------------------------------------------------------------------------

def create_last_log_symlink(log_filename):
    """Create a symbolic link ``last`` in ``~/.cai/logs`` pointing to the current log file."""
    try:
        from pathlib import Path

        from cai.util.config_utils import get_session_logs_dir

        if not log_filename:
            return
        log_path = Path(log_filename).resolve()
        if not log_path.exists():
            return

        logs_dir = get_session_logs_dir()
        symlink_path = logs_dir / "last"
        if symlink_path.exists() or symlink_path.is_symlink():
            symlink_path.unlink()
        symlink_path.symlink_to(log_path.name)
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Convenience: run everything in order
# ---------------------------------------------------------------------------

def _ensure_cai_dirs():
    """Create the standard ~/.cai/ directory tree on first run."""
    import os
    base = os.path.join(os.path.expanduser("~"), ".cai")
    for sub in ("workspace", "logs", "debug", "agents"):
        os.makedirs(os.path.join(base, sub), exist_ok=True)


def bootstrap():
    """Run all setup steps in the correct order. Call once at import time."""
    _ensure_cai_dirs()
    load_dotenv_and_defaults()
    configure_warnings()
    configure_loggers()
    suppress_aiohttp_warnings()
