"""
Terminal output formatting and display utilities for CAI.
"""

import json
import os
import re
import shutil
import sys
import textwrap
import time
from typing import Any, Dict, List, Optional

from rich.box import ROUNDED
from rich.console import Console, Group
from rich.markdown import Markdown
from rich.padding import Padding
from rich.panel import Panel
from rich.pretty import install as install_pretty
from rich.syntax import Syntax
from rich.table import Table
from rich.text import Text
from rich.theme import Theme
from rich.traceback import install
from rich.tree import Tree
from wasabi import color


def _session_wall_seconds() -> float:
    """Elapsed wall-clock seconds since CLI session start (for Layout 1 ⏱ pill)."""
    try:
        from cai.util.cli_session_clock import START_TIME as _start

        if _start:
            return time.time() - _start
    except ImportError:
        pass
    return 0.0


def _get_timing_info(execution_info=None):
    """Get timing information for display."""
    import time as _time

    # Get session timing information
    try:
        from cai.util.cli_session_clock import START_TIME as _start

        total_time = _time.time() - _start if _start else None
    except ImportError:
        total_time = None

    # Extract execution timing info
    tool_time = None
    if execution_info:
        tool_time = execution_info.get("tool_time")

    # Format timing info for display
    timing_info = []
    if total_time:
        timing_info.append(f"Total: {format_time(total_time)}")
    if tool_time:
        timing_info.append(f"Tool: {format_time(tool_time)}")

    return timing_info, tool_time


def _tool_command_line_display(tool_name, args) -> str:
    """One-line command for Layout 1 ``>>`` row (no timeout suffix)."""
    if tool_name == "generic_linux_command" and isinstance(args, dict):
        fc = args.get("full_command")
        if isinstance(fc, str) and fc.strip():
            return fc.strip()
        cmd = (args.get("command") or "").strip()
        rest = (args.get("args") or "").strip()
        if rest:
            return f"{cmd} {rest}".strip()
        return cmd or _format_tool_args(args, tool_name=tool_name)
    return _format_tool_args(args, tool_name=tool_name)


# Layout 1 rail: ``  |  `` is 5 columns; continuations indent to match text column.
_RAIL_GUTTER_FIRST = 5
_RAIL_GUTTER_CONT = 5


def _wrap_line_for_tool_rail(
    line: str,
    term_width: int,
    pipe_style: str,
    grey_style: str,
) -> List[Text]:
    """Soft-wrap *line* so continuation rows keep the same left margin as the rail column."""
    usable = max(12, term_width - _RAIL_GUTTER_FIRST)
    chunks = textwrap.wrap(
        line,
        width=usable,
        break_long_words=True,
        break_on_hyphens=False,
    )
    if not chunks:
        chunks = [""]
    rows: List[Text] = []
    for i, chunk in enumerate(chunks):
        t = Text()
        if i == 0:
            t.append("  ", style="")
            t.append("|", style=pipe_style)
            t.append(f"  {chunk}", style=grey_style)
        else:
            t.append(" " * _RAIL_GUTTER_CONT, style="")
            t.append(chunk, style=grey_style)
        rows.append(t)
    return rows


def _tool_capture_output_looks_like_markdown(raw: str) -> bool:
    """Whether to render tool stdout as Markdown under Result/captured (reports, GFM tables).

    Assistant text uses ``parse_message_content``; tool output historically used a plain
    grey rail. Enable with CAI_TOOL_OUTPUT_MARKDOWN=true (default).
    """
    if not raw or not isinstance(raw, str) or len(raw.strip()) < 12:
        return False
    if os.getenv("CAI_TOOL_OUTPUT_MARKDOWN", "true").lower() in ("0", "false", "no"):
        return False
    if re.search(r"^#{1,6}\s+\S", raw, re.MULTILINE):
        return True
    if re.search(r"^\|[^\n]+\|\s*$", raw, re.MULTILINE):
        return True
    if re.search(r"^ {0,3}---+\s*$", raw, re.MULTILINE):
        return True
    if "**" in raw or "__" in raw:
        return True
    if re.search(r"\[[^\]]+\]\([^)]+\)", raw):
        return True
    if re.search(r"^\s*[-*+]\s+\S", raw, re.MULTILINE):
        return True
    if re.search(r"^\s*\d+\.\s+\S", raw, re.MULTILINE):
        return True
    return False


def _layout1_status_pills(execution_info, tool_time_s: float, args=None) -> Text:
    """⏱ wall·tool, ⌂ env:host, ✓ COMPLETED / ✗ ERROR / running."""
    from cai.util.cli_palette import (
        BADGE_ENV_BG,
        BADGE_ENV_FG,
        BADGE_TIME_BG,
        BADGE_TIME_FG,
        COMPLETED_PILL,
        ERROR_PILL,
    )

    wall_s = _session_wall_seconds()
    if execution_info and execution_info.get("wall_elapsed") is not None:
        try:
            wall_s = float(execution_info["wall_elapsed"])
        except (TypeError, ValueError):
            pass

    tt = float(tool_time_s or 0.0)
    status = (execution_info or {}).get("status") or ""
    rc = (execution_info or {}).get("return_code")
    is_error = status == "error" or status == "timeout" or (
        rc is not None and rc != 0
    )

    t = Text()
    t.append("⏱ ", style=f"bold {BADGE_TIME_FG} on {BADGE_TIME_BG}")
    t.append(f"{wall_s:.1f}s · {tt:.1f}s ", style=f"{BADGE_TIME_FG} on {BADGE_TIME_BG}")

    env = (execution_info or {}).get("environment", "") or "Local"
    host = (execution_info or {}).get("host", "") or ""
    if not host and isinstance(args, dict):
        w = args.get("workspace")
        if isinstance(w, str) and w.strip():
            host = w.strip()
    label = f"{env}:{host}" if host else env
    t.append("⌂ ", style=f"bold {BADGE_ENV_FG} on {BADGE_ENV_BG}")
    t.append(f"{label} ", style=f"{BADGE_ENV_FG} on {BADGE_ENV_BG}")

    if status == "running":
        t.append("⋯ ", style="bold yellow")
        t.append("RUNNING", style="bold yellow")
    elif is_error:
        t.append("✗ ", style=ERROR_PILL)
        t.append("ERROR", style=ERROR_PILL)
    else:
        t.append("✓ ", style=COMPLETED_PILL)
        t.append("COMPLETED", style=COMPLETED_PILL)
    return t


def _layout1_combined_header(
    tool_name: str,
    args,
    execution_info,
    token_info,
) -> Text:
    """● agent ─ tool_id TOOL  [pills].

    The ``[Pn]`` agent-id pill is only rendered when more than one agent is
    running in parallel (``CAI_PARALLEL > 1``); the static ``AGENT`` sufix has
    been retired as redundant noise.
    """
    from cai.util.cli_palette import CAI_GREEN
    from cai.util.session import is_parallel_session

    agent_name = ""
    if token_info and isinstance(token_info, dict):
        agent_name = (token_info.get("agent_name") or "").strip()

    _, tool_time = _get_timing_info(execution_info)
    tool_time = tool_time if tool_time is not None else 0.0
    if execution_info and execution_info.get("tool_time") is not None:
        try:
            tool_time = float(execution_info["tool_time"])
        except (TypeError, ValueError):
            pass

    is_parallel = is_parallel_session()

    pills = _layout1_status_pills(execution_info, tool_time, args)
    t = Text()
    is_handoff = tool_name.startswith("transfer_to_")

    t.append("● ", style=f"bold {CAI_GREEN}")
    if agent_name:
        t.append(agent_name, style=f"bold {CAI_GREEN}")
        aid = ""
        if token_info and isinstance(token_info, dict):
            aid = (token_info.get("agent_id") or "").strip()
        if is_parallel and aid and f"[{aid}]" not in agent_name:
            t.append(f" [{aid}]", style="bold cyan")
        t.append(" ─ ", style="dim white")
    else:
        t.append("Agent", style=f"bold {CAI_GREEN}")
        t.append(" ─ ", style="dim white")

    if is_handoff:
        raw = tool_name[len("transfer_to_") :]
        nice = " ".join(w.capitalize() for w in raw.split("_"))
        t.append(f"→ {nice}", style="bold yellow")
        t.append("   ")
        t.append_text(pills)
        return t

    t.append(tool_name, style="bold white")
    t.append(" TOOL", style=f"italic {CAI_GREEN}")
    t.append("   ")
    t.append_text(pills)
    return t


def _create_token_info_display(token_info=None):
    """Create token information display text."""
    if not token_info:
        return None

    from cai.util.tokens import _create_token_display

    model = token_info.get("model", "")
    interaction_input_tokens = token_info.get("interaction_input_tokens", 0)
    interaction_output_tokens = token_info.get("interaction_output_tokens", 0)
    interaction_reasoning_tokens = token_info.get("interaction_reasoning_tokens", 0)
    total_input_tokens = token_info.get("total_input_tokens", 0)
    total_output_tokens = token_info.get("total_output_tokens", 0)
    total_reasoning_tokens = token_info.get("total_reasoning_tokens", 0)

    has_interaction_tokens = (interaction_input_tokens > 0 or interaction_output_tokens > 0)
    has_total_tokens = (total_input_tokens > 0 or total_output_tokens > 0)
    if not (has_interaction_tokens or has_total_tokens):
        return None

    return _create_token_display(
        interaction_input_tokens,
        interaction_output_tokens,
        interaction_reasoning_tokens,
        total_input_tokens,
        total_output_tokens,
        total_reasoning_tokens,
        model,
        interaction_cost=token_info.get("interaction_cost"),
        interaction_input_cost=token_info.get("interaction_input_cost"),
        interaction_output_cost=token_info.get("interaction_output_cost"),
        total_cost=token_info.get("total_cost"),
        total_input_cost=token_info.get("total_input_cost"),
        total_output_cost=token_info.get("total_output_cost"),
        cache_read_tokens=token_info.get("cache_read_tokens", 0),
        cache_creation_tokens=token_info.get("cache_creation_tokens", 0),
        cache_read_savings=token_info.get("cache_read_savings", 0.0),
        cache_creation_extra=token_info.get("cache_creation_extra", 0.0),
    )


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


theme = Theme(
    {
        "timestamp": "#00BCD4",
        "agent": "#4CAF50",
        "arrow": "#FFFFFF",
        "content": "#ECEFF1",
        "tool": "#F44336",
        "cost": "#009688",
        "args_str": "#FFC107",
        "border": "#2196F3",
        "border_state": "#FFD700",
        "model": "#673AB7",
        "dim": "#9E9E9E",
        "current_token_count": "#E0E0E0",
        "total_token_count": "#757575",
        "context_tokens": "#0A0A0A",
        "success": "#4CAF50",
        "warning": "#FF9800",
        "error": "#F44336",
    }
)

console = Console(theme=theme)
install()
install_pretty()

# Helper function to format time in a human-readable way
def format_time(seconds):
    """Human-friendly time formatter (robust).

    Accepts int/float or tuple/list (uses first element). Returns "N/A"
    for None or non-numeric values instead of throwing.
    """
    try:
        if seconds is None:
            return "N/A"
        if isinstance(seconds, (list, tuple)) and seconds:
            seconds = seconds[0]
        seconds = float(seconds)
    except Exception:
        return "N/A"

    if seconds < 60:
        return f"{seconds:.1f}s"
    elif seconds < 3600:
        minutes = int(seconds / 60)
        seconds_remainder = seconds % 60
        return f"{minutes}m {seconds_remainder:.1f}s"
    else:
        hours = int(seconds / 3600)
        minutes = int((seconds % 3600) / 60)
        return f"{hours}h {minutes}m"


# Helper function to sanitize output for display in Rich panels
def _sanitize_output_for_display(output):
    """
    Sanitize output to remove control characters that cause display issues.

    Some commands like `lynis` use:
    - Carriage return (\\r) to update progress on the same line
    - ANSI cursor movement codes like \\x1b[2C (move cursor N positions)
    - Other escape sequences that confuse Rich Live panel rendering

    This causes Rich Live panels to miscalculate content size, resulting in
    the panel header being duplicated/repeated down the screen.

    This function:
    - Removes ANSI cursor movement/positioning sequences
    - Handles \\r (carriage return) by keeping only the last segment per line
    - Preserves colors and basic formatting (bold, etc.)
    - Preserves binary/hex/assembly output
    """
    if not output or not isinstance(output, str):
        return output

    import re

    # Check if this looks like binary/hex output - if so, minimal processing
    hex_pattern_count = len(re.findall(r'\\x[0-9a-fA-F]{2}|0x[0-9a-fA-F]+|[0-9a-fA-F]{8}:', output[:1000]))
    if hex_pattern_count > 10:
        return output.replace('\r\n', '\n').replace('\r', '')

    # Remove ANSI cursor movement/positioning sequences that break Rich Live panels
    # These are the problematic ones that cause panel duplication:
    # - \x1b[nC = cursor forward n columns (lynis uses this heavily)
    # - \x1b[nD = cursor back n columns
    # - \x1b[nA = cursor up n lines
    # - \x1b[nB = cursor down n lines
    # - \x1b[n;mH or \x1b[n;mf = cursor position
    # - \x1b[nG = cursor to column n
    # - \x1b[s/\x1b[u = save/restore cursor
    # - \x1b[?25h/l = show/hide cursor
    result = re.sub(r'\x1b\[\d*[ABCDGHJKST]', '', output)  # Cursor movement
    result = re.sub(r'\x1b\[\d+;\d*[Hf]', '', result)      # Cursor positioning
    result = re.sub(r'\x1b\[[su]', '', result)              # Save/restore cursor
    result = re.sub(r'\x1b\[\?25[hl]', '', result)          # Show/hide cursor
    result = re.sub(r'\x1b\[\d*[JK]', '', result)           # Clear screen/line

    # Handle \r if present
    if '\r' in result:
        lines = result.split('\n')
        cleaned_lines = []
        for line in lines:
            if '\r' in line:
                segments = line.split('\r')
                non_empty_segments = [s for s in segments if s.strip()]
                if non_empty_segments:
                    cleaned_lines.append(non_empty_segments[-1])
                elif segments:
                    cleaned_lines.append(segments[-1])
            else:
                cleaned_lines.append(line)
        result = '\n'.join(cleaned_lines)

    return result


def _format_tool_args(args, tool_name=None):
    """Format tool arguments as a clean string."""
    # If the tool is execute_code, we don't want to show any args in the main header,
    # as they are detailed in subsequent panels (either code or args string).
    if tool_name == "execute_code":
        return ""

    # If args is already a string, it might be pre-formatted or a simple arg string
    if isinstance(args, str):
        # If it looks like a JSON dict string, try to parse and format nicely
        if args.strip().startswith("{") and args.strip().endswith("}"):
            try:
                parsed_dict = json.loads(args)
                # Recursively call with the parsed dict for consistent formatting
                return _format_tool_args(parsed_dict, tool_name=tool_name)
            except json.JSONDecodeError:
                # Not valid JSON, or not a dict; return as is
                return args
        else:
            # Simple string arg, return as is
            return args

    # Format arguments from a dictionary
    if isinstance(args, dict):
        # Keys to skip in regular display (shown separately or internal)
        skip_keys = {"async_mode", "streaming", "refresh_rate", "full_command",
                     "timeout", "timeout_source", "timeout_countdown", "elapsed",
                     "command", "args", "workspace", "container", "environment"}

        # For generic_linux_command, show as normalized terminal command
        if tool_name == "generic_linux_command":
            # Build command string like a real terminal
            cmd = args.get("command", "")
            cmd_args = args.get("args", "")
            full_cmd = f"{cmd} {cmd_args}".strip() if cmd_args else cmd

            # Truncate if too long
            if len(full_cmd) > 120:
                full_cmd = full_cmd[:117] + "..."

            # Build timeout/elapsed info for the end
            timeout_info = ""
            source = args.get("timeout_source", "")
            source_label = {"llm": "llm", "env:CAI_TOOL_TIMEOUT": "env", "default": "default"}.get(source, source)

            if "timeout_countdown" in args and args["timeout_countdown"]:
                # Streaming state: show live countdown (e.g., "300s|285.2s")
                countdown = args["timeout_countdown"]
                timeout_info = f" [{source_label}:{countdown}]"
            elif "elapsed" in args and args["elapsed"]:
                # Final state: show elapsed time
                elapsed = args["elapsed"]
                timeout_info = f" [{source_label} elapsed:{elapsed}]"
            elif "timeout" in args and args["timeout"]:
                # Initial state: show timeout limit
                timeout_val = args["timeout"]
                timeout_info = f" [{source_label}:{timeout_val}s]"

            return f"{full_cmd}{timeout_info}"

        # For other tools, use key=value format
        arg_parts = []
        for key, value in args.items():
            # Skip empty values
            if value == "" or value == {} or value is None:
                continue
            # Skip special flags and timeout-related keys
            if key in skip_keys:
                continue
            if key in ["async_mode", "streaming"] and not value:
                continue

            value_str = str(value)

            # Format the value
            if isinstance(value, str):
                # Truncate long string values
                if len(value_str) > 70 and key not in ["code", "args"]:
                    value_str = value_str[:67] + "..."
                arg_parts.append(f"{key}={value_str}")
            else:
                arg_parts.append(f"{key}={value_str}")

        # Add timeout info at the end for non-generic tools
        if "elapsed" in args and args["elapsed"]:
            elapsed = args["elapsed"]
            source = args.get("timeout_source", "")
            source_label = {"llm": "llm", "env:CAI_TOOL_TIMEOUT": "env", "default": "default"}.get(source, source)
            arg_parts.append(f"[timeout:{source_label} elapsed:{elapsed}]")
        elif "timeout" in args and args["timeout"]:
            timeout_val = args["timeout"]
            source = args.get("timeout_source", "")
            source_label = {"llm": "llm", "env:CAI_TOOL_TIMEOUT": "env", "default": "default"}.get(source, source)
            arg_parts.append(f"[timeout:{timeout_val}s src:{source_label}]")

        return ", ".join(arg_parts)
    else:
        return str(args)


def _print_simple_tool_output(tool_name, args, output, execution_info=None, token_info=None):
    """Print tool output without Rich formatting.
    
    Pricing suppressed — shown only in agent messages via cli_print_agent_messages.
    """
    # Format arguments
    args_str = _format_tool_args(args, tool_name=tool_name)

    # Get tool execution time if available
    tool_time_str = ""
    execution_status = ""
    if execution_info:
        time_taken = execution_info.get("time_taken", 0) or execution_info.get("tool_time", 0)
        status = execution_info.get("status", "completed")

        # Add execution info to the tool call display
        if time_taken:
            tool_time_str = f"Tool: {format_time(time_taken)}"
            execution_status = f" [{status} in {time_taken:.2f}s]"
        else:
            execution_status = f" [{status}]"

    # Create timing display string
    timing_info, _ = _get_timing_info(execution_info)
    timing_display = f" [{' | '.join(timing_info)}]" if timing_info else ""

    # Show tool name, args, execution status and timing display
    tool_call = f"{tool_name}({args_str})"

    # Truncate output if it's too long, except for Memory (show full)
    # CAI_DISPLAY_MAX_OUTPUT=true shows full output, false (default) truncates at 10000 chars
    show_full_output = os.getenv("CAI_DISPLAY_MAX_OUTPUT", "false").lower() in ("true", "1", "yes")
    if not show_full_output and tool_name != "Memory" and output and len(str(output)) > 10000:
        output_str = str(output)
        first_part = output_str[:5000]
        last_part = output_str[-5000:]
        output = f"{first_part}\n\n... TRUNCATED ...\n\n{last_part}"

    # Print the actual output (but not in TUI mode where it's already shown in panels)
    if os.getenv("CAI_TUI_MODE") != "true":
        print(output)
        print()


# Add a new function to start a streaming tool execution

def _create_tool_panel_content(
    tool_name,
    args,
    output,
    execution_info=None,
    token_info=None,
    *,
    include_tool_wait_hint: bool = True,
):
    """Create the header and flat content for tool output (Layout 1 — compact rail, no panel)."""
    from cai.util.cli_palette import CAI_GREEN, GREY_HINT, GREY_TEXT, PIPE_GREY

    # Truncate output if it's too long, except for Memory (show full)
    show_full_output = os.getenv("CAI_DISPLAY_MAX_OUTPUT", "false").lower() in ("true", "1", "yes")
    if not show_full_output and tool_name != "Memory" and output and len(str(output)) > 10000:
        output_str = str(output)
        first_part = output_str[:5000]
        last_part = output_str[-5000:]
        output = f"{first_part}\n\n... TRUNCATED ...\n\n{last_part}"

    # Sanitize output; strip trailing newlines so we do not render blank lines under tool output
    if output and isinstance(output, str):
        output = _sanitize_output_for_display(output)
        output = output.rstrip("\n\r")

    header_line = _layout1_combined_header(tool_name, args, execution_info, token_info)
    body_parts: list = [header_line]

    cmd_display = _tool_command_line_display(tool_name, args)
    t_cmd = Text()
    t_cmd.append("  >> ", style=f"bold {CAI_GREEN}")
    t_cmd.append(cmd_display if cmd_display else "(no command)", style="bold white")
    body_parts.append(t_cmd)

    body_parts.append(Text("  Result", style="bold white"))
    body_parts.append(Text("  captured", style=GREY_TEXT))

    pipe_style = f"dim {PIPE_GREY}"
    term_w = max(40, shutil.get_terminal_size((100, 24)).columns)

    if include_tool_wait_hint:
        try:
            from cai.util.wait_hints import get_tool_wait_body_plain

            wait_msg = get_tool_wait_body_plain()
        except Exception:
            wait_msg = None
        # If the async hint loop is off (no TTY / disabled), still show a rail line while RUNNING.
        if (
            (not wait_msg or not str(wait_msg).strip())
            and (execution_info or {}).get("status") == "running"
        ):
            cmd = _tool_command_line_display(tool_name, args)
            if cmd and cmd.strip() and cmd != "(no command)":
                wait_msg = f"Executing: {cmd}"
        if wait_msg and str(wait_msg).strip():
            wstrip = str(wait_msg).strip()
            out_first = ""
            if output and str(output).strip():
                out_first = str(output).strip().split("\n", 1)[0].strip()
            if out_first != wstrip:
                wait_style = f"italic dim {GREY_HINT}"
                for line in str(wait_msg).split("\n"):
                    body_parts.extend(
                        _wrap_line_for_tool_rail(line, term_w, pipe_style, wait_style)
                    )

    if tool_name == "execute_code" and isinstance(args, dict):
        code_str = args.get("code") or args.get("args") or ""
        if code_str:
            for line in code_str.split("\n"):
                body_parts.extend(
                    _wrap_line_for_tool_rail(line, term_w, pipe_style, GREY_TEXT)
                )

    if output and output.strip():
        lines = output.split("\n")
        while lines and lines[-1].strip() == "":
            lines.pop()
        max_lines = 40
        if len(lines) > max_lines:
            head = lines[: max_lines // 2]
            tail = lines[-(max_lines // 2) :]
            omitted = len(lines) - max_lines
            lines = head + [f"  ... ({omitted} lines omitted) ..."] + tail
        joined = "\n".join(lines)
        if _tool_capture_output_looks_like_markdown(joined):
            body_parts.append(Padding(Markdown(joined), (0, 2, 0, 2)))
        else:
            for line in lines:
                body_parts.extend(
                    _wrap_line_for_tool_rail(line, term_w, pipe_style, GREY_TEXT)
                )

    return header_line, Group(*body_parts)


def parse_message_content(message):
    """
    Parse a message object to extract its textual content.
    Only processes messages that don't have tool calls.
    Detects markdown code blocks and applies syntax highlighting in non-streaming mode.
    Also formats other markdown elements like headers, lists, and text formatting.

    Args:
        message: Can be a string or a Message object with content attribute

    Returns:
        str or rich.console.Group: The extracted content as a string or as a rich Group with Syntax highlighting
    """
    import re

    from rich.markdown import Markdown

    # Extract the raw content
    raw_content = ""

    # If message is already a string, use it
    if isinstance(message, str):
        raw_content = message
    # If message is a Message object with content attribute
    elif hasattr(message, "content") and message.content is not None:
        raw_content = message.content
    # If message is a dict with content key
    elif isinstance(message, dict) and "content" in message:
        raw_content = message["content"]
    # If we can't extract content, convert to string
    else:
        raw_content = str(message)

    # Check if streaming is enabled
    streaming_enabled = is_tool_streaming_enabled()

    # Only apply markdown formatting in non-streaming mode
    if not streaming_enabled and raw_content:
        # Check if content contains markdown code blocks with improved regex
        code_block_pattern = r"```(\w*)\s*([\s\S]*?)\s*```"
        matches = re.findall(code_block_pattern, raw_content, re.DOTALL)

        if matches:
            # Prepare to process markdown with code blocks highlighted
            elements = []
            last_end = 0

            # Find all code blocks with improved regex pattern
            for match in re.finditer(r"```(\w*)\s*([\s\S]*?)\s*```", raw_content, re.DOTALL):
                # Get text before the code block
                start = match.start()
                if start > last_end:
                    text_before = raw_content[last_end:start]

                    # Process markdown in the text before the code block
                    if text_before.strip():
                        md = Markdown(text_before)
                        elements.append(md)

                # Process the code block
                lang = match.group(1) or "text"
                code = match.group(2)

                # Use the language mapping helper to get proper syntax highlighting
                syntax_lang = get_language_from_code_block(lang)

                # Create syntax highlighted code
                syntax = Syntax(
                    code,
                    syntax_lang,
                    theme="monokai",
                    line_numbers=True,
                    word_wrap=True,
                    background_color="#272822",
                )
                elements.append(syntax)

                last_end = match.end()

            # Add any remaining text after the last code block
            if last_end < len(raw_content):
                text_after = raw_content[last_end:]

                # Process markdown in the text after the code block
                if text_after.strip():
                    md = Markdown(text_after)
                    elements.append(md)

            return Group(*elements)
        else:
            # If no code blocks, but still contains markdown, use Rich's markdown renderer
            # Check for markdown elements (headers, lists, formatting)
            has_markdown = any(
                [
                    # Headers
                    re.search(r"^#{1,6}\s+\w+", raw_content, re.MULTILINE),
                    # Lists
                    re.search(r"^\s*[-*+]\s+\w+", raw_content, re.MULTILINE),
                    re.search(r"^\s*\d+\.\s+\w+", raw_content, re.MULTILINE),
                    # Bold/Italic
                    "**" in raw_content,
                    "*" in raw_content and "**" not in raw_content,
                    "__" in raw_content,
                    "_" in raw_content and "__" not in raw_content,
                    # Links
                    re.search(r"\[.+?\]\(.+?\)", raw_content),
                ]
            )

            if has_markdown:
                return Group(Markdown(raw_content))

    # For streaming mode or no markdown, return the raw content
    return raw_content


def parse_message_tool_call(message, tool_output=None):
    """
    Parse a message object to extract its content and tool calls.
    Displays tool calls in the format: tool_name({"command":"","args":"","ctf":{},"async_mode":false,"session_id":""})
    and shows the tool output in a separated panel.

    Args:
        message: A Message object or dict with content and tool_calls attributes
        tool_output: String containing the output from the tool execution

    Returns:
        tuple: (content, tool_panels) where content is the message text and
               tool_panels is a list of panels representing tool calls and outputs
    """
    content = ""
    tool_panels = []

    # Extract the content text (LLM's inference)
    if isinstance(message, str):
        content = message
    elif hasattr(message, "content") and message.content is not None:
        content = message.content
    elif isinstance(message, dict) and "content" in message:
        content = message["content"]

    # Extract tool calls
    tool_calls = None
    if hasattr(message, "tool_calls") and message.tool_calls:
        tool_calls = message.tool_calls
    elif isinstance(message, dict) and "tool_calls" in message and message["tool_calls"]:
        tool_calls = message["tool_calls"]

    # Process tool calls if they exist
    if tool_calls:
        from rich.text import Text

        for tool_call in tool_calls:
            # Extract tool name and arguments
            tool_name = None
            args_dict = {}
            call_id = None

            # Handle different formats of tool_call objects
            if hasattr(tool_call, "function"):
                if hasattr(tool_call.function, "name"):
                    tool_name = tool_call.function.name
                if hasattr(tool_call.function, "arguments"):
                    try:
                        import json

                        args_dict = json.loads(tool_call.function.arguments)
                    except:
                        args_dict = {"raw_arguments": tool_call.function.arguments}
            elif isinstance(tool_call, dict):
                if "function" in tool_call:
                    if "name" in tool_call["function"]:
                        tool_name = tool_call["function"]["name"]
                    if "arguments" in tool_call["function"]:
                        try:
                            import json

                            args_dict = json.loads(tool_call["function"]["arguments"])
                        except:
                            args_dict = {"raw_arguments": tool_call["function"]["arguments"]}
                elif tool_call.get("name"):
                    # Responses-API style: { "name", "arguments", "id" } without nested "function"
                    tool_name = tool_call["name"]
                    raw_args = tool_call.get("arguments", "{}")
                    try:
                        import json

                        if isinstance(raw_args, str):
                            args_dict = json.loads(raw_args)
                        elif isinstance(raw_args, dict):
                            args_dict = raw_args
                        else:
                            args_dict = {"raw_arguments": raw_args}
                    except Exception:
                        args_dict = {"raw_arguments": raw_args}
                    call_id = tool_call.get("id")
            elif hasattr(tool_call, "name") and not hasattr(tool_call, "function"):
                # Simple wrapper objects (e.g. openai_responses ToolCallWrapper)
                tool_name = getattr(tool_call, "name", None)
                raw_args = getattr(tool_call, "arguments", None)
                call_id = getattr(tool_call, "id", None)
                if raw_args is not None:
                    try:
                        import json

                        if isinstance(raw_args, str):
                            args_dict = json.loads(raw_args)
                        elif isinstance(raw_args, dict):
                            args_dict = raw_args
                        else:
                            args_dict = {"raw_arguments": raw_args}
                    except Exception:
                        args_dict = {"raw_arguments": raw_args}

            # Create a panel for this tool call if name is not None
            # NOTE: Tool execution panel will be handled in cli_print_tool_output
            # Pass on tool info to generate panels for display in cli_print_agent_messages
            if tool_name and tool_output:
                # Skip creating tool output panel for execute_code
                # execute_code already shows its output through streaming panels
                if tool_name == "execute_code":
                    # Check if we're in streaming mode
                    streaming_enabled = is_tool_streaming_enabled()
                    if streaming_enabled:
                        # Skip creating the panel - output already shown via streaming
                        continue

                # Flat tool output (Layout 1 rail)
                from cai.util.cli_palette import GREY_TEXT, PIPE_GREY

                pipe = f"dim {PIPE_GREY}"
                output_text = Text()
                output_text.append("  ", style="")
                output_text.append("|", style=pipe)
                output_text.append(f"  {tool_output}", style=GREY_TEXT)

                tool_panels.append(output_text)

                # Store the call_id with tool name to help cli_print_tool_output avoid duplicates
                if not hasattr(parse_message_tool_call, "_processed_calls"):
                    parse_message_tool_call._processed_calls = set()

                call_key = call_id if call_id else f"{tool_name}:{args_dict}"
                parse_message_tool_call._processed_calls.add(call_key)

    return content, tool_panels


def is_tool_output_message(message):
    """Check if a message appears to be a tool output panel display message."""
    if isinstance(message, str):
        msg_lower = message.lower()
        return ("call id:" in msg_lower and "output:" in msg_lower) or msg_lower.startswith(
            "tool output"
        )
    return False



def print_message_history(messages, title="Message History"):
    """
    Pretty-print a sequence of messages with enhanced debug information.

    Args:
        messages (List[dict]): List of message dictionaries to display
        title (str, optional): Title to display above the message history
    """
    from rich.console import Console
    from rich.panel import Panel

    console = Console()

    # Create a table for displaying messages
    table = Table(show_header=True, header_style="bold magenta", expand=True)
    table.add_column("#", style="dim", width=3)
    table.add_column("Role", style="cyan", width=10)
    table.add_column("Content", width=1000)
    table.add_column("Metadata", width=1000)

    # Process each message
    for i, msg in enumerate(messages):
        # Get role with color based on type
        role = msg.get("role", "unknown")
        role_style = {
            "user": "green",
            "assistant": "blue",
            "system": "yellow",
            "tool": "magenta",
        }.get(role, "white")

        # Get content preview
        content = msg.get("content")
        content_preview = ""
        if content is None:
            content_preview = "[dim]None[/dim]"
        elif isinstance(content, str):
            # Truncate and escape long content
            content_preview = (content[:37] + "...") if len(content) > 40 else content
            content_preview = content_preview.replace("\n", "\\n")
        elif isinstance(content, list):
            content_preview = f"[list with {len(content)} items]"
        else:
            content_preview = f"[{type(content).__name__}]"

        # Gather metadata
        metadata = []
        if msg.get("tool_calls"):
            tc_count = len(msg["tool_calls"])
            tc_info = []
            for tc in msg["tool_calls"]:
                tc_id = tc.get("id", "unknown")
                tc_name = (
                    tc.get("function", {}).get("name", "unknown") if "function" in tc else "unknown"
                )
                tc_info.append(f"{tc_name}({tc_id})")
            metadata.append(f"tool_calls[{tc_count}]: {', '.join(tc_info)}")

        if msg.get("tool_call_id"):
            metadata.append(f"tool_call_id: {msg['tool_call_id']}")

        metadata_str = ", ".join(metadata)

        # Add row to table
        table.add_row(str(i), f"[{role_style}]{role}[/{role_style}]", content_preview, metadata_str)

    # Create the panel with the table
    panel = Panel(table, title=f"[bold]{title}[/bold]", expand=False)

    # Display the panel
    console.print(panel)

    return len(messages)  # Return message count for convenience



def get_language_from_code_block(lang_identifier):
    """
    Maps a language identifier from a markdown code block to a proper syntax
    highlighting language name. Handles common aliases and defaults.

    Args:
        lang_identifier (str): Language identifier from markdown code block

    Returns:
        str: Proper language name for syntax highlighting
    """
    # Convert to lowercase and strip whitespace
    lang = lang_identifier.lower().strip() if lang_identifier else ""

    # Map common language aliases to their proper names
    lang_map = {
        # Empty strings or unknown
        "": "text",
        # Python variants
        "py": "python",
        "python3": "python",
        # JavaScript variants
        "js": "javascript",
        "jsx": "jsx",
        "ts": "typescript",
        "tsx": "tsx",
        "typescript": "typescript",
        # Shell variants
        "sh": "bash",
        "shell": "bash",
        "console": "bash",
        "terminal": "bash",
        # Web languages
        "html": "html",
        "css": "css",
        "json": "json",
        "xml": "xml",
        "yml": "yaml",
        "yaml": "yaml",
        # C family
        "c": "c",
        "cpp": "cpp",
        "c++": "cpp",
        "csharp": "csharp",
        "cs": "csharp",
        "java": "java",
        # Other common languages
        "go": "go",
        "golang": "go",
        "ruby": "ruby",
        "rb": "ruby",
        "rust": "rust",
        "php": "php",
        "sql": "sql",
        "diff": "diff",
        "markdown": "markdown",
        "md": "markdown",
        # Default fallback
        "text": "text",
        "plaintext": "text",
        "txt": "text",
    }

    # Return mapped language or default to the original if not in map
    return lang_map.get(lang, lang or "text")


def check_flag(output, ctf, challenge=None):
    """
    Check if the CTF flag is present in the output.

    Args:
        output (str): The output to check for the flag.
        ctf: The CTF environment object.
        challenge (str, optional): The specific challenge to check.
            Defaults to None.

    Returns:
        tuple: A tuple containing a boolean indicating if the flag was
            found and the flag itself if found, otherwise None.
    """
    # Get the challenge from the environment variable or default to the first
    # challenge
    challenge_key = os.getenv("CTF_CHALLENGE")
    challenges = list(ctf.get_challenges().keys())
    challenge = (
        challenge_key
        if challenge_key in challenges
        else (challenges[0] if len(challenges) > 0 else None)
    )
    if ctf:
        if ctf.check_flag(output, challenge):  # check if the flag is in the output
            flag = ctf.flags[challenge]
            print(
                color(f"Flag found: {flag}", fg="green")
                + " in output "
                + color(f"{output}", fg="blue")
            )
            return True, flag
    else:
        print(color("CTF environment not found or provided", fg="yellow"))
    return False, None


def sanitize_message_list(messages):  # pylint: disable=R0914,R0915,R0912
    """
    Sanitizes the message list passed as a parameter to align with the
    OpenAI API message format.

    Adjusts the message list to comply with the following rules:
        1. A tool call id appears no more than twice.
        2. Each tool call id appears as a pair, and both messages
            must have content.
        3. If a tool call id appears alone (without a pair), it is removed.
        4. There cannot be empty messages.
        5. Each tool_use block (assistant with tool_calls) must be followed by
           a tool_result block (tool message with matching tool_call_id).
        6. Each 'tool' message must be immediately preceded by an 'assistant' message
           with matching tool_call_id in its tool_calls.
        7. Tool call IDs are truncated to 40 characters for API compatibility.

    Args:
        messages (List[dict]): List of message dictionaries containing
                            role, content, and optionally tool_calls or
                            tool_call_id fields.

    Returns:
        List[dict]: Sanitized list of messages with invalid tool calls
                   and empty messages removed.
    """
    # Deep-copy to ensure we don't modify the input
    sanitized_messages = []

    # First, truncate all tool call IDs to 40 characters throughout the messages
    # This ensures consistency for providers like DeepSeek that have strict ID matching
    for msg in messages:
        msg_copy = msg.copy()

        # IMPORTANT: Preserve cache_control for Anthropic prompt caching
        if "cache_control" in msg:
            msg_copy["cache_control"] = msg["cache_control"]

        # Truncate tool_call_id in tool messages
        if msg_copy.get("role") == "tool" and msg_copy.get("tool_call_id"):
            if len(msg_copy["tool_call_id"]) > 40:
                msg_copy["tool_call_id"] = msg_copy["tool_call_id"][:40]

        # Truncate IDs in assistant tool_calls
        if msg_copy.get("role") == "assistant" and msg_copy.get("tool_calls"):
            tool_calls_copy = []
            for tc in msg_copy["tool_calls"]:
                tc_copy = tc.copy()
                if tc_copy.get("id") and len(tc_copy["id"]) > 40:
                    tc_copy["id"] = tc_copy["id"][:40]
                tool_calls_copy.append(tc_copy)
            msg_copy["tool_calls"] = tool_calls_copy

        sanitized_messages.append(msg_copy)

    # Now process the messages with truncated IDs
    processed_messages = []
    tool_call_map = {}  # Map from tool_call_id to (assistant_idx, tool_idx)

    for i, msg in enumerate(sanitized_messages):
        # Skip empty messages (considered empty if 'content' is None or only whitespace)
        if msg.get("role") in ["user", "system"] and (
            msg.get("content") is None or not str(msg.get("content", "")).strip()
        ):
            # Special case: if it's a system message, set content to empty string instead of skipping
            if msg.get("role") == "system":
                # Replace None with empty string
                msg["content"] = ""
                processed_messages.append(msg)
            # Skip empty user messages entirely
            continue

        # Add valid messages to our processed list first
        processed_messages.append(msg)

        # Now track tool calls and tool messages for pairing
        if msg.get("role") == "assistant" and msg.get("tool_calls"):
            for tc in msg["tool_calls"]:
                if tc.get("id"):
                    tool_id = tc.get("id")
                    if tool_id not in tool_call_map:
                        tool_call_map[tool_id] = {
                            "assistant_idx": len(processed_messages) - 1,
                            "tool_idx": None,
                        }

        if msg.get("role") == "tool" and msg.get("tool_call_id"):
            tool_id = msg.get("tool_call_id")
            if tool_id in tool_call_map:
                tool_call_map[tool_id]["tool_idx"] = len(processed_messages) - 1
            else:
                # Tool response without a matching tool call - create a synthetic pair
                # by adding a dummy assistant message with a tool_call
                assistant_msg = {
                    "role": "assistant",
                    "content": None,
                    "tool_calls": [
                        {
                            "id": tool_id,
                            "type": "function",
                            "function": {"name": "unknown_function", "arguments": "{}"},
                        }
                    ],
                }
                # Insert the assistant message *before* the tool message
                processed_messages.insert(len(processed_messages) - 1, assistant_msg)
                # Update mapping
                tool_call_map[tool_id] = {
                    "assistant_idx": len(processed_messages) - 2,
                    "tool_idx": len(processed_messages) - 1,
                }

    # Second pass - ensure correct sequence (tool messages must directly follow their assistant messages)
    # This fixes the error "messages with role 'tool' must be a response to a preceeding message with 'tool_calls'"
    i = 0
    processed_positions = set()  # Track positions we've already processed to avoid infinite loops
    while i < len(processed_messages):
        # Skip if we've already processed this position
        if i in processed_positions:
            i += 1
            continue

        msg = processed_messages[i]

        # Check if this is a tool message that might be out of sequence
        if msg.get("role") == "tool" and msg.get("tool_call_id"):
            tool_id = msg.get("tool_call_id")

            # If this isn't the first message, check if the previous message is a matching assistant message
            if i > 0:
                prev_msg = processed_messages[i - 1]

                # Check if the previous message is an assistant message with matching tool_call_id
                is_valid_sequence = (
                    prev_msg.get("role") == "assistant"
                    and prev_msg.get("tool_calls")
                    and any(tc.get("id") == tool_id for tc in prev_msg.get("tool_calls", []))
                )

                if not is_valid_sequence:
                    # Find the assistant message with this tool_call_id
                    assistant_idx = None
                    for j, assistant_msg in enumerate(processed_messages):
                        if (
                            assistant_msg.get("role") == "assistant"
                            and assistant_msg.get("tool_calls")
                            and any(
                                tc.get("id") == tool_id
                                for tc in assistant_msg.get("tool_calls", [])
                            )
                        ):
                            assistant_idx = j
                            break

                    # If we found a matching assistant message, move this tool message right after it
                    if assistant_idx is not None:
                        # Mark current position as processed before moving
                        processed_positions.add(i)

                        # Remember to save the tool message
                        tool_msg = processed_messages.pop(i)

                        # Insert right after the assistant message
                        processed_messages.insert(assistant_idx + 1, tool_msg)

                        # Adjust i to account for the move
                        if assistant_idx < i:
                            # We moved the message backward, so i should point to the next message
                            # which is now at position i (since we removed a message before it)
                            # Don't increment i, just continue to reprocess the same position
                            continue
                        else:
                            # We moved the message forward, so i should now point to the message
                            # that is now at position i. Skip to the next position to avoid reprocessing
                            i += 1
                            continue
                    else:
                        # No matching assistant message found - create one
                        assistant_msg = {
                            "role": "assistant",
                            "content": None,
                            "tool_calls": [
                                {
                                    "id": tool_id,
                                    "type": "function",
                                    "function": {"name": "unknown_function", "arguments": "{}"},
                                }
                            ],
                        }

                        # Insert the assistant message before the tool message
                        processed_messages.insert(i, assistant_msg)

                        # Skip past both messages
                        i += 2
                        continue
            else:
                # This tool message is at index 0, which means there's no preceding assistant message
                # Create a dummy assistant message
                assistant_msg = {
                    "role": "assistant",
                    "content": None,
                    "tool_calls": [
                        {
                            "id": tool_id,
                            "type": "function",
                            "function": {"name": "unknown_function", "arguments": "{}"},
                        }
                    ],
                }

                # Insert the assistant message before the tool message
                processed_messages.insert(0, assistant_msg)

                # Skip past both messages
                i += 2
                continue

        # Move to the next message
        i += 1

    # Final validation - ensure all tool calls have responses
    for tool_id, indices in list(tool_call_map.items()):
        if indices["tool_idx"] is None:
            # Tool call without a response - create a synthetic tool message
            assistant_idx = indices["assistant_idx"]
            assistant_msg = processed_messages[assistant_idx]

            # Find the relevant tool call
            tool_name = "unknown_function"
            for tc in assistant_msg["tool_calls"]:
                if tc.get("id") == tool_id:
                    if tc.get("function") and tc["function"].get("name"):
                        tool_name = tc["function"]["name"]
                    break

            # Create an automatic tool response message
            tool_msg = {
                "role": "tool",
                "tool_call_id": tool_id,
                "content": f"Auto-generated response for {tool_name}",
            }

            # Insert immediately after the assistant message
            if assistant_idx + 1 < len(processed_messages):
                # Insert at the position after assistant
                processed_messages.insert(assistant_idx + 1, tool_msg)
            else:
                # Just append if we're at the end
                processed_messages.append(tool_msg)

            # Update the map to note that this tool call now has a response
            tool_call_map[tool_id]["tool_idx"] = assistant_idx + 1

    # Ensure messages have non-null content (required by some providers)
    for msg in processed_messages:
        # For assistant messages with tool_calls, content can be None
        if msg.get("role") == "assistant" and msg.get("tool_calls"):
            # Assistant messages with tool calls can have None content - this is valid
            pass
        elif msg.get("role") != "tool" and msg.get("content") is None and not msg.get("tool_calls"):
            # For non-tool messages without tool_calls, ensure content is not None
            msg["content"] = ""

        # For tool messages, ensure content is never null or empty
        if msg.get("role") == "tool":
            if msg.get("content") is None or msg.get("content") == "":
                msg["content"] = f"Tool response for {msg.get('tool_call_id', 'unknown')}"

    # Special case for Claude: ensure strict alternating pattern between assistant tool_calls and tool results
    # If multiple consecutive assistant messages with tool_calls exist, interleave them with tool responses
    i = 0
    while i < len(processed_messages) - 1:
        current_msg = processed_messages[i]
        next_msg = processed_messages[i + 1]

        # When current message is assistant with tool_calls and next message is NOT a tool response
        if (
            current_msg.get("role") == "assistant"
            and current_msg.get("tool_calls")
            and (next_msg.get("role") != "tool" or not next_msg.get("tool_call_id"))
        ):
            # Get the first tool call ID
            tool_id = current_msg["tool_calls"][0].get("id", "unknown")
            tool_name = "unknown_function"
            if current_msg["tool_calls"][0].get("function"):
                tool_name = current_msg["tool_calls"][0]["function"].get("name", "unknown_function")

            # Create a tool result message
            tool_msg = {
                "role": "tool",
                "tool_call_id": tool_id,
                "content": f"Auto-generated response for {tool_name}",
            }

            # Insert the tool message after the current assistant message
            processed_messages.insert(i + 1, tool_msg)

            # Skip over the newly inserted message
            i += 2
        else:
            i += 1

    return processed_messages


# Alias for backward compatibility - used by openai_chatcompletions.py
fix_message_list = sanitize_message_list


