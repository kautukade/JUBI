"""Backward-compatible re-export shim for tools/common.py.

The original 3,343-LOC monolith has been split into:
  - streaming.py   : Output streaming utilities (_get_idle_timeout, etc.)
  - container.py   : Docker/workspace helpers (_get_workspace_dir, etc.)
  - executor.py    : ShellSession, run_command, run_command_async, etc.

All public names are re-exported here so that existing imports like
``from cai.tools.common import run_command`` continue to work.
"""

# --- Streaming utilities ---
from cai.tools.streaming import (  # noqa: F401
    _get_idle_timeout,
    is_tool_streaming_enabled,
    _get_agent_token_info,
)

# --- Workspace / container helpers ---
from cai.tools.container import (  # noqa: F401
    _get_workspace_dir,
    _get_container_workspace_path,
    _run_docker_async,
)

# --- Sudo handling ---
from cai.util.user_prompts import (  # noqa: F401
    is_sudo_command,
    output_needs_sudo,
    ensure_sudo_credentials,
    prompt_sudo_elevation,
    run_sudo_command,
    clear_cached_password,
)

# --- Session management, execution ---
from cai.tools.executor import (  # noqa: F401
    ACTIVE_SESSIONS,
    FRIENDLY_SESSION_MAP,
    REVERSE_SESSION_MAP,
    SESSION_COUNTER,
    SESSION_OUTPUT_COUNTER,
    ShellSession,
    create_shell_session,
    list_shell_sessions,
    _resolve_session_id,
    send_to_session,
    get_session_output,
    terminate_session,
    execute_generic_linux_command_async,
    _run_ctf,
    _run_ssh,
    _run_local_async,
    _run_local,
    run_command_async,
    run_command,
)
