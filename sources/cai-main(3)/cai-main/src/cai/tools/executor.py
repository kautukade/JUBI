"""Subprocess/PTY management, signal handling, and timeout logic.

Extracted from tools/common.py (3,343 LOC) as part of the core-engine refactor.
Contains ShellSession for interactive PTY sessions, local/CTF/SSH execution,
and the top-level run_command / run_command_async dispatchers.

Emits OutputManager events (ToolStartEvent, ToolCompleteEvent, ToolErrorEvent) [T].
"""

import subprocess  # nosec B404
import threading
import os

_CAI_DEBUG_DIR = os.path.join(os.path.expanduser("~"), ".cai", "debug")
import pty
import signal
import time
import uuid
import sys
import shlex
import select
from wasabi import color  # pylint: disable=import-error
from cai.util import (
    format_time,
    start_active_timer,
    stop_active_timer,
    start_idle_timer,
    stop_idle_timer,
    cli_print_tool_output,
)
# OutputManager integration [T] — emit events for tool lifecycle
from cai.output import OUTPUT, ToolStartEvent, ToolCompleteEvent, ToolErrorEvent
# CAIConfig integration [B] — centralized config replaces os.getenv
from cai.config import get_config as _get_config

# Instead of direct import
try:
    from cai.cli import START_TIME
except ImportError:
    START_TIME = None

# --- Sibling module imports ---
from cai.tools.streaming import (
    _get_idle_timeout,
    is_tool_streaming_enabled,
    _get_agent_token_info,
)
from cai.tools.container import (
    _get_workspace_dir,
    _get_container_workspace_path,
    _run_docker_async,
)


# ---------------------------------------------------------------------------
# Session management globals
# ---------------------------------------------------------------------------

ACTIVE_SESSIONS = {}
FRIENDLY_SESSION_MAP = {}
REVERSE_SESSION_MAP = {}
SESSION_COUNTER = 0
SESSION_OUTPUT_COUNTER = {}


# ---------------------------------------------------------------------------
# ShellSession
# ---------------------------------------------------------------------------

class ShellSession:  # pylint: disable=too-many-instance-attributes
    """Class to manage interactive shell sessions"""

    def __init__(self, command, session_id=None, ctf=None, workspace_dir=None, container_id=None):  # noqa E501
        self.session_id = session_id or str(uuid.uuid4())[:8]
        self.command = command
        self.ctf = ctf
        self.container_id = container_id
        if self.container_id:
            self.workspace_dir = _get_container_workspace_path()
        elif self.ctf:
            self.workspace_dir = workspace_dir or _get_workspace_dir()
        else:
            self.workspace_dir = _get_workspace_dir()
        self.friendly_id = None
        self.created_at = time.time()
        self.process = None
        self.master = None
        self.slave = None
        self.output_buffer = []
        self._buffer_lock = threading.Lock()
        self.is_running = False
        self.last_activity = time.time()

    def start(self):
        """Start the shell session in the appropriate environment."""
        start_message_cmd = self.command

        # --- Start in Container ---
        if self.container_id:
            try:
                self.master, self.slave = pty.openpty()
                docker_cmd_list = [
                    "docker", "exec", "-i", "-t",
                    "-w", self.workspace_dir,
                    self.container_id,
                    "sh", "-c",
                    self.command,
                ]
                self.process = subprocess.Popen(
                    docker_cmd_list,
                    stdin=self.slave, stdout=self.slave, stderr=self.slave,
                    preexec_fn=os.setsid, universal_newlines=True,
                )
                self.is_running = True
                with self._buffer_lock:
                    self.output_buffer.append(
                        f"[Session {self.session_id}] Started in container {self.container_id[:12]}: "
                        f"{start_message_cmd} in {self.workspace_dir}"
                    )
                threading.Thread(target=self._read_output, daemon=True).start()
                return None
            except Exception as e:
                with self._buffer_lock:
                    self.output_buffer.append(f"Error starting container session: {str(e)}")
                self.is_running = False
                return str(e)

        # --- Start in CTF ---
        if self.ctf:
            try:
                self.is_running = True
                with self._buffer_lock:
                    self.output_buffer.append(
                        f"[Session {self.session_id}] Started CTF command: {self.command}"
                    )
                output = self.ctf.get_shell(self.command)
                if output:
                    with self._buffer_lock:
                        self.output_buffer.append(output)
                self.is_running = False
                return None
            except Exception as e:  # pylint: disable=broad-except
                with self._buffer_lock:
                    self.output_buffer.append(f"Error executing CTF command: {str(e)}")
                self.is_running = False
                return str(e)

        # --- Start Locally (Host) ---
        try:
            self.master, self.slave = pty.openpty()
            self.process = subprocess.Popen(  # pylint: disable=subprocess-popen-preexec-fn, consider-using-with # noqa: E501
                self.command, shell=True,  # nosec B602
                stdin=self.slave, stdout=self.slave, stderr=self.slave,
                cwd=self.workspace_dir, preexec_fn=os.setsid, universal_newlines=True,
            )
            self.is_running = True
            with self._buffer_lock:
                self.output_buffer.append(f"[Session {self.session_id}] Started: {self.command}")
            threading.Thread(target=self._read_output, daemon=True).start()
        except Exception as e:  # pylint: disable=broad-except
            with self._buffer_lock:
                self.output_buffer.append(f"Error starting local session: {str(e)}")
            self.is_running = False
            return str(e)

    def _read_output(self):
        """Read output with non-blocking select"""
        start_time = time.time()
        max_lifetime = 3600  # 1 hour max session lifetime
        try:
            while self.is_running and self.master is not None:
                if time.time() - start_time > max_lifetime:
                    self.is_running = False
                    with self._buffer_lock:
                        self.output_buffer.append("\n[Session timed out after max lifetime]")
                    break
                try:
                    if self.process and self.process.poll() is not None:
                        self.is_running = False
                        break
                    ready, _, _ = select.select([self.master], [], [], 0.5)
                    if not ready:
                        if self.process and self.process.poll() is not None:
                            self.is_running = False
                            break
                        continue
                    output = os.read(self.master, 4096).decode('utf-8', errors='replace')
                    if output is not None and output != "":
                        with self._buffer_lock:
                            self.output_buffer.append(output)
                        self.last_activity = time.time()
                except OSError:
                    if self.process and self.process.poll() is not None:
                        self.is_running = False
                    break
                except Exception as read_err:
                    with self._buffer_lock:
                        self.output_buffer.append(f"Error reading output buffer: {str(read_err)}")
                    self.is_running = False
                    break
        except Exception as e:
            with self._buffer_lock:
                self.output_buffer.append(f"Error in read_output loop: {str(e)}")
            self.is_running = False
            return str(e)

    def is_process_running(self):
        """Check if the process is still running"""
        if self.container_id or self.ctf:
            return self.is_running
        if not self.process:
            return False
        return self.process.poll() is None

    def send_input(self, input_data):
        """Send input to the process (local or container)"""
        if not self.is_running:
            if self.process and self.process.poll() is None:
                self.is_running = True
            else:
                return "Session is not running"
        try:
            if self.ctf:
                output = self.ctf.get_shell(input_data)
                with self._buffer_lock:
                    self.output_buffer.append(output)
                return "Input sent to CTF session"
            if self.master is not None:
                input_data_bytes = (input_data.rstrip() + "\n").encode()
                bytes_written = os.write(self.master, input_data_bytes)
                if bytes_written != len(input_data_bytes):
                    with self._buffer_lock:
                        self.output_buffer.append(
                            f"[Session {self.session_id}] Warning: Partial input write."
                        )
                self.last_activity = time.time()
                return "Input sent to session"
            else:
                return "Session PTY not available for input"
        except Exception as e:  # pylint: disable=broad-except
            with self._buffer_lock:
                self.output_buffer.append(f"Error sending input: {str(e)}")
            return f"Error sending input: {str(e)}"

    def get_output(self, clear=True):
        """Get and optionally clear the output buffer"""
        with self._buffer_lock:
            output = "\n".join(self.output_buffer)
            if clear:
                self.output_buffer = []
        return output

    def get_new_output(self, mark_position=True):
        """Get only new output since last marked position"""
        with self._buffer_lock:
            if not hasattr(self, "_last_output_position"):
                self._last_output_position = 0
            new_output_lines = self.output_buffer[self._last_output_position:]
            new_output = "\n".join(new_output_lines)
            if mark_position:
                self._last_output_position = len(self.output_buffer)
        return new_output

    def terminate(self):
        """Terminate the session"""
        session_id_short = self.session_id[:8]
        termination_message = f"Session {session_id_short} terminated"

        if not self.is_running:
            if self.process and self.process.poll() is None:
                pass
            else:
                return f"Session {session_id_short} already terminated or finished."

        try:
            self.is_running = False
            if self.process:
                try:
                    pgid = os.getpgid(self.process.pid)
                    os.killpg(pgid, signal.SIGTERM)
                    # Wait up to 2 seconds for graceful shutdown
                    for _ in range(20):
                        if self.process.poll() is not None:
                            break
                        time.sleep(0.1)
                    # If still running, force kill
                    if self.process.poll() is None:
                        print(color(
                            f"Session {session_id_short} did not terminate gracefully, sending SIGKILL...",
                            fg="yellow",
                        ))
                        os.killpg(pgid, signal.SIGKILL)
                        time.sleep(0.5)
                except ProcessLookupError:
                    pass
                except Exception as term_err:
                    termination_message = f" (Error during termination: {term_err})"
                    try:
                        self.process.kill()
                    except Exception:
                        pass

                if self.process.poll() is None:
                    print(color(
                        f"Session {session_id_short} process {self.process.pid} may still be running after termination attempts.",
                        fg="red",
                    ))
                    termination_message += " (Warning: Process may still be running)"

            if self.master:
                try:
                    os.close(self.master)
                except OSError:
                    pass
                self.master = None
            if self.slave:
                try:
                    os.close(self.slave)
                except OSError:
                    pass
                self.slave = None

            return termination_message
        except Exception as e:  # pylint: disable=broad-except
            return f"Error terminating session {session_id_short}: {str(e)}"


# ---------------------------------------------------------------------------
# Session management helpers
# ---------------------------------------------------------------------------

def create_shell_session(command, ctf=None, container_id=None, workspace_dir=None, **kwargs):
    """Create a new shell session in the correct workspace/environment."""
    if container_id:
        session = ShellSession(command, ctf=ctf, container_id=container_id)
    else:
        wd = workspace_dir if workspace_dir is not None else _get_workspace_dir()
        session = ShellSession(command, ctf=ctf, workspace_dir=wd)

    session.start()
    if session.is_running or (ctf and not session.is_running):
        global SESSION_COUNTER
        SESSION_COUNTER += 1
        friendly = f"S{SESSION_COUNTER}"
        session.friendly_id = friendly
        ACTIVE_SESSIONS[session.session_id] = session
        FRIENDLY_SESSION_MAP[friendly] = session.session_id
        REVERSE_SESSION_MAP[session.session_id] = friendly
        return session.session_id
    else:
        error_msg = session.get_output(clear=True)
        print(color(f"Failed to start session: {error_msg}", fg="red"))
        return f"Failed to start session: {error_msg}"


def list_shell_sessions():
    """List all active shell sessions"""
    result = []
    for session_id, session in list(ACTIVE_SESSIONS.items()):
        if not session.is_running:
            del ACTIVE_SESSIONS[session_id]
            continue
        result.append({
            "friendly_id": getattr(session, 'friendly_id', None),
            "session_id": session_id,
            "command": session.command,
            "running": session.is_running,
            "last_activity": time.strftime("%H:%M:%S", time.localtime(session.last_activity))
        })
    return result


def _resolve_session_id(session_identifier):
    """Resolve a session identifier (real ID, friendly alias S1/#1/1, or 'last')."""
    if not session_identifier:
        return None
    sid = str(session_identifier).strip()
    key = sid
    if sid.lower() == 'last':
        if not ACTIVE_SESSIONS:
            return None
        latest = None
        latest_t = -1
        for _sid, sess in ACTIVE_SESSIONS.items():
            if hasattr(sess, 'created_at') and sess.created_at > latest_t and sess.is_running:
                latest = _sid
                latest_t = sess.created_at
        return latest or next(iter(ACTIVE_SESSIONS.keys()))
    if sid.startswith('#'):
        key = f"S{sid[1:]}"
    elif sid.isdigit():
        key = f"S{sid}"
    elif sid.upper().startswith('S') and sid[1:].isdigit():
        key = sid.upper()
    if sid in ACTIVE_SESSIONS:
        return sid
    if key in FRIENDLY_SESSION_MAP:
        return FRIENDLY_SESSION_MAP[key]
    return None


def send_to_session(session_id, input_data):
    """Send input to a specific session"""
    resolved = _resolve_session_id(session_id)
    if not resolved or resolved not in ACTIVE_SESSIONS:
        return f"Session {session_id} not found"
    return ACTIVE_SESSIONS[resolved].send_input(input_data)


def get_session_output(session_id, clear=True, stdout=True):
    """Get output from a specific session"""
    resolved = _resolve_session_id(session_id)
    if not resolved or resolved not in ACTIVE_SESSIONS:
        return f"Session {session_id} not found"
    return ACTIVE_SESSIONS[resolved].get_output(clear)


def terminate_session(session_id):
    """Terminate a specific session"""
    resolved = _resolve_session_id(session_id)
    if not resolved or resolved not in ACTIVE_SESSIONS:
        return f"Session {session_id} not found or already terminated."
    session = ACTIVE_SESSIONS[resolved]
    result = session.terminate()
    if resolved in ACTIVE_SESSIONS:
        del ACTIVE_SESSIONS[resolved]
        friendly = REVERSE_SESSION_MAP.pop(resolved, None)
        if friendly:
            FRIENDLY_SESSION_MAP.pop(friendly, None)
    return result


# ---------------------------------------------------------------------------
# Unified async dispatcher
# ---------------------------------------------------------------------------

async def execute_generic_linux_command_async(
    command: str,
    *,
    interactive: bool = False,
    session_id: str | None = None,
    stream: bool = True,
    timeout: int = 100,
    tool_name: str = "generic_linux_command",
    call_id: str | None = None,
    workspace_dir: str | None = None,
):
    """Unified async dispatcher for generic_linux_command."""
    import asyncio as _asyncio

    command = str(command or "").strip()

    if interactive and not session_id:
        return await _asyncio.to_thread(
            run_command, command, None, False, True, None,
            timeout, stream, call_id, tool_name, None, workspace_dir,
        )

    if session_id:
        return await _asyncio.to_thread(
            run_command, command, None, False, False, session_id,
            timeout, stream, call_id, tool_name, None, workspace_dir,
        )

    return await run_command_async(
        command, ctf=None, stdout=False, async_mode=False, session_id=None,
        timeout=timeout, stream=stream, call_id=call_id,
        tool_name=tool_name, args=None, workspace_dir=workspace_dir,
    )


# ---------------------------------------------------------------------------
# Environment-specific runners
# ---------------------------------------------------------------------------

def _run_ctf(ctf, command, stdout=False, timeout=100, workspace_dir=None, stream=False):
    """Runs command in CTF env."""
    env_timeout = str(_get_config().tool_timeout) if _get_config().tool_timeout else None
    if env_timeout:
        try:
            timeout = int(env_timeout)
        except ValueError:
            pass
    target_dir = workspace_dir or _get_workspace_dir()
    full_command = f"{command}"
    original_cmd_for_msg = command
    context_msg = f"(ctf:{target_dir})"
    try:
        output = ctf.get_shell(full_command, timeout=timeout)
        if stdout and not stream:
            print(f"\033[32m{context_msg} $ {original_cmd_for_msg}\n{output}\033[0m")
        return output
    except Exception as e:  # pylint: disable=broad-except
        error_msg = f"Error executing CTF command '{original_cmd_for_msg}' in '{target_dir}': {e}"
        print(color(error_msg, fg="red"))
        return error_msg


def _run_ssh(command, stdout=False, timeout=100, workspace_dir=None, stream=False):
    """Runs command via SSH."""
    env_timeout = str(_get_config().tool_timeout) if _get_config().tool_timeout else None
    if env_timeout:
        try:
            timeout = int(env_timeout)
        except ValueError:
            pass
    ssh_user = os.environ.get("SSH_USER")
    ssh_host = os.environ.get("SSH_HOST")
    ssh_pass = os.environ.get("SSH_PASS")
    remote_command = command
    original_cmd_for_msg = command
    context_msg = f"({ssh_user}@{ssh_host})"

    if ssh_pass:
        ssh_cmd_list = ["sshpass", "-p", ssh_pass, "ssh", f"{ssh_user}@{ssh_host}"]
    else:
        ssh_cmd_list = ["ssh", f"{ssh_user}@{ssh_host}"]
    ssh_cmd_list.append(remote_command)

    try:
        result = subprocess.run(
            ssh_cmd_list, capture_output=True, text=True,
            check=False, timeout=timeout,
        )
        output = result.stdout if result.stdout else result.stderr
        if stdout and not stream:
            print(f"\033[32m{context_msg} $ {original_cmd_for_msg}\n{output}\033[0m")
        return output.strip()
    except subprocess.TimeoutExpired as e:
        error_output = e.stdout if e.stdout else str(e)
        timeout_msg = f"Timeout executing SSH command: {error_output}"
        if stdout and not stream:
            print(f"\033[33m{context_msg} $ {original_cmd_for_msg}\nTIMEOUT\n{error_output}\033[0m")
        return timeout_msg
    except FileNotFoundError:
        error_msg = "'sshpass' or 'ssh' command not found. Ensure they are installed and in PATH."
        print(color(error_msg, fg="red"))
        return error_msg
    except Exception as e:  # pylint: disable=broad-except
        error_msg = f"Error executing SSH command '{original_cmd_for_msg}' on {ssh_host}: {e}"
        print(color(error_msg, fg="red"))
        return error_msg


# ---------------------------------------------------------------------------
# _run_local_async and _run_local are large functions that remain faithful
# to the original common.py implementation.  They are imported verbatim
# rather than re-implemented to preserve exact behavior.
# ---------------------------------------------------------------------------
# NOTE: Due to their size (~600 LOC each) and tight coupling with TUI/
# streaming subsystems, they are kept as-is from the original monolith.
# Future refactoring should extract the TUI capture setup into its own helper.
# ---------------------------------------------------------------------------

# We import the original implementations at the bottom of common.py's shim,
# but the actual code lives here.  For the initial extraction we embed them
# directly so the module is self-contained.

# _run_local_async is defined below -- it is an exact copy from common.py
# with bare `except:` blocks replaced by `except Exception:`.

async def _run_local_async(
    command, stdout=False, timeout=100, stream=False,
    call_id=None, tool_name=None, workspace_dir=None, custom_args=None,
):
    """Async local command execution with streaming and TUI capture support."""
    import asyncio

    stop_idle_timer()
    start_active_timer()

    process_start_time = time.time()
    try:
        target_dir = workspace_dir or _get_workspace_dir()

        # Sudo interception — validate credentials before subprocess creation.
        # If validation succeeds (returns None), OS caches the credentials
        # and the normal streaming path executes the command with sudo.
        # If validation fails, returns a fallback string to use instead.
        from cai.util.user_prompts import is_sudo_command, ensure_sudo_credentials
        if is_sudo_command(command):
            token_info = _get_agent_token_info()
            result = await asyncio.to_thread(
                ensure_sudo_credentials, command, target_dir, timeout,
                tool_name, token_info,
            )
            if result is not None:
                # Auth failed — return fallback message, skip execution
                stop_active_timer()
                start_idle_timer()
                return result
            # result is None → credentials validated, proceed normally

        original_cmd_for_msg = command
        context_msg = f"(local:{target_dir})"

        terminal_id = None
        capture = None
        capture_id = None
        tool_args = None
        token_info = None

        if _get_config().tui_enabled:
            try:
                from cai.tui.core.terminal_tracking import get_current_terminal_id
                from cai.tui.core.terminal_tracking_async import get_current_terminal_id_async
                from cai.tui.display.live_output_capture import get_live_output_capture

                with open(f"{_CAI_DEBUG_DIR}/cai_live_capture_debug.log", "a") as f:
                    import datetime
                    f.write(f"\n[{datetime.datetime.now().isoformat()}] TUI mode check in _run_local_async\n")
                    f.write(f"  command: {command[:100]}\n")
                    f.write(f"  stream: {stream}\n")
                    f.write(f"  tool_name: {tool_name}\n")

                terminal_id = get_current_terminal_id_async() or get_current_terminal_id()

                if not terminal_id:
                    token_info = _get_agent_token_info()
                    terminal_id = token_info.get('terminal_id')
                    if not terminal_id and token_info.get('terminal_number'):
                        terminal_id = f"terminal-{token_info['terminal_number']}"

                with open(f"{_CAI_DEBUG_DIR}/cai_live_capture_debug.log", "a") as f:
                    f.write(f"  terminal_id resolved: {terminal_id}\n")
                    f.write(f"  from async context: {get_current_terminal_id_async()}\n")
                    f.write(f"  from thread local: {get_current_terminal_id()}\n")
                if terminal_id:
                    capture = get_live_output_capture()
                    from cai.tui.core.terminal_console import get_terminal_output
                    terminal_output = get_terminal_output(terminal_id)
                    if terminal_output:
                        capture.register_terminal(terminal_id, terminal_output)
                        with open(f"{_CAI_DEBUG_DIR}/cai_live_capture_debug.log", "a") as f:
                            f.write(f"  terminal_id found: {terminal_id}\n")
                            f.write(f"  terminal_output registered: {terminal_output is not None}\n")
                    else:
                        with open(f"{_CAI_DEBUG_DIR}/cai_live_capture_debug.log", "a") as f:
                            f.write(f"  ERROR: No terminal_output found for {terminal_id}\n")

                    if not tool_name:
                        parts = command.strip().split(" ", 1)
                        tool_name = f"{parts[0]}_command" if parts else "command"

                    parts = command.strip().split(" ", 1)
                    tool_args = {
                        "command": parts[0] if parts else command,
                        "args": parts[1] if len(parts) > 1 else "",
                        "workspace": os.path.basename(target_dir),
                        "full_command": command,
                    }
                    if custom_args and isinstance(custom_args, dict):
                        tool_args.update(custom_args)

                    token_info = _get_agent_token_info()
                    capture_id = capture.start_capture(terminal_id, tool_name, tool_args)
            except Exception as e:
                import traceback
                with open(f"{_CAI_DEBUG_DIR}/cai_live_capture_error.log", "a") as f:
                    f.write(f"\n[{time.strftime('%Y-%m-%d %H:%M:%S')}] Failed to set up live capture\n")
                    f.write(f"Error: {e}\nTraceback: {traceback.format_exc()}\n")

        if stream:
            from cai.util import start_tool_streaming, update_tool_streaming, finish_tool_streaming

            parts = command.strip().split(" ", 1)
            cmd_var = parts[0] if parts else ""
            args_param_val = parts[1] if len(parts) > 1 else ""

            if not tool_name:
                tool_name = f"{cmd_var}_command" if cmd_var else "command"

            tool_args = {}
            if cmd_var:
                tool_args["command"] = cmd_var
            if args_param_val and args_param_val.strip():
                tool_args["args"] = args_param_val
            tool_args["workspace"] = os.path.basename(target_dir)
            tool_args["full_command"] = command

            if custom_args is not None and isinstance(custom_args, dict):
                for key, value in custom_args.items():
                    tool_args[key] = value

            if not call_id:
                call_id = f"cmd_{cmd_var}_{str(uuid.uuid4())[:8]}"

            token_info = _get_agent_token_info()
            call_id = start_tool_streaming(tool_name, tool_args, call_id, token_info)
            capture_id = None  # Streaming handler owns action bar updates

            process = None
            try:
                process = await asyncio.create_subprocess_shell(
                    command, stdout=asyncio.subprocess.PIPE,
                    stderr=asyncio.subprocess.PIPE, cwd=target_dir,
                )
                output_buffer = []
                update_interval = 0.15

                def _format_countdown(elapsed, total_timeout):
                    remaining = max(0, total_timeout - elapsed)
                    return f"{total_timeout}s|{remaining:.1f}s"

                try:
                    async def read_and_stream():
                        nonlocal output_buffer
                        last_update_time = time.time()
                        start_time = time.time()

                        if _get_config().tui_enabled:
                            with open(f"{_CAI_DEBUG_DIR}/cai_timeout_debug.log", "a") as f:
                                import datetime
                                f.write(f"\n[{datetime.datetime.now()}] Starting command with timeout {timeout}s: {command[:50]}...\n")

                        last_output = time.time()
                        while True:
                            if process.returncode is not None:
                                try:
                                    remaining = await asyncio.wait_for(process.stdout.read(), timeout=0.5)
                                    if remaining:
                                        output_buffer.append(remaining.decode('utf-8', errors='replace'))
                                except asyncio.TimeoutError:
                                    pass
                                break
                            try:
                                line = await asyncio.wait_for(process.stdout.readline(), timeout=0.5)
                                if line:
                                    output_buffer.append(line.decode('utf-8', errors='replace'))
                                    last_output = time.time()
                                    current_time = time.time()
                                    if current_time - last_update_time >= update_interval:
                                        elapsed = current_time - start_time
                                        streaming_args = dict(tool_args)
                                        streaming_args["timeout_countdown"] = _format_countdown(elapsed, timeout)
                                        update_tool_streaming(tool_name, streaming_args, ''.join(output_buffer), call_id, token_info)
                                        last_update_time = current_time
                                else:
                                    break
                            except asyncio.TimeoutError:
                                idle_timeout = _get_idle_timeout()
                                if time.time() - last_output > idle_timeout:
                                    process.terminate()
                                    try:
                                        await asyncio.wait_for(process.wait(), timeout=1.0)
                                    except asyncio.TimeoutError:
                                        process.kill()
                                        await process.wait()
                                    output_buffer.append(f"\n[Terminated: idle {idle_timeout}s, likely waiting for input]")
                                    break

                        if process.returncode is None:
                            return_code = await process.wait()
                        else:
                            return_code = process.returncode
                        return return_code

                    if _get_config().tui_enabled:
                        with open(f"{_CAI_DEBUG_DIR}/cai_timeout_debug.log", "a") as f:
                            f.write(f"  Applying asyncio.wait_for with timeout={timeout}s\n")

                    return_code = await asyncio.wait_for(read_and_stream(), timeout=timeout)

                except asyncio.TimeoutError:
                    if _get_config().tui_enabled:
                        with open(f"{_CAI_DEBUG_DIR}/cai_timeout_debug.log", "a") as f:
                            f.write(f"  TIMEOUT TRIGGERED after {timeout}s! Killing process...\n")

                    process.kill()
                    try:
                        await asyncio.wait_for(process.wait(), timeout=5)
                    except asyncio.TimeoutError:
                        process.terminate()

                    partial_output = "".join(output_buffer) if 'output_buffer' in locals() else ""
                    timeout_msg = f"\n[Command timed out after {timeout} seconds]"

                    execution_info = {
                        "status": "timeout", "environment": "Local",
                        "host": os.path.basename(target_dir), "tool_time": timeout,
                    }
                    finish_tool_streaming(
                        tool_name, tool_args, partial_output + timeout_msg, call_id, execution_info, token_info
                    )
                    return partial_output + timeout_msg

                process_execution_time = time.time() - process_start_time

                stderr_data = await process.stderr.read()
                if stderr_data:
                    stderr_str = stderr_data.decode("utf-8", errors="replace")
                    output_buffer.append("\nERROR OUTPUT:\n" + stderr_str)

                final_output = "".join(output_buffer)
                if return_code != 0:
                    final_output += f"\nCommand exited with code {return_code}"

                execution_info = {
                    "status": "completed" if return_code == 0 else "error",
                    "return_code": return_code, "environment": "Local",
                    "host": os.path.basename(target_dir), "tool_time": process_execution_time,
                }
                tool_args["elapsed"] = f"{process_execution_time:.1f}s"
                finish_tool_streaming(
                    tool_name, tool_args, final_output, call_id, execution_info, token_info
                )
                return final_output

            except asyncio.CancelledError:
                if process and process.returncode is None:
                    process.kill()
                    try:
                        await process.wait()
                    except Exception:
                        pass
                execution_info = {
                    "status": "cancelled", "environment": "Local",
                    "host": os.path.basename(target_dir),
                    "tool_time": time.time() - process_start_time,
                }
                cancelled_output = "".join(output_buffer) if 'output_buffer' in locals() else ""
                cancelled_output += "\n[Execution cancelled]"
                finish_tool_streaming(
                    tool_name, tool_args, cancelled_output, call_id, execution_info, token_info
                )
                raise
        else:
            # Non-streaming async execution
            process_start_time = time.time()
            if capture_id:
                capture.append_output(capture_id, f"Executing: {command}\n", is_error=False)

            process = await asyncio.create_subprocess_shell(
                command, stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE, cwd=target_dir,
            )
            stdout_chunks, stderr_chunks = [], []
            last_output = time.time()
            start = time.time()

            try:
                while True:
                    if time.time() - start > timeout:
                        process.kill()
                        await process.wait()
                        raise subprocess.TimeoutExpired(command, timeout)
                    if process.returncode is not None:
                        try:
                            remaining_stdout = await asyncio.wait_for(process.stdout.read(), timeout=0.5)
                            if remaining_stdout:
                                stdout_chunks.append(remaining_stdout)
                        except asyncio.TimeoutError:
                            pass
                        try:
                            remaining_stderr = await asyncio.wait_for(process.stderr.read(), timeout=0.5)
                            if remaining_stderr:
                                stderr_chunks.append(remaining_stderr)
                        except asyncio.TimeoutError:
                            pass
                        break
                    try:
                        out_task = asyncio.create_task(process.stdout.read(4096))
                        err_task = asyncio.create_task(process.stderr.read(4096))
                        done, pending = await asyncio.wait(
                            [out_task, err_task], timeout=0.5, return_when=asyncio.FIRST_COMPLETED,
                        )
                        for task in pending:
                            task.cancel()
                        for task in done:
                            data = await task
                            if data:
                                (stdout_chunks if task == out_task else stderr_chunks).append(data)
                                last_output = time.time()
                                if capture_id:
                                    data_str = data.decode("utf-8", errors="replace")
                                    is_stderr = (task == err_task)
                                    capture.append_output(capture_id, data_str, is_error=is_stderr)
                    except asyncio.TimeoutError:
                        pass
                    idle_timeout = _get_idle_timeout()
                    if time.time() - last_output > idle_timeout:
                        try:
                            await asyncio.wait_for(process.wait(), timeout=0.1)
                            break
                        except asyncio.TimeoutError:
                            process.terminate()
                            try:
                                await asyncio.wait_for(process.wait(), timeout=1.0)
                            except asyncio.TimeoutError:
                                process.kill()
                                await process.wait()
                            stderr_chunks.append(f"\n[Terminated: idle {idle_timeout}s]".encode())
                            break
            except asyncio.CancelledError:
                # Ctrl+C / cancellation: kill the spawned subprocess so it
                # does not keep running in the background after the user
                # interrupts the agent.
                if process.returncode is None:
                    try:
                        process.kill()
                    except Exception:
                        pass
                    try:
                        await asyncio.wait_for(process.wait(), timeout=2.0)
                    except Exception:
                        pass
                raise

            stdout_data = b''.join(stdout_chunks)
            stderr_data = b''.join(stderr_chunks)
            output = stdout_data.decode('utf-8', errors='replace') if stdout_data else ""
            stderr_output = stderr_data.decode('utf-8', errors='replace') if stderr_data else ""

            if not output and stderr_output:
                output = stderr_output
            elif stderr_output:
                output += "\nERROR OUTPUT:\n" + stderr_output

            parts = command.strip().split(" ", 1)
            token_info = _get_agent_token_info()

            if terminal_id and token_info:
                token_info['terminal_id'] = terminal_id
                if terminal_id.startswith("terminal-") and terminal_id[9:].isdigit():
                    token_info['terminal_number'] = int(terminal_id[9:])

            is_parallel = False
            if token_info and token_info.get("agent_id"):
                agent_id = token_info.get("agent_id")
                if agent_id and agent_id.startswith("P") and agent_id[1:].isdigit():
                    if _get_config().parallel > 1:
                        is_parallel = True

            in_tui_mode = _get_config().tui_enabled
            streaming_enabled = is_tool_streaming_enabled()

            if in_tui_mode or (streaming_enabled and is_parallel):
                from cai.util import cli_print_tool_output
                execution_time = time.time() - process_start_time
                if not call_id:
                    cmd_name = parts[0] if parts else "cmd"
                    call_id = f"{cmd_name}_{str(uuid.uuid4())[:8]}"
                execution_info = {
                    "status": "completed" if process.returncode == 0 else "error",
                    "return_code": process.returncode, "environment": "Local",
                    "host": os.path.basename(target_dir), "tool_time": execution_time,
                }
                if terminal_id and token_info:
                    token_info['terminal_id'] = terminal_id
                    if terminal_id.startswith("terminal-") and terminal_id[9:].isdigit():
                        token_info['terminal_number'] = int(terminal_id[9:])
                cli_print_tool_output(
                    tool_name=tool_name or "generic_linux_command",
                    args={"command": parts[0] if parts else command,
                          "args": parts[1] if len(parts) > 1 else "",
                          "full_command": command,
                          "workspace": os.path.basename(target_dir)},
                    output=output.strip(), call_id=call_id,
                    execution_info=execution_info, token_info=token_info,
                    streaming=False,
                )

            if capture and capture_id:
                terminal_output = capture.finish_capture(capture_id, output.strip())
                if terminal_output and hasattr(terminal_output, "write"):
                    from cai.tui.display.panel_formatter import PanelFormatter
                    execution_time = time.time() - process_start_time
                    execution_info = {
                        "status": "completed" if process.returncode == 0 else "error",
                        "return_code": process.returncode, "environment": "Local",
                        "host": os.path.basename(target_dir), "tool_time": execution_time,
                    }
                    panel = PanelFormatter.create_tool_panel(
                        tool_name=tool_name or "generic_linux_command",
                        args=tool_args if 'tool_args' in locals() else {"command": command},
                        output=output.strip(), execution_info=execution_info,
                        token_info=token_info if 'token_info' in locals() else None,
                        streaming=False,
                    )
                    terminal_output.write(panel)
                    terminal_output.write("")

            return output.strip()

    except subprocess.TimeoutExpired as e:
        error_output = e.stdout if hasattr(e, "stdout") and e.stdout else str(e)
        error_msg = f"Command timed out after {timeout} seconds\n{error_output}"
        if stream and call_id:
            from cai.util import finish_tool_streaming
            parts = command.strip().split(" ", 1)
            cmd_var = parts[0] if parts else ""
            args_var = parts[1] if len(parts) > 1 else ""
            tool_args = {"command": cmd_var, "args": args_var if args_var.strip() else "",
                         "full_command": command, "environment": "Local",
                         "workspace": os.path.basename(target_dir)}
            execution_info = {"status": "timeout", "error": str(e),
                              "environment": "Local", "host": os.path.basename(target_dir)}
            token_info = _get_agent_token_info()
            finish_tool_streaming(
                tool_name or f"{cmd_var}_command", tool_args, error_msg,
                call_id, execution_info, token_info,
            )
        if stdout:
            print("\033[32m" + error_msg + "\033[0m")
        return error_msg
    except Exception as e:  # pylint: disable=broad-except
        error_msg = f"Error executing local command: {e}"
        if stream and call_id:
            from cai.util import finish_tool_streaming
            parts = command.strip().split(" ", 1)
            cmd_var = parts[0] if parts else ""
            args_var = parts[1] if len(parts) > 1 else ""
            tool_args = {"command": cmd_var, "args": args_var if args_var.strip() else "",
                         "full_command": command, "environment": "Local",
                         "workspace": os.path.basename(target_dir)}
            execution_info = {"status": "error", "error": str(e),
                              "environment": "Local", "host": os.path.basename(target_dir)}
            token_info = _get_agent_token_info()
            finish_tool_streaming(
                tool_name or f"{cmd_var}_command", tool_args, error_msg,
                call_id, execution_info, token_info,
            )
        print(color(error_msg, fg="red"))
        return error_msg
    finally:
        stop_active_timer()
        start_idle_timer()


def _run_local(
    command, stdout=False, timeout=100, stream=False,
    call_id=None, tool_name=None, workspace_dir=None, custom_args=None,
):
    """Runs command locally in the specified workspace_dir."""
    stop_idle_timer()
    start_active_timer()

    process_start_time = time.time()
    capture = None
    capture_id = None
    terminal_output = None
    if _get_config().tui_enabled:
        try:
            from cai.tui.core.terminal_tracking import get_current_terminal_id
            from cai.tui.display.live_output_capture import get_live_output_capture
            with open(f"{_CAI_DEBUG_DIR}/cai_live_capture_debug.log", "a") as f:
                import datetime
                f.write(f"\n[{datetime.datetime.now().isoformat()}] TUI mode check in _run_local (SYNC)\n")
                f.write(f"  command: {command[:100]}\n  stream: {stream}\n  tool_name: {tool_name}\n")
            terminal_id = get_current_terminal_id()
            if terminal_id:
                capture = get_live_output_capture()
                parts = command.strip().split(" ", 1)
                tool_args_capture = {
                    "command": parts[0] if parts else command,
                    "args": parts[1] if len(parts) > 1 else "",
                    "full_command": command,
                }
                capture_id = capture.start_capture(terminal_id, tool_name or "generic_linux_command", tool_args_capture)
        except ImportError:
            pass

    try:
        target_dir = workspace_dir or _get_workspace_dir()

        # Sudo interception — validate credentials before subprocess creation.
        from cai.util.user_prompts import is_sudo_command, ensure_sudo_credentials
        if is_sudo_command(command):
            token_info = _get_agent_token_info()
            result = ensure_sudo_credentials(
                command, target_dir, timeout, tool_name, token_info,
            )
            if result is not None:
                stop_active_timer()
                start_idle_timer()
                return result
            # result is None → credentials validated, proceed normally

        original_cmd_for_msg = command
        context_msg = f"(local:{target_dir})"

        if stream:
            from cai.util import start_tool_streaming, update_tool_streaming, finish_tool_streaming
            parts = command.strip().split(" ", 1)
            cmd_var = parts[0] if parts else ""
            args_param_val = parts[1] if len(parts) > 1 else ""

            if not tool_name:
                tool_name = f"{cmd_var}_command" if cmd_var else "command"

            tool_args = {}
            if cmd_var:
                tool_args["command"] = cmd_var
            if args_param_val and args_param_val.strip():
                tool_args["args"] = args_param_val
            tool_args["workspace"] = os.path.basename(target_dir)
            tool_args["full_command"] = command

            if custom_args is not None and isinstance(custom_args, dict):
                for key, value in custom_args.items():
                    tool_args[key] = value

            if not call_id:
                call_id = f"cmd_{cmd_var}_{str(uuid.uuid4())[:8]}"

            token_info = _get_agent_token_info()
            call_id = start_tool_streaming(tool_name, tool_args, call_id, token_info)

            process = subprocess.Popen(
                command, shell=True,  # nosec B602
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, bufsize=1, cwd=target_dir,
            )

            output_buffer = []
            buffer_size = 0
            update_interval = 3 if tool_name == "generic_linux_command" else 10

            def _format_countdown(elapsed, total_timeout):
                remaining = max(0, total_timeout - elapsed)
                return f"{total_timeout}s|{remaining:.1f}s"

            for line in iter(process.stdout.readline, ""):
                if not line:
                    break
                output_buffer.append(line)
                buffer_size += 1
                if buffer_size >= update_interval:
                    current_output = "".join(output_buffer)
                    elapsed = time.time() - process_start_time
                    streaming_args = dict(tool_args)
                    streaming_args["timeout_countdown"] = _format_countdown(elapsed, timeout)
                    update_tool_streaming(tool_name, streaming_args, current_output, call_id, token_info)
                    buffer_size = 0

            process.stdout.close()
            return_code = process.wait(timeout=timeout)
            process_execution_time = time.time() - process_start_time

            stderr_data = process.stderr.read()
            if stderr_data:
                output_buffer.append("\nERROR OUTPUT:\n" + stderr_data)

            final_output = "".join(output_buffer)
            if return_code != 0:
                final_output += f"\nCommand exited with code {return_code}"

            execution_info = {
                "status": "completed" if return_code == 0 else "error",
                "return_code": return_code, "environment": "Local",
                "host": os.path.basename(target_dir), "tool_time": process_execution_time,
            }
            finish_tool_streaming(tool_name, tool_args, final_output, call_id, execution_info, token_info)
            return final_output
        else:
            if capture_id and _get_config().tui_enabled:
                try:
                    from cai.tui.display.execution_interceptor import run_with_live_capture
                    result = run_with_live_capture(
                        command, tool_name=tool_name or "generic_linux_command",
                        args=custom_args or {"command": command},
                        shell=True, text=True, timeout=timeout, cwd=target_dir,
                    )
                    output = result.stdout if result.stdout else result.stderr
                except ImportError:
                    result = subprocess.run(command, shell=True, capture_output=True,  # nosec B602
                                            text=True, check=False, timeout=timeout, cwd=target_dir)
                    output = result.stdout if result.stdout else result.stderr
            else:
                result = subprocess.run(command, shell=True, capture_output=True,  # nosec B602
                                        text=True, check=False, timeout=timeout, cwd=target_dir)
                output = result.stdout if result.stdout else result.stderr

            parts = command.strip().split(" ", 1)
            token_info = _get_agent_token_info()

            is_parallel = False
            if token_info and token_info.get("agent_id"):
                agent_id = token_info.get("agent_id")
                if agent_id and agent_id.startswith("P") and agent_id[1:].isdigit():
                    if _get_config().parallel > 1:
                        is_parallel = True

            in_tui_mode = _get_config().tui_enabled
            streaming_enabled = is_tool_streaming_enabled()

            if in_tui_mode or (streaming_enabled and is_parallel):
                from cai.util import cli_print_tool_output
                execution_time = time.time() - process_start_time
                if not call_id:
                    cmd_name = parts[0] if parts else "cmd"
                    call_id = f"{cmd_name}_{str(uuid.uuid4())[:8]}"
                execution_info = {
                    "status": "completed" if result.returncode == 0 else "error",
                    "return_code": result.returncode, "environment": "Local",
                    "host": os.path.basename(target_dir), "tool_time": execution_time,
                }
                display_args = custom_args if custom_args is not None else {
                    "command": parts[0] if parts else command,
                    "args": parts[1] if len(parts) > 1 else "",
                    "full_command": command, "workspace": os.path.basename(target_dir),
                }
                cli_print_tool_output(
                    tool_name=tool_name or "generic_linux_command",
                    args=display_args, output=output.strip(), call_id=call_id,
                    execution_info=execution_info, token_info=token_info, streaming=False,
                )
            return output.strip()
    except subprocess.TimeoutExpired as e:
        error_output = e.stdout if hasattr(e, "stdout") and e.stdout else str(e)
        error_msg = f"Command timed out after {timeout} seconds\n{error_output}"
        if stream and call_id:
            from cai.util import finish_tool_streaming
            parts = command.strip().split(" ", 1)
            cmd_var = parts[0] if parts else ""
            args_var = parts[1] if len(parts) > 1 else ""
            tool_args = {"command": cmd_var, "args": args_var if args_var.strip() else "",
                         "full_command": command, "environment": "Local",
                         "workspace": os.path.basename(target_dir)}
            execution_info = {"status": "timeout", "error": str(e),
                              "environment": "Local", "host": os.path.basename(target_dir)}
            token_info = _get_agent_token_info()
            finish_tool_streaming(
                tool_name or f"{cmd_var}_command", tool_args, error_msg,
                call_id, execution_info, token_info,
            )
        if stdout:
            print("\033[32m" + error_msg + "\033[0m")
            return error_msg
        return error_msg
    except Exception as e:  # pylint: disable=broad-except
        error_msg = f"Error executing local command: {e}"
        if stream and call_id:
            from cai.util import finish_tool_streaming
            parts = command.strip().split(" ", 1)
            cmd_var = parts[0] if parts else ""
            args_var = parts[1] if len(parts) > 1 else ""
            tool_args = {"command": cmd_var, "args": args_var if args_var.strip() else "",
                         "full_command": command, "environment": "Local",
                         "workspace": os.path.basename(target_dir)}
            execution_info = {"status": "error", "error": str(e),
                              "environment": "Local", "host": os.path.basename(target_dir)}
            token_info = _get_agent_token_info()
            finish_tool_streaming(
                tool_name or f"{cmd_var}_command", tool_args, error_msg,
                call_id, execution_info, token_info,
            )
        print(color(error_msg, fg="red"))
        return error_msg
    finally:
        if capture and capture_id:
            try:
                terminal_output = capture.finish_capture(capture_id, output if 'output' in locals() else "")
                if terminal_output and hasattr(terminal_output, "write") and not stream:
                    from cai.tui.display.panel_formatter import PanelFormatter
                    execution_time = time.time() - process_start_time
                    parts = command.strip().split(" ", 1)
                    execution_info = {
                        "status": "completed" if 'result' in locals() and result.returncode == 0 else "error",
                        "return_code": result.returncode if 'result' in locals() else -1,
                        "environment": "Local",
                        "host": os.path.basename(target_dir) if 'target_dir' in locals() else "",
                        "tool_time": execution_time,
                    }
                    panel = PanelFormatter.create_tool_panel(
                        tool_name=tool_name or "generic_linux_command",
                        args=custom_args or {"command": command},
                        output=output.strip() if 'output' in locals() else "",
                        execution_info=execution_info, token_info=_get_agent_token_info(),
                        streaming=False,
                    )
                    terminal_output.write(panel)
                    terminal_output.write("")
            except Exception:
                pass
        stop_active_timer()
        start_idle_timer()


# ---------------------------------------------------------------------------
# Top-level dispatchers: run_command_async and run_command
# These are imported from common.py -- keeping the original implementation.
# ---------------------------------------------------------------------------

# To avoid duplicating the massive run_command/run_command_async bodies here
# AND in common.py, these functions are defined once in this module and
# common.py re-exports them.  See common.py for the backward-compat shim.

async def run_command_async(
    command, ctf=None, stdout=False, async_mode=False, session_id=None,
    timeout=None, stream=False, call_id=None, tool_name=None, args=None,
    workspace_dir=None,
):
    """Async command dispatcher -- routes to Docker/CTF/SSH/Local backends.

    Emits ToolStartEvent at dispatch and ToolCompleteEvent/ToolErrorEvent on finish [T].
    """
    if timeout is None:
        env_timeout = str(_get_config().tool_timeout) if _get_config().tool_timeout else None
        if env_timeout:
            try:
                timeout = int(env_timeout)
            except ValueError:
                timeout = 100
        else:
            timeout = 100

    if ctf and not hasattr(ctf, "get_shell"):
        ctf = None

    parts = command.strip().split(" ", 1)
    cmd_name = parts[0] if parts else ""

    if not call_id and stream:
        call_id = f"cmd_{cmd_name}_{str(uuid.uuid4())[:8]}"
    if not tool_name:
        tool_name = f"{cmd_name}_command" if cmd_name else "command"

    # Emit ToolStartEvent [T]
    OUTPUT.emit(ToolStartEvent(tool_name=tool_name, call_id=call_id or ""))

    _start_ts = time.time()

    from cai.cli import ctf_global
    ctf = ctf_global

    # Wrap dispatch in try/except to emit ToolComplete/ToolError events [T]
    try:
        if session_id:
            import asyncio, functools
            loop = asyncio.get_event_loop()
            func = functools.partial(
                run_command, command, ctf, stdout, async_mode, session_id,
                timeout, stream, call_id, tool_name, args,
            )
            result = await loop.run_in_executor(None, func)
            OUTPUT.emit(ToolCompleteEvent(
                tool_name=tool_name, call_id=call_id or "",
                output=str(result)[:500] if result else "",
                duration_seconds=time.time() - _start_ts,
            ))
            return result

        # Determine container
        active_container = ""
        try:
            token_info = _get_agent_token_info()
            agent_id_env = agent_name_env = None
            if token_info:
                if token_info.get("agent_id"):
                    agent_id_env = os.getenv(f"CAI_ACTIVE_CONTAINER_FOR_{token_info.get('agent_id')}", "")
                if token_info.get("agent_name"):
                    import re
                    safe_name = re.sub(r"[^A-Za-z0-9_]+", "_", str(token_info.get("agent_name")))
                    agent_name_env = os.getenv(f"CAI_ACTIVE_CONTAINER_FOR_NAME_{safe_name}", "")
            active_container = agent_id_env or agent_name_env or (_get_config().active_container or "")
            if not active_container:
                active_container = os.getenv("CAI_ACTIVE_CONTAINER_DEFAULT", "")
        except Exception:
            active_container = _get_config().active_container or ""
        _cfg = _get_config()
        is_ssh_env = bool(_cfg.ssh_user and _cfg.ssh_host)

        if active_container and not is_ssh_env:
            result = await _run_docker_async(
                command, container_id=active_container, stdout=stdout, timeout=timeout,
                stream=stream, call_id=call_id, tool_name=tool_name, args=args,
            )
            OUTPUT.emit(ToolCompleteEvent(
                tool_name=tool_name, call_id=call_id or "",
                output=str(result)[:500] if result else "",
                duration_seconds=time.time() - _start_ts,
            ))
            return result

        if ctf and _get_config().ctf_inside:
            import asyncio, functools
            loop = asyncio.get_event_loop()
            func = functools.partial(_run_ctf, ctf, command, stdout, timeout, _get_workspace_dir(), stream)
            result = await loop.run_in_executor(None, func)
            OUTPUT.emit(ToolCompleteEvent(
                tool_name=tool_name, call_id=call_id or "",
                output=str(result)[:500] if result else "",
                duration_seconds=time.time() - _start_ts,
            ))
            return result

        if is_ssh_env:
            import asyncio, functools
            loop = asyncio.get_event_loop()
            func = functools.partial(_run_ssh, command, stdout, timeout, _get_workspace_dir(), stream)
            result = await loop.run_in_executor(None, func)
            OUTPUT.emit(ToolCompleteEvent(
                tool_name=tool_name, call_id=call_id or "",
                output=str(result)[:500] if result else "",
                duration_seconds=time.time() - _start_ts,
            ))
            return result

        local_cwd = workspace_dir if workspace_dir is not None else _get_workspace_dir()
        result = await _run_local_async(
            command, stdout=stdout, timeout=timeout, stream=stream,
            call_id=call_id, tool_name=tool_name,
            workspace_dir=local_cwd, custom_args=args,
        )

        # Post-execution sudo elevation: if the command failed because it
        # needed root privileges, OPTIONALLY offer the user to authenticate
        # and re-run. Disabled by default (opt-in) because the interactive
        # getpass prompt silently hijacks user keystrokes typed for the next
        # CAI prompt, manifesting as "Enter doesn't work" after long agent
        # turns. The agent still sees the permission-denied error and can
        # re-issue the command with an explicit ``sudo`` prefix.
        import asyncio as _aio
        from cai.util.user_prompts import is_sudo_command as _is_sudo, output_needs_sudo, prompt_sudo_elevation
        _cops_no_sudo = os.getenv("CAI_CONTINUOUS_OPS_NO_SUDO", "").strip().lower() in ("1", "true", "yes")
        _auto_sudo = os.getenv("CAI_AUTO_SUDO_ELEVATION", "").strip().lower() in ("1", "true", "yes", "on")
        if _auto_sudo and result and not _is_sudo(command) and not _cops_no_sudo:
            if output_needs_sudo(result):
                elevated = await _aio.to_thread(
                    prompt_sudo_elevation, command, local_cwd,
                )
                if elevated:
                    result = await _run_local_async(
                        elevated, stdout=stdout, timeout=timeout,
                        stream=stream, call_id=None, tool_name=tool_name,
                        workspace_dir=local_cwd, custom_args=args,
                    )

        from cai.tools.evidence.capture_notice import apply_packet_capture_notice

        if isinstance(result, str):
            result = apply_packet_capture_notice(command, result)

        OUTPUT.emit(ToolCompleteEvent(
            tool_name=tool_name, call_id=call_id or "",
            output=str(result)[:500] if result else "",
            duration_seconds=time.time() - _start_ts,
        ))
        return result

    except Exception as exc:
        OUTPUT.emit(ToolErrorEvent(
            tool_name=tool_name, call_id=call_id or "",
            error=str(exc)[:500], error_type=type(exc).__name__,
        ))
        raise


def run_command(
    command, ctf=None, stdout=False, async_mode=False, session_id=None,
    timeout=None, stream=False, call_id=None, tool_name=None, args=None,
    workspace_dir=None,
):
    """Sync command dispatcher -- routes to Docker/CTF/SSH/Local backends."""
    if timeout is None:
        env_timeout = str(_get_config().tool_timeout) if _get_config().tool_timeout else None
        if env_timeout:
            try:
                timeout = int(env_timeout)
            except ValueError:
                timeout = 100
        else:
            timeout = 100

    if ctf and not hasattr(ctf, "get_shell"):
        ctf = None
    stop_idle_timer()
    start_active_timer()

    from cai.cli import ctf_global
    ctf = ctf_global

    parts = command.strip().split(" ", 1)
    cmd_name = parts[0] if parts else ""
    cmd_args = parts[1] if len(parts) > 1 else ""

    if not call_id and stream:
        call_id = f"cmd_{cmd_name}_{str(uuid.uuid4())[:8]}"
    if not tool_name:
        tool_name = f"{cmd_name}_command" if cmd_name else "command"

    try:
        # --- Session routing ---
        if session_id:
            resolved_session_id = _resolve_session_id(session_id)
            if not resolved_session_id or resolved_session_id not in ACTIVE_SESSIONS:
                stop_active_timer(); start_idle_timer()
                return f"Session {session_id} not found"
            session = ACTIVE_SESSIONS[resolved_session_id]
            session.send_input(command)

            wait_time = _get_config().session_input_wait

            session.get_new_output(mark_position=True)
            max_wait = wait_time
            check_interval = 0.2
            elapsed = 0.0
            new_output_detected = False
            while elapsed < max_wait:
                time.sleep(check_interval)
                elapsed += check_interval
                current_new_output = session.get_new_output(mark_position=False)
                if current_new_output and current_new_output.strip():
                    new_output_detected = True
                    time.sleep(0.3)
                    break

            counter_key = f"session_input_{resolved_session_id}"
            if counter_key not in SESSION_OUTPUT_COUNTER:
                SESSION_OUTPUT_COUNTER[counter_key] = 0
            SESSION_OUTPUT_COUNTER[counter_key] += 1

            env_type = "Local"
            if session.container_id:
                env_type = f"Container({session.container_id[:12]})"
            elif session.ctf:
                env_type = "CTF"

            label = getattr(session, 'friendly_id', None) or resolved_session_id
            session_args = {
                "command": command, "args": "", "session_id": session_id,
                "call_counter": SESSION_OUTPUT_COUNTER[counter_key],
                "input_to_session": True, "environment": env_type,
            }
            if args and isinstance(args, dict) and "auto_output" in args:
                session_args["auto_output"] = args["auto_output"]
            else:
                session_args["auto_output"] = True

            output = session.get_new_output(mark_position=True)
            try:
                full_state = session.get_output(clear=False)
            except Exception:
                full_state = output

            execution_info = {
                "status": "completed", "environment": env_type,
                "host": session.workspace_dir, "session_id": label,
                "wait_time": elapsed, "new_output_detected": new_output_detected,
            }

            from cai.util import cli_print_tool_output
            cli_print_tool_output(
                tool_name="generic_linux_command",
                args={**session_args, "full_state": full_state, "new_output": output},
                output=output, execution_info=execution_info,
                token_info=_get_agent_token_info(), streaming=False,
            )

            if not async_mode:
                stop_active_timer(); start_idle_timer()
            if output and output.strip():
                return output
            return f"Command sent to session {label}. No output captured."

        # --- Environment detection (via CAIConfig) ---
        _cfg_sync = _get_config()
        active_container = _cfg_sync.active_container or ""
        is_ssh_env = bool(_cfg_sync.ssh_user and _cfg_sync.ssh_host)

        # --- Docker ---
        if active_container and not is_ssh_env:
            container_id = active_container
            container_workspace = _get_container_workspace_path()
            context_msg = f"(docker:{container_id[:12]}:{container_workspace})"

            if async_mode and not session_id:
                new_session_id = create_shell_session(command, container_id=container_id)
                if "Failed" in new_session_id:
                    stop_active_timer(); start_idle_timer()
                    return new_session_id
                from cai.util import cli_print_tool_output
                label = getattr(ACTIVE_SESSIONS.get(new_session_id), 'friendly_id', None) or new_session_id
                session = ACTIVE_SESSIONS.get(new_session_id)
                initial_output = ""
                if session:
                    time.sleep(0.2)
                    initial_output = session.get_new_output(mark_position=True)
                output_msg = f"Started async session {label} in container {container_id[:12]}. Use this ID to interact."
                if initial_output:
                    output_msg += f"\n\n{initial_output}"
                cli_print_tool_output(
                    tool_name="generic_linux_command",
                    args={"command": command, "args": "", "session_id": label, "async_mode": True},
                    output=output_msg,
                    execution_info={"status": "session_created", "environment": f"Container({container_id[:12]})",
                                    "host": container_workspace, "session_id": label},
                    token_info=_get_agent_token_info(), streaming=False,
                )
                stop_active_timer(); start_idle_timer()
                return f"Started async session {label} in container {container_id[:12]}. Use this ID to interact."

            if stream:
                from cai.util import start_tool_streaming, update_tool_streaming, finish_tool_streaming
                if args is not None:
                    tool_args = args.copy() if isinstance(args, dict) else {"args": str(args)}
                    tool_args["container"] = container_id[:12]
                    tool_args["environment"] = "Container"
                    tool_args["workspace"] = container_workspace
                    tool_args["full_command"] = command
                else:
                    tool_args = {"command": cmd_name, "args": cmd_args if cmd_args.strip() else "",
                                 "full_command": command, "container": container_id[:12],
                                 "environment": "Container", "workspace": container_workspace}
                if tool_name == "generic_linux_command":
                    tool_args["refresh_rate"] = 2
                token_info = _get_agent_token_info()
                call_id = start_tool_streaming(tool_name, tool_args, call_id, token_info)
                update_tool_streaming(tool_name, tool_args, f"Executing: {command}", call_id, token_info)
                mkdir_cmd = ["docker", "exec", container_id, "mkdir", "-p", container_workspace]
                subprocess.run(mkdir_cmd, capture_output=True, text=True, check=False, timeout=10)
                docker_exec_cmd = (
                    "docker exec -w "
                    f"{shlex.quote(container_workspace)} "
                    f"{shlex.quote(container_id)} sh -c "
                    f"{shlex.quote(command)}"
                )
                try:
                    start_time = time.time()
                    process = subprocess.Popen(
                        docker_exec_cmd, shell=True, stdout=subprocess.PIPE,  # nosec B602
                        stderr=subprocess.PIPE, text=True, bufsize=1, cwd=_get_workspace_dir(),
                    )
                    output_buffer = []
                    buffer_size = 0
                    update_interval = 10
                    for line in iter(process.stdout.readline, ""):
                        if not line:
                            break
                        output_buffer.append(line)
                        buffer_size += 1
                        if buffer_size >= update_interval:
                            token_info = _get_agent_token_info()
                            update_tool_streaming(tool_name, tool_args, "".join(output_buffer), call_id, token_info)
                            buffer_size = 0
                    process.stdout.close()
                    return_code = process.wait(timeout=timeout)
                    execution_time = time.time() - start_time
                    stderr_data = process.stderr.read()
                    if stderr_data:
                        output_buffer.append("\nERROR OUTPUT:\n" + stderr_data)
                    final_output = "".join(output_buffer)
                    if return_code != 0:
                        final_output += f"\nCommand exited with code {return_code}"
                    execution_info = {"status": "completed" if return_code == 0 else "error",
                                      "return_code": return_code, "environment": "Container",
                                      "host": container_id[:12], "tool_time": execution_time}
                    finish_tool_streaming(tool_name, tool_args, final_output, call_id, execution_info, token_info)
                    stop_active_timer(); start_idle_timer()
                    return final_output
                except subprocess.TimeoutExpired as e:
                    error_output = e.stdout if hasattr(e, "stdout") and e.stdout else str(e)
                    error_msg = f"Command timed out after {timeout} seconds\n{error_output}"
                    finish_tool_streaming(tool_name, tool_args, error_msg, call_id,
                                          {"status": "timeout", "environment": "Container",
                                           "host": container_id[:12], "error": str(e)}, token_info)
                    stop_active_timer(); start_idle_timer()
                    print(color("Container execution timed out. Attempting execution on host instead.", fg="yellow"))
                    return _run_local(command, stdout, timeout, False, None, tool_name, _get_workspace_dir(), args)
                except Exception as e:
                    error_msg = f"Error executing command in container: {str(e)}"
                    finish_tool_streaming(tool_name, tool_args, error_msg, call_id,
                                          {"status": "error", "environment": "Container",
                                           "host": container_id[:12], "error": str(e)}, token_info)
                    stop_active_timer(); start_idle_timer()
                    print(color("Container execution failed. Attempting execution on host instead.", fg="yellow"))
                    return _run_local(command, stdout, timeout, False, None, tool_name, _get_workspace_dir(), args)

            # Sync non-streaming container execution
            process_start_time = time.time()
            try:
                mkdir_cmd = ["docker", "exec", container_id, "mkdir", "-p", container_workspace]
                subprocess.run(mkdir_cmd, capture_output=True, text=True, check=False, timeout=10)
                cmd_list = ["docker", "exec", "-w", container_workspace, container_id, "sh", "-c", command]
                result = subprocess.run(cmd_list, capture_output=True, text=True, check=False, timeout=timeout)
                output = result.stdout if result.stdout else result.stderr
                output = output.strip()
                if stdout and not stream:
                    print(f"\033[32m{context_msg} $ {command}\n{output}\033[0m")
                if result.returncode != 0 and "is not running" in result.stderr:
                    print(color(f"{context_msg} Container is not running. Attempting execution on host instead.", fg="yellow"))
                    stop_active_timer(); start_idle_timer()
                    return _run_local(command, stdout, timeout, stream, call_id, tool_name, _get_workspace_dir(), args)
                if not stream:
                    token_info = _get_agent_token_info()
                    is_parallel = False
                    if token_info and token_info.get("agent_id"):
                        aid = token_info.get("agent_id")
                        if aid and aid.startswith("P") and aid[1:].isdigit() and _get_config().parallel > 1:
                            is_parallel = True
                    if _get_config().tui_enabled or (is_tool_streaming_enabled() and is_parallel):
                        from cai.util import cli_print_tool_output
                        execution_time = time.time() - process_start_time if "process_start_time" in locals() else 0
                        parts = command.strip().split(" ", 1)
                        if not call_id:
                            call_id = f"container_{parts[0] if parts else 'cmd'}_{str(uuid.uuid4())[:8]}"
                        display_args = args if args is not None else {
                            "command": parts[0] if parts else command,
                            "args": parts[1] if len(parts) > 1 else "",
                            "full_command": command, "container": container_id[:12], "workspace": container_workspace,
                        }
                        cli_print_tool_output(
                            tool_name=tool_name or "generic_linux_command", args=display_args, output=output,
                            call_id=call_id, execution_info={
                                "status": "completed" if result.returncode == 0 else "error",
                                "return_code": result.returncode, "environment": "Container",
                                "host": container_id[:12], "tool_time": execution_time,
                            }, token_info=token_info, streaming=False,
                        )
                stop_active_timer(); start_idle_timer()
                return output
            except subprocess.TimeoutExpired:
                if stdout:
                    print(f"\033[33m{context_msg} $ {command}\nTIMEOUT\033[0m")
                    print(color("Attempting execution on host instead.", fg="yellow"))
                stop_active_timer(); start_idle_timer()
                return _run_local(command, stdout, timeout, stream, call_id, tool_name, _get_workspace_dir(), args)
            except Exception as e:  # pylint: disable=broad-except
                print(color(f"{context_msg} Error executing command in container: {str(e)}", fg="red"))
                print(color("Attempting execution on host instead.", fg="yellow"))
                stop_active_timer(); start_idle_timer()
                return _run_local(command, stdout, timeout, stream, call_id, tool_name, _get_workspace_dir(), args)

        # --- CTF ---
        if ctf and _get_config().ctf_inside:
            if stream:
                from cai.util import start_tool_streaming, update_tool_streaming, finish_tool_streaming
                if args is not None:
                    tool_args = args.copy() if isinstance(args, dict) else {"args": str(args)}
                    tool_args["environment"] = "CTF"
                    tool_args["workspace"] = os.path.basename(_get_workspace_dir())
                    tool_args["full_command"] = command
                else:
                    tool_args = {"command": cmd_name, "args": cmd_args if cmd_args.strip() else "",
                                 "full_command": command, "environment": "CTF",
                                 "workspace": os.path.basename(_get_workspace_dir())}
                if tool_name == "generic_linux_command":
                    tool_args["refresh_rate"] = 2
                token_info = _get_agent_token_info()
                call_id = start_tool_streaming(tool_name, tool_args, call_id, token_info)
                full_command = command
                update_tool_streaming(tool_name, tool_args,
                                      f"Executing in CTF environment: {full_command}\n\nWaiting for response...",
                                      call_id, token_info)
                try:
                    start_time = time.time()
                    output = ctf.get_shell(full_command, timeout=timeout)
                    execution_time = time.time() - start_time
                    finish_tool_streaming(tool_name, tool_args, output, call_id,
                                          {"status": "completed", "environment": "CTF", "tool_time": execution_time}, token_info)
                    stop_active_timer(); start_idle_timer()
                    return output
                except Exception as e:
                    error_msg = f"Error executing CTF command: {str(e)}"
                    finish_tool_streaming(tool_name, tool_args, error_msg, call_id,
                                          {"status": "error", "environment": "CTF", "error": str(e)}, token_info)
                    stop_active_timer(); start_idle_timer()
                    return error_msg
            else:
                result = _run_ctf(ctf, command, stdout, timeout, _get_workspace_dir(), stream)
                stop_active_timer(); start_idle_timer()
                return result

        # --- SSH ---
        if is_ssh_env:
            if stream:
                from cai.util import start_tool_streaming, update_tool_streaming, finish_tool_streaming
                ssh_user = os.environ.get("SSH_USER", "user")
                ssh_host = os.environ.get("SSH_HOST", "host")
                ssh_connection = f"{ssh_user}@{ssh_host}"
                if args is not None:
                    tool_args = args.copy() if isinstance(args, dict) else {"args": str(args)}
                    tool_args["ssh_host"] = ssh_connection
                    tool_args["environment"] = "SSH"
                    tool_args["full_command"] = command
                else:
                    tool_args = {"command": cmd_name, "args": cmd_args if cmd_args.strip() else "",
                                 "full_command": command, "ssh_host": ssh_connection, "environment": "SSH"}
                if tool_name == "generic_linux_command":
                    tool_args["refresh_rate"] = 2
                token_info = _get_agent_token_info()
                call_id = start_tool_streaming(tool_name, tool_args, call_id, token_info)
                update_tool_streaming(tool_name, tool_args,
                                      f"Executing on {ssh_connection}: {command}\n\nWaiting for response...",
                                      call_id, token_info)
                try:
                    ssh_pass = os.environ.get("SSH_PASS")
                    ssh_cmd_list = (["sshpass", "-p", ssh_pass, "ssh", ssh_connection] if ssh_pass
                                    else ["ssh", ssh_connection])
                    ssh_cmd_list.append(command)
                    start_time = time.time()
                    result = subprocess.run(ssh_cmd_list, capture_output=True, text=True, check=False, timeout=timeout)
                    execution_time = time.time() - start_time
                    output = result.stdout if result.stdout else result.stderr
                    result_with_info = f"Command executed on {ssh_connection}:\n\n{output}"
                    token_info = _get_agent_token_info()
                    finish_tool_streaming(tool_name, tool_args, result_with_info, call_id,
                                          {"status": "completed" if result.returncode == 0 else "error",
                                           "environment": "SSH", "host": ssh_connection,
                                           "return_code": result.returncode, "tool_time": execution_time}, token_info)
                    stop_active_timer(); start_idle_timer()
                    return output.strip()
                except subprocess.TimeoutExpired as e:
                    error_output = e.stdout if e.stdout else str(e)
                    error_msg = f"Command timed out after {timeout} seconds\n{error_output}"
                    token_info = _get_agent_token_info()
                    finish_tool_streaming(tool_name, tool_args, error_msg, call_id,
                                          {"status": "timeout", "environment": "SSH",
                                           "host": ssh_connection, "error": str(e)}, token_info)
                    stop_active_timer(); start_idle_timer()
                    return error_msg
                except Exception as e:
                    error_msg = f"Error executing SSH command: {str(e)}"
                    token_info = _get_agent_token_info()
                    finish_tool_streaming(tool_name, tool_args, error_msg, call_id,
                                          {"status": "error", "environment": "SSH",
                                           "host": ssh_connection, "error": str(e)}, token_info)
                    stop_active_timer(); start_idle_timer()
                    return error_msg
            else:
                result = _run_ssh(command, stdout, timeout, _get_workspace_dir(), stream)
                stop_active_timer(); start_idle_timer()
                return result

        # --- Local (default fallback) ---
        if async_mode and not session_id:
            local_cwd = workspace_dir if workspace_dir is not None else _get_workspace_dir()
            new_session_id = create_shell_session(command, workspace_dir=local_cwd)
            if isinstance(new_session_id, str) and "Failed" in new_session_id:
                stop_active_timer(); start_idle_timer()
                return new_session_id
            from cai.util import cli_print_tool_output
            session = ACTIVE_SESSIONS.get(new_session_id)
            actual_workspace = session.workspace_dir if session else "unknown"
            label = getattr(session, 'friendly_id', None) or new_session_id
            initial_output = ""
            if session:
                time.sleep(0.2)
                initial_output = session.get_new_output(mark_position=True)
            output_msg = f"Started async session {label} locally. Use this ID to interact."
            if initial_output:
                output_msg += f"\n\n{initial_output}"
            cli_print_tool_output(
                tool_name="generic_linux_command",
                args={"command": command, "args": "", "session_id": label, "async_mode": True},
                output=output_msg,
                execution_info={"status": "session_created", "environment": "Local",
                                "host": os.path.basename(actual_workspace), "session_id": label},
                token_info=_get_agent_token_info(), streaming=False,
            )
            stop_active_timer(); start_idle_timer()
            return f"Started async session {label} locally. Use this ID to interact."

        local_cwd = workspace_dir if workspace_dir is not None else _get_workspace_dir()
        result = _run_local(
            command, stdout, timeout, stream=stream, call_id=call_id,
            tool_name=tool_name, workspace_dir=local_cwd, custom_args=args,
        )

        # Post-execution sudo elevation (sync path). Opt-in via
        # ``CAI_AUTO_SUDO_ELEVATION`` for the same reason as the async path:
        # the interactive getpass silently hijacks the user's next keystrokes.
        from cai.util.user_prompts import is_sudo_command, output_needs_sudo, prompt_sudo_elevation
        _cops_no_sudo = os.getenv("CAI_CONTINUOUS_OPS_NO_SUDO", "").strip().lower() in ("1", "true", "yes")
        _auto_sudo = os.getenv("CAI_AUTO_SUDO_ELEVATION", "").strip().lower() in ("1", "true", "yes", "on")
        if _auto_sudo and result and not is_sudo_command(command) and not _cops_no_sudo:
            if output_needs_sudo(result):
                elevated = prompt_sudo_elevation(command, local_cwd)
                if elevated:
                    result = _run_local(
                        elevated, stdout, timeout, stream=stream, call_id=None,
                        tool_name=tool_name, workspace_dir=local_cwd,
                        custom_args=args,
                    )

        from cai.tools.evidence.capture_notice import apply_packet_capture_notice

        if isinstance(result, str):
            result = apply_packet_capture_notice(command, result)

        stop_active_timer(); start_idle_timer()
        return result
    except Exception as e:
        stop_active_timer(); start_idle_timer()
        raise
