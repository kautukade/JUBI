"""
Session resume functionality for CAI.

This module provides functions to resume a CAI session from a JSONL log file,
displaying the previous session state (tool calls, outputs, messages) and
restoring the message history so the agent can continue from where it stopped.

Similar to Claude Code's session resume functionality.

Uses the improved load_history_from_jsonl from run_to_jsonl.py which properly
extracts costs, cache tokens, and other metadata for accurate session display.
"""

import json
import os
import re
from datetime import datetime
from pathlib import Path
from typing import Optional, Tuple

from rich.console import Console
from rich.table import Table

from cai.util.config_utils import get_session_logs_dir

console = Console()

# Shared limits for /sessions and /resume (same ordering as list_recent_sessions)
DEFAULT_RECENT_SESSION_COUNT = 10

# REPL tables: match quick-guide / banner accent (see ``cai.repl.ui.banner``).
_CAI_ACCENT = "#00ff9d"
_CAI_MUTED = "#9aa0a6"


def _default_session_logs_directories() -> list[Path]:
    """Primary ``~/.cai/logs`` plus legacy ``./logs`` when it differs (not a symlink to same)."""
    primary = get_session_logs_dir()
    dirs = [primary]
    legacy = Path("logs")
    try:
        legacy_res = legacy.resolve() if legacy.exists() else None
    except OSError:
        legacy_res = None
    if legacy.exists() and legacy_res != primary.resolve():
        dirs.append(legacy)
    return dirs


def _sorted_cai_jsonl_session_files(directories: list[Path]) -> list[Path]:
    """Unique ``cai_*.jsonl`` paths under each directory root, newest mtime first."""
    seen: set[str] = set()
    collected: list[Path] = []
    for d in directories:
        if not d.is_dir():
            continue
        for f in d.glob("cai_*.jsonl"):
            key = str(f.resolve())
            if key in seen:
                continue
            seen.add(key)
            collected.append(f)
    collected.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return collected


def find_newest_cai_jsonl_by_filename_prefix(session_arg: str) -> Optional[str]:
    """Newest file matching ``cai_{session_arg}*.jsonl`` in default session log dirs."""
    prefix = f"cai_{session_arg}"
    for f in _sorted_cai_jsonl_session_files(_default_session_logs_directories()):
        if f.name.startswith(prefix):
            return str(f)
    return None


def fast_load_messages(log_path: str) -> list[dict]:
    """
    Load messages from JSONL using the improved loader.

    Uses load_history_from_jsonl which properly extracts:
    - Message content and tool calls
    - Token usage (input_tokens, output_tokens)
    - Cache tokens (cache_read_tokens, cache_creation_tokens)
    - Costs (interaction_cost, total_cost)
    - Agent names
    """
    log_path = os.path.normpath(os.path.expanduser(str(log_path).strip()))

    from cai.sdk.agents.run_to_jsonl import load_history_from_jsonl

    try:
        # Use the improved loader with optimization for large files
        messages = load_history_from_jsonl(
            log_path,
            system_prompt=False,
            truncate_tool_responses=False,
            use_last_record_optimization=True
        )
        return messages
    except Exception as e:
        console.print(f"[yellow]Warning: Error loading with improved loader: {e}[/yellow]")
        # Fallback to legacy loading
        return _legacy_fast_load_messages(log_path)


def get_session_stats(log_path: str) -> Tuple[str, int, int, float, float, float]:
    """
    Get session statistics from JSONL file.

    Returns:
        Tuple of (model_name, total_input_tokens, total_output_tokens,
                 total_cost, active_time, idle_time)
    """
    from cai.sdk.agents.run_to_jsonl import get_token_stats

    try:
        return get_token_stats(log_path)
    except Exception:
        return (None, 0, 0, 0.0, 0.0, 0.0)


def _legacy_fast_load_messages(log_path: str) -> list[dict]:
    """
    Legacy message loader - fallback if improved loader fails.

    Strategy (priority order):
    1. Find last "messages" array (contains complete conversation history)
    2. Fallback to event-based format if no "messages" found
    """
    file_size = os.path.getsize(log_path)

    # For ALL files: try to find the last "messages" array first
    if file_size > 10 * 1024 * 1024:  # > 10MB - read from end
        messages = _find_last_messages_from_end(log_path, file_size, 20 * 1024 * 1024)
        if messages:
            return messages
        return _load_from_events(log_path)

    # Small files: scan the whole file for last "messages" array
    last_messages_line = None
    with open(log_path, encoding="utf-8", errors="ignore") as f:
        for line in f:
            if '"messages"' in line and '"messages": []' not in line:
                last_messages_line = line

    if last_messages_line:
        try:
            record = json.loads(last_messages_line)
            messages = record.get("messages", [])
            return [m for m in messages if m.get("role") != "system"]
        except json.JSONDecodeError:
            pass

    return _load_from_events(log_path)


def _find_last_messages_from_end(
    log_path: str, file_size: int, chunk_size: int
) -> Optional[list[dict]]:
    """Read file from end to find the last 'messages' line quickly."""
    pattern = b'"messages"'
    empty_pattern = b'"messages": []'

    with open(log_path, "rb") as f:
        for read_size in [1024 * 1024, 5 * 1024 * 1024, 20 * 1024 * 1024]:
            if read_size > file_size:
                read_size = file_size

            f.seek(file_size - read_size)
            data = f.read(read_size)

            pos = len(data)
            while pos > 0:
                idx = data.rfind(pattern, 0, pos)
                if idx == -1:
                    break

                line_start = data.rfind(b"\n", 0, idx)
                line_start = line_start + 1 if line_start != -1 else 0

                line_end = data.find(b"\n", idx)
                line_end = line_end if line_end != -1 else len(data)

                line_bytes = data[line_start:line_end]

                if empty_pattern in line_bytes:
                    pos = idx
                    continue

                try:
                    line = line_bytes.decode("utf-8", errors="ignore")
                    record = json.loads(line)
                    messages = record.get("messages", [])
                    if messages:
                        return [m for m in messages if m.get("role") != "system"]
                except (json.JSONDecodeError, UnicodeDecodeError):
                    pass

                pos = idx

            if read_size >= file_size:
                break

    return None


def _load_from_events(log_path: str, max_messages: int = 100) -> list[dict]:
    """Load from event-based format."""
    file_size = os.path.getsize(log_path)

    if file_size > 10 * 1024 * 1024:
        return _load_events_from_tail(log_path, file_size, max_messages)

    messages = []
    with open(log_path, encoding="utf-8", errors="ignore") as f:
        for line in f:
            if '"event"' not in line:
                continue
            msg = _parse_event_line(line)
            if msg:
                messages.append(msg)

    return messages[-max_messages:] if len(messages) > max_messages else messages


def _load_events_from_tail(log_path: str, file_size: int, max_messages: int) -> list[dict]:
    """Load last N events from tail of large file."""
    messages = []
    read_size = min(5 * 1024 * 1024, file_size)

    with open(log_path, "rb") as f:
        f.seek(file_size - read_size)
        data = f.read(read_size)

    text = data.decode("utf-8", errors="ignore")
    lines = text.split("\n")

    for line in lines[1:]:
        if '"event"' not in line:
            continue
        msg = _parse_event_line(line)
        if msg:
            messages.append(msg)

    return messages[-max_messages:] if len(messages) > max_messages else messages


def _parse_event_line(line: str) -> Optional[dict]:
    """Parse a single event line."""
    try:
        record = json.loads(line)
        event = record.get("event", "")

        if event == "user_message":
            return {"role": "user", "content": record.get("content", "")}
        elif event == "assistant_message":
            return {
                "role": "assistant",
                "content": record.get("content"),
                "tool_calls": record.get("tool_calls", []),
            }
    except json.JSONDecodeError:
        pass
    return None


def _has_messages(log_path: str) -> bool:
    """
    Quickly check if a log file has any messages.

    Uses a fast scan that only reads enough to find message indicators.
    """
    try:
        file_size = os.path.getsize(log_path)
        if file_size == 0:
            return False

        # Read first 50KB to check for messages
        with open(log_path, "rb") as f:
            content = f.read(min(50000, file_size))

        # Check for indicators of messages
        has_messages = (
            b'"role"' in content or
            b'"messages"' in content and b'"messages": []' not in content
        )
        return has_messages
    except Exception:
        return False


def find_last_session_log() -> Optional[str]:
    """
    Find the most recent session log file that has messages.

    Skips empty sessions and returns the last session with actual content.

    Returns:
        Path to the last session log with messages, or None if not found.
    """
    # Symlink "last" under session log dir (preferred) or legacy ./logs/last
    for last_symlink in (get_session_logs_dir() / "last", Path("logs/last")):
        if last_symlink.exists() or last_symlink.is_symlink():
            try:
                actual_path = last_symlink.resolve()
                if actual_path.exists() and _has_messages(str(actual_path)):
                    return str(actual_path)
            except Exception:
                pass

    dirs = _default_session_logs_directories()
    jsonl_files = _sorted_cai_jsonl_session_files(dirs)
    if not jsonl_files:
        return None

    for log_file in jsonl_files:
        if _has_messages(str(log_file)):
            return str(log_file)

    return None


def normalize_message_content(content):
    """
    Normalize message content to a simple string format.

    The JSONL logs may store content in various formats:
    - Simple string: "Hello"
    - List of content blocks: [{'type': 'text', 'text': 'Hello', ...}]
    - List with input_text: [{'type': 'input_text', 'text': 'Hello'}]

    This function extracts the text and returns a simple string.

    Args:
        content: The content to normalize (string, list, or None)

    Returns:
        A simple string with the extracted text content
    """
    if content is None:
        return ""

    if isinstance(content, str):
        return content

    if isinstance(content, list):
        # Extract text from list of content blocks
        text_parts = []
        for item in content:
            if isinstance(item, str):
                text_parts.append(item)
            elif isinstance(item, dict):
                # Handle various content block types
                if "text" in item:
                    text_parts.append(item["text"])
                elif "content" in item:
                    text_parts.append(str(item["content"]))
        return "\n".join(text_parts) if text_parts else str(content)

    return str(content)


def display_resumed_session(
    messages: list[dict],
    usage: Optional[tuple] = None,
    log_path: Optional[str] = None,
) -> None:
    """
    Display the resumed session using the SAME format as cai-replay.

    Uses cli_print_agent_messages and cli_print_tool_output for consistent
    display with live sessions and replay.
    """
    from cai.util import cli_print_agent_messages, cli_print_tool_output, color

    if not messages:
        return

    # Header
    console.print()
    console.print("[bold cyan]↻ Resuming session[/bold cyan]")

    # Display session stats if available
    model = None
    total_input_tokens = 0
    total_output_tokens = 0
    total_cost = 0.0

    if log_path:
        stats = get_session_stats(log_path)
        model, total_input_tokens, total_output_tokens, total_cost, active_time, idle_time = stats
        if total_cost > 0 or total_input_tokens > 0:
            stats_parts = []
            if model:
                stats_parts.append(f"[cyan]{model}[/cyan]")
            if total_input_tokens > 0 or total_output_tokens > 0:
                stats_parts.append(f"[dim]Tokens: {total_input_tokens}in/{total_output_tokens}out[/dim]")
            if total_cost > 0:
                stats_parts.append(f"[green]${total_cost:.4f}[/green]")
            if active_time > 0:
                stats_parts.append(f"[dim]{active_time:.1f}s active[/dim]")
            if stats_parts:
                console.print(" | ".join(stats_parts))
    console.print()

    # Build tool_outputs map from tool messages
    tool_outputs = {}
    for msg in messages:
        if msg.get("role") == "tool":
            tool_call_id = msg.get("tool_call_id", "")
            if tool_call_id:
                tool_outputs[tool_call_id] = normalize_message_content(msg.get("content", ""))

    interaction_counter = 0
    debug = False

    for msg in messages:
        role = msg.get("role", "")
        content = normalize_message_content(msg.get("content", ""))

        if role == "user":
            # User message - same format as replay
            if content:
                print(color("CAI> ", fg="cyan") + content)

        elif role == "assistant":
            # Get agent name
            agent_name = msg.get("agent_name", "Assistant")
            display_sender = agent_name if agent_name else "Assistant"

            # Get tool calls
            tool_calls = msg.get("tool_calls", [])

            if tool_calls:
                # Print assistant message if there's content
                if content and content.strip():
                    cli_print_agent_messages(
                        display_sender,
                        content,
                        interaction_counter,
                        model or "unknown",
                        debug,
                        interaction_input_tokens=msg.get("input_tokens", 0),
                        interaction_output_tokens=msg.get("output_tokens", 0),
                        interaction_reasoning_tokens=msg.get("reasoning_tokens", 0),
                        total_input_tokens=total_input_tokens,
                        total_output_tokens=total_output_tokens,
                        total_reasoning_tokens=msg.get("total_reasoning_tokens", 0),
                        interaction_cost=msg.get("interaction_cost", 0.0),
                        total_cost=total_cost,
                        cache_read_tokens=msg.get("cache_read_tokens", 0),
                        cache_creation_tokens=msg.get("cache_creation_tokens", 0),
                    )

                # Print each tool call with its output
                for tool_call in tool_calls:
                    function = tool_call.get("function", {})
                    name = function.get("name", "")
                    arguments = function.get("arguments", "{}")
                    call_id = tool_call.get("id", "")

                    tool_output = tool_outputs.get(call_id, "")

                    if not name:
                        continue

                    # Parse arguments
                    try:
                        if arguments and isinstance(arguments, str) and arguments.strip().startswith("{"):
                            args_obj = json.loads(arguments)
                        else:
                            args_obj = arguments
                    except json.JSONDecodeError:
                        args_obj = arguments

                    # Print tool call using cli_print_tool_output
                    cli_print_tool_output(
                        tool_name=name,
                        args=args_obj,
                        output=tool_output,
                        call_id=call_id,
                        token_info={
                            "interaction_input_tokens": msg.get("input_tokens", 0),
                            "interaction_output_tokens": msg.get("output_tokens", 0),
                            "interaction_reasoning_tokens": msg.get("reasoning_tokens", 0),
                            "total_input_tokens": total_input_tokens,
                            "total_output_tokens": total_output_tokens,
                            "total_reasoning_tokens": msg.get("total_reasoning_tokens", 0),
                            "model": model or "unknown",
                            "interaction_cost": msg.get("interaction_cost", 0.0),
                            "total_cost": total_cost,
                            "agent_name": f"{display_sender} [P1]",
                            "cache_read_tokens": msg.get("cache_read_tokens", 0),
                            "cache_creation_tokens": msg.get("cache_creation_tokens", 0),
                        },
                    )
            else:
                # Print regular assistant message
                cli_print_agent_messages(
                    display_sender,
                    content or "",
                    interaction_counter,
                    model or "unknown",
                    debug,
                    interaction_input_tokens=msg.get("input_tokens", 0),
                    interaction_output_tokens=msg.get("output_tokens", 0),
                    interaction_reasoning_tokens=msg.get("reasoning_tokens", 0),
                    total_input_tokens=total_input_tokens,
                    total_output_tokens=total_output_tokens,
                    total_reasoning_tokens=msg.get("total_reasoning_tokens", 0),
                    interaction_cost=msg.get("interaction_cost", 0.0),
                    total_cost=total_cost,
                    cache_read_tokens=msg.get("cache_read_tokens", 0),
                    cache_creation_tokens=msg.get("cache_creation_tokens", 0),
                )

            interaction_counter += 1

        # Skip tool messages - they're already displayed with assistant messages

    console.print()
    console.print("[bold cyan]Session restored. Continue where you left off.[/bold cyan]")
    console.print()


def resume_session(
    log_path: Optional[str] = None,
    check_parallel: bool = True,
) -> tuple[list[dict], Optional[str], Optional[list[str]]]:
    """
    Resume a session from a JSONL log file.

    This function:
    1. Finds the log file to resume from
    2. Loads the message history
    3. Displays the session state visually
    4. Checks for parallel agent configuration
    5. Returns the messages to be loaded into the agent

    Args:
        log_path: Optional path to specific log file. If None, uses latest session
            log with messages.
        check_parallel: Whether to check for and prompt about parallel agents.

    Returns:
        Tuple of (messages list, log file path used, parallel agents list or None)
    """
    # Find the log file
    if log_path:
        resolved_path = Path(log_path).expanduser()
        if not resolved_path.exists():
            console.print(f"[red]Error: Log file not found: {log_path}[/red]")
            return [], None, None
        log_file = str(resolved_path)
    else:
        log_file = find_last_session_log()
        if not log_file:
            console.print("[yellow]No previous session found to resume.[/yellow]")
            console.print("[dim]Start a new session instead.[/dim]")
            return [], None, None

    try:
        # Ultra-fast load - single pass, minimal parsing
        messages = fast_load_messages(log_file)

        if not messages:
            console.print("[yellow]No messages found in session log.[/yellow]")
            return [], log_file, None

        # Check for parallel agents
        parallel_agents = None
        if check_parallel:
            parallel_agents = check_parallel_agent_config(log_file)
            if parallel_agents and len(parallel_agents) > 1:
                # Prompt user to set up parallel agent config
                if prompt_parallel_agent_setup(parallel_agents):
                    # Get model from session stats
                    stats = get_session_stats(log_file)
                    model = stats[0] if stats else None
                    setup_parallel_agents_from_log(parallel_agents, model)

        # Display with cost/token stats
        display_resumed_session(messages, None, log_path=log_file)

        return messages, log_file, parallel_agents

    except Exception as e:
        console.print(f"[red]Error loading session: {str(e)}[/red]")
        return [], None, None


def get_messages_by_agent(log_path: str) -> dict[str, list[dict]]:
    """
    Extract messages grouped by agent from a session log.

    For parallel agent sessions, this returns a dictionary with messages
    for each agent separately, so they can be loaded into their respective
    terminals in TUI mode.

    Args:
        log_path: Path to the JSONL log file

    Returns:
        Dictionary mapping agent names to their message lists
    """
    agent_messages: dict[str, list[dict]] = {}

    try:
        with open(log_path, encoding="utf-8") as f:
            for line in f:
                if not line.strip():
                    continue
                try:
                    record = json.loads(line)

                    # Process chat.completion records
                    if record.get("object") == "chat.completion":
                        agent_name = record.get("agent_name", "Agent")

                        if agent_name not in agent_messages:
                            agent_messages[agent_name] = []

                        # Extract messages from the record
                        messages = record.get("messages", [])
                        choices = record.get("choices", [])

                        # Add messages
                        for msg in messages:
                            if msg.get("role") != "system":
                                # Tag message with agent
                                msg_copy = msg.copy()
                                msg_copy["agent_name"] = agent_name
                                agent_messages[agent_name].append(msg_copy)

                        # Add assistant response from choices
                        for choice in choices:
                            assistant_msg = choice.get("message", {})
                            if assistant_msg and assistant_msg.get("role") == "assistant":
                                assistant_copy = assistant_msg.copy()
                                assistant_copy["agent_name"] = agent_name
                                agent_messages[agent_name].append(assistant_copy)

                except json.JSONDecodeError:
                    continue

    except Exception:
        pass

    return agent_messages


def normalize_messages_for_agent(messages: list[dict]) -> list[dict]:
    """
    Normalize all messages to ensure content is in simple string format.

    Args:
        messages: List of message dictionaries

    Returns:
        List of normalized message dictionaries
    """
    normalized = []
    for msg in messages:
        new_msg = msg.copy()

        # Normalize content field
        if "content" in new_msg:
            new_msg["content"] = normalize_message_content(new_msg["content"])

        normalized.append(new_msg)

    return normalized


def restore_session_stats(log_path: str) -> bool:
    """
    Restore session statistics (costs, tokens) from a log file.

    This ensures that when resuming a session, the cost tracker and other
    statistics continue from where the previous session left off.

    Args:
        log_path: Path to the JSONL log file

    Returns:
        True if successful, False otherwise
    """
    try:
        from cai.util import COST_TRACKER
        from cai.sdk.agents.global_usage_tracker import GLOBAL_USAGE_TRACKER

        # Get session stats
        stats = get_session_stats(log_path)
        model, total_input, total_output, total_cost, active_time, idle_time = stats

        # Restore COST_TRACKER state
        if total_cost > 0:
            COST_TRACKER.session_total_cost = total_cost
            COST_TRACKER.current_agent_total_cost = total_cost
            COST_TRACKER.last_total_cost = total_cost

        if total_input > 0:
            COST_TRACKER.current_agent_input_tokens = total_input

        if total_output > 0:
            COST_TRACKER.current_agent_output_tokens = total_output

        # Restore GLOBAL_USAGE_TRACKER if available
        if total_cost > 0 or total_input > 0:
            GLOBAL_USAGE_TRACKER.total_input_tokens = total_input
            GLOBAL_USAGE_TRACKER.total_output_tokens = total_output
            GLOBAL_USAGE_TRACKER.total_cost = total_cost

        console.print(
            f"[dim]Restored session stats: ${total_cost:.4f}, "
            f"{total_input}in/{total_output}out tokens[/dim]"
        )
        return True

    except Exception as e:
        console.print(f"[yellow]Warning: Could not restore session stats: {e}[/yellow]")
        return False


def load_session_into_agent(agent, messages: list[dict], log_path: Optional[str] = None) -> bool:
    """
    Load session messages into an agent's message history.

    Args:
        agent: The agent to load messages into
        messages: List of message dictionaries to load
        log_path: Optional path to log file for restoring session stats

    Returns:
        True if successful, False otherwise
    """
    if not messages:
        return True

    try:
        # Normalize messages to ensure content is in simple string format
        normalized_messages = normalize_messages_for_agent(messages)

        # Get the model instance
        if not hasattr(agent, "model"):
            console.print("[yellow]Warning: Agent has no model, cannot load history[/yellow]")
            return False

        model = agent.model

        # Clear existing history
        if hasattr(model, "message_history"):
            model.message_history.clear()

        # Add messages to history with skip_deduplication=True to preserve order
        # The messages from the JSONL are already in correct order, so we should
        # not deduplicate (which can cause reordering)
        for msg in normalized_messages:
            if hasattr(model, "add_to_message_history"):
                model.add_to_message_history(msg, skip_deduplication=True)
            elif hasattr(model, "message_history"):
                model.message_history.append(msg)

        # Also update AGENT_MANAGER for consistency
        try:
            from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER

            agent_name = getattr(agent, "name", "Agent")
            AGENT_MANAGER._message_history[agent_name] = list(normalized_messages)
        except ImportError:
            pass

        # Restore session stats if log_path is provided
        if log_path:
            restore_session_stats(log_path)

        console.print(
            f"[green]Loaded {len(normalized_messages)} messages into agent history[/green]"
        )
        return True

    except Exception as e:
        console.print(f"[red]Error loading messages into agent: {str(e)}[/red]")
        return False


def get_session_agents(log_path: str) -> list[str]:
    """
    Extract unique agent names from a session log file.

    Args:
        log_path: Path to the JSONL log file

    Returns:
        List of unique agent names found in the log
    """
    agent_names = set()

    try:
        with open(log_path, encoding="utf-8") as f:
            for line in f:
                if not line.strip():
                    continue
                try:
                    record = json.loads(line)

                    # Get agent name from completion records
                    if record.get("object") == "chat.completion":
                        agent_name = record.get("agent_name")
                        if agent_name:
                            agent_names.add(agent_name)

                except json.JSONDecodeError:
                    continue
    except Exception:
        pass

    return list(agent_names)


def check_parallel_agent_config(log_path: str) -> Optional[list[str]]:
    """
    Check if a session log has multiple agents (parallel execution).

    Returns the list of agent names if multiple agents found, None otherwise.
    Filters out "Summary Agent" as it's not a real parallel agent.

    Args:
        log_path: Path to the JSONL log file

    Returns:
        List of agent names if multiple agents, None if single agent or error
    """
    agents = get_session_agents(log_path)

    # Filter out Summary Agent - it's not a real parallel agent
    agents = [a for a in agents if a.lower() != "summary agent"]

    if len(agents) > 1:
        return agents
    return None


def prompt_parallel_agent_setup(log_agents: list[str]) -> bool:
    """
    Prompt user to set up parallel agents matching the log configuration.

    Args:
        log_agents: List of agent names from the log file

    Returns:
        True if user wants to set up parallel agents, False otherwise
    """
    try:
        import questionary
        from cai.repl.commands.settings.general import custom_style
    except ImportError:
        # Fallback to simple input
        console.print()
        console.print(f"[yellow]The session used {len(log_agents)} parallel agents:[/yellow]")
        for agent in log_agents:
            console.print(f"  - [cyan]{agent}[/cyan]")
        console.print()

        try:
            response = console.input(
                "[bold]Set up the same parallel agent configuration? (y/n): [/bold]"
            )
            return response.strip().lower() in ("y", "yes")
        except (KeyboardInterrupt, EOFError):
            return False

    console.print()
    console.print(f"[yellow]The session used {len(log_agents)} parallel agents:[/yellow]")
    for agent in log_agents:
        console.print(f"  - [cyan]{agent}[/cyan]")
    console.print()

    try:
        result = questionary.confirm(
            "Set up the same parallel agent configuration?",
            default=True,
            style=custom_style,
        ).ask()
        return result if result is not None else False
    except (KeyboardInterrupt, EOFError):
        return False


def setup_parallel_agents_from_log(log_agents: list[str], model: Optional[str] = None) -> bool:
    """
    Set up parallel agent configuration matching a log file.

    Args:
        log_agents: List of agent names to configure
        model: Optional model override

    Returns:
        True if successful, False otherwise
    """
    try:
        from cai.repl.commands.parallel import PARALLEL_CONFIGS, ParallelConfig

        # Clear existing config
        PARALLEL_CONFIGS.clear()

        # Add each agent from the log
        for idx, agent_name in enumerate(log_agents):
            config = ParallelConfig(agent_name=agent_name, model=model)
            config.id = idx + 1
            PARALLEL_CONFIGS.append(config)

        # Sync to environment
        import os
        os.environ["CAI_PARALLEL"] = str(len(PARALLEL_CONFIGS))
        os.environ["CAI_PARALLEL_AGENTS"] = ",".join(log_agents)

        console.print(
            f"[green]Configured {len(log_agents)} parallel agents: "
            f"{', '.join(log_agents)}[/green]"
        )
        return True

    except Exception as e:
        console.print(f"[red]Error setting up parallel agents: {e}[/red]")
        return False


def get_session_metadata(log_path: str) -> dict:
    """
    Extract metadata from a session log file.

    Args:
        log_path: Path to the JSONL log file

    Returns:
        Dictionary containing session metadata
    """
    metadata = {
        "session_id": None,
        "start_time": None,
        "end_time": None,
        "model": None,
        "agent_name": None,
        "total_cost": 0.0,
        "active_time": 0.0,
        "idle_time": 0.0,
        "message_count": 0,
    }

    try:
        with open(log_path, encoding="utf-8") as f:
            for line in f:
                if not line.strip():
                    continue
                try:
                    record = json.loads(line)

                    # Session start event
                    if record.get("event") == "session_start":
                        metadata["session_id"] = record.get("session_id")
                        metadata["start_time"] = record.get("timestamp")

                    # Session end event
                    elif record.get("event") == "session_end":
                        metadata["end_time"] = record.get("timestamp")
                        timing = record.get("timing_metrics", {})
                        metadata["active_time"] = timing.get("active_time_seconds", 0.0)
                        metadata["idle_time"] = timing.get("idle_time_seconds", 0.0)
                        cost = record.get("cost", {})
                        if isinstance(cost, dict):
                            metadata["total_cost"] = cost.get("total_cost", 0.0)

                    # Model and agent info from completion records
                    if record.get("object") == "chat.completion":
                        if record.get("model"):
                            metadata["model"] = record["model"]
                        if record.get("agent_name"):
                            metadata["agent_name"] = record["agent_name"]

                    # Count messages
                    if "messages" in record:
                        metadata["message_count"] += len(record["messages"])

                except json.JSONDecodeError:
                    continue

    except Exception:
        pass

    return metadata


def list_recent_sessions(limit: int = 10) -> list[dict]:
    """
    List recent session logs with their metadata.

    Only includes sessions that have messages.

    Args:
        limit: Maximum number of sessions to list

    Returns:
        List of dictionaries containing session info with:
        - file_path, file_name, session_id
        - model, agent_name, message_count
        - total_cost, total_input_tokens, total_output_tokens
        - active_time, idle_time, start_time
        - last_assistant_message: preview of last assistant response
    """
    sessions = []
    dirs = _default_session_logs_directories()
    jsonl_files = _sorted_cai_jsonl_session_files(dirs)

    if not jsonl_files:
        return sessions

    for log_file in jsonl_files:
        if len(sessions) >= limit:
            break
        # Check if session has messages
        messages = fast_load_messages(str(log_file))
        if not messages:
            continue  # Skip sessions without messages

        # Get basic metadata
        metadata = get_session_metadata(str(log_file))
        metadata["file_path"] = str(log_file)
        metadata["file_name"] = log_file.name
        metadata["message_count"] = len(messages)

        # Get token stats for more detailed info
        stats = get_session_stats(str(log_file))
        model, total_input, total_output, total_cost, active_time, idle_time = stats

        # Override with more accurate stats
        if model:
            metadata["model"] = model
        if total_cost > 0:
            metadata["total_cost"] = total_cost
        if total_input > 0:
            metadata["total_input_tokens"] = total_input
        if total_output > 0:
            metadata["total_output_tokens"] = total_output
        if active_time > 0:
            metadata["active_time"] = active_time
        if idle_time > 0:
            metadata["idle_time"] = idle_time

        # Get last assistant message for preview
        last_assistant_msg = ""
        for msg in reversed(messages):
            if msg.get("role") == "assistant":
                content = normalize_message_content(msg.get("content", ""))
                if content and content.strip():
                    # Clean up the content for display
                    last_assistant_msg = content.strip()
                    # Remove newlines and excessive whitespace
                    last_assistant_msg = " ".join(last_assistant_msg.split())
                    break

        metadata["last_assistant_message"] = last_assistant_msg

        sessions.append(metadata)

    return sessions


def list_recent_sessions_in_directory(
    dir_path: str | Path, limit: int = DEFAULT_RECENT_SESSION_COUNT
) -> list[dict]:
    """
    Same shape as list_recent_sessions entries, but only JSONL under dir_path
    (recursive), newest first, capped at ``limit`` sessions that contain messages.
    """
    expanded = Path(dir_path).expanduser()
    if not expanded.is_dir():
        return []

    sessions: list[dict] = []
    for log_file in _get_log_files_sorted(str(expanded)):
        if len(sessions) >= limit:
            break
        lp = str(log_file)
        if not _has_messages(lp):
            continue
        messages = fast_load_messages(lp)
        if not messages:
            continue
        metadata = get_session_metadata(lp)
        metadata["file_path"] = lp
        metadata["file_name"] = log_file.name
        metadata["message_count"] = len(messages)
        stats = get_session_stats(lp)
        model, total_input, total_output, total_cost, active_time, idle_time = stats
        if model:
            metadata["model"] = model
        if total_cost > 0:
            metadata["total_cost"] = total_cost
        if total_input > 0:
            metadata["total_input_tokens"] = total_input
        if total_output > 0:
            metadata["total_output_tokens"] = total_output
        if active_time > 0:
            metadata["active_time"] = active_time
        if idle_time > 0:
            metadata["idle_time"] = idle_time
        last_assistant_msg = ""
        for msg in reversed(messages):
            if msg.get("role") == "assistant":
                content = normalize_message_content(msg.get("content", ""))
                if content and content.strip():
                    last_assistant_msg = " ".join(content.strip().split())
                    break
        metadata["last_assistant_message"] = last_assistant_msg
        sessions.append(metadata)
    return sessions


def sessions_table_from_metadatas(
    sessions: list[dict],
    *,
    title: str,
    row_offset: int = 0,
    include_pick_hint_caption: bool = True,
) -> Table:
    """Rich table for /sessions and /resume (CAI accent palette, numbered rows)."""
    table_kw: dict = {
        "title": f"[bold {_CAI_ACCENT}]{title}[/bold {_CAI_ACCENT}]",
        "show_header": True,
        "header_style": f"bold {_CAI_ACCENT}",
        "border_style": _CAI_ACCENT,
    }
    if include_pick_hint_caption:
        table_kw["caption"] = (
            f"[dim {_CAI_MUTED}]# = row number for bare /resume; "
            f"IDs match /sessions[/dim {_CAI_MUTED}]"
        )
        table_kw["caption_justify"] = "left"
    table = Table(**table_kw)
    table.add_column("#", justify="right", style=f"bold {_CAI_ACCENT}", width=4)
    table.add_column("ID", style="magenta", width=12)
    table.add_column("Date/Time", style="green")
    table.add_column("Agent", style="yellow")
    table.add_column("Msgs", justify="right", style=f"dim {_CAI_MUTED}")
    table.add_column("Cost", justify="right", style=f"bold {_CAI_ACCENT}")
    table.add_column("Duration", justify="right", style=f"dim {_CAI_MUTED}")

    for idx, session in enumerate(sessions):
        row_n = str(row_offset + idx + 1)
        session_id = session.get("session_id", "")
        short_id = session_id[:8] if session_id else "-"

        start_time = session.get("start_time", "")
        if start_time:
            try:
                dt = datetime.fromisoformat(start_time.replace("Z", "+00:00"))
                formatted_time = dt.strftime("%Y-%m-%d %H:%M")
            except (ValueError, AttributeError):
                formatted_time = start_time[:16] if len(start_time) > 16 else start_time
        else:
            file_path = session.get("file_path", "")
            if file_path and Path(file_path).exists():
                mtime = Path(file_path).stat().st_mtime
                formatted_time = datetime.fromtimestamp(mtime).strftime("%Y-%m-%d %H:%M")
            else:
                formatted_time = "-"

        agent_name = session.get("agent_name", "-")
        if agent_name and len(agent_name) > 20:
            agent_name = agent_name[:17] + "..."

        msg_count = str(session.get("message_count", 0))

        cost = session.get("total_cost", 0.0)
        cost_str = f"${cost:.4f}" if cost > 0 else "-"

        active = session.get("active_time", 0)
        idle = session.get("idle_time", 0)
        total_secs = active + idle
        if total_secs > 0:
            mins, secs = divmod(int(total_secs), 60)
            hours, mins = divmod(mins, 60)
            if hours > 0:
                duration = f"{hours}h {mins}m"
            elif mins > 0:
                duration = f"{mins}m {secs}s"
            else:
                duration = f"{secs}s"
        else:
            duration = "-"

        table.add_row(row_n, short_id, formatted_time, agent_name, msg_count, cost_str, duration)

    return table


def prompt_pick_session_path(sessions: list[dict]) -> Optional[str]:
    """
    Numbered prompt to pick one session; returns file_path or None if cancelled.
    """
    if not sessions:
        return None
    n = len(sessions)
    console.print()
    console.print(
        f"[dim {_CAI_MUTED}]Enter #[/dim {_CAI_MUTED}][bold {_CAI_ACCENT}]1[/bold {_CAI_ACCENT}]"
        f"[dim {_CAI_MUTED}]–[/dim {_CAI_MUTED}][bold {_CAI_ACCENT}]{n}[/bold {_CAI_ACCENT}]"
        f"[dim {_CAI_MUTED}] from the table, or [/dim {_CAI_MUTED}][bold {_CAI_ACCENT}]q[/bold {_CAI_ACCENT}]"
        f"[dim {_CAI_MUTED}] to cancel.[/dim {_CAI_MUTED}]"
    )
    try:
        raw = console.input(f"[bold {_CAI_ACCENT}]❯[/bold {_CAI_ACCENT}] [dim {_CAI_MUTED}]Session:[/dim {_CAI_MUTED}] ").strip().lower()
    except (KeyboardInterrupt, EOFError):
        return None
    if raw in ("q", "quit", "exit"):
        return None
    if not raw:
        return None
    try:
        idx = int(raw)
    except ValueError:
        console.print("[yellow]Invalid choice.[/yellow]")
        return None
    if not 1 <= idx <= n:
        console.print(f"[yellow]Enter a number between 1 and {n}.[/yellow]")
        return None
    path = sessions[idx - 1].get("file_path")
    return str(path) if path else None


def find_jsonl_by_token_in_dir(dir_path: Path, token: str) -> Optional[str]:
    """Newest JSONL under dir whose filename contains token."""
    if not dir_path.is_dir():
        return None
    matching = [f for f in dir_path.rglob("*.jsonl") if token in f.name]
    if not matching:
        return None
    matching.sort(key=lambda f: f.stat().st_mtime, reverse=True)
    return str(matching[0])


def find_jsonl_by_token_in_logs(token: str) -> Optional[str]:
    """Newest ``cai_*.jsonl`` in session dirs (``~/.cai/logs``, legacy ``./logs``).

    Returns the path whose filename contains ``token``, preferring newest mtime.
    """
    for f in _sorted_cai_jsonl_session_files(_default_session_logs_directories()):
        if token in f.name:
            return str(f)
    return None


def _format_session_choice_ansi(session: dict, idx: int) -> str:
    """
    Format a session with ANSI escape codes for terminal display.

    Args:
        session: Session metadata dictionary
        idx: Index of the session (0 = latest)
    """
    from datetime import datetime

    # ANSI codes
    RESET = "\033[0m"
    BOLD = "\033[1m"
    DIM = "\033[2m"
    MAGENTA = "\033[35m"
    CYAN = "\033[36m"
    YELLOW = "\033[33m"
    GREEN = "\033[32m"
    RED = "\033[31m"

    # Session ID
    session_id = session.get("session_id", "")
    short_id = session_id[:8] if session_id else "--------"

    # Format time
    start_time = session.get("start_time", "")
    if start_time:
        try:
            dt = datetime.fromisoformat(start_time.replace("Z", "+00:00"))
            time_str = dt.strftime("%m-%d %H:%M")
        except (ValueError, AttributeError):
            time_str = start_time[:11] if len(start_time) > 11 else start_time
    else:
        file_path = session.get("file_path", "")
        if file_path and Path(file_path).exists():
            mtime = Path(file_path).stat().st_mtime
            time_str = datetime.fromtimestamp(mtime).strftime("%m-%d %H:%M")
        else:
            time_str = "Unknown"

    # Model (shortened)
    model = session.get("model", "")
    if model:
        model_short = model.replace("claude-", "").replace("gpt-", "")
        model_short = model_short.replace("-20241022", "").replace("-20240307", "")
        model_short = model_short.replace("openai/", "")
        if len(model_short) > 14:
            model_short = model_short[:11] + "..."
    else:
        model_short = "unknown"

    # Message count
    msg_count = session.get("message_count", 0)

    # Cost with color based on amount
    cost = session.get("total_cost", 0.0)
    cost_str = f"${cost:.2f}" if cost > 0 else "$0.00"
    if cost > 5.0:
        cost_color = RED
    elif cost > 1.0:
        cost_color = YELLOW
    elif cost > 0:
        cost_color = GREEN
    else:
        cost_color = DIM

    # Last assistant message preview
    last_msg = session.get("last_assistant_message", "")
    if last_msg:
        # Handle both actual newlines and escaped \n
        last_msg = last_msg.replace("\\n", " ").replace("\n", " ")
        last_msg = last_msg.replace("\\t", " ").replace("\t", " ")
        last_msg = " ".join(last_msg.split())  # Normalize whitespace
        if len(last_msg) > 45:
            last_msg_preview = last_msg[:42] + "..."
        else:
            last_msg_preview = last_msg
    else:
        last_msg_preview = "(no response)"

    # Latest indicator
    latest_badge = f" {GREEN}{BOLD}★ LATEST{RESET}" if idx == 0 else ""

    # Build ANSI formatted string
    line1 = (
        f"{MAGENTA}{BOLD}{short_id}{RESET} "
        f"{DIM}│{RESET} "
        f"{CYAN}{time_str}{RESET} "
        f"{DIM}│{RESET} "
        f"{YELLOW}{model_short:14}{RESET} "
        f"{DIM}│{RESET} "
        f"{msg_count:3} msgs "
        f"{DIM}│{RESET} "
        f"{cost_color}{cost_str:>7}{RESET}"
        f"{latest_badge}"
    )
    line2 = f"         {DIM}└─ {last_msg_preview}{RESET}"

    return f"{line1}\n{line2}"


def _format_session_choice(session: dict, idx: int, use_ansi: bool = False) -> str:
    """
    Format a session as plain text string (fallback).

    Args:
        session: Session metadata dictionary
        idx: Index of the session (0 = latest)
        use_ansi: Unused, kept for compatibility
    """
    from datetime import datetime

    # Session ID
    session_id = session.get("session_id", "")
    short_id = session_id[:8] if session_id else "--------"

    # Format time
    start_time = session.get("start_time", "")
    if start_time:
        try:
            dt = datetime.fromisoformat(start_time.replace("Z", "+00:00"))
            time_str = dt.strftime("%m-%d %H:%M")
        except (ValueError, AttributeError):
            time_str = start_time[:11] if len(start_time) > 11 else start_time
    else:
        file_path = session.get("file_path", "")
        if file_path and Path(file_path).exists():
            mtime = Path(file_path).stat().st_mtime
            time_str = datetime.fromtimestamp(mtime).strftime("%m-%d %H:%M")
        else:
            time_str = "Unknown"

    # Model (shortened)
    model = session.get("model", "")
    if model:
        model_short = model.replace("claude-", "").replace("gpt-", "")
        model_short = model_short.replace("-20241022", "").replace("-20240307", "")
        model_short = model_short.replace("openai/", "")
        if len(model_short) > 14:
            model_short = model_short[:11] + "..."
    else:
        model_short = "unknown"

    # Message count
    msg_count = session.get("message_count", 0)

    # Cost
    cost = session.get("total_cost", 0.0)
    cost_str = f"${cost:.2f}" if cost > 0 else "$0.00"

    # Last assistant message preview
    last_msg = session.get("last_assistant_message", "")
    if last_msg:
        last_msg = last_msg.replace("\\n", " ").replace("\\t", " ")
        if len(last_msg) > 45:
            last_msg_preview = last_msg[:42] + "..."
        else:
            last_msg_preview = last_msg
    else:
        last_msg_preview = "(no response)"

    # Latest indicator
    latest_badge = " ★" if idx == 0 else ""

    line1 = f"{short_id} │ {time_str} │ {model_short:14} │ {msg_count:3} msgs │ {cost_str:>7}{latest_badge}"
    line2 = f"         └─ {last_msg_preview}"

    return f"{line1}\n{line2}"


def _get_log_files_sorted(logpath: Optional[str] = None) -> list[Path]:
    """
    Get all log files sorted by modification time (newest first).

    Args:
        logpath: Optional custom logs directory. If provided, recursively
                 searches for all .jsonl files in subdirectories.
                 If None, uses ``~/.cai/logs`` (and legacy ``./logs`` when distinct).

    This is fast because it only reads file metadata, not contents.
    """
    if logpath:
        logs_dir = Path(logpath).expanduser()
        if not logs_dir.exists():
            console.print(f"[yellow]Warning: Log path not found: {logpath}[/yellow]")
            return []
        # Recursive search for all .jsonl files in subdirectories
        jsonl_files = list(logs_dir.rglob("*.jsonl"))
    else:
        # Default: cai_*.jsonl in ~/.cai/logs (and legacy ./logs when distinct)
        jsonl_files = _sorted_cai_jsonl_session_files(_default_session_logs_directories())

    jsonl_files.sort(key=lambda f: f.stat().st_mtime, reverse=True)
    return jsonl_files


def _fast_get_session_metadata(log_file: Path) -> Optional[dict]:
    """
    Quickly extract essential metadata from a log file.

    Uses optimized parsing - reads only first and last few KB of file
    to extract model, cost, and message count without full parsing.
    For message count, scans the entire file to find the record with most messages.
    """
    try:
        file_size = log_file.stat().st_size
        if file_size == 0:
            return None

        metadata = {
            "file_path": str(log_file),
            "file_name": log_file.name,
            "session_id": None,
            "model": None,
            "total_cost": 0.0,
            "message_count": 0,
            "start_time": None,
            "last_assistant_message": "",
        }

        # Read first 8KB for session start and model info
        with open(log_file, "rb") as f:
            head_data = f.read(min(8192, file_size)).decode("utf-8", errors="ignore")

            # Quick extraction using string search (faster than json parsing)
            for line in head_data.split("\n")[:20]:
                if not line.strip():
                    continue
                try:
                    # Session ID
                    if '"session_id"' in line and metadata["session_id"] is None:
                        match = re.search(r'"session_id":\s*"([^"]+)"', line)
                        if match:
                            metadata["session_id"] = match.group(1)

                    # Model
                    if '"model"' in line and metadata["model"] is None:
                        match = re.search(r'"model":\s*"([^"]+)"', line)
                        if match:
                            metadata["model"] = match.group(1)

                    # Start time
                    if '"timestamp"' in line and metadata["start_time"] is None:
                        match = re.search(r'"timestamp":\s*"([^"]+)"', line)
                        if match:
                            metadata["start_time"] = match.group(1)

                except Exception:
                    continue

            # Read last 32KB for cost and last assistant message
            read_size = min(32768, file_size)
            if file_size > read_size:
                f.seek(file_size - read_size)
                tail_data = f.read().decode("utf-8", errors="ignore")
            else:
                f.seek(0)
                tail_data = f.read().decode("utf-8", errors="ignore")

        # Extract cost from tail
        lines = tail_data.split("\n")
        for line in reversed(lines):
            if not line.strip():
                continue
            try:
                # Total cost from session_end
                if '"total_cost"' in line and metadata["total_cost"] == 0:
                    match = re.search(r'"total_cost":\s*([\d.]+)', line)
                    if match:
                        metadata["total_cost"] = float(match.group(1))

                # Last assistant content - extract up to 1200 chars
                if '"role": "assistant"' in line and not metadata["last_assistant_message"]:
                    # Try to extract content, handling escaped quotes
                    match = re.search(r'"content":\s*"(.{0,1200}?)(?:"|$)', line, re.DOTALL)
                    if match:
                        content = match.group(1)
                        # Unescape common JSON escapes
                        content = content.replace('\\"', '"')
                        metadata["last_assistant_message"] = content

            except Exception:
                continue

        # Message count - scan entire file for record with most messages
        # Skip Summary Agent records
        role_pattern = b'"role":'
        model_pattern = b'"model":'
        messages_pattern = b'"messages":'
        summary_agent_pattern = b'"Summary Agent"'

        best_msg_count = 0
        with open(log_file, "rb") as f:
            lines_bytes = f.readlines()

        for i, line_bytes in enumerate(lines_bytes):
            # Only check model records with messages
            if model_pattern not in line_bytes or messages_pattern not in line_bytes:
                continue

            # Skip Summary Agent records
            if i + 1 < len(lines_bytes) and summary_agent_pattern in lines_bytes[i + 1]:
                continue

            # Count "role": occurrences as estimate of messages
            msg_count = line_bytes.count(role_pattern)
            if msg_count > best_msg_count:
                best_msg_count = msg_count

        metadata["message_count"] = best_msg_count

        # Skip files with no messages
        if metadata["message_count"] == 0:
            # Try a quick check for chat.completion records
            with open(log_file, "rb") as f:
                content = f.read(min(50000, file_size)).decode("utf-8", errors="ignore")
                if '"chat.completion"' not in content and '"messages"' not in content:
                    return None
                # Estimate message count
                metadata["message_count"] = max(1, content.count('"role"') // 3)

        return metadata

    except Exception:
        return None


def _load_sessions_page(
    log_files: list[Path], file_start_idx: int, count: int
) -> tuple[list[dict], int]:
    """
    Load metadata for a specific page of sessions.

    Scans files starting from file_start_idx until we get `count` valid sessions
    or run out of files. Returns the sessions and the next file index to scan.

    Args:
        log_files: List of all log files sorted by time
        file_start_idx: Index in log_files to start scanning from
        count: Number of valid sessions to return

    Returns:
        Tuple of (sessions list, next file index to scan)
    """
    sessions = []
    idx = file_start_idx

    while len(sessions) < count and idx < len(log_files):
        log_file = log_files[idx]
        metadata = _fast_get_session_metadata(log_file)
        if metadata:
            sessions.append(metadata)
        idx += 1

    return sessions, idx


def _get_all_sessions_with_messages(logpath: Optional[str] = None) -> list[dict]:
    """
    Get all sessions with messages (for fallback selector).

    Args:
        logpath: Optional custom logs directory path.

    Uses fast metadata extraction for each file.
    """
    log_files = _get_log_files_sorted(logpath)
    sessions = []

    for log_file in log_files:
        metadata = _fast_get_session_metadata(log_file)
        if metadata:
            sessions.append(metadata)

    return sessions


def _format_session_line(session: dict, idx: int, selected: bool = False) -> str:
    """Format a single session line for the custom selector."""
    from datetime import datetime

    # ANSI codes
    RESET = "\033[0m"
    BOLD = "\033[1m"
    DIM = "\033[2m"
    MAGENTA = "\033[35m"
    CYAN = "\033[36m"
    YELLOW = "\033[33m"
    GREEN = "\033[32m"
    RED = "\033[31m"
    BG_BLUE = "\033[44m"
    WHITE = "\033[97m"

    session_id = session.get("session_id") or ""
    session_id = session_id[:8] if session_id else "--------"

    start_time = session.get("start_time") or ""
    if start_time:
        try:
            dt = datetime.fromisoformat(start_time.replace("Z", "+00:00"))
            time_str = dt.strftime("%m-%d %H:%M")
        except (ValueError, AttributeError):
            time_str = "Unknown"
    else:
        time_str = "Unknown"

    model = session.get("model", "") or "unknown"
    model_short = model.replace("claude-", "").replace("gpt-", "")
    model_short = model_short.replace("-20241022", "").replace("-20240307", "")
    model_short = model_short.replace("openai/", "")[:12]

    msg_count = session.get("message_count", 0)
    cost = session.get("total_cost", 0.0)
    cost_str = f"${cost:.2f}"

    if cost > 5.0:
        cost_color = RED
    elif cost > 1.0:
        cost_color = YELLOW
    else:
        cost_color = GREEN

    latest = f" {GREEN}★{RESET}" if idx == 0 else ""

    if selected:
        return (
            f"{BG_BLUE}{WHITE}{BOLD} ❯ {session_id} │ {time_str} │ {model_short:12} │ "
            f"{msg_count:3} msgs │ {cost_str:>7}{latest} {RESET}"
        )
    else:
        return (
            f"   {MAGENTA}{BOLD}{session_id}{RESET} {DIM}│{RESET} "
            f"{CYAN}{time_str}{RESET} {DIM}│{RESET} "
            f"{YELLOW}{model_short:12}{RESET} {DIM}│{RESET} "
            f"{msg_count:3} msgs {DIM}│{RESET} "
            f"{cost_color}{cost_str:>7}{RESET}{latest}"
        )


def interactive_session_selector(limit: int = 10, logpath: Optional[str] = None) -> Optional[str]:
    """
    Display an interactive menu to select a session to resume.

    Features:
    - Arrow keys ↑/↓ to navigate sessions
    - Arrow keys ←/→ to navigate between pages
    - Enter to select, Esc/q to cancel
    - Shows full assistant message preview (up to 1000 chars) for highlighted item
    - Lazy loading: only loads metadata for current page

    Args:
        limit: Number of sessions per page
        logpath: Optional custom logs directory. If provided, recursively
                 searches for all .jsonl files in subdirectories.

    Returns:
        Path to the selected session log, or None if cancelled
    """
    import sys
    import tty
    import termios

    def get_key():
        """Read a single keypress."""
        fd = sys.stdin.fileno()
        old_settings = termios.tcgetattr(fd)
        try:
            tty.setraw(fd)
            ch = sys.stdin.read(1)
            if ch == '\x1b':  # Escape sequence
                ch2 = sys.stdin.read(1)
                if ch2 == '[':
                    ch3 = sys.stdin.read(1)
                    if ch3 == 'A':
                        return 'up'
                    elif ch3 == 'B':
                        return 'down'
                    elif ch3 == 'C':
                        return 'right'
                    elif ch3 == 'D':
                        return 'left'
                return 'esc'
            elif ch == '\r' or ch == '\n':
                return 'enter'
            elif ch == 'q' or ch == 'Q':
                return 'quit'
            elif ch == 'j':
                return 'down'
            elif ch == 'k':
                return 'up'
            elif ch == 'h':
                return 'left'
            elif ch == 'l':
                return 'right'
            return ch
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)

    # Get sorted log files (fast - only file metadata)
    log_files = _get_log_files_sorted(logpath)

    if not log_files:
        path_msg = logpath if logpath else str(get_session_logs_dir())
        console.print(f"[yellow]No sessions found in {path_msg}[/yellow]")
        return None

    total_files = len(log_files)
    sessions_per_page = limit

    # Cache for pages: page_num -> (sessions, file_idx_after_page)
    page_cache: dict[int, tuple[list[dict], int]] = {}
    # Track file indices for each page start
    page_file_indices: dict[int, int] = {0: 0}  # Page 0 starts at file index 0
    current_page = 0
    selected_idx = 0  # Currently highlighted session index

    def render_screen(page_sessions, selected_idx, current_page, has_prev, has_next):
        """Render the full selector screen."""
        # Clear screen
        print("\033[2J\033[H", end="", flush=True)

        # ANSI codes
        RESET = "\033[0m"
        BOLD = "\033[1m"
        DIM = "\033[2m"
        CYAN = "\033[36m"
        WHITE = "\033[97m"
        YELLOW = "\033[33m"

        # Header
        print()
        print(f"{CYAN}{BOLD}╭──────────────────────────────────────────────────────────────────────────────╮{RESET}")
        print(f"{CYAN}{BOLD}│{RESET}  {WHITE}{BOLD}↻ Select a session to resume{RESET}                                            {CYAN}{BOLD}│{RESET}")
        print(f"{CYAN}{BOLD}│{RESET}  {DIM}↑/↓/j/k navigate  │  ←/→/h/l pages  │  Enter select  │  q/Esc cancel{RESET}  {CYAN}{BOLD}│{RESET}")
        print(f"{CYAN}{BOLD}╰──────────────────────────────────────────────────────────────────────────────╯{RESET}")
        print()

        # Page info
        nav_hints = []
        if has_prev:
            nav_hints.append("← prev")
        if has_next:
            nav_hints.append("→ next")
        nav_str = f" │ {' │ '.join(nav_hints)}" if nav_hints else ""
        print(f"   {CYAN}{BOLD}Page {current_page + 1}{'+' if has_next else ''}{RESET} {DIM}│ {len(page_sessions)} sessions{nav_str}{RESET}")
        print()

        # Column header
        print(f"   {DIM}ID       │ Date       │ Model        │ Msgs    │ Cost{RESET}")
        print(f"   {DIM}─────────┼────────────┼──────────────┼─────────┼────────{RESET}")

        # Session list
        for idx, session in enumerate(page_sessions):
            global_idx = idx  # For "LATEST" badge on first item of first page
            if current_page == 0:
                global_idx = idx
            else:
                global_idx = idx + 10  # Not first page, no LATEST badge
            line = _format_session_line(session, global_idx if current_page == 0 else idx + 10, selected=(idx == selected_idx))
            print(line)

        # Preview of selected session's full message
        print()
        print(f"   {DIM}{'─' * 74}{RESET}")
        print(f"   {CYAN}{BOLD}Preview:{RESET}")

        if page_sessions and 0 <= selected_idx < len(page_sessions):
            selected_session = page_sessions[selected_idx]
            last_msg = selected_session.get("last_assistant_message", "")
            if last_msg:
                # Clean up the message
                last_msg = last_msg.replace("\\n", "\n").replace("\\t", "  ")
                # Limit to 1000 chars
                if len(last_msg) > 1000:
                    last_msg = last_msg[:997] + "..."
                # Word wrap at ~75 chars
                lines = []
                for paragraph in last_msg.split("\n"):
                    if not paragraph.strip():
                        lines.append("")
                        continue
                    words = paragraph.split()
                    current_line = ""
                    for word in words:
                        if len(current_line) + len(word) + 1 <= 72:
                            current_line += (" " if current_line else "") + word
                        else:
                            if current_line:
                                lines.append(current_line)
                            current_line = word
                    if current_line:
                        lines.append(current_line)
                # Show up to 8 lines of preview
                for line in lines[:8]:
                    print(f"   {DIM}{line}{RESET}")
                if len(lines) > 8:
                    print(f"   {DIM}... ({len(lines) - 8} more lines){RESET}")
            else:
                print(f"   {DIM}(no assistant response){RESET}")
        print()

    try:
        while True:
            # Load only current page (with caching)
            if current_page not in page_cache:
                start_file_idx = page_file_indices.get(current_page, 0)
                sessions, next_file_idx = _load_sessions_page(
                    log_files, start_file_idx, sessions_per_page
                )
                page_cache[current_page] = (sessions, next_file_idx)
                # Remember where next page starts
                if sessions and next_file_idx < len(log_files):
                    page_file_indices[current_page + 1] = next_file_idx

            page_sessions, next_file_idx = page_cache[current_page]

            if not page_sessions:
                console.print("[yellow]No sessions found[/yellow]")
                return None

            # Calculate if there are more pages
            has_next_page = next_file_idx < len(log_files)
            has_prev_page = current_page > 0

            # Clamp selected index
            if selected_idx >= len(page_sessions):
                selected_idx = len(page_sessions) - 1
            if selected_idx < 0:
                selected_idx = 0

            # Render
            render_screen(page_sessions, selected_idx, current_page, has_prev_page, has_next_page)

            # Get key input
            key = get_key()

            if key == 'up':
                selected_idx = max(0, selected_idx - 1)
            elif key == 'down':
                selected_idx = min(len(page_sessions) - 1, selected_idx + 1)
            elif key == 'left' and has_prev_page:
                current_page -= 1
                selected_idx = 0
            elif key == 'right' and has_next_page:
                current_page += 1
                selected_idx = 0
            elif key == 'enter':
                selected_session = page_sessions[selected_idx]
                # Clear and show selection
                print("\033[2J\033[H", end="", flush=True)
                session_id = selected_session.get("session_id") or ""
                session_id = session_id[:8] if session_id else "--------"
                model = selected_session.get("model") or "unknown"
                msg_count = selected_session.get("message_count") or 0
                cost = selected_session.get("total_cost") or 0.0
                console.print()
                console.print(
                    f"[bold green]✓ Selected:[/bold green] [magenta]{session_id}[/magenta] "
                    f"[dim]([/dim][yellow]{model}[/yellow][dim], "
                    f"{msg_count} msgs, ${cost:.2f})[/dim]"
                )
                return selected_session.get("file_path")
            elif key in ('esc', 'quit'):
                print("\033[2J\033[H", end="", flush=True)
                console.print("[dim]Cancelled[/dim]")
                return None

    except KeyboardInterrupt:
        print("\033[2J\033[H", end="", flush=True)
        console.print("[dim]Cancelled[/dim]")
        return None
    except Exception as e:
        print("\033[2J\033[H", end="", flush=True)
        console.print(f"[yellow]Error: {e}, using fallback selector[/yellow]")
        return _fallback_session_selector(limit, logpath)


def _fallback_session_selector(limit: int = 15, logpath: Optional[str] = None) -> Optional[str]:
    """
    Fallback session selector using numbered input (no arrow keys).

    Used when questionary is not available or fails.
    Includes pagination support with 'n' for next page and 'p' for previous.

    Args:
        limit: Number of sessions per page
        logpath: Optional custom logs directory path.
    """
    from datetime import datetime

    # Load all sessions for pagination
    all_sessions = _get_all_sessions_with_messages(logpath)

    if not all_sessions:
        path_msg = logpath if logpath else str(get_session_logs_dir())
        console.print(f"[yellow]No sessions found in {path_msg}[/yellow]")
        return None

    total_sessions = len(all_sessions)
    sessions_per_page = limit
    total_pages = (total_sessions + sessions_per_page - 1) // sessions_per_page
    current_page = 0

    while True:
        # Calculate page slice
        start_idx = current_page * sessions_per_page
        end_idx = min(start_idx + sessions_per_page, total_sessions)
        page_sessions = all_sessions[start_idx:end_idx]

        # Header with box
        console.print()
        console.print("[bold cyan]╭──────────────────────────────────────────────────────────────────────────────╮[/bold cyan]")
        console.print("[bold cyan]│[/bold cyan]  [bold white]↻ Select a session to resume[/bold white]                                            [bold cyan]│[/bold cyan]")
        console.print("[bold cyan]│[/bold cyan]  [dim]Enter number to select  │  n/p for next/prev page  │  q to cancel[/dim]     [bold cyan]│[/bold cyan]")
        console.print("[bold cyan]╰──────────────────────────────────────────────────────────────────────────────╯[/bold cyan]")
        console.print()

        # Pagination info
        console.print(
            f"[bold cyan]   Page {current_page + 1}/{total_pages}[/bold cyan] "
            f"[dim]│[/dim] "
            f"[dim]Showing {start_idx + 1}-{end_idx} of {total_sessions} sessions[/dim]"
        )
        console.print()

        # Column headers
        console.print(
            "[dim]   #  │ ID       │ Date       │ Model          │ Msgs    │ Cost[/dim]"
        )
        console.print("[dim]   ───┼──────────┼────────────┼────────────────┼─────────┼────────[/dim]")

        # Display sessions as a numbered list
        for idx, session in enumerate(page_sessions):
            global_idx = start_idx + idx
            display = _format_session_choice(session, global_idx, use_ansi=False)

            # Color based on recency
            if global_idx == 0:
                color = "bold green"
                badge = " [green]★[/green]"
            elif global_idx < 3:
                color = "yellow"
                badge = ""
            else:
                color = "white"
                badge = ""

            # Format the session info nicely
            session_id = session.get("session_id") or ""
            session_id = session_id[:8] if session_id else "--------"
            start_time = session.get("start_time") or ""
            if start_time:
                try:
                    dt = datetime.fromisoformat(start_time.replace("Z", "+00:00"))
                    time_str = dt.strftime("%m-%d %H:%M")
                except (ValueError, AttributeError):
                    time_str = "Unknown"
            else:
                time_str = "Unknown"

            model = session.get("model") or "unknown"
            model_short = model.replace("claude-", "").replace("gpt-", "")[:14]
            msg_count = session.get("message_count", 0)
            cost = session.get("total_cost", 0.0)
            cost_str = f"${cost:.2f}" if cost > 0 else "$0.00"

            # Cost color
            if cost > 5.0:
                cost_color = "red"
            elif cost > 1.0:
                cost_color = "yellow"
            else:
                cost_color = "green"

            console.print(
                f"  [{color}]{idx + 1:2}[/{color}] [dim]│[/dim] "
                f"[magenta]{session_id}[/magenta] [dim]│[/dim] "
                f"[cyan]{time_str}[/cyan] [dim]│[/dim] "
                f"[yellow]{model_short:14}[/yellow] [dim]│[/dim] "
                f"[white]{msg_count:3} msgs[/white] [dim]│[/dim] "
                f"[{cost_color}]{cost_str:>7}[/{cost_color}]"
                f"{badge}"
            )

            # Last message preview
            last_msg = session.get("last_assistant_message", "")
            if last_msg:
                preview = last_msg[:55] + "..." if len(last_msg) > 55 else last_msg
            else:
                preview = "(no response)"
            console.print(f"       [dim]└─ {preview}[/dim]")

        console.print()

        # Navigation hints
        nav_hints = []
        if current_page > 0:
            nav_hints.append("[cyan]p[/cyan]=prev")
        if current_page < total_pages - 1:
            nav_hints.append("[cyan]n[/cyan]=next")
        nav_hints.append("[cyan]q[/cyan]=cancel")

        console.print(f"[dim]   {' │ '.join(nav_hints)}[/dim]")
        console.print()

        # Get user selection
        try:
            choice = console.input(
                f"[bold cyan]❯[/bold cyan] [bold]Enter selection (1-{len(page_sessions)}): [/bold]"
            )
            choice = choice.strip().lower()

            if choice in ("q", "quit", "exit"):
                console.print("[dim]Cancelled[/dim]")
                return None

            if choice == "n" and current_page < total_pages - 1:
                current_page += 1
                continue

            if choice == "p" and current_page > 0:
                current_page -= 1
                continue

            if choice == "":
                continue

            num = int(choice)
            if 1 <= num <= len(page_sessions):
                selected = page_sessions[num - 1]
                session_id = selected.get("session_id") or ""
                session_id = session_id[:8] if session_id else "--------"
                model = selected.get("model") or "unknown"
                msg_count = selected.get("message_count") or 0
                cost = selected.get("total_cost") or 0.0
                console.print()
                console.print(
                    f"[bold green]✓ Selected:[/bold green] [magenta]{session_id}[/magenta] "
                    f"[dim]([/dim][yellow]{model}[/yellow][dim], "
                    f"{msg_count} msgs, ${cost:.2f})[/dim]"
                )
                return selected.get("file_path")
            else:
                console.print(f"[red]Please enter a number between 1 and {len(page_sessions)}[/red]")

        except ValueError:
            console.print("[red]Invalid input. Enter a number, 'n', 'p', or 'q'.[/red]")
        except KeyboardInterrupt:
            console.print("\n[dim]Cancelled[/dim]")
            return None
