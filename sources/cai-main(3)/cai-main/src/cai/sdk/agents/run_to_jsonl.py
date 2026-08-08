"""
Data recorder
"""

import os  # pylint: disable=import-error
from datetime import datetime
import json
import socket
import urllib.request
import getpass
import platform
from urllib.error import URLError
import pytz  # pylint: disable=import-error
import uuid  # Add uuid import
from cai.util import get_active_time, get_idle_time
import atexit
from typing import Any, List, Dict, Tuple

# Global recorder instance for session-wide logging
_session_recorder = None


def _format_log_message(message, args) -> str:
    """Format a log message in the same printf-style way as :mod:`logging`.

    The OpenAI Agents SDK and other CAI modules use ``self.logger.warning(msg,
    arg1, arg2)`` expecting a ``logging.Logger``-compatible API. When the
    logger is a :class:`DataRecorder`, fall through to ``%``-formatting so we
    record the substituted message instead of crashing.
    """
    if not args:
        return str(message)
    try:
        return str(message) % args
    except Exception:
        try:
            return f"{message} | args={args!r}"
        except Exception:
            return str(message)


def get_session_recorder(workspace_name=None):
    """
    Get the global session recorder instance.
    If one doesn't exist, it will be created.

    Args:
        workspace_name (str | None): Optional workspace name.

    Returns:
        DataRecorder: The session recorder instance.
    """
    global _session_recorder

    # Check if session recording is disabled (e.g., during replay)
    if os.environ.get("CAI_DISABLE_SESSION_RECORDING", "").lower() == "true":
        return None

    if _session_recorder is None:
        _session_recorder = DataRecorder(workspace_name)
    return _session_recorder


class DataRecorder:  # pylint: disable=too-few-public-methods
    """
    Records training data from litellm.completion
    calls in OpenAI-like JSON format.

    Stores both input messages and completion
    responses during execution in a single JSONL file.
    """

    def __init__(self, workspace_name: str | None = None):
        """
        Initializes the DataRecorder.

        Args:
            workspace_name (str | None): The name of the current workspace.
        """
        # Generate a session ID that will be used for the entire session
        self.session_id = str(uuid.uuid4())

        # Track the last message to ensure it's logged
        self.last_assistant_message = None
        self.last_assistant_tool_calls = None
        self._last_message_logged = False
        self._session_end_logged = False

        log_dir = os.path.join(os.path.expanduser("~"), ".cai", "logs")
        os.makedirs(log_dir, exist_ok=True)

        # Get current username
        try:
            username = getpass.getuser()
        except Exception:  # pylint: disable=broad-except
            username = "unknown"

        # Get operating system and version information
        try:
            os_name = platform.system().lower()
            os_version = platform.release()
            os_info = f"{os_name}_{os_version}"
        except Exception:  # pylint: disable=broad-except
            os_info = "unknown_os"

        # Check internet connection and get public IP
        public_ip = "127.0.0.1"

        # Skip network check if disabled for faster startup
        if os.getenv("CAI_SKIP_NETWORK_CHECK", "false").lower() != "true":
            try:
                # Quick connection check with minimal traffic
                socket.create_connection(("1.1.1.1", 53), timeout=1)

                # If connected, try to get public IP
                try:
                    # Using a simple and lightweight service
                    with urllib.request.urlopen(  # nosec: B310
                        "https://api.ipify.org", timeout=2
                    ) as response:
                        public_ip = response.read().decode("utf-8")
                except (URLError, socket.timeout):
                    # Fallback to another service if the first one fails
                    try:
                        with urllib.request.urlopen(  # nosec: B310
                            "https://ifconfig.me", timeout=2
                        ) as response:
                            public_ip = response.read().decode("utf-8")
                    except (URLError, socket.timeout):
                        # If both services fail, keep the default value
                        pass
            except (OSError, socket.timeout, socket.gaierror):
                # No internet connection, keep the default value
                pass

        # Create filename with username, OS info, and IP
        timestamp = (
            datetime.now().astimezone(pytz.timezone("Europe/Madrid")).strftime("%Y%m%d_%H%M%S")
        )
        base_filename = f'cai_{self.session_id}_{timestamp}_{username}_{os_info}_{public_ip.replace(".", "_")}.jsonl'

        if workspace_name:
            self.filename = os.path.join(log_dir, f"{workspace_name}_{base_filename}")
        else:
            self.filename = os.path.join(log_dir, base_filename)

        # Inicializar el coste total acumulado
        self.total_cost = 0.0

        # Log the session start
        with open(self.filename, "a", encoding="utf-8") as f:
            session_start = {
                "event": "session_start",
                "timestamp": datetime.now().astimezone(pytz.timezone("Europe/Madrid")).isoformat(),
                "session_id": self.session_id,
                "alias_api_key": os.getenv("ALIAS_API_KEY", ""),
            }
            json.dump(session_start, f)
            f.write("\n")

    def rec_training_data(self, create_params, msg, total_cost=None, agent_name=None) -> None:
        """
        Records a single training data entry to the JSONL file

        Args:
            create_params: Parameters used for the LLM call
            msg: Response from the LLM
            total_cost: Optional total accumulated cost from CAI instance
            agent_name: Optional agent name/type for tracking
        """
        request_data = {
            "model": create_params["model"],
            "messages": create_params["messages"],
            "stream": create_params["stream"],
        }
        if "tools" in create_params:
            request_data.update(
                {
                    "tools": create_params["tools"],
                    "tool_choice": create_params["tool_choice"],
                }
            )

        # Get interaction cost - try COST_TRACKER first, then msg.cost
        interaction_cost = 0.0
        try:
            from cai.util import COST_TRACKER
            interaction_cost = getattr(COST_TRACKER, "last_interaction_cost", 0.0)
            if interaction_cost == 0.0:
                interaction_cost = getattr(COST_TRACKER, "interaction_cost", 0.0)
        except ImportError:
            pass
        # Fallback to msg.cost if COST_TRACKER didn't have it
        if interaction_cost == 0.0 and hasattr(msg, "cost"):
            interaction_cost = float(msg.cost) if msg.cost is not None else 0.0

        # Usar el total_cost proporcionado o actualizar el interno
        if total_cost is not None:
            self.total_cost = float(total_cost)
        else:
            self.total_cost += interaction_cost

        # Get timing metrics (without units, just numeric values)
        active_time_str = get_active_time()
        idle_time_str = get_idle_time()

        # Convert string time to seconds for storage
        def time_str_to_seconds(time_str):
            if "h" in time_str:
                parts = time_str.split()
                hours = float(parts[0].replace("h", ""))
                minutes = float(parts[1].replace("m", ""))
                seconds = float(parts[2].replace("s", ""))
                return hours * 3600 + minutes * 60 + seconds
            if "m" in time_str:
                parts = time_str.split()
                minutes = float(parts[0].replace("m", ""))
                seconds = float(parts[1].replace("s", ""))
                return minutes * 60 + seconds
            return float(time_str.replace("s", ""))

        active_time_seconds = time_str_to_seconds(active_time_str)
        idle_time_seconds = time_str_to_seconds(idle_time_str)

        # Get token usage from the usage object - handle both field names
        prompt_tokens = 0
        completion_tokens = 0
        total_tokens = 0
        cache_read_input_tokens = 0
        cache_creation_input_tokens = 0

        if hasattr(msg, "usage"):
            # Try input_tokens first (ResponseUsage)
            if hasattr(msg.usage, "input_tokens"):
                prompt_tokens = msg.usage.input_tokens
            # Fall back to prompt_tokens (ChatCompletion)
            elif hasattr(msg.usage, "prompt_tokens"):
                prompt_tokens = msg.usage.prompt_tokens

            # Try output_tokens first (ResponseUsage)
            if hasattr(msg.usage, "output_tokens"):
                completion_tokens = msg.usage.output_tokens
            # Fall back to completion_tokens (ChatCompletion)
            elif hasattr(msg.usage, "completion_tokens"):
                completion_tokens = msg.usage.completion_tokens

            # Get total tokens - calculate if not available
            if hasattr(msg.usage, "total_tokens"):
                total_tokens = msg.usage.total_tokens
            else:
                total_tokens = prompt_tokens + completion_tokens

            # Get cache tokens from msg.usage
            if hasattr(msg.usage, "cache_read_input_tokens"):
                cache_read_input_tokens = msg.usage.cache_read_input_tokens or 0
            if hasattr(msg.usage, "cache_creation_input_tokens"):
                cache_creation_input_tokens = msg.usage.cache_creation_input_tokens or 0

        # Try to get cache tokens from COST_TRACKER if not available from msg
        if cache_read_input_tokens == 0 or cache_creation_input_tokens == 0:
            try:
                from cai.util import COST_TRACKER
                if cache_read_input_tokens == 0:
                    cache_read_input_tokens = getattr(COST_TRACKER, "cache_read_tokens", 0) or 0
                if cache_creation_input_tokens == 0:
                    cache_creation_input_tokens = getattr(COST_TRACKER, "cache_creation_tokens", 0) or 0
            except ImportError:
                pass

        completion_data = {
            "id": msg.id,
            "object": "chat.completion",
            "created": int(datetime.now().timestamp()),
            "model": msg.model,
            "agent_name": agent_name if agent_name else "unknown",
            "messages": [
                {
                    "role": m.role,
                    "content": m.content,
                    "tool_calls": [t.model_dump() for t in (m.tool_calls or [])],  # pylint: disable=line-too-long  # noqa: E501
                }
                for m in msg.messages
            ]
            if hasattr(msg, "messages")
            else [],
            "choices": [
                {
                    "index": 0,
                    "message": {
                        "role": msg.choices[0].message.role
                        if hasattr(msg, "choices") and msg.choices
                        else "assistant",
                        "content": msg.choices[0].message.content
                        if hasattr(msg, "choices") and msg.choices
                        else None,
                        "tool_calls": [
                            t.model_dump() for t in (msg.choices[0].message.tool_calls or [])
                        ]
                        if hasattr(msg, "choices") and msg.choices
                        else [],  # pylint: disable=line-too-long  # noqa: E501
                    },
                    "finish_reason": msg.choices[0].finish_reason
                    if hasattr(msg, "choices") and msg.choices
                    else "stop",
                }
            ],
            "usage": {
                "prompt_tokens": prompt_tokens,
                "completion_tokens": completion_tokens,
                "total_tokens": total_tokens,
                "cache_read_input_tokens": cache_read_input_tokens,
                "cache_creation_input_tokens": cache_creation_input_tokens,
            },
            "cost": {"interaction_cost": interaction_cost, "total_cost": self.total_cost},
            "timing": {"active_seconds": active_time_seconds, "idle_seconds": idle_time_seconds},
            "timestamp_iso": datetime.now().astimezone(pytz.timezone("Europe/Madrid")).isoformat(),
        }

        # Append both request and completion to the instance's jsonl file
        with open(self.filename, "a", encoding="utf-8") as f:
            json.dump(request_data, f)
            f.write("\n")
            json.dump(completion_data, f)
            f.write("\n")

    def log_user_message(self, user_message):
        """
        Logs a user message to the JSONL file.

        Args:
            user_message: The message from the user to log
        """
        with open(self.filename, "a", encoding="utf-8") as f:
            user_data = {
                "event": "user_message",
                "timestamp": datetime.now().astimezone(pytz.timezone("Europe/Madrid")).isoformat(),
                "content": user_message,
            }
            json.dump(user_data, f)
            f.write("\n")

    def log_assistant_message(self, assistant_message, tool_calls=None):
        """
        Logs an assistant message to the JSONL file.

        Args:
            assistant_message: The message from the assistant to log
            tool_calls: Optional tool calls included in the assistant message
        """
        # Store the last message in case we need to log it at exit
        self.last_assistant_message = assistant_message
        self.last_assistant_tool_calls = tool_calls

        with open(self.filename, "a", encoding="utf-8") as f:
            assistant_data = {
                "event": "assistant_message",
                "timestamp": datetime.now().astimezone(pytz.timezone("Europe/Madrid")).isoformat(),
                "content": assistant_message,
            }
            if tool_calls:
                assistant_data["tool_calls"] = tool_calls
            json.dump(assistant_data, f)
            f.write("\n")

        # Mark that the message has been logged
        self._last_message_logged = True

    def log_session_end(self):
        """
        Logs the end of the session to the JSONL file.
        Includes timing metrics from active/idle time tracking.
        """
        # Set a flag to indicate we've already logged the session end
        self._session_end_logged = True

        try:
            from cai.util import get_active_time_seconds, get_idle_time_seconds, COST_TRACKER

            active_time = get_active_time_seconds()
            idle_time = get_idle_time_seconds()
            # Get the global session cost from COST_TRACKER
            session_cost = COST_TRACKER.session_total_cost
        except ImportError:
            active_time = 0.0
            idle_time = 0.0
            session_cost = self.total_cost

        with open(self.filename, "a", encoding="utf-8") as f:
            session_end = {
                "event": "session_end",
                "timestamp": datetime.now().astimezone(pytz.timezone("Europe/Madrid")).isoformat(),
                "session_id": self.session_id,
                "timing_metrics": {
                    "active_time_seconds": active_time,
                    "idle_time_seconds": idle_time,
                    "total_time_seconds": active_time + idle_time,
                    "active_percentage": round((active_time / (active_time + idle_time)) * 100, 2)
                    if (active_time + idle_time) > 0
                    else 0.0,
                },
                "cost": {
                    "total_cost": session_cost  # Use the global session cost
                },
            }
            json.dump(session_end, f)
            f.write("\n")

    def warning(self, message, *args, **kwargs):
        """Log a warning message; mimics ``logging.Logger.warning``."""
        self._log_event("warning", _format_log_message(message, args))

    def error(self, message, *args, **kwargs):
        """Log an error message; mimics ``logging.Logger.error``."""
        self._log_event("error", _format_log_message(message, args))

    def info(self, message, *args, **kwargs):
        """Log an info message; mimics ``logging.Logger.info``."""
        self._log_event("info", _format_log_message(message, args))

    def debug(self, message, *args, **kwargs):
        """Log a debug message; mimics ``logging.Logger.debug``."""
        self._log_event("debug", _format_log_message(message, args))

    def _log_event(self, level: str, message: str):
        """
        Helper method to log events with a specific level.
        
        Args:
            level: The log level (warning, error, info, debug)
            message: The message to log
        """
        with open(self.filename, "a", encoding="utf-8") as f:
            event_data = {
                "event": f"log_{level}",
                "timestamp": datetime.now().astimezone(pytz.timezone("Europe/Madrid")).isoformat(),
                "level": level,
                "message": message,
                "session_id": self.session_id,
            }
            json.dump(event_data, f)
            f.write("\n")


def _find_last_model_record_fast(file_path: str) -> Tuple[Dict, Dict]:
    """
    Efficiently find the model record with the most messages from a JSONL file.
    Uses pattern matching to find the best record, then parses only that one.
    Ignores records from "Summary Agent" as it's not a real agent.

    Args:
        file_path: Path to the JSONL file

    Returns:
        Tuple of (model_record, completion_record) or (None, None) if not found
    """
    # Try to use orjson for faster parsing
    try:
        import orjson
        json_loads = orjson.loads
    except ImportError:
        json_loads = lambda x: json.loads(x.decode('utf-8') if isinstance(x, bytes) else x)

    # Pattern matching - find the record with most "role": occurrences
    role_pattern = b'"role":'
    model_pattern = b'"model":'
    messages_pattern = b'"messages":'
    summary_agent_pattern = b'"Summary Agent"'

    best_line_bytes = None
    best_next_line_bytes = None
    best_estimated_count = 0
    bytes_since_last_model = 0
    found_any_model = False
    prev_line_bytes = None

    with open(file_path, 'rb') as f:
        lines = list(f)

    for i, line_bytes in enumerate(lines):
        # Quick pattern check
        if model_pattern not in line_bytes or messages_pattern not in line_bytes:
            bytes_since_last_model += len(line_bytes)
            continue

        # Skip Summary Agent records - check next line for agent_name
        if i + 1 < len(lines):
            next_line = lines[i + 1]
            if summary_agent_pattern in next_line:
                bytes_since_last_model += len(line_bytes)
                continue

        # Estimate message count by counting "role": occurrences
        estimated_count = line_bytes.count(role_pattern)

        if estimated_count > best_estimated_count:
            best_estimated_count = estimated_count
            best_line_bytes = line_bytes
            best_next_line_bytes = lines[i + 1] if i + 1 < len(lines) else None

        found_any_model = True
        bytes_since_last_model = 0

    if best_line_bytes is None:
        return None, None

    # Parse the best record
    try:
        best_model_record = json_loads(best_line_bytes)
    except:
        return None, None

    # Parse completion record for agent_name
    completion_record = None
    if best_next_line_bytes:
        try:
            completion_record = json_loads(best_next_line_bytes)
        except:
            pass

    return best_model_record, completion_record


def _deduplicate_messages_fast(messages: List[Dict]) -> List[Dict]:
    """
    Deduplicate messages from resumed sessions.

    In resumed sessions, the model record contains the full history from the
    previous session plus new messages. This can result in duplicate messages
    appearing twice when the history is replayed.

    Strategy:
    Detect where the session was resumed by finding the point where user messages
    repeat, then keep only the messages from the resumed session onwards.
    This preserves the complete conversation flow without duplicates.
    """
    if not messages or len(messages) < 4:
        return messages

    # Find user messages and their positions
    user_positions = []
    for i, msg in enumerate(messages):
        if isinstance(msg, dict) and msg.get("role") == "user":
            content = str(msg.get("content", ""))
            user_positions.append((i, content))

    # Look for pattern where user messages repeat (indicating session resume)
    # Pattern: user1, [assistant/tool]*, user1, ...
    if len(user_positions) >= 2:
        # Check if the first user message appears again later
        first_user_content = user_positions[0][1]
        for j in range(1, len(user_positions)):
            if user_positions[j][1] == first_user_content:
                # Found potential resume point
                resume_start_idx = user_positions[j][0]

                # Verify this is a true resume by checking if:
                # 1. There are assistant/tool messages between the first user and this one
                # 2. If there are more users after the first, they should match too
                messages_between = resume_start_idx - user_positions[0][0]
                if messages_between > 1:  # At least some messages between
                    # Check if subsequent users also match (if they exist)
                    is_resume = True
                    users_before_resume = j  # Number of users before resume point
                    users_after_resume = len(user_positions) - j  # Users from resume onwards

                    # If we have multiple users before resume, verify pattern matches
                    if users_before_resume >= 2 and users_after_resume >= 2:
                        for k in range(min(users_before_resume, users_after_resume)):
                            if user_positions[k][1] != user_positions[j + k][1]:
                                is_resume = False
                                break

                    if is_resume:
                        # Keep only messages from resume point onwards
                        return messages[resume_start_idx:]

    # No resume pattern detected, return as-is
    return messages


def load_history_from_jsonl(file_path: str, system_prompt: bool = False, truncate_tool_responses: bool = False, max_tool_response_chars: int = 500, use_last_record_optimization: bool = True) -> List[Dict]:
    """
    Load conversation history from JSONL using only model and completion records.

    See FORMAT.md for more details on the different formats.

    This implementation ignores event records to avoid confusion and ensures
    we get the complete conversation history as it was sent to and received from
    the models.

    For large files (>10MB), an optimization is used that reads only the last
    model/completion record pair, since each record contains the full message
    history up to that point.

    Args:
        file_path: Path to the JSONL file
        system_prompt: Whether to include system prompts
        truncate_tool_responses: Whether to truncate tool response content to reduce token usage
        max_tool_response_chars: Maximum characters for tool responses when truncating (default: 500)
        use_last_record_optimization: Whether to use optimization for large files (default: True)

    Returns:
        List of message dictionaries in conversation order
    """
    file_path = os.path.normpath(os.path.expanduser(str(file_path).strip()))

    records = []

    # Try fast loading for large files
    fast_path_used = False
    if use_last_record_optimization:
        # Find the model record with the most messages directly
        model_record, completion_record = _find_last_model_record_fast(file_path)

        if model_record:
            # Fast path: directly extract messages from the model record
            fast_path_used = True
            messages = []
            model_messages = model_record.get("messages", [])
            agent_name = completion_record.get("agent_name", "Agent") if completion_record else "Agent"

            # Extract all messages directly from the model record
            for msg in model_messages:
                if not isinstance(msg, dict):
                    continue
                role = msg.get("role")
                if role == "system" and not system_prompt:
                    continue

                # Process message
                processed_msg = msg.copy()

                # Handle content that's a list (structured content)
                content = processed_msg.get("content", "")
                if isinstance(content, list):
                    # Extract text from structured content
                    text_parts = []
                    for item in content:
                        if isinstance(item, dict) and item.get("type") == "text":
                            text_parts.append(item.get("text", ""))
                        elif isinstance(item, str):
                            text_parts.append(item)
                    processed_msg["content"] = "\n".join(text_parts)

                    # Check for cache_control
                    for item in content:
                        if isinstance(item, dict) and item.get("cache_control"):
                            processed_msg["has_cache_control"] = True
                            break

                # Truncate tool responses if requested
                if role == "tool" and truncate_tool_responses:
                    tool_content = processed_msg.get("content", "")
                    if len(tool_content) > max_tool_response_chars:
                        processed_msg["content"] = tool_content[:max_tool_response_chars] + "... [TRUNCATED]"

                # Add agent name to assistant messages
                if role == "assistant" and not processed_msg.get("agent_name"):
                    processed_msg["agent_name"] = agent_name

                messages.append(processed_msg)

            # Add the final assistant message from completion record
            if completion_record and "choices" in completion_record:
                choice = completion_record["choices"][0]
                if "message" in choice:
                    response_msg = choice["message"].copy()
                    response_msg["agent_name"] = agent_name

                    # Extract token usage
                    usage = completion_record.get("usage", {})
                    if usage:
                        response_msg["input_tokens"] = usage.get("prompt_tokens", 0)
                        response_msg["output_tokens"] = usage.get("completion_tokens", 0)
                        if usage.get("cache_read_input_tokens", 0) > 0:
                            response_msg["cache_read_tokens"] = usage.get("cache_read_input_tokens", 0)
                        if usage.get("cache_creation_input_tokens", 0) > 0:
                            response_msg["cache_creation_tokens"] = usage.get("cache_creation_tokens", 0)

                    # Only add if not duplicate of last message
                    if not messages or not (
                        messages[-1].get("role") == "assistant" and
                        messages[-1].get("content") == response_msg.get("content")
                    ):
                        messages.append(response_msg)

            # Deduplicate messages for resumed sessions (fast path)
            # In resumed sessions, the model record may contain duplicate messages
            # from the previous session history
            messages = _deduplicate_messages_fast(messages)

            return messages

    # Load all records if fast loading didn't work
    if not records:
        with open(file_path, 'r') as f:
            for line in f:
                if line.strip():
                    try:
                        records.append(json.loads(line))
                    except Exception as e:
                        print(f"Error loading line: {e}")
                        continue
    
    # Simple approach: collect all messages and system prompts, then deduplicate
    messages = []
    system_prompts_by_agent = {}
    tool_outputs = {}
    is_resumed_session_log = False  # Track if this is a resumed session log
    event_messages = []  # Collect messages from event records as fallback

    i = 0
    while i < len(records):
        record = records[i]

        # Handle event records - extract messages from user_message and assistant_message events
        if "event" in record:
            event_type = record.get("event")
            if event_type == "user_message" and record.get("content"):
                event_messages.append({
                    "role": "user",
                    "content": record.get("content"),
                    "timestamp": record.get("timestamp")
                })
            elif event_type == "assistant_message" and record.get("content"):
                msg = {
                    "role": "assistant",
                    "content": record.get("content"),
                    "timestamp": record.get("timestamp")
                }
                if record.get("tool_calls"):
                    msg["tool_calls"] = record.get("tool_calls")
                event_messages.append(msg)
            elif event_type == "tool_message" and record.get("content"):
                tool_call_id = record.get("tool_call_id", "")
                if tool_call_id:
                    tool_outputs[tool_call_id] = record.get("content")
                    event_messages.append({
                        "role": "tool",
                        "tool_call_id": tool_call_id,
                        "content": record.get("content"),
                        "timestamp": record.get("timestamp")
                    })
            i += 1
            continue

        # Per-line messages from /save (and legacy /history export): role at root, optional agent/tools
        if (
            "role" in record
            and "messages" not in record
            and "model" not in record
            and "event" not in record
            and "choices" not in record
            and record.get("object") not in ("chat.completion", "chat.completion.chunk")
        ):
            flat_msg: Dict[str, Any] = {
                "role": record.get("role", "unknown"),
                "content": record.get("content", ""),
            }
            if record.get("tool_calls"):
                flat_msg["tool_calls"] = record["tool_calls"]
            if record.get("tool_call_id"):
                flat_msg["tool_call_id"] = record["tool_call_id"]
            agent_label = record.get("agent")
            if agent_label:
                flat_msg["agent_name"] = agent_label
            messages.append(flat_msg)
            i += 1
            continue

        # Handle simple format: {"messages": [...]} without "model" key
        # This is used in some older training data exports
        if "messages" in record and "model" not in record and "object" not in record:
            simple_messages = record["messages"]
            for msg in simple_messages:
                if not isinstance(msg, dict):
                    continue
                role = msg.get("role")
                if role == "system" and not system_prompt:
                    continue
                # Skip if it's a duplicate of the last message
                if messages and messages[-1].get("role") == role and messages[-1].get("content") == msg.get("content"):
                    continue
                messages.append(msg.copy())
            i += 1
            continue

        # Process model record (format with model + messages)
        if "model" in record and "messages" in record:
            model_messages = record["messages"]

            # Get agent name from next completion record (Format 1) or infer from system message (Format 2)
            agent_name = None
            if i + 1 < len(records) and records[i + 1].get("agent_name"):
                agent_name = records[i + 1]["agent_name"]
            elif i + 1 < len(records):
                agent_name = "Agent"  # Default agent name

            # Skip Summary Agent records - they are not real conversation messages
            if agent_name and agent_name.lower() == "summary agent":
                i += 2 if i + 1 < len(records) and "choices" in records[i + 1] else 1
                continue

            # Check if there's a completion record following this model record
            has_completion = i + 1 < len(records) and "choices" in records[i + 1]

            # Check for cache information in usage from completion record
            cache_read_tokens = 0
            cache_creation_tokens = 0
            if i + 1 < len(records) and "usage" in records[i + 1]:
                usage = records[i + 1]["usage"]
                cache_read_tokens = usage.get("cache_read_input_tokens", 0)
                cache_creation_tokens = usage.get("cache_creation_input_tokens", 0)

            # Detect if this is a "resumed session" log where model_messages contains
            # the full conversation history (many messages in few records)
            # vs "incremental" logs where each model_record adds new messages
            if len(model_messages) > 100 and len(records) < 30:
                is_resumed_session_log = True

            # Process messages
            for msg in model_messages:
                # Skip non-dictionary items (strings, etc.)
                if not isinstance(msg, dict):
                    continue

                role = msg.get("role")

                if role == "system" and system_prompt and agent_name:
                    # Store system prompt for this agent
                    system_prompts_by_agent[agent_name] = msg.get("content", "")
                elif role == "tool":
                    # Store tool output
                    tool_id = msg.get("tool_call_id")
                    if tool_id:
                        content = msg.get("content", "")
                        # Truncate tool responses if requested
                        if truncate_tool_responses and len(content) > max_tool_response_chars:
                            content = content[:max_tool_response_chars] + "... [TRUNCATED]"
                        tool_outputs[tool_id] = content
                    # For resumed session logs, also add tool messages directly
                    if is_resumed_session_log:
                        messages.append(msg)
                elif role == "user":
                    # User messages from model.messages are the actual prompts
                    # Check for cache_control in content (list format)
                    content = msg.get("content", "")
                    if isinstance(content, list):
                        for item in content:
                            if isinstance(item, dict) and item.get("cache_control"):
                                msg["has_cache_control"] = True
                                break
                    messages.append(msg)
                elif role == "assistant":
                    # For resumed session logs, add all assistant messages
                    # For incremental logs, only add if no completion record
                    if is_resumed_session_log or not has_completion:
                        messages.append(msg)
            
            # Process completion record if it exists (only if model record doesn't have assistant message)
            if i + 1 < len(records) and "choices" in records[i + 1]:
                next_record = records[i + 1]
                choice = next_record["choices"][0]
                if "message" in choice:
                    response_msg = choice["message"].copy()
                    # Handle both Format 1 (with agent_name) and Format 2 (without agent_name)
                    if next_record.get("agent_name") and response_msg.get("role") == "assistant":
                        response_msg["agent_name"] = next_record["agent_name"]
                    elif response_msg.get("role") == "assistant" and agent_name:
                        # For Format 2, use the inferred agent name
                        response_msg["agent_name"] = agent_name

                    # Extract token usage from completion record
                    usage = next_record.get("usage", {})
                    if usage:
                        response_msg["input_tokens"] = usage.get("prompt_tokens", 0)
                        response_msg["output_tokens"] = usage.get("completion_tokens", 0)
                        # Extract cache information
                        if usage.get("cache_read_input_tokens", 0) > 0:
                            response_msg["cache_read_tokens"] = usage.get("cache_read_input_tokens", 0)
                        if usage.get("cache_creation_input_tokens", 0) > 0:
                            response_msg["cache_creation_tokens"] = usage.get("cache_creation_input_tokens", 0)

                    # Extract cost information from completion record
                    cost = next_record.get("cost", {})
                    if cost:
                        if cost.get("interaction_cost", 0) > 0:
                            response_msg["interaction_cost"] = cost.get("interaction_cost", 0)
                        if cost.get("total_cost", 0) > 0:
                            response_msg["total_cost"] = cost.get("total_cost", 0)

                    # Only add if this exact assistant message is not already in model record
                    is_duplicate = any(
                        isinstance(msg, dict) and
                        msg.get("role") == "assistant" and
                        msg.get("content") == response_msg.get("content") and
                        str(msg.get("tool_calls", [])) == str(response_msg.get("tool_calls", []))
                        for msg in model_messages
                    )
                    if not is_duplicate:
                        messages.append(response_msg)
                i += 1  # Skip the completion record
        
        i += 1
    
    # Simple deduplication: remove exact duplicates but preserve user messages that trigger different agents
    # Skip deduplication for resumed session logs - messages are already in correct order
    if is_resumed_session_log:
        unique_messages = messages
    else:
        unique_messages = []

        for i, msg in enumerate(messages):
            # Skip non-dictionary items
            if not isinstance(msg, dict):
                continue

            is_duplicate = False

            # For user messages, check if there's a subsequent assistant message with a different agent
            if msg.get("role") == "user":
                # Look ahead to see which agent responds to this user message
                responding_agent = None
                for j in range(i + 1, len(messages)):
                    if isinstance(messages[j], dict) and messages[j].get("role") == "assistant" and messages[j].get("agent_name"):
                        responding_agent = messages[j].get("agent_name")
                        break

                # Check if we already have this user message with the same responding agent
                for existing_msg in unique_messages:
                    if (isinstance(existing_msg, dict) and existing_msg.get("role") == "user" and
                        existing_msg.get("content") == msg.get("content")):
                        # Find the agent that responded to the existing message
                        existing_responding_agent = None
                        existing_idx = unique_messages.index(existing_msg)
                        for k in range(existing_idx + 1, len(unique_messages)):
                            if (isinstance(unique_messages[k], dict) and unique_messages[k].get("role") == "assistant" and
                                unique_messages[k].get("agent_name")):
                                existing_responding_agent = unique_messages[k].get("agent_name")
                                break

                        # Only consider it a duplicate if same content AND same responding agent
                        if responding_agent == existing_responding_agent:
                            is_duplicate = True
                            break
            else:
                # For non-user messages, use the original logic
                for j, existing_msg in enumerate(unique_messages):
                    if not isinstance(existing_msg, dict):
                        continue
                    # Check if content and tool_calls match (core duplicate detection)
                    same_role = existing_msg.get("role") == msg.get("role")
                    same_content = existing_msg.get("content") == msg.get("content")
                    same_tool_call_id = existing_msg.get("tool_call_id") == msg.get("tool_call_id")
                    same_tool_calls = str(existing_msg.get("tool_calls", [])) == str(msg.get("tool_calls", []))

                    # For agent_name, consider it a match if either is None or they're equal
                    existing_agent = existing_msg.get("agent_name")
                    new_agent = msg.get("agent_name")
                    agent_compatible = (existing_agent is None or new_agent is None or existing_agent == new_agent)

                    if same_role and same_content and same_tool_call_id and same_tool_calls and agent_compatible:
                        is_duplicate = True
                        # If the new message has more info (agent_name, tokens), replace the existing one
                        if new_agent and not existing_agent:
                            unique_messages[j] = msg
                        elif msg.get("input_tokens") and not existing_msg.get("input_tokens"):
                            unique_messages[j] = msg
                        break

            if not is_duplicate:
                unique_messages.append(msg)

    messages = unique_messages

    # Fallback: if no messages found from model records, use event-based messages
    if not messages and event_messages:
        messages = event_messages

    # Now insert system prompts and tool outputs
    final_messages = []
    last_agent = None
    
    for i, msg in enumerate(messages):
        # Skip non-dictionary items
        if not isinstance(msg, dict):
            continue
            
        role = msg.get("role")
        
        # Insert system prompt before user message if agent changes
        if system_prompt and role == "user":
            # Look ahead to find responding agent
            next_agent = None
            for j in range(i + 1, len(messages)):
                if isinstance(messages[j], dict) and messages[j].get("role") == "assistant" and messages[j].get("agent_name"):
                    next_agent = messages[j]["agent_name"]
                    break
            
            # Insert system prompt if agent changes
            if next_agent and next_agent != last_agent and next_agent in system_prompts_by_agent:
                system_msg = {
                    "role": "system",
                    "content": system_prompts_by_agent[next_agent],
                    "agent_name": next_agent
                }
                final_messages.append(system_msg)
                last_agent = next_agent
        
        final_messages.append(msg)
        
        # Update last agent
        if msg.get("agent_name"):
            last_agent = msg["agent_name"]
        
        # Add tool outputs (skip for resumed session logs - they're already included)
        if not is_resumed_session_log and role == "assistant" and msg.get("tool_calls"):
            for tool_call in msg["tool_calls"]:
                tool_id = tool_call.get("id")
                if tool_id and tool_id in tool_outputs:
                    content = tool_outputs[tool_id]
                    # Truncate tool responses if requested (additional safety check)
                    if truncate_tool_responses and len(content) > max_tool_response_chars:
                        content = content[:max_tool_response_chars] + "... [TRUNCATED]"
                    tool_msg = {
                        "role": "tool",
                        "tool_call_id": tool_id,
                        "content": content
                    }
                    final_messages.append(tool_msg)
    
    return final_messages

def load_history_from_json_legacy(file_path):
    """
    Load conversation history from a JSONL file and
    return it as a list of messages.

    Args:
        file_path (str): The path to the JSONL file.
            NOTE: file_path assumes it's either relative to the
            current directory or absolute.

    Returns:
        list: A list of messages extracted from the JSONL file.
    """
    messages = []
    last_assistant_message = None
    tool_outputs = {}  # Map tool_call_id to output content
    
    try:
        with open(file_path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    record = json.loads(line)
                except Exception:  # pylint: disable=broad-except
                    print(f"Error loading line: {line}")
                    continue

                # Collect tool outputs from tool_message events
                if record.get("event") == "tool_message":
                    tool_call_id = record.get("tool_call_id", "")
                    content = record.get("content", "")
                    if tool_call_id and content:
                        tool_outputs[tool_call_id] = content

                # process assistant messages and keep the last one
                # for additing it manually at the end
                if record.get("event") == "assistant_message":
                    last_assistant_message = record.get("content")

                # Extract messages from model record
                if (
                    "model" in record
                    and "messages" in record
                    and isinstance(record["messages"], list)
                ):
                    # Store only complete conversation message objects
                    for msg in record["messages"]:
                        if "role" in msg:
                            # Skip system messages
                            if msg.get("role") == "system":
                                continue

                            # Add this message if we haven't seen it already
                            if not any(m.get("role") == msg.get("role") and 
                                       m.get("content") == msg.get("content") and
                                       m.get("tool_call_id") == msg.get("tool_call_id") for m in messages):
                                messages.append(msg)

                # Extract assistant messages and tool responses from model record choices
                elif (
                    "choices" in record
                    and isinstance(record["choices"], list)
                    and record["choices"]
                ):
                    choice = record["choices"][0]
                    if "message" in choice and "role" in choice["message"]:
                        msg = choice["message"]
                        if not any(m.get("role") == msg.get("role") and 
                                  m.get("content") == msg.get("content") and
                                  m.get("tool_call_id") == msg.get("tool_call_id") for m in messages):
                            messages.append(msg)
    except Exception as e:  # pylint: disable=broad-except
        print(f"Error loading history from {file_path}: {e}")

    # Clean up duplicates and reorder
    unique_messages = []
    for msg in messages:
        if not any(
            m.get("role") == msg.get("role")
            and m.get("content") == msg.get("content")
            and m.get("tool_call_id", "") == msg.get("tool_call_id", "")
            and m.get("tool_calls") == msg.get("tool_calls")
            for m in unique_messages
        ):
            unique_messages.append(msg)

    # Now add tool result messages for any tool calls that have outputs
    final_messages = []
    for msg in unique_messages:
        final_messages.append(msg)

        # If this is an assistant message with tool_calls, add corresponding tool results
        if (
            msg.get("role") == "assistant"
            and msg.get("tool_calls")
            and isinstance(msg.get("tool_calls"), list)
        ):
            for tool_call in msg.get("tool_calls", []):
                tool_call_id = tool_call.get("id")
                if tool_call_id and tool_call_id in tool_outputs:
                    # Add the tool result message immediately after the assistant message
                    content = tool_outputs[tool_call_id]
                    # Note: truncation is not supported in legacy loader
                    tool_result_msg = {
                        "role": "tool",
                        "tool_call_id": tool_call_id,
                        "content": content
                    }
                    final_messages.append(tool_result_msg)

    # Add last message to the end of the list if it exists and isn't already there
    if last_assistant_message:
        # Check if this message is already in the list
        if not any(m.get("role") == "assistant" and 
                  m.get("content") == last_assistant_message for m in final_messages):
            final_messages.append({
                "role": "assistant",
                "content": last_assistant_message
            })
    
    return final_messages

def get_token_stats(file_path):
    """
    Get token usage statistics from a JSONL file.

    Args:
        file_path (str): Path to the JSONL file

    Returns:
        tuple: (model_name, total_prompt_tokens, total_completion_tokens,
                total_cost, active_time, idle_time)
    """
    total_prompt_tokens = 0
    total_completion_tokens = 0
    total_cost = 0.0
    model_name = None
    last_total_cost = 0.0
    last_active_time = 0.0
    last_idle_time = 0.0

    with open(file_path, encoding="utf-8") as file:
        for line in file:
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
                if "usage" in record:
                    total_prompt_tokens += record["usage"]["prompt_tokens"]
                    total_completion_tokens += record["usage"]["completion_tokens"]
                if "cost" in record:
                    if isinstance(record["cost"], dict):
                        # Si cost es un diccionario, obtener total_cost
                        last_total_cost = record["cost"].get("total_cost", 0.0)
                    else:
                        # Si cost es un valor directo
                        last_total_cost = float(record["cost"])
                if "timing_metrics" in record:
                    if isinstance(record["timing_metrics"], dict):
                        last_active_time = record["timing_metrics"].get("active_time_seconds", 0.0)
                        last_idle_time = record["timing_metrics"].get("idle_time_seconds", 0.0)
                if "model" in record:
                    model_name = record["model"]
                # Keep track of the last record for session_end event
                if record.get("event") == "session_end":
                    if "timing_metrics" in record and isinstance(record["timing_metrics"], dict):
                        last_active_time = record["timing_metrics"].get("active_time_seconds", 0.0)
                        last_idle_time = record["timing_metrics"].get("idle_time_seconds", 0.0)
                    if "cost" in record and isinstance(record["cost"], dict):
                        last_total_cost = record["cost"].get("total_cost", 0.0)
            except Exception as e:  # pylint: disable=broad-except
                print(f"Error loading line: {line}: {e}")
                continue

    # Use the last total_cost found as the total
    total_cost = last_total_cost

    return (
        model_name,
        total_prompt_tokens,
        total_completion_tokens,
        total_cost,
        last_active_time,
        last_idle_time,
    )


def atexit_handler():
    """
    Ensure session_end is logged when the program exits.
    Only logs if a session recorder exists and session_end hasn't already been logged.
    """
    global _session_recorder
    if _session_recorder is None:
        return

    # Check if we have an unlogged assistant message and log it
    if hasattr(_session_recorder, "last_assistant_message") and not getattr(
        _session_recorder, "_last_message_logged", False
    ):
        if _session_recorder.last_assistant_message or _session_recorder.last_assistant_tool_calls:
            _session_recorder.log_assistant_message(
                _session_recorder.last_assistant_message,
                _session_recorder.last_assistant_tool_calls,
            )

    # Check if we've already logged the session end (via KeyboardInterrupt)
    if getattr(_session_recorder, "_session_end_logged", False):
        return

    # Log the session end
    _session_recorder.log_session_end()


# Register the exit handler
atexit.register(atexit_handler)
