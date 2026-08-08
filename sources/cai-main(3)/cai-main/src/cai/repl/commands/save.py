"""
Save command for CAI REPL.

Writes conversation histories to JSONL (round-trip with /load) or Markdown (human-readable).
"""

import json
import os
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional, Tuple

from rich.console import Console  # pylint: disable=import-error

from cai.repl.commands.base import Command, register_command

console = Console()


def _resolve_output_path(filepath: str) -> str:
    """Expand ~ and env-style user paths; normalize for the OS."""
    return os.path.normpath(os.path.expanduser(filepath.strip()))


def _is_markdown_path(filepath: str) -> bool:
    p = filepath.lower().strip()
    return p.endswith(".md") or p.endswith(".markdown")


def _ensure_parent_dir(out_path: str) -> bool:
    parent = os.path.dirname(out_path)
    if not parent:
        return True
    try:
        os.makedirs(parent, exist_ok=True)
    except OSError as exc:
        console.print(f"[red]Error creating directory: {exc}[/red]")
        return False
    return True


def _collect_histories() -> Dict[str, list]:
    from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
    from cai.sdk.agents.parallel_isolation import PARALLEL_ISOLATION

    if PARALLEL_ISOLATION.has_isolated_histories():
        return dict(PARALLEL_ISOLATION._isolated_histories.items())
    return AGENT_MANAGER.get_all_histories()


def _format_md_body(text: str) -> str:
    text = text.strip() if text else ""
    if not text:
        return "*[empty]*\n\n"
    fence = "```"
    if fence in text:
        fence = "~~~"
    return f"{fence}\n{text}\n{fence}\n\n"


def write_conversation_jsonl(filepath: str) -> Tuple[bool, int, int]:
    """Write all agent histories to filepath as JSONL (one message per line).

    Returns:
        (success, line_count, agent_count). On failure line_count and agent_count are 0.
    """
    all_histories = _collect_histories()

    if not all_histories or all(len(h) == 0 for h in all_histories.values()):
        return True, 0, 0

    out_path = _resolve_output_path(filepath)
    if not _ensure_parent_dir(out_path):
        return False, 0, 0

    total_lines = 0
    try:
        with open(out_path, "w", encoding="utf-8") as f:
            for agent_name, history in all_histories.items():
                for msg in history:
                    record: Dict[str, Any] = {
                        "agent": agent_name,
                        "role": msg.get("role", "unknown"),
                        "content": msg.get("content", ""),
                    }
                    if msg.get("tool_calls"):
                        record["tool_calls"] = msg["tool_calls"]
                    if msg.get("tool_call_id"):
                        record["tool_call_id"] = msg["tool_call_id"]
                    f.write(json.dumps(record, ensure_ascii=False) + "\n")
                    total_lines += 1
    except OSError as exc:
        console.print(f"[red]Error writing file: {exc}[/red]")
        return False, 0, 0

    return True, total_lines, len(all_histories)


def write_conversation_markdown(filepath: str) -> Tuple[bool, int, int]:
    """Write all agent histories as a readable Markdown report.

    Returns:
        (success, message_count, agent_count).
    """
    from cai.repl.session_resume import normalize_message_content

    all_histories = _collect_histories()

    if not all_histories or all(len(h) == 0 for h in all_histories.values()):
        return True, 0, 0

    out_path = _resolve_output_path(filepath)
    if not _ensure_parent_dir(out_path):
        return False, 0, 0

    total_messages = 0
    parts: List[str] = [
        "# CAI conversation export\n\n",
        f"*Exported: {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')}*\n\n",
        f"*Agents: {len(all_histories)}*\n\n",
        "---\n\n",
    ]

    try:
        for agent_name, history in all_histories.items():
            safe_heading = str(agent_name).replace("\n", " ").replace("#", "").strip() or "unknown"
            parts.append(f"## {safe_heading}\n\n")

            for msg in history:
                total_messages += 1
                role = str(msg.get("role", "unknown")).replace("\n", " ")
                parts.append(f"### {role}\n\n")

                body = normalize_message_content(msg.get("content"))
                parts.append(_format_md_body(body))

                if msg.get("tool_calls"):
                    parts.append("**tool_calls**\n\n")
                    tc = json.dumps(msg["tool_calls"], ensure_ascii=False, indent=2)
                    parts.append(_format_md_body(tc))

                if msg.get("role") == "tool" and msg.get("tool_call_id"):
                    parts.append(f"*tool_call_id:* `{msg['tool_call_id']}`\n\n")

        with open(out_path, "w", encoding="utf-8") as f:
            f.write("".join(parts))
    except OSError as exc:
        console.print(f"[red]Error writing file: {exc}[/red]")
        return False, 0, 0

    return True, total_messages, len(all_histories)


class SaveCommand(Command):
    """Command to save conversation histories to JSONL or Markdown."""

    def __init__(self):
        super().__init__(
            name="/save",
            description="Save all agent histories to .jsonl (for /load) or .md (readable report)",
            aliases=[],
        )

    def handle(self, args: Optional[List[str]] = None) -> bool:
        if not args:
            console.print("[red]Error: Output file path required[/red]")
            console.print("[dim]Usage: /save <file>[/dim]")
            console.print("[dim]Examples: /save session.jsonl   /save report.md[/dim]")
            return False

        raw_path = " ".join(args)
        if _is_markdown_path(raw_path):
            ok, count, agent_count = write_conversation_markdown(raw_path)
            kind = "markdown"
        else:
            ok, count, agent_count = write_conversation_jsonl(raw_path)
            kind = "jsonl"

        if not ok:
            return False

        if count == 0:
            console.print("[yellow]No conversation history to save[/yellow]")
            return True

        display_path = _resolve_output_path(raw_path)
        unit = "messages" if kind == "markdown" else "lines"
        console.print(
            f"[green]Saved {count} {unit} from "
            f"{agent_count} agent(s) to [bold]{display_path}[/bold][/green]"
        )
        if kind == "jsonl":
            console.print(f"[dim]Reload with: /load {display_path}[/dim]")
        else:
            console.print(
                "[dim]Markdown is for reading or sharing; use [bold].jsonl[/bold] with /load to restore context.[/dim]"
            )
        return True


register_command(SaveCommand())
