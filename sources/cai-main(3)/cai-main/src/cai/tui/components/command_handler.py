"""Command handler component for CAI TUI"""

import os
import sys
import subprocess
from io import StringIO
from typing import List, Optional, Any
from textual.widgets import RichLog
from rich.panel import Panel
from rich.table import Table
from rich.markdown import Markdown
from rich.console import Console

from cai.repl.commands import get_command, handle_command as commands_handle_command

try:
    from cai.repl.commands.compact import TUI_COMPACTION_MONITOR
except Exception:  # pragma: no cover - compact command might not be loaded yet
    TUI_COMPACTION_MONITOR = None


class CommandHandler:
    """Handles CLI command execution with proper output capture"""

    def __init__(self, output: RichLog, terminal_number: int = 1, terminal_id: Optional[str] = None):
        self.output = output
        self.terminal_number = terminal_number
        self.terminal_id = terminal_id
        self.current_agent = None
        # Default to redteam_agent for TUI sessions
        self.current_agent_name = "redteam_agent"

    def handle_command(self, command: str) -> None:
        """Handle CLI commands with full output capture"""
        if command is None:
            return

        command = command.strip()
        if not command:
            return

        if (
            os.getenv("CAI_TUI_MODE") == "true"
            and TUI_COMPACTION_MONITOR
            and TUI_COMPACTION_MONITOR.is_active(self.terminal_number)
            and not command.lower().startswith("/compact")
        ):
            if self.output:
                self.output.write(
                    "[yellow]Compaction in progress. Wait until it finishes before running new commands.[/yellow]"
                )
            return

        # Special handling for $ command - treat everything after $ as the shell command
        if command.startswith("$"):
            if len(command) > 1:
                # Remove $ and any space after it
                if command[1] == " ":
                    shell_cmd = command[2:].strip()  # "$ ls" -> "ls"
                else:
                    shell_cmd = command[1:].strip()  # "$ls" -> "ls"
                
                if shell_cmd:
                    parts = ["/shell"] + shell_cmd.split()
                else:
                    parts = ["/shell"]
            else:
                # Just $ by itself
                parts = ["/shell"]
            cmd_name = parts[0]
            args = parts[1:] if len(parts) > 1 else None
            cmd_name_for_lookup = cmd_name
        else:
            # Normal command parsing
            parts = command.split()
            if not parts:
                return

            cmd_name = parts[0]
            args = parts[1:] if len(parts) > 1 else None
            
            # Keep the leading slash for command lookup since commands are registered with it
            cmd_name_for_lookup = cmd_name

        # Create a custom console that captures Rich objects
        # Custom console that intercepts all output
        class InterceptConsole(Console):
            def __init__(self, output_widget, *args, **kwargs):
                super().__init__(*args, **kwargs)
                self.output_widget = output_widget

            def print(self, *objects, **kwargs):
                # Write directly to the RichLog widget if available
                wrote = False
                if self.output_widget:
                    for obj in objects:
                        try:
                            self.output_widget.write(obj)
                            wrote = True
                        except Exception:
                            # If widget isn't attached to an active App, fall back to stdout
                            try:
                                Console(file=sys.__stdout__).print(obj)
                            except Exception:
                                pass
                if not wrote:
                    # Fallback to parent behavior (goes to provided file or stdout)
                    try:
                        super().print(*objects, **kwargs)
                    except Exception:
                        # As last resort, plain print
                        try:
                            __builtins__["print"](*objects)
                        except Exception:
                            pass
                # Don't call parent to avoid double output

        # Create intercept console that writes directly to our output widget
        # Get width, with fallback
        had_active_terminal = "CAI_ACTIVE_COMMAND_TERMINAL" in os.environ
        previous_active_terminal = os.environ.get("CAI_ACTIVE_COMMAND_TERMINAL")
        had_active_terminal_id = "CAI_ACTIVE_COMMAND_TERMINAL_ID" in os.environ
        previous_active_terminal_id = os.environ.get("CAI_ACTIVE_COMMAND_TERMINAL_ID")

        try:
            width = max(80, self.output.size.width - 4) if hasattr(self.output, "size") else 80
        except:
            width = 80

        intercept_console = InterceptConsole(
            self.output,  # Pass the output widget
            file=StringIO(),  # Still need a file for compatibility
            force_terminal=True,
            width=width,
            legacy_windows=False,
        )

        # Backup original console
        import rich.console

        original_console_class = rich.console.Console
        original_get_console = getattr(rich.console, "get_console", None)

        # Create a console instance that writes to our output
        def create_intercept_console(*args, **kwargs):
            return intercept_console

        # Patch console creation (scoped)
        rich.console.Console = create_intercept_console
        if hasattr(rich.console, "get_console"):
            rich.console.get_console = lambda: intercept_console

        # Also patch the module-level console if it exists
        if hasattr(rich.console, "console"):
            original_module_console = rich.console.console
            rich.console.console = intercept_console

        # Backup print and stdout
        original_print = print
        original_stdout = sys.stdout
        original_stderr = sys.stderr

        # Redirect stdout/stderr to capture subprocess output (scoped)
        stdout_capture = StringIO()
        stderr_capture = StringIO()
        sys.stdout = stdout_capture
        sys.stderr = stderr_capture

        # Create a print function that uses our intercept console
        def capture_print(*args, **kwargs):
            # Convert args to strings and join with space
            text = " ".join(str(arg) for arg in args)
            intercept_console.print(text)

        __builtins__["print"] = capture_print

        try:
            # Import base command handler to ensure console patching affects it
            from cai.repl.commands import base

            if hasattr(base, "console"):
                original_base_console = base.console
                base.console = intercept_console

            # Patch console in all command modules
            command_modules = []

            # Force reload of command modules to ensure they use our console
            modules_to_patch = []
            for name in list(sys.modules.keys()):
                if name.startswith("cai.repl.commands.") and name != "cai.repl.commands":
                    modules_to_patch.append(name)

            # Patch all found modules
            for module_name in modules_to_patch:
                if module_name in sys.modules:
                    module = sys.modules[module_name]
                    if hasattr(module, "console"):
                        command_modules.append((module, module.console))
                        module.console = intercept_console

            # REMOVED special handling for /model - it was blocking the command
            # This was causing /model without arguments to not work
            # if cmd_name.lower() == "/model":
            #     handled = True
            #     suggested = None
            # else:
            
            # Set terminal context before executing command (router)
            from cai.tui.routing.output_router import set_terminal_context
            
            # Try to get terminal ID from output widget if available
            terminal_id = None
            if hasattr(self.output, 'terminal_id'):
                terminal_id = self.output.terminal_id
            elif hasattr(self, 'terminal_id'):
                terminal_id = self.terminal_id
            else:
                # Generate terminal ID from terminal number
                terminal_id = f"terminal-{os.getpid()}-{self.terminal_number}"
            
            
            # Set the terminal ID for this execution context
            set_terminal_context(terminal_id, self.terminal_number)

            # Expose active terminal context to command modules (e.g., to sync UI dropdowns)
            os.environ["CAI_ACTIVE_COMMAND_TERMINAL"] = str(self.terminal_number)
            if terminal_id:
                os.environ["CAI_ACTIVE_COMMAND_TERMINAL_ID"] = str(terminal_id)
            else:
                os.environ.pop("CAI_ACTIVE_COMMAND_TERMINAL_ID", None)
            
            # No tocar thread-locals internos; routing gestiona el contexto
            
            # Try using the commands_handle_command first with autocorrect
            from cai.repl.commands import handle_command_with_autocorrect
            handled, suggested = handle_command_with_autocorrect(cmd_name_for_lookup, args)

            if not handled:
                if suggested:
                    intercept_console.print(f"[red]Unknown command: {cmd_name}[/red]")
                    intercept_console.print(f"[yellow]Did you mean: {suggested}?[/yellow]")
                else:
                    intercept_console.print(f"[red]Unknown command: {cmd_name}[/red]")

            # Special handling for agent commands
            # Convert /agent <number> to /agent select <agent_name> for secondary terminals
            if cmd_name.lower() in ["/agent", "/a"] and args:
                # Check if first arg is a number (not "select", "list", etc)
                if args and args[0].isdigit():
                    agent_number = int(args[0])
                    
                    # For secondary terminals, convert number to agent selection
                    if self.terminal_number > 1 or (self.terminal_number == 1 and agent_number < 20):
                        # Get available agents to map number to name
                        try:
                            from cai.agents import get_available_agents
                            agents = get_available_agents()
                            agent_list = list(agents.keys())
                            
                            # Check if number is valid
                            if 1 <= agent_number <= len(agent_list):
                                agent_name = agent_list[agent_number - 1]
                                # Convert to select command
                                args = ["select", agent_name]
                                intercept_console.print(f"[cyan]Agent {agent_number}: {agent_name}[/cyan]")
                                # Important: update the command string so it gets processed correctly
                                command = f"/agent select {agent_name}"
                            else:
                                intercept_console.print(f"[red]Invalid agent number: {agent_number}[/red]")
                                intercept_console.print(f"[yellow]Available agents: 1-{len(agent_list)}[/yellow]")
                                return
                        except Exception as e:
                            intercept_console.print(f"[red]Error getting agent list: {e}[/red]")
                            return
                    # For main terminal with agent >= 20, let it handle parallel patterns
                    elif self.terminal_number == 1 and agent_number >= 20:
                        # Let the normal REPL handle parallel patterns
                        pass
                
            # Normalize "/agent <name>" to "/agent select <name>" in TUI (exclude real subcommands)
            if cmd_name.lower() in ["/agent", "/a"] and args:
                # If user typed "/agent <name>" (not a subcommand and not a number), treat as select
                sub = args[0]
                # Known subcommands from agent.py
                known_subs = {"select", "list", "info", "current", "new"}
                if len(args) == 1 and sub not in known_subs and not sub.isdigit():
                    agent_name = sub
                    # Convert to select so downstream logic handles it uniformly
                    args = ["select", agent_name]
                    command = f"/agent select {agent_name}"

            # Special handling for /agent select command
            # Skip this if we're in broadcast mode (the command will handle it itself)
            is_broadcast_mode = os.getenv("CAI_BROADCAST_MODE") == "true"
            if (
                not is_broadcast_mode
                and cmd_name.lower() in ["/agent", "/a"]
                and args
                and len(args) > 1
                and args[0] == "select"
            ):
                agent_name = args[1]
                
                # Update terminal's agent via session manager for ALL terminals
                if hasattr(self, 'session_manager') and self.session_manager:
                    import asyncio
                    
                    # Debug info
                    if os.getenv("CAI_DEBUG") == "2":
                        intercept_console.print(f"[cyan]DEBUG: CommandHandler terminal_number: {self.terminal_number}[/cyan]")
                        intercept_console.print(f"[cyan]DEBUG: Has session_manager: {hasattr(self, 'session_manager')}[/cyan]")
                    
                    # Show immediate feedback
                    intercept_console.print(f"[yellow]Updating agent to: {agent_name} for Terminal {self.terminal_number}...[/yellow]")
                    
                    # Create async task with proper error handling
                    async def update_agent_async():
                        try:
                            await self.session_manager.update_terminal_agent(
                                self.terminal_number, agent_name
                            )
                            intercept_console.print(f"[green]✓ Agent updated to: {agent_name} for Terminal {self.terminal_number}[/green]")

                            # Also update the top-bar dropdown as if selected there
                            try:
                                runner = self.session_manager.terminal_runners.get(self.terminal_number)
                                if runner and hasattr(runner, 'terminal') and runner.terminal:
                                    terminal_widget = runner.terminal
                                    def _sync_agent_dropdown():
                                        try:
                                            select = terminal_widget.query_one(f"#agent-select-{terminal_widget.terminal_id}")
                                            
                                            # Use same logic as initial dropdown population (Bug 4 fix)
                                            from cai.agents import get_available_agents
                                            agents_to_display = get_available_agents()
                                            
                                            # Filter out ONLY parallel pattern pseudo-agents (same as /agent list command)
                                            agents = []
                                            for agent_key, agent in agents_to_display.items():
                                                # Skip only parallel patterns in the dropdown
                                                if hasattr(agent, "_pattern"):
                                                    pattern = agent._pattern
                                                    if hasattr(pattern, "type"):
                                                        pattern_type_value = getattr(pattern.type, "value", str(pattern.type))
                                                        if pattern_type_value == "parallel":
                                                            continue
                                                agents.append(agent_key)
                                            
                                            # Ensure the selected agent is at the top (like model sync)
                                            if agent_name in agents:
                                                agents.remove(agent_name)
                                            agents.insert(0, agent_name)
                                            
                                            # Create the full options list
                                            agent_options = [(a, a) for a in agents]
                                            select.set_options(agent_options)
                                            select.value = agent_name
                                            
                                            if hasattr(select, 'refresh'):
                                                select.refresh()
                                        except Exception:
                                            pass
                                    try:
                                        terminal_widget.call_after_refresh(_sync_agent_dropdown)
                                    except Exception:
                                        _sync_agent_dropdown()
                            except Exception:
                                pass
                        except Exception as e:
                            intercept_console.print(f"[red]Error updating agent: {e}[/red]")
                            if os.getenv("CAI_DEBUG") == "2":
                                import traceback
                                intercept_console.print(f"[red]{traceback.format_exc()}[/red]")
                    
                    # Handle both cases - running event loop and no event loop
                    try:
                        # Get the running loop
                        loop = asyncio.get_running_loop()
                        # Create task but don't wait for it - let it run in background
                        task = loop.create_task(update_agent_async())
                        
                        # Add error handler but don't block
                        def done_callback(async_task):
                            try:
                                async_task.result()
                            except Exception as e:
                                # Log error but don't block
                                if os.getenv("CAI_DEBUG") == "2":
                                    print(f"Agent update error (async): {e}")
                        
                        task.add_done_callback(done_callback)
                        
                        # Don't wait - return immediately
                        intercept_console.print(f"[green]Agent change initiated for Terminal {self.terminal_number}[/green]")
                    except RuntimeError:
                        # No running event loop, create one
                        asyncio.run(update_agent_async())
                    
                    # Update local tracking for command handler
                    try:
                        from cai.agents import get_agent_by_name
                        self.current_agent = get_agent_by_name(agent_name, agent_id=f"T{self.terminal_number}_{agent_name}")
                        self.current_agent_name = agent_name
                    except Exception as e:
                        if os.getenv("CAI_DEBUG") == "2":
                            intercept_console.print(f"[red]Error updating local agent: {e}[/red]")
                    
                    # Return to prevent double command execution
                    return
                else:
                    intercept_console.print(f"[red]Error: Session manager not available for agent update[/red]")
                    return

            elif cmd_name.lower() == "/model" and args:
                # Model command is handled by the REPL system and _sync_tui_model_selection
                # No need to duplicate the update_model call here since it's already handled
                # by the command itself and _sync_tui_model_selection
                
                # Just show processing feedback
                model_name = args[0]
                intercept_console.print(f"[yellow]Processing model change to: {model_name}...[/yellow]")
                
                # The actual model update will be handled by:
                # 1. The /model command itself (which sets the environment variable)
                # 2. _sync_tui_model_selection (which updates the TUI and calls update_model)
                # This avoids duplicate calls to update_model
                
                return

        except Exception as e:
            intercept_console.print(f"[red]Command error: {e}[/red]")
        finally:
            # Restore active-terminal context environment variables
            if had_active_terminal:
                os.environ["CAI_ACTIVE_COMMAND_TERMINAL"] = previous_active_terminal or ""
            else:
                os.environ.pop("CAI_ACTIVE_COMMAND_TERMINAL", None)

            if had_active_terminal_id:
                if previous_active_terminal_id is not None:
                    os.environ["CAI_ACTIVE_COMMAND_TERMINAL_ID"] = previous_active_terminal_id
                else:
                    os.environ.pop("CAI_ACTIVE_COMMAND_TERMINAL_ID", None)
            else:
                os.environ.pop("CAI_ACTIVE_COMMAND_TERMINAL_ID", None)

            # Restore everything
            rich.console.Console = original_console_class
            if hasattr(rich.console, "get_console") and original_get_console:
                rich.console.get_console = original_get_console
            if hasattr(rich.console, "console"):
                rich.console.console = original_module_console

            # Restore base console
            if "base" in locals() and hasattr(base, "console"):
                base.console = original_base_console

            # Restore console in all command modules
            if "command_modules" in locals():
                for module, original_console in command_modules:
                    module.console = original_console

            # Restore builtins print and std streams
            try:
                __builtins__["print"] = original_print
            except Exception:
                pass
            try:
                sys.stdout = original_stdout
                sys.stderr = original_stderr
            except Exception:
                pass

            __builtins__["print"] = original_print
            sys.stdout = original_stdout
            sys.stderr = original_stderr

        # Restore rich console globals to original
        import rich.console
        rich.console.Console = original_console_class
        if original_get_console:
            rich.console.get_console = original_get_console
        if 'original_module_console' in locals():
            rich.console.console = original_module_console

        # Get any stdout/stderr output
        stdout_output = stdout_capture.getvalue()
        stderr_output = stderr_capture.getvalue()

        # Rich objects have already been written directly to the widget
        # Just add any stdout/stderr output
        if stdout_output and stdout_output.strip():
            self.output.write(stdout_output.rstrip())

        if stderr_output and stderr_output.strip():
            self.output.write(f"[red]{stderr_output.rstrip()}[/red]")

        # Add spacing after command output
        self.output.write("")
