"""
Replay command for CAI REPL/TUI.

This command replays a JSONL conversation file inside the TUI output, similar to
the standalone `cai-replay` CLI tool.
"""

from __future__ import annotations

import os
import asyncio
import json
from typing import List, Optional, Dict

from rich.console import Console  # pylint: disable=import-error

from cai.repl.commands.base import Command, register_command

console = Console()
_REPLAY_TASKS: Dict[str, asyncio.Task] = {}


class ReplayCommand(Command):
    """Command to replay a JSONL conversation in the TUI."""

    def __init__(self) -> None:
        super().__init__(
            name="/replay",
            description="Replay a JSONL conversation (like cai-replay)",
            aliases=[],
        )

    def handle(self, args: Optional[List[str]] = None) -> bool:  # noqa: D401
        """Handle `/replay <jsonl_file> [delay_seconds]`.

        Examples:
        - /replay logs/last
        - /replay logs/session.jsonl 0.25
        - /replay "/path/with spaces/session.jsonl" 0.1
        """
        # Disable session recording during replay to avoid polluting logs
        os.environ["CAI_DISABLE_SESSION_RECORDING"] = "true"

        # Stop subcommand (cancel running replay in this terminal)
        if args and len(args) == 1 and args[0].lower() in {"stop", "cancel"}:
            if os.getenv("CAI_TUI_MODE") == "true":
                try:
                    from cai.tui.routing.output_router import get_current_terminal_context, get_terminal_output
                    term_id, _term_num = get_current_terminal_context()
                    if term_id and term_id in _REPLAY_TASKS:
                        task = _REPLAY_TASKS.pop(term_id)
                        task.cancel()
                        out = get_terminal_output(term_id)
                        if out:
                            out.write("[yellow]Replay cancelled[/yellow]")
                        return True
                except Exception:
                    pass
            console.print("[yellow]No active replay to cancel[/yellow]")
            return True

        # Defaults
        replay_delay = 0.5

        # Parse args: allow an optional numeric delay as the last arg, treat the rest as file path
        file_path = None
        if args and len(args) > 0:
            # If the last arg looks like a float, treat it as delay
            maybe_delay = args[-1]
            try:
                # Accept both integer and float values
                replay_delay = float(maybe_delay)
                # Remaining args (possibly joined) is the file path
                remaining = args[:-1]
                file_path = " ".join(remaining).strip() if remaining else None
            except ValueError:
                # No delay provided; whole args compose the file path (allow spaces)
                file_path = " ".join(args).strip()
        else:
            # No args -> default to logs/last
            file_path = "logs/last"

        if not file_path:
            file_path = "logs/last"

        # Expand user and env vars, keep relative paths as-is (same behavior as tools/replay)
        file_path = os.path.expandvars(os.path.expanduser(file_path))

        # Validate file existence or symlink
        if not os.path.exists(file_path):
            console.print(f"[red]Error: JSONL file not found: {file_path}[/red]")
            console.print("Usage: /replay <jsonl_file> [delay_seconds]")
            return False

        try:
            # If running in TUI, schedule an async live replay so the UI stays responsive
            if os.getenv("CAI_TUI_MODE") == "true":
                # Resolve current terminal context
                try:
                    from cai.tui.routing.output_router import (
                        get_current_terminal_context,
                        set_terminal_context,
                        get_terminal_output,
                    )
                except Exception:  # pragma: no cover
                    get_current_terminal_context = None  # type: ignore
                    set_terminal_context = None  # type: ignore
                    get_terminal_output = None  # type: ignore

                term_id, term_num = (None, None)
                if get_current_terminal_context:
                    term_id, term_num = get_current_terminal_context()

                # Write a small header to the terminal if available
                if term_id and get_terminal_output:
                    out = get_terminal_output(term_id)
                    if out:
                        try:
                            out.write(f"[cyan]Replaying:[/cyan] {file_path}  [dim](delay={replay_delay}s)[/dim]")
                        except Exception:
                            pass

                # Schedule the async task and return immediately
                try:
                    loop = asyncio.get_running_loop()
                    # Cancel any existing replay on this terminal
                    if term_id and term_id in _REPLAY_TASKS:
                        try:
                            _REPLAY_TASKS[term_id].cancel()
                        except Exception:
                            pass

                    # Put action bar into compact mode for replay to maximize space
                    try:
                        from cai.tui.core.terminal_widget_registry import get_terminal_widget
                        tw = get_terminal_widget(term_id) if term_id else None
                        if tw and hasattr(tw, 'action_bar') and hasattr(tw.action_bar, 'set_compact'):
                            tw.action_bar.set_compact(True)
                    except Exception:
                        pass

                    task = loop.create_task(
                        _async_live_replay(file_path, replay_delay, term_id, term_num)
                    )
                    if term_id:
                        _REPLAY_TASKS[term_id] = task
                    return True
                except RuntimeError:
                    # No running loop (unlikely in TUI) – fall back to sync path
                    pass

            # Fallback: non-TUI or no loop – use synchronous CLI replay
            from tools import replay as replay_tool
            from cai.sdk.agents.run_to_jsonl import get_token_stats, load_history_from_jsonl

            console.print(f"[cyan]Replaying:[/cyan] {file_path}  [dim](delay={replay_delay}s)[/dim]")
            full_data = replay_tool.load_jsonl(file_path)
            messages = load_history_from_jsonl(file_path)
            usage = get_token_stats(file_path)

            if not messages:
                console.print(f"[yellow]No messages found in {file_path}[/yellow]")
                return True

            replay_tool.replay_conversation(
                messages, replay_delay=replay_delay, usage=usage, jsonl_file_path=file_path, full_data=full_data
            )

            # Show summary
            active_time = usage[4] if len(usage) > 4 else 0
            idle_time = usage[5] if len(usage) > 5 else 0
            total_time = active_time + idle_time

            def _fmt(seconds: float) -> str:
                if seconds < 60:
                    return f"{seconds:.1f}s"
                h, rem = divmod(int(seconds), 3600)
                m, s = divmod(rem, 60)
                return f"{h}h {m}m {s}s" if h else f"{m}m {s}s"

            metrics = {
                "session_time": _fmt(total_time),
                "llm_time": "0.0s",
                "llm_percentage": 0,
                "active_time": _fmt(active_time),
                "idle_time": _fmt(idle_time),
            }
            try:
                replay_tool.display_execution_time(metrics)
            except Exception:
                console.print(
                    f"[dim]Session: {metrics['session_time']} • Active: {metrics['active_time']} • Idle: {metrics['idle_time']}[/dim]"
                )
            console.print("[green]Replay completed[/green]")
            return True

        except Exception as e:  # pylint: disable=broad-exception-caught
            console.print(f"[red]Replay error: {e}[/red]")
            return False


async def _async_live_replay(file_path: str, delay: float, term_id: Optional[str], term_num: Optional[int]) -> None:
    """Async, TUI-native, step-by-step replay that yields to the event loop.

    Args:
        file_path: Path to JSONL
        delay: Seconds between steps
        term_id: Target terminal id (for routing)
        term_num: Target terminal number
    """
    # Import lazily to keep command import light
    from tools import replay as replay_tool
    from cai.sdk.agents.run_to_jsonl import get_token_stats, load_history_from_jsonl
    from cai.tui.routing.output_router import set_terminal_context, get_terminal_output
    from cai.tui.display.integration import display_agent_messages, display_tool_output

    # Ensure routing context is set for this coroutine
    if term_id and term_num:
        set_terminal_context(term_id, term_num)

    # Load data
    full_data = replay_tool.load_jsonl(file_path)
    messages = load_history_from_jsonl(file_path)
    usage = get_token_stats(file_path)

    # Resolve output widget for user prompts and summaries
    out = get_terminal_output(term_id) if term_id else None

    if not messages:
        if out:
            out.write(f"[yellow]No messages found in {file_path}[/yellow]")
        return

    # Build mappings for tool outputs to avoid hanging tool calls during replay
    tool_outputs: Dict[str, str] = {}  # by call_id
    tool_outputs_by_name: Dict[str, list[str]] = {}  # by tool name (fallback queue)
    tool_msg_indices: list[int] = []  # indices of standalone tool messages (ordered)
    for idx, msg in enumerate(messages):
        if not isinstance(msg, dict):
            continue
        if msg.get("role") == "tool":
            # Map by tool_call_id when present
            call_id = msg.get("tool_call_id")
            content = msg.get("content", "") or ""
            if call_id and content:
                tool_outputs[call_id] = content
            # Also collect by name if available
            name = msg.get("name") or msg.get("tool_name")
            if name and content:
                tool_outputs_by_name.setdefault(name, []).append(content)
            # Track index for ordered fallback search
            tool_msg_indices.append(idx)

    # Extract agent names from full data if available
    current_agent_name = None
    for entry in full_data:
        if entry.get("agent_name"):
            current_agent_name = entry.get("agent_name")
        if entry.get("event") == "agent_run_start" and entry.get("agent_name"):
            current_agent_name = entry.get("agent_name")

    # Utilities to sanitize token info to avoid formatting errors
    def _as_int(val):
        try:
            if val is None:
                return 0
            if isinstance(val, bool):
                return int(val)
            if isinstance(val, (int, float)):
                return int(val)
            s = str(val).strip()
            if s == "" or s.lower() == "none":
                return 0
            return int(float(s))
        except Exception:
            return 0

    def _as_float(val):
        try:
            if val is None:
                return 0.0
            if isinstance(val, (int, float)):
                return float(val)
            s = str(val).strip()
            if s == "" or s.lower() == "none":
                return 0.0
            return float(s)
        except Exception:
            return 0.0

    def _build_token_info(msg: dict, fallback_model: str, agent_name: str, counter: int) -> dict:
        return {
            "interaction_input_tokens": _as_int(msg.get("input_tokens") or msg.get("interaction_input_tokens")),
            "interaction_output_tokens": _as_int(msg.get("output_tokens") or msg.get("interaction_output_tokens")),
            "interaction_reasoning_tokens": _as_int(msg.get("reasoning_tokens") or msg.get("interaction_reasoning_tokens")),
            "total_input_tokens": _as_int(msg.get("total_input_tokens")),
            "total_output_tokens": _as_int(msg.get("total_output_tokens")),
            "total_reasoning_tokens": _as_int(msg.get("total_reasoning_tokens")),
            "interaction_cost": _as_float(msg.get("interaction_cost")),
            "total_cost": _as_float(usage[3] if len(usage) > 3 else 0.0),
            "model": str(msg.get("model") or fallback_model or ""),
            "agent_name": str(agent_name or "Assistant"),
            "interaction_counter": int(counter),
        }

    # Step through messages with async sleeps to keep UI responsive
    interaction_counter = 0
    file_model = usage[0]

    consumed_tool_msg_indices: set[int] = set()
    displayed_call_ids: set[str] = set()
    for i, message in enumerate(messages):
        try:
            role = message.get("role", "")
            if role == "system":
                continue

            if role == "user":
                content = str(message.get("content", "")).strip()
                if out and content:
                    out.write(f"{replay_tool.color('CAI> ', fg='cyan')}{content}")
                interaction_counter += 1
                await asyncio.sleep(delay)
                continue

            if role == "assistant":
                # Prepare a single-message list for the display API
                display_msg = dict(message)  # shallow copy
                # Attach last known agent name if not present
                if current_agent_name and not display_msg.get("agent_name"):
                    display_msg["agent_name"] = current_agent_name

                # Display the assistant message panel
                try:
                    ti = _build_token_info(
                        display_msg,
                        display_msg.get("model", file_model),
                        display_msg.get("agent_name") or "Assistant",
                        interaction_counter,
                    )
                    display_agent_messages(
                        messages=[display_msg],
                        model=ti.get("model", file_model),
                        agent_name=ti.get("agent_name", "Assistant"),
                        counter=interaction_counter,
                        token_info=ti,
                    )
                except Exception:
                    # Never break replay due to display issues
                    pass

                await asyncio.sleep(delay)

                # Display tool calls and outputs progressively
                for tool_call in display_msg.get("tool_calls", []) or []:
                    fn = tool_call.get("function", {}) if isinstance(tool_call, dict) else {}
                    tool_name = fn.get("name", "")
                    args_str = fn.get("arguments", "{}")
                    try:
                        args_obj = json.loads(args_str) if isinstance(args_str, str) and args_str.strip().startswith("{") else args_str
                    except Exception:
                        args_obj = args_str

                    call_id = tool_call.get("id", "")
                    output_text = ""

                    # 1) Primary: by exact call_id
                    if call_id and call_id in tool_outputs and call_id not in displayed_call_ids:
                        output_text = tool_outputs[call_id]
                        displayed_call_ids.add(call_id)

                    # 2) Secondary: by function name queue
                    if not output_text and tool_name and tool_name in tool_outputs_by_name and tool_outputs_by_name[tool_name]:
                        output_text = tool_outputs_by_name[tool_name].pop(0)

                    # 3) Tertiary: first unconsumed standalone tool message after this assistant
                    if not output_text:
                        for j in range(i + 1, len(messages)):
                            nxt = messages[j]
                            if isinstance(nxt, dict) and nxt.get("role") == "tool" and j not in consumed_tool_msg_indices:
                                output_text = str(nxt.get("content", ""))
                                consumed_tool_msg_indices.add(j)
                                break

                    # 4) Existing mapping inserted by loader under assistant["tool_outputs"]
                    if not output_text and "tool_outputs" in display_msg and isinstance(display_msg["tool_outputs"], dict):
                        if call_id and call_id in display_msg["tool_outputs"]:
                            output_text = display_msg["tool_outputs"][call_id]

                    # Show tool output when any content was found
                    if output_text:
                        try:
                            ti_tool = _build_token_info(
                                display_msg,
                                display_msg.get("model", file_model),
                                display_msg.get("agent_name") or "Assistant",
                                interaction_counter,
                            )
                            display_tool_output(
                                tool_name=tool_name,
                                args=args_obj,
                                output=output_text,
                                call_id=call_id,
                                token_info=ti_tool,
                                streaming=False,
                            )
                        except Exception:
                            # Do not surface errors during replay
                            pass
                        await asyncio.sleep(delay)
                    else:
                        # As a last resort, emit a small note to avoid a "hanging" call with no visible result
                        if out:
                            out.write(f"[dim yellow]No output captured for tool '{tool_name}'[/dim yellow]")

                # Count this assistant turn as a step after printing tools
                interaction_counter += 1
                continue

        except asyncio.CancelledError:  # Propagate cancellation cleanly
            return
        except Exception as e:
            # Log error in debug mode but don't break the replay
            if os.getenv("CAI_DEBUG") == "1" and out:
                out.write(f"[dim red]Replay step error: {str(e)[:100]}[/dim red]")
            await asyncio.sleep(delay)

    # Final summary in TUI
    active_time = usage[4] if len(usage) > 4 else 0
    idle_time = usage[5] if len(usage) > 5 else 0
    total_time = active_time + idle_time

    def _fmt(seconds: float) -> str:
        if seconds < 60:
            return f"{seconds:.1f}s"
        h, rem = divmod(int(seconds), 3600)
        m, s = divmod(rem, 60)
        return f"{h}h {m}m {s}s" if h else f"{m}m {s}s"

    if out:
        out.write(
            f"[dim]Session: {_fmt(total_time)} • Active: {_fmt(active_time)} • Idle: {_fmt(idle_time)}[/dim]"
        )
        out.write("[green]Replay completed[/green]")

    # Restore action bar size and clean up streaming indicators
    try:
        from cai.tui.core.terminal_widget_registry import get_terminal_widget
        tw = get_terminal_widget(term_id) if term_id else None
        if tw and hasattr(tw, 'action_bar'):
            ab = tw.action_bar
            if hasattr(ab, 'stop_streaming'):
                ab.stop_streaming()
            if hasattr(ab, 'set_compact'):
                ab.set_compact(False)
            if hasattr(ab, 'clear_execution_indicator'):
                ab.clear_execution_indicator()
    except Exception:
        pass


# Register command on import
register_command(ReplayCommand())
