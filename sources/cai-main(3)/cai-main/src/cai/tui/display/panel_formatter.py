"""
Panel formatting utilities for TUI display
"""

import json
import re
from typing import Any, Dict, Optional, Tuple, Union
from datetime import datetime

from rich.box import ROUNDED
from rich.console import Group
from rich.panel import Panel
from rich.syntax import Syntax
from rich.text import Text
from rich.table import Table

from cai.util import (
    enrich_token_info_for_pricing,
    _create_token_display as cli_create_token_display,
)


class PanelFormatter:
    """Formats data into Rich panels for display"""

    # Simple cache for recently created panels
    _panel_cache = {}
    _cache_size = 50

    # Language mapping for syntax highlighting
    LANGUAGE_MAP = {
        "": "text",
        "py": "python",
        "python3": "python",
        "js": "javascript",
        "jsx": "jsx",
        "ts": "typescript",
        "tsx": "tsx",
        "sh": "bash",
        "shell": "bash",
        "console": "bash",
        "terminal": "bash",
        "yml": "yaml",
        "yaml": "yaml",
        "c++": "cpp",
        "cs": "csharp",
        "rb": "ruby",
        "md": "markdown",
        "txt": "text",
        "plaintext": "text",
    }

    @classmethod
    def create_tool_panel(
        cls,
        tool_name: str,
        args: Union[Dict, str],
        output: str,
        execution_info: Optional[Dict] = None,
        token_info: Optional[Dict] = None,
        streaming: bool = False,
    ) -> Panel:
        """Create a panel for tool execution display"""
        # Normalize args: allow JSON string to act like dict so special renderers work
        try:
            if isinstance(args, str):
                import json as _json
                parsed = _json.loads(args)
                if isinstance(parsed, dict):
                    args = parsed
        except Exception:
            pass
        # Create header
        header = cls._create_tool_header(tool_name, args, execution_info, streaming)

        # Create content
        content_group = [header]

        # Enrich header context for session inputs of generic_linux_command
        try:
            if tool_name == "generic_linux_command" and isinstance(args, dict) and args.get("input_to_session"):
                sid = str(args.get("session_id", "")).strip()
                env = str(args.get("environment", "")).strip()
                if sid or env:
                    meta = Text()
                    meta.append("Session ", style="dim")
                    if sid:
                        meta.append(f"{sid}", style="cyan")
                    if env:
                        meta.append("  ")
                        meta.append(f"[{env}]", style="magenta")
                    content_group.extend([Text(""), meta])
        except Exception:
            pass

        # Special handling for different tool types
        if tool_name == "execute_code":
            # Show both the code (for context) and its output in the universal terminal
            cls._add_execute_code_panels(content_group, args, output)
        elif tool_name == "generic_linux_command" or "command" in tool_name.lower() or "shell" in tool_name.lower():
            # Include the invoked command line before the output if available
            cls._add_command_output_panel(content_group, output, args)
        elif output and output.strip():
            cls._add_generic_output_panel(content_group, output)

        # Add token info if available
        if token_info:
            # Prefer an explicit model from token_info; avoid referencing undefined vars
            try:
                default_model = token_info.get("model") if isinstance(token_info, dict) else None
            except Exception:
                default_model = None
            token_display = cls._create_token_display(token_info, default_model=default_model)
            if token_display:
                content_group.extend([Text("\n"), token_display])

        # Determine panel style
        border_style, title = cls._get_panel_style(
            tool_name, args, execution_info, token_info, streaming
        )

        return Panel(
            Group(*content_group),
            title=title,
            border_style=border_style,
            padding=(0, 1),
            box=ROUNDED,
            title_align="left",
        )

    @classmethod
    def create_agent_panel(
        cls,
        agent_name: str,
        message: str,
        metadata: Optional[Dict] = None,
        streaming: bool = False,
        token_info: Optional[Dict] = None,
    ) -> Panel:
        """Create a panel for agent messages"""
        # Extract interaction counter
        interaction = 1
        if metadata and "interaction" in metadata:
            interaction = metadata["interaction"]

        # Check if this is a reasoner agent
        is_reasoner = "reasoner" in agent_name.lower()
        border_style = "red" if is_reasoner else "green"

        # Build title matching CLI format exactly
        title_parts = []

        # Counter and agent name
        title_parts.append(f"[bold cyan][{interaction}][/bold cyan]")
        title_parts.append(f"[bold green]{agent_name}[/bold green] >>")

        if streaming:
            title_parts.append("[yellow]Streaming...[/yellow]")
        else:
            title_parts.append("[green]Response[/green]")

        # Timestamp and model
        if metadata:
            title_parts.append(f"[dim][{metadata.get('timestamp', '')}")
            if metadata.get("model"):
                title_parts.append(f"({metadata['model']})[/dim]")
            else:
                title_parts.append("][/dim]")

        # Add comprehensive token stats if available
        if token_info and (
            token_info.get("interaction_input_tokens", 0) > 0
            or token_info.get("input_tokens", 0) > 0
        ):
            # Ensure token info carries correct pricing and model context
            try:
                default_model = metadata.get("model") if isinstance(metadata, dict) else None
                token_info = enrich_token_info_for_pricing(token_info, default_model=default_model)
            except Exception:
                # If enrichment fails for any reason, continue with the original data
                pass
            # Current iteration stats
            input_tokens = token_info.get(
                "interaction_input_tokens", token_info.get("input_tokens", 0)
            )
            output_tokens = token_info.get(
                "interaction_output_tokens", token_info.get("output_tokens", 0)
            )
            # Get individual costs for input and output
            interaction_input_cost = token_info.get("interaction_input_cost", 0.0)
            interaction_output_cost = token_info.get("interaction_output_cost", 0.0)
            interaction_cost = token_info.get("interaction_cost", token_info.get("cost", 0.0))

            # Agent/Terminal totals
            agent_total_input = token_info.get("total_input_tokens", 0)
            agent_total_output = token_info.get("total_output_tokens", 0)
            agent_total_cost = token_info.get("total_cost", 0.0)

            # Global session total
            session_cost = token_info.get("session_total_cost", agent_total_cost)

            # Get global totals - prefer session cost from COST_TRACKER
            global_total_cost = session_cost  # Start with session cost

            # Try to get from COST_TRACKER first (current session)
            try:
                from cai.util import COST_TRACKER
                if COST_TRACKER and hasattr(COST_TRACKER, "session_total_cost"):
                    current_session_cost = COST_TRACKER.session_total_cost
                    if current_session_cost > 0:
                        global_total_cost = current_session_cost
            except:
                pass

            # Only use GLOBAL_USAGE_TRACKER if we don't have session cost
            if global_total_cost == 0.0:
                try:
                    from cai.sdk.agents.global_usage_tracker import GLOBAL_USAGE_TRACKER
                    if GLOBAL_USAGE_TRACKER.enabled and GLOBAL_USAGE_TRACKER.usage_data:
                        tracker_cost = GLOBAL_USAGE_TRACKER.usage_data.get("global_totals", {}).get("total_cost", 0.0)
                        # Only use if reasonable (not accumulated from many sessions)
                        if tracker_cost > 0 and tracker_cost < 100:  # Sanity check
                            global_total_cost = tracker_cost
                except:
                    pass

            # Be robust to unexpected types
            try:
                context_usage_pct = float(
                    token_info.get("context_usage_pct", token_info.get("context_percentage", 0.0))
                    or 0.0
                )
            except Exception:
                context_usage_pct = 0.0

            if input_tokens > 0 or output_tokens > 0:
                # All costs on same line in title
                title_parts.append(" | ")

                # Section 1: Current iteration with individual costs (include reasoning)
                title_parts.append(f"[cyan]In:[/cyan] [green]{input_tokens}[/green]")
                if interaction_input_cost > 0:
                    title_parts.append(f" → [yellow](${interaction_input_cost:.6f})[/yellow]")
                title_parts.append(" ")

                title_parts.append(f"[cyan]Out:[/cyan] [green]{output_tokens}[/green]")
                if interaction_output_cost > 0:
                    title_parts.append(f" → [yellow](${interaction_output_cost:.6f})[/yellow]")
                title_parts.append(" ")
                # Reasoning tokens in title (when present)
                if token_info.get("interaction_reasoning_tokens", 0) > 0:
                    title_parts.append(f"[cyan]R:[/cyan] [yellow]{token_info['interaction_reasoning_tokens']}[/yellow] ")
                # Cached tokens indicator (informational only)
                if "cached_tokens" in token_info:
                    try:
                        _ctk = int(token_info.get("cached_tokens") or 0)
                    except Exception:
                        _ctk = 0
                    title_parts.append(f"[cyan]C:[/cyan] [cyan]{_ctk}[/cyan] [dim cyan](0.00$)[/dim cyan] ")

                # Section 2: Removed Agent total to avoid clutter in title

                # Section 3: Global total
                title_parts.append("| ")
                title_parts.append(f"[cyan]Total:[/cyan][bold red]${global_total_cost:.4f}[/bold red]")

                # Context usage indicator
                if context_usage_pct > 0:
                    if context_usage_pct < 50:
                        indicator = "🟩"
                        color = "green"
                    elif context_usage_pct < 80:
                        indicator = "🟨"
                        color = "yellow"
                    else:
                        indicator = "🟥"
                        color = "red"
                    title_parts.append(
                        f" {indicator} [bold {color}]{context_usage_pct:.1f}%[/bold {color}]"
                    )

        # Join all title parts
        title = "".join(title_parts)

        # Create content group
        content_group = []
        # Render message as Markdown for proper formatting
        if message:
            from rich.markdown import Markdown
            md_content = Markdown(message)
            content_group.append(md_content)
        else:
            content_group.append(Text(""))

        # Add cost stats as footer if available and has actual data
        if token_info and (input_tokens > 0 or output_tokens > 0):
            # Add separator
            content_group.append(Text("\n" + "─" * 60, style="dim"))

            # Create stats footer
            stats_text = Text()
            stats_text.append("📊 ", style="cyan")

            # Interaction stats with individual costs
            stats_text.append("Interaction: ", style="dim cyan")
            stats_text.append(f"In: {input_tokens}", style="green")
            if interaction_input_cost > 0:
                stats_text.append(f" → (${interaction_input_cost:.6f})", style="yellow")
            stats_text.append(f" Out: {output_tokens}", style="green")
            if interaction_output_cost > 0:
                stats_text.append(f" → (${interaction_output_cost:.6f})", style="yellow")
            # Show total iteration cost
            if interaction_cost > 0:
                stats_text.append(f" Total:${interaction_cost:.6f}", style="bold yellow")

            # Cached tokens indicator (informativo; no afecta totales)
            try:
                _ctk = int(token_info.get("cached_tokens") or 0)
            except Exception:
                _ctk = 0
            if _ctk:
                stats_text.append(" ", style="")
                stats_text.append("C: ", style="cyan")
                stats_text.append(str(_ctk), style="cyan")
                stats_text.append(" (0.00$)", style="dim cyan")

            # Agent total removed to avoid clutter in footer

            # Global total
            stats_text.append("\n", style="")
            stats_text.append("💰 ", style="red")
            stats_text.append("Session Total: ", style="dim cyan")
            stats_text.append(f"${global_total_cost:.6f}", style="bold red")

            # Context usage
            if context_usage_pct > 0:
                if context_usage_pct < 50:
                    indicator = "🟢"
                elif context_usage_pct < 80:
                    indicator = "🟡"
                else:
                    indicator = "🔴"
                stats_text.append(f" | Context: {indicator} {context_usage_pct:.1f}%", style="dim")

            content_group.append(stats_text)

        # Add streaming indicator if streaming
        if streaming:
            # Add a separator
            content_group.append(Text("\n" + "─" * 50, style="dim"))

            # Create animated streaming bar
            streaming_bar = Text()
            streaming_bar.append("⚡ ", style="yellow")
            streaming_bar.append("Streaming in progress", style="italic yellow")
            streaming_bar.append(" ", style="")

            # Add rotating animation
            animation_frames = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"]
            import time
            frame_index = int(time.time() * 10) % len(animation_frames)
            streaming_bar.append(animation_frames[frame_index], style="bold yellow")

            content_group.append(streaming_bar)

        # Create panel with content group
        return Panel(
            Group(*content_group) if len(content_group) > 1 else content_group[0],
            title=title,
            title_align="left",
            border_style=border_style,
            box=ROUNDED,
            padding=(0, 1),
        )

    @classmethod
    def create_tool_call_panel(
        cls,
        tool_name: str,
        args: dict,
        call_id: str = "",
        agent_name: str = "Agent",
        interaction: int = 1
    ) -> Panel:
        """Create a panel for tool call display"""
        # Create title
        title = f"[bold cyan][{interaction}][/bold cyan] [bold green]{agent_name}[/bold green] >> [yellow]Tool Call[/yellow]"

        # Add call ID if available
        if call_id:
            title += f" [dim]({call_id[:8]}...)[/dim]"

        # Create content
        content = Text()
        content.append("🔧 ", style="bold yellow")
        content.append(tool_name, style="bold cyan")
        content.append("\n")

        # Format arguments
        if args and any(args.values()):
            content.append("\nArguments:\n", style="dim")
            for key, value in args.items():
                if value:  # Only show non-empty values
                    content.append(f"  • {key}: ", style="bold")
                    # Truncate long values
                    str_value = str(value)
                    if len(str_value) > 100:
                        str_value = str_value[:97] + "..."
                    content.append(f"{str_value}\n", style="white")
        else:
            content.append("\n[dim]Executing...[/dim]\n", style="dim")

        return Panel(
            content,
            title=title,
            title_align="left",
            border_style="yellow",
            box=ROUNDED,
            padding=(0, 1),
        )

    @classmethod
    def create_thinking_panel(
        cls, agent_name: str, thinking_content: str, model_name: str, finished: bool = False
    ) -> Panel:
        """Create a panel for AI thinking/reasoning display"""
        # Determine model display name
        model_str = str(model_name).lower()
        if "claude" in model_str:
            model_display = "Claude"
        elif "deepseek" in model_str:
            model_display = "DeepSeek"
        else:
            model_display = "AI"

        # Create header
        header = Text()
        if finished:
            header.append("🧠 ", style="bold green")
            header.append(f"{model_display} Reasoning Complete", style="bold green")
        else:
            header.append("🧠 ", style="bold yellow")
            header.append(f"{model_display} Reasoning", style="bold yellow")

        header.append(f" | {agent_name}", style="bold cyan")
        header.append(f" | {datetime.now().strftime('%H:%M:%S')}", style="dim")

        # Create content
        if thinking_content.strip():
            if len(thinking_content) > 500:
                # Choose theme based on Textual theme
                try:
                    from textual.app import App
                    app = App.get_app()
                    current_theme = getattr(app, "theme", "textual-dark") if app else "textual-dark"
                except Exception:
                    current_theme = "textual-dark"
                pygments_theme_map = {
                    "textual-dark": "monokai",
                    "tokyo-night": "monokai",
                    "nord": "nord-darker",
                    "solarized-light": "solarized-light",
                    "textual-light": "default",
                    "nature": "monokai",
                }
                pygments_theme = pygments_theme_map.get(current_theme, "monokai")
                content = Syntax(
                    thinking_content,
                    "markdown",
                    theme=pygments_theme,
                    word_wrap=True,
                    line_numbers=False,
                )
            else:
                content = Text(thinking_content, style="white")
        else:
            content = Text("Thinking...", style="italic dim")

        # Determine style
        if finished:
            border_style = "green"
            title = f"[bold green]🧠 {model_display} Thinking Complete[/bold green]"
        else:
            border_style = "yellow"
            title = f"[bold yellow]🧠 {model_display} Thinking Process[/bold yellow]"

        return Panel(
            Group(header, Text("\n"), content),
            title=title,
            border_style=border_style,
            padding=(1, 2),
            box=ROUNDED,
            title_align="left",
        )

    @classmethod
    def _create_tool_header(
        cls, tool_name: str, args: Union[Dict, str], execution_info: Optional[Dict], streaming: bool
    ) -> Text:
        """Create header for tool panel"""
        header = Text()

        # Format tool name
        is_handoff = tool_name.startswith("transfer_to_")
        if is_handoff:
            # Use accent text color from theme when available
            header.append(tool_name, style="bold cyan")
            # Extract and format agent name
            agent_name = cls._extract_agent_name_from_handoff(tool_name)
            if agent_name:
                header.append(" → ", style="bold yellow")
                header.append(agent_name, style="bold green")
        else:
            header.append(tool_name, style="bold cyan")

        # Add arguments
        args_str = cls._format_tool_args(args, tool_name)
        if args_str:
            header.append("(", style="yellow")
            header.append(args_str, style="yellow")
            header.append(")", style="yellow")

        # Add timing info
        if execution_info:
            timing_info = cls._get_timing_info(execution_info)
            if timing_info:
                header.append(f" [{' | '.join(timing_info)}]", style="cyan")

        # Add status
        if execution_info and not streaming:
            status = execution_info.get("status", "completed")
            if status == "completed":
                header.append(" [Completed]", style="green")
            elif status == "running":
                header.append(" [Running]", style="yellow")
            elif status == "error":
                header.append(" [Error]", style="red")
            elif status == "timeout":
                header.append(" [Timeout]", style="red")

        return header

    @classmethod
    def _format_tool_args(cls, args: Union[Dict, str], tool_name: str) -> str:
        """Format tool arguments for display"""
        if tool_name == "execute_code":
            return ""

        if isinstance(args, str):
            if args.strip().startswith("{") and args.strip().endswith("}"):
                try:
                    parsed_dict = json.loads(args)
                    return cls._format_tool_args(parsed_dict, tool_name)
                except json.JSONDecodeError:
                    return args
            return args

        if isinstance(args, dict):
            arg_parts = []
            for key, value in args.items():
                if value == "" or value == {} or value is None:
                    continue
                if (
                    key in ["async_mode", "streaming", "call_counter", "input_to_session"]
                    and not value
                ):
                    continue

                value_str = str(value)
                if isinstance(value, str) and len(value_str) > 70 and key not in ["code", "args"]:
                    value_str = value_str[:67] + "..."
                arg_parts.append(f"{key}={value_str}")

            return ", ".join(arg_parts)

        return str(args)

    @classmethod
    def _add_execute_code_panels(cls, content_group: list, args: Dict | str, output: str) -> None:
        """Add panels for execute_code tool: Code panel first, then Output panel.

        Args may arrive as dict or JSON string.
        """
        # Normalize args
        try:
            if isinstance(args, str):
                parsed = json.loads(args)
                if isinstance(parsed, dict):
                    args = parsed
                else:
                    args = {}
        except Exception:
            args = args if isinstance(args, dict) else {}

        code = (args or {}).get("code", None)
        language = (args or {}).get("language", "python")
        filename = (args or {}).get("filename", "code")

        try:
            from textual.app import App
            app = App.get_app()
            current_theme = getattr(app, "theme", "textual-dark") if app else "textual-dark"
        except Exception:
            current_theme = "textual-dark"
        pygments_theme_map = {
            "textual-dark": "monokai",
            "tokyo-night": "monokai",
            "nord": "nord-darker",
            "solarized-light": "solarized-light",
            "textual-light": "default",
            "nature": "monokai",
        }
        pygments_theme = pygments_theme_map.get(current_theme, "monokai")

        # Code panel
        if code:
            code_syntax = Syntax(
                code,
                language or "text",
                theme=pygments_theme,
                line_numbers=True,
                indent_guides=True,
                word_wrap=True,
            )
            code_title = f"Code ({language})"
            try:
                if filename:
                    code_title = f"{filename} – {code_title}"
            except Exception:
                pass
            code_panel = Panel(
                code_syntax,
                title=code_title,
                border_style="cyan",
                title_align="left",
                box=ROUNDED,
                padding=(0, 1),
            )
            content_group.extend([Text("\n"), code_panel])

        # Output panel
        if output:
            output_syntax = Syntax(
                output,
                "text",
                theme=pygments_theme,
                word_wrap=True,
            )
            output_panel = Panel(
                output_syntax,
                title="Output",
                border_style="green",
                title_align="left",
                box=ROUNDED,
                padding=(0, 1),
            )
            content_group.extend([Text("\n"), output_panel])

    @classmethod
    def _add_command_output_panel(
        cls, content_group: list, output: str, args: Optional[Union[Dict, str]] = None
    ) -> None:
        """Add panel for command output with optional "$ command" preface.

        If available, shows a one-line shell prompt with the invoked command
        ("$ command ..."), followed by the command output block, to achieve
        the pattern:
          $ comando\n
          output\n
        Supports args as dict (with 'full_command' or 'command'+'args')
        or JSON/string.
        """
        # Build a best-effort command line from args
        command_line = None
        if isinstance(args, dict):
            full = str(args.get("full_command") or "").strip()
            if full:
                command_line = full
            else:
                base = str(args.get("command") or "").strip()
                extra = str(args.get("args") or "").strip()
                if base or extra:
                    command_line = (base + (f" {extra}" if extra else "")).strip()
        elif isinstance(args, str):
            # Try to parse JSON first
            try:
                parsed = json.loads(args)
                if isinstance(parsed, dict):
                    return cls._add_command_output_panel(content_group, output, parsed)
                command_line = str(args)
            except Exception:
                command_line = str(args)

        # If we inferred a command, show it as a one-line prompt above the output
        if command_line:
            prompt_text = Text()
            prompt_text.append("$ ", style="bold cyan")
            prompt_text.append(command_line, style="white")
            content_group.extend([prompt_text, Text("\n")])

        if not output:
            return

        try:
            from textual.app import App
            app = App.get_app()
            current_theme = getattr(app, "theme", "textual-dark") if app else "textual-dark"
        except Exception:
            current_theme = "textual-dark"
        pygments_theme_map = {
            "textual-dark": "monokai",
            "tokyo-night": "monokai",
            "nord": "nord-darker",
            "solarized-light": "solarized-light",
            "textual-light": "default",
            "nature": "monokai",
        }
        pygments_theme = pygments_theme_map.get(current_theme, "monokai")
        output_syntax = Syntax(
            output,
            "bash",
            theme=pygments_theme,
            word_wrap=True,
        )
        output_panel = Panel(
            output_syntax,
            title="Command Output",
            border_style="green",
            title_align="left",
            box=ROUNDED,
            padding=(0, 1),
        )
        content_group.extend([output_panel])

    @classmethod
    def _add_generic_output_panel(cls, content_group: list, output: str) -> None:
        """Add panel for generic output"""
        # Detect output type
        output_lang = "text"
        try:
            json.loads(output)
            output_lang = "json"
        except json.JSONDecodeError:
            if output.strip().startswith("<") and output.strip().endswith(">"):
                output_lang = "xml"

        try:
            from textual.app import App
            app = App.get_app()
            current_theme = getattr(app, "theme", "textual-dark") if app else "textual-dark"
        except Exception:
            current_theme = "textual-dark"
        pygments_theme_map = {
            "textual-dark": "monokai",
            "tokyo-night": "monokai",
            "nord": "nord-darker",
            "solarized-light": "solarized-light",
            "textual-light": "default",
            "nature": "monokai",
        }
        pygments_theme = pygments_theme_map.get(current_theme, "monokai")
        output_syntax = Syntax(
            output,
            cls.LANGUAGE_MAP.get(output_lang, output_lang),
            theme=pygments_theme,
            word_wrap=True,
            line_numbers=True,
            indent_guides=True,
        )
        output_panel = Panel(
            output_syntax,
            title="Tool Output",
            border_style="green",
            title_align="left",
            box=ROUNDED,
            padding=(0, 1),
        )
        content_group.extend([Text("\n"), output_panel])

    @classmethod
    def _create_token_display(cls, token_info: Dict, default_model: Optional[str] = None) -> Optional[Text]:
        """Create token information display aligned with CLI formatting."""
        if not token_info:
            return None

        # Enrich with best-known model when available to avoid falling back to env var
        enriched = enrich_token_info_for_pricing(token_info, default_model=default_model)

        interaction_tokens = (
            enriched.get("interaction_input_tokens", 0)
            + enriched.get("interaction_output_tokens", 0)
            + enriched.get("interaction_reasoning_tokens", 0)
        )
        agent_tokens = (
            enriched.get("total_input_tokens", 0)
            + enriched.get("total_output_tokens", 0)
            + enriched.get("total_reasoning_tokens", 0)
        )
        has_costs = any(
            float(enriched.get(field, 0.0)) > 0.0
            for field in (
                "interaction_cost",
                "interaction_input_cost",
                "interaction_output_cost",
                "total_cost",
                "total_input_cost",
                "total_output_cost",
                "session_total_cost",
            )
        )

        if interaction_tokens == 0 and agent_tokens == 0 and not has_costs:
            return None

        tokens_text = cli_create_token_display(
            enriched.get("interaction_input_tokens", 0),
            enriched.get("interaction_output_tokens", 0),
            enriched.get("interaction_reasoning_tokens", 0),
            enriched.get("total_input_tokens", 0),
            enriched.get("total_output_tokens", 0),
            enriched.get("total_reasoning_tokens", 0),
            enriched.get("model", ""),
            interaction_cost=enriched.get("interaction_cost"),
            interaction_input_cost=enriched.get("interaction_input_cost"),
            interaction_output_cost=enriched.get("interaction_output_cost"),
            total_cost=enriched.get("total_cost"),
            total_input_cost=enriched.get("total_input_cost"),
            total_output_cost=enriched.get("total_output_cost"),
            cached_tokens=enriched.get("cached_tokens"),
            cached_cost=enriched.get("cached_cost"),
        )

        # Remove the leading newline Rich Text produced for CLI panels to
        # integrate cleanly in TUI layouts where we already insert spacing.
        if tokens_text and tokens_text.plain.startswith("\n"):
            tokens_text = tokens_text[1:]

        return tokens_text

    @classmethod
    def _get_panel_style(
        cls,
        tool_name: str,
        args: Union[Dict, str],
        execution_info: Optional[Dict],
        token_info: Optional[Dict],
        streaming: bool,
    ) -> Tuple[str, str]:
        """Get panel border style and title"""
        # Get agent prefix
        agent_prefix = ""
        if token_info and token_info.get("agent_name"):
            agent_prefix = f"[cyan]{token_info['agent_name']}[/cyan] - "
        # Add interaction counter prefix if available (tool calls are not interactions,
        # but we display the last recorded interaction number for context)
        counter_prefix = ""
        try:
            if token_info and token_info.get("interaction_counter"):
                counter_prefix = f"[bold cyan][{int(token_info['interaction_counter'])}][/bold cyan] "
        except Exception:
            pass

        # Determine status
        if streaming:
            if execution_info:
                status = execution_info.get("status", "running")
                if status == "completed":
                    border_style = "green"
                    title = f"{agent_prefix}[bold green]Completed[/bold green]"
                elif status == "error":
                    border_style = "red"
                    title = f"{agent_prefix}[bold red]Error[/bold red]"
                elif status == "timeout":
                    border_style = "red"
                    title = f"{agent_prefix}[bold red]Timeout[/bold red]"
                else:
                    border_style = "yellow"
                    title = f"{agent_prefix}[bold yellow]Running[/bold yellow]"
            else:
                border_style = "yellow"
                title = f"{agent_prefix}[bold yellow]Running[/bold yellow]"
        else:
            # Non-streaming
            if execution_info:
                status = execution_info.get("status", "completed")
                args_str = cls._format_tool_args(args, tool_name)

                if tool_name.startswith("transfer_to_"):
                    # Handoff
                    agent_name = cls._extract_agent_name_from_handoff(tool_name)
                    base_title = f"Handoff: {agent_name}"
                else:
                    base_title = f"{tool_name}({args_str})"

                if status == "completed":
                    border_style = "green"
                    title = f"{counter_prefix}{agent_prefix}[bold green]{base_title} [Completed][/bold green]"
                elif status == "error":
                    border_style = "red"
                    title = f"{counter_prefix}{agent_prefix}[bold red]{base_title} [Error][/bold red]"
                elif status == "timeout":
                    border_style = "red"
                    title = f"{counter_prefix}{agent_prefix}[bold red]{base_title} [Timeout][/bold red]"
                else:
                    border_style = "blue"
                    title = f"{counter_prefix}{agent_prefix}[bold blue]{base_title}[/bold blue]"
            else:
                border_style = "blue"
                args_str = cls._format_tool_args(args, tool_name)

                if tool_name.startswith("transfer_to_"):
                    agent_name = cls._extract_agent_name_from_handoff(tool_name)
                    title = f"{counter_prefix}{agent_prefix}[bold blue]Handoff: {agent_name}[/bold blue]"
                else:
                    title = f"{counter_prefix}{agent_prefix}[bold blue]{tool_name}({args_str})[/bold blue]"

        return border_style, title

    @classmethod
    def _extract_agent_name_from_handoff(cls, tool_name: str) -> str:
        """Extract agent name from transfer_to_X function name"""
        if not tool_name.startswith("transfer_to_"):
            return ""

        agent_name_raw = tool_name[len("transfer_to_") :]
        # Convert underscores to spaces and capitalize
        agent_name = " ".join(word.capitalize() for word in agent_name_raw.split("_"))

        # Handle acronyms
        parts = agent_name.split()
        for i, part in enumerate(parts):
            if part.upper() == part and len(part) > 1:
                parts[i] = part.upper()
        return " ".join(parts)

    @classmethod
    def _get_timing_info(cls, execution_info: Dict) -> list:
        """Extract timing information"""
        timing_info = []

        if execution_info.get("tool_time"):
            timing_info.append(f"Tool: {cls._format_time(execution_info['tool_time'])}")

        if execution_info.get("total_time"):
            timing_info.append(f"Total: {cls._format_time(execution_info['total_time'])}")

        return timing_info

    @classmethod
    def _format_time(cls, seconds: float) -> str:
        """Format time duration robustly.

        Accepts ints, floats, and tuples/lists (uses first element). Any
        non-numeric or missing values return "N/A" instead of raising.
        """
        try:
            if isinstance(seconds, (list, tuple)) and seconds:
                seconds = seconds[0]
            seconds = float(seconds)
        except Exception:
            return "N/A"

        if seconds < 1:
            return f"{seconds * 1000:.0f}ms"
        elif seconds < 60:
            return f"{seconds:.1f}s"
        else:
            minutes = int(seconds // 60)
            secs = seconds % 60
            return f"{minutes}m {secs:.0f}s"
