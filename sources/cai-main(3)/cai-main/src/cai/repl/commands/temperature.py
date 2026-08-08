"""
Temperature and Top-P commands for CAI REPL.
This module provides /temperature (and /temp) plus /topp for nucleus sampling.
"""

import os
from dataclasses import replace
from typing import List, Optional

from rich import box
from rich.console import Console
from rich.panel import Panel
from rich.text import Text

from cai.repl.commands.base import Command, register_command
from cai.repl.ui.banner import _CAI_GREEN, _quick_guide_subpanel_title
from cai.sdk.agents.model_settings import get_default_temperature, get_default_top_p

console = Console()


def _cai_panel(body: str, *, title: str, border_style: str = _CAI_GREEN) -> Panel:
    """Rich panel aligned with REPL palette (e.g. ``/model``)."""
    return Panel(
        Text.from_markup(body, overflow="fold"),
        title=_quick_guide_subpanel_title(title),
        title_align="left",
        border_style=border_style,
        box=box.ROUNDED,
        padding=(1, 1),
    )


def _repl_resolved_temperature_for_display() -> float:
    """REPL temperature: active agent's resolved value, else CAI_TEMPERATURE."""
    try:
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER

        agent = AGENT_MANAGER.get_active_agent()
        if agent is not None:
            return float(agent.model_settings.with_defaults().temperature)
    except Exception:
        pass
    return get_default_temperature()


def _sync_repl_active_agent_temperature(value: float) -> None:
    """Apply temperature to the active REPL agent so Runner uses it on the next turn."""
    try:
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER

        agent = AGENT_MANAGER.get_active_agent()
        if agent is not None:
            agent.model_settings = replace(agent.model_settings, temperature=value)
    except Exception:
        pass


def _repl_resolved_top_p_for_display() -> float:
    """Top_p shown in REPL: active agent's resolved settings, else CAI_TOP_P."""
    try:
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER

        agent = AGENT_MANAGER.get_active_agent()
        if agent is not None:
            return float(agent.model_settings.with_defaults().top_p)
    except Exception:
        pass
    return get_default_top_p()


def _sync_repl_active_agent_top_p(value: float) -> None:
    """Apply top_p to the active REPL agent so Runner uses it on the next turn."""
    try:
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER

        agent = AGENT_MANAGER.get_active_agent()
        if agent is not None:
            agent.model_settings = replace(agent.model_settings, top_p=value)
    except Exception:
        pass


class TemperatureCommand(Command):
    """Command for viewing and changing the agent's temperature."""

    def __init__(self):
        """Initialize the temperature command."""
        super().__init__(
            name="/temperature",
            description="View or change the agent's temperature (0.0-2.0)",
            aliases=["/temp"],
        )

    def handle(self, args: Optional[List[str]] = None) -> bool:
        """Handle the temperature command.

        Args:
            args: Optional list of command arguments

        Returns:
            True if the command was handled successfully, False otherwise
        """
        # Base value before TUI-specific resolution
        current_temp = get_default_temperature()
        terminal_info = ""

        # In TUI mode, try to get the specific terminal's temperature
        if os.getenv("CAI_TUI_MODE") == "true":
            try:
                from cai.tui.cai_terminal import CAITerminal
                from cai.tui.core.terminal_tracking import get_current_terminal_id

                app = CAITerminal._instance
                if app and hasattr(app, 'session_manager'):
                    # Try to determine current terminal
                    terminal_number = None

                    # Method 1: Try to get terminal number from command handler context
                    import inspect
                    for frame_info in inspect.stack():
                        frame_locals = frame_info.frame.f_locals
                        if 'self' in frame_locals:
                            obj = frame_locals['self']
                            if hasattr(obj, 'terminal_number') and hasattr(obj, 'handle_command'):
                                terminal_number = obj.terminal_number
                                break

                    # Method 2: Try to get terminal ID and extract number
                    if terminal_number is None:
                        terminal_id = get_current_terminal_id()
                        if terminal_id:
                            import re
                            match = re.search(r'terminal-(\d+)', terminal_id)
                            if match:
                                terminal_number = int(match.group(1))

                    # Method 3: If still none, try to get from focused terminal
                    if terminal_number is None and hasattr(app, 'terminal_grid'):
                        focused_terminal = app.terminal_grid.get_focused_terminal()
                        if focused_terminal and hasattr(focused_terminal, 'state'):
                            terminal_number = focused_terminal.state.terminal_number

                    if terminal_number is not None:
                        runner = app.session_manager.terminal_runners.get(terminal_number)
                        if runner and runner.agent and hasattr(runner.agent, 'model'):
                            current_temp = runner.agent.model.temperature
                            terminal_info = f" (Terminal {terminal_number})"
            except Exception:
                pass

        if not args:
            if os.getenv("CAI_TUI_MODE") != "true":
                current_temp = _repl_resolved_temperature_for_display()
            z = _CAI_GREEN
            panel_body = (
                f"Current temperature{terminal_info}: [bold {z}]{current_temp:.1f}[/bold {z}]\n"
                f"\n[bold {z}]Reference scale[/bold {z}]\n"
                f"  • [bold {z}]0.0[/bold {z}] [white]— deterministic[/white]\n"
                f"  • [bold {z}]0.7[/bold {z}] [white]— balanced default[/white]\n"
                f"  • [bold {z}]1.0[/bold {z}] [white]— more varied[/white]\n"
                f"  • [bold {z}]2.0[/bold {z}] [white]— maximum randomness[/white]"
            )
            if os.getenv("CAI_TUI_MODE") != "true":
                panel_body += (
                    f"\n\n[dim]REPL: updates [bold {z}]CAI_TEMPERATURE[/bold {z}] and the active "
                    "agent's model_settings; some providers may clamp or ignore.[/dim]"
                )
            console.print(_cai_panel(panel_body, title="Temperature"))
            console.print(f"\n[bold {z}]Usage:[/bold {z}]")
            console.print(f"  [bold {z}]/temperature <value>[/bold {z}] [dim]— set (0.0–2.0)[/dim]")
            console.print(f"  [bold {z}]/temp <value>[/bold {z}]        [dim]— alias[/dim]")
            return True

        # Parse temperature value
        try:
            new_temp = float(args[0])

            # Validate temperature range
            if not 0.0 <= new_temp <= 2.0:
                console.print(
                    _cai_panel(
                        f"[bold bright_red]Invalid temperature:[/bold bright_red] {new_temp}\n"
                        "[white]Use a value between 0.0 and 2.0.[/white]",
                        title="Error",
                        border_style="red",
                    )
                )
                return True

        except ValueError:
            console.print(
                _cai_panel(
                    f"[bold bright_red]Invalid value:[/bold bright_red] {args[0]!r}\n"
                    "[white]Enter a number between 0.0 and 2.0.[/white]",
                    title="Error",
                    border_style="red",
                )
            )
            return True

        z = _CAI_GREEN

        # Determine temperature description
        if new_temp <= 0.2:
            desc = "Very focused and deterministic"
        elif new_temp <= 0.5:
            desc = "Focused with slight variation"
        elif new_temp <= 0.8:
            desc = "Balanced creativity"
        elif new_temp <= 1.2:
            desc = "Creative and varied"
        elif new_temp <= 1.5:
            desc = "Very creative"
        else:
            desc = "Maximum creativity and randomness"

        # In TUI mode, only update the current terminal's temperature
        if os.getenv("CAI_TUI_MODE") == "true":
            # Import here to avoid circular imports
            try:
                from cai.tui.cai_terminal import CAITerminal
                from cai.tui.core.terminal_tracking import get_current_terminal_id

                app = CAITerminal._instance
                if app and hasattr(app, 'session_manager'):
                    # Try to determine current terminal from various sources
                    terminal_number = None

                    # Method 1: Try to get terminal number from command handler context
                    import inspect
                    for frame_info in inspect.stack():
                        frame_locals = frame_info.frame.f_locals
                        if 'self' in frame_locals:
                            obj = frame_locals['self']
                            # Check if this is a CommandHandler with terminal_number
                            if hasattr(obj, 'terminal_number') and hasattr(obj, 'handle_command'):
                                terminal_number = obj.terminal_number
                                break

                    # Method 2: Try to get terminal ID and extract number
                    if terminal_number is None:
                        terminal_id = get_current_terminal_id()
                        if terminal_id:
                            # Extract terminal number from ID (e.g., "terminal-1")
                            import re
                            match = re.search(r'terminal-(\d+)', terminal_id)
                            if match:
                                terminal_number = int(match.group(1))

                    # Method 3: If still none, try to get from focused terminal
                    if terminal_number is None and hasattr(app, 'terminal_grid'):
                        focused_terminal = app.terminal_grid.get_focused_terminal()
                        if focused_terminal and hasattr(focused_terminal, 'state'):
                            terminal_number = focused_terminal.state.terminal_number

                    if terminal_number is not None:
                        # Update only the specific terminal's runner
                        runner = app.session_manager.terminal_runners.get(terminal_number)
                        if runner:
                            if runner.agent and hasattr(runner.agent, 'model'):
                                runner.agent.model.temperature = new_temp
                                # Update the terminal's header to refresh tooltip
                                if hasattr(runner.terminal, '_update_header'):
                                    runner.terminal._update_header()
                                # Also refresh the terminal
                                if hasattr(runner.terminal, 'refresh'):
                                    runner.terminal.refresh()

                            # Display terminal-specific message
                            change_message = (
                                f"Temperature set to [bold {z}]{new_temp:.1f}[/bold {z}] "
                                f"(Terminal {terminal_number})\n"
                                f"[white]{desc}[/white]\n"
                                f"\n[dim]Applies on the next agent interaction.[/dim]"
                            )
                        else:
                            change_message = (
                                f"[yellow]Warning:[/yellow] [white]no runner for terminal "
                                f"{terminal_number}.[/white]\n"
                                f"[white]Value given:[/white] [bold {z}]{new_temp:.1f}[/bold {z}]"
                            )
                    else:
                        # Fallback if terminal number cannot be determined
                        change_message = (
                            f"[yellow]Warning:[/yellow] [white]could not determine terminal; "
                            f"set [bold {z}]CAI_TEMPERATURE[/bold {z}] globally.[/white]\n"
                            f"[bold {z}]{new_temp:.1f}[/bold {z}] — [white]{desc}[/white]"
                        )
                        # Set global temperature as fallback
                        os.environ["CAI_TEMPERATURE"] = str(new_temp)

                else:
                    # Session manager not available, set global
                    os.environ["CAI_TEMPERATURE"] = str(new_temp)
                    change_message = (
                        f"Temperature set to [bold {z}]{new_temp:.1f}[/bold {z}]\n"
                        f"[white]{desc}[/white]\n"
                        f"\n[dim]Applies on the next agent interaction.[/dim]"
                    )

            except Exception:
                # Error occurred, set global temperature as fallback
                os.environ["CAI_TEMPERATURE"] = str(new_temp)
                change_message = (
                    f"Temperature set to [bold {z}]{new_temp:.1f}[/bold {z}]\n"
                    f"[white]{desc}[/white]\n"
                    f"\n[dim]Applies on the next agent interaction.[/dim]"
                )
        else:
            # REPL: persist env and active agent model_settings for the next Runner.run
            os.environ["CAI_TEMPERATURE"] = str(new_temp)
            _sync_repl_active_agent_temperature(new_temp)
            change_message = (
                f"Temperature set to [bold {z}]{new_temp:.1f}[/bold {z}]\n"
                f"[white]{desc}[/white]\n"
                f"\n[dim]Applies on the next agent interaction.[/dim]"
            )

        # Display temperature change notification
        console.print(_cai_panel(change_message, title="Temperature"))

        return True


class TopPCommand(Command):
    """Command for viewing and changing the agent's top_p (nucleus sampling)."""

    def __init__(self):
        """Initialize the top_p command."""
        super().__init__(
            name="/topp",
            description="View or change the agent's top_p nucleus sampling (0.0-1.0)",
            aliases=[],
        )

    def handle(self, args: Optional[List[str]] = None) -> bool:
        """Handle the top_p command.

        Args:
            args: Optional list of command arguments

        Returns:
            True if the command was handled successfully, False otherwise
        """
        current_top_p = get_default_top_p()
        terminal_info = ""

        # In TUI mode, try to get the specific terminal's top_p
        if os.getenv("CAI_TUI_MODE") == "true":
            try:
                from cai.tui.cai_terminal import CAITerminal
                from cai.tui.core.terminal_tracking import get_current_terminal_id

                app = CAITerminal._instance
                if app and hasattr(app, 'session_manager'):
                    terminal_number = None

                    import inspect
                    for frame_info in inspect.stack():
                        frame_locals = frame_info.frame.f_locals
                        if 'self' in frame_locals:
                            obj = frame_locals['self']
                            if hasattr(obj, 'terminal_number') and hasattr(obj, 'handle_command'):
                                terminal_number = obj.terminal_number
                                break

                    if terminal_number is None:
                        terminal_id = get_current_terminal_id()
                        if terminal_id:
                            import re
                            match = re.search(r'terminal-(\d+)', terminal_id)
                            if match:
                                terminal_number = int(match.group(1))

                    if terminal_number is None and hasattr(app, 'terminal_grid'):
                        focused_terminal = app.terminal_grid.get_focused_terminal()
                        if focused_terminal and hasattr(focused_terminal, 'state'):
                            terminal_number = focused_terminal.state.terminal_number

                    if terminal_number is not None:
                        runner = app.session_manager.terminal_runners.get(terminal_number)
                        if runner and runner.agent and hasattr(runner.agent, 'model_settings'):
                            if runner.agent.model_settings.top_p is not None:
                                current_top_p = runner.agent.model_settings.top_p
                            terminal_info = f" (Terminal {terminal_number})"
            except Exception:
                pass

        if not args:
            if os.getenv("CAI_TUI_MODE") != "true":
                current_top_p = _repl_resolved_top_p_for_display()
            z = _CAI_GREEN
            panel_body = (
                f"Current top_p{terminal_info}: [bold {z}]{current_top_p:.2f}[/bold {z}]\n"
                f"\n[bold {z}]Reference scale[/bold {z}]\n"
                f"  • [bold {z}]0.5[/bold {z}] [white]— tighter nucleus[/white]\n"
                f"  • [bold {z}]0.9[/bold {z}] [white]— broader sampling[/white]\n"
                f"  • [bold {z}]1.0[/bold {z}] [white]— default (full mass)[/white]"
            )
            if os.getenv("CAI_TUI_MODE") != "true":
                panel_body += (
                    f"\n\n[dim]REPL: updates [bold {z}]CAI_TOP_P[/bold {z}] and the active "
                    "agent's model_settings; some providers may clamp or ignore.[/dim]"
                )
            console.print(_cai_panel(panel_body, title="Top-P"))
            console.print(f"\n[bold {z}]Usage:[/bold {z}]")
            console.print(f"  [bold {z}]/topp <value>[/bold {z}]   [dim]— set (0.0–1.0)[/dim]")
            return True

        # Parse top_p value
        try:
            new_top_p = float(args[0])

            # Validate top_p range
            if not 0.0 <= new_top_p <= 1.0:
                console.print(
                    _cai_panel(
                        f"[bold bright_red]Invalid top_p:[/bold bright_red] {new_top_p}\n"
                        "[white]Use a value between 0.0 and 1.0.[/white]",
                        title="Error",
                        border_style="red",
                    )
                )
                return True

        except ValueError:
            console.print(
                _cai_panel(
                    f"[bold bright_red]Invalid value:[/bold bright_red] {args[0]!r}\n"
                    "[white]Enter a number between 0.0 and 1.0.[/white]",
                    title="Error",
                    border_style="red",
                )
            )
            return True

        # Determine top_p description
        if new_top_p <= 0.5:
            desc = "More focused, fewer tokens considered"
        elif new_top_p <= 0.7:
            desc = "Balanced focus"
        elif new_top_p <= 0.9:
            desc = "Broader sampling"
        else:
            desc = "All tokens considered"

        z = _CAI_GREEN
        os.environ["CAI_TOP_P"] = str(new_top_p)
        if os.getenv("CAI_TUI_MODE") != "true":
            _sync_repl_active_agent_top_p(new_top_p)
        change_message = (
            f"Top_p set to [bold {z}]{new_top_p:.2f}[/bold {z}]\n"
            f"[white]{desc}[/white]\n"
            f"\n[dim]Applies on the next agent interaction.[/dim]"
        )

        # Display top_p change notification
        console.print(_cai_panel(change_message, title="Top-P"))

        return True


# Register the commands
register_command(TemperatureCommand())
register_command(TopPCommand())
