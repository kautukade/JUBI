"""
Terminal Runner - Manages agent execution within a terminal
"""

import os

_CAI_DEBUG_DIR = os.path.join(os.path.expanduser("~"), ".cai", "debug")
import asyncio
import logging
import threading
import traceback
import sys
import time
from contextlib import asynccontextmanager
from typing import Optional, Dict, Any, List, Union
from dataclasses import dataclass
from openai import AsyncOpenAI
import litellm

from cai.agents import get_agent_by_name
from cai.sdk.agents import Agent, Runner, OpenAIChatCompletionsModel
from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
from cai.repl.commands.parallel import PARALLEL_CONFIGS, ParallelConfig
from cai.util import update_agent_models_recursively
from cai.tui.display.manager import DisplayManager
from cai.tui.core.terminal_console import get_terminal_console, set_terminal_output
from cai.tui.core.terminal_tracking import set_current_terminal_id, clear_current_terminal_id
from cai.tui.core.execution_context import set_terminal_id_context, reset_terminal_id_context
from cai.tui.core.environment_overrides import async_environment_override

try:
    from cai.repl.commands.compact import TUI_COMPACTION_MONITOR
except Exception:  # pragma: no cover - compact command may not be available
    TUI_COMPACTION_MONITOR = None


@dataclass
class TerminalConfig:
    """Configuration for a terminal instance"""

    terminal_id: str
    terminal_number: int
    agent_name: str = "redteam_agent"
    model: str = "alias1"
    is_parallel: bool = False
    parallel_config: Optional[ParallelConfig] = None
    env: Optional[Dict[str, str]] = None


class TerminalRunner:
    """Manages agent execution within a terminal"""

    @asynccontextmanager
    async def _agent_environment(self):
        """Temporarily apply environment overrides for this terminal."""
        overrides = self.config.env
        if not overrides:
            yield
            return
        async with async_environment_override(overrides):
            yield

    def __init__(self, terminal_widget, config: TerminalConfig):
        """
        Initialize terminal runner with isolated state

        Args:
            terminal_widget: The UniversalTerminal widget instance
            config: Terminal configuration
        """
        self.terminal = terminal_widget
        self.config = config
        self.agent: Optional[Agent] = None
        self.is_running = False
        self.current_task: Optional[asyncio.Task] = None
        # Note: Message history is now managed by the agent's model, not here
        # Store the runner ID for tracking
        self.runner_id = f"runner_{config.terminal_id}"
        self.logger = logging.getLogger(f"TerminalRunner-{config.terminal_number}")

        # Set up display manager context for this terminal
        self.display_manager = DisplayManager()
        
        # For parallel mode, also register with a predictable terminal ID
        if config.is_parallel and config.parallel_config:
            # Register with both the actual terminal_id and a predictable one
            predictable_id = f"terminal-{config.terminal_number}"
            self.display_manager.set_terminal_output(predictable_id, terminal_widget)
            # Also create a context for the predictable ID
            self.display_manager.create_context(
                terminal_id=predictable_id,
                terminal_number=config.terminal_number,
                agent_name=config.agent_name,
                agent_id=config.parallel_config.id if config.parallel_config else None,
                is_parallel=config.is_parallel,
            )
        
        self.display_context = self.display_manager.create_context(
            terminal_id=config.terminal_id,
            terminal_number=config.terminal_number,
            agent_name=config.agent_name,
            agent_id=config.parallel_config.id if config.parallel_config else None,
            is_parallel=config.is_parallel,
        )

        # Set up console for display manager
        if terminal_widget:
            # Set terminal output in display manager - use UniversalTerminal not RichLog
            self.display_manager.set_terminal_output(config.terminal_id, terminal_widget)

            # Also set up terminal output mapping for legacy support
            set_terminal_output(config.terminal_id, terminal_widget)
            
            # Create and set terminal console for this terminal
            from cai.tui.core.terminal_console import TerminalConsole, set_terminal_console
            terminal_console = TerminalConsole(terminal_widget)
            set_terminal_console(config.terminal_id, terminal_console)

    async def initialize(self) -> None:
        """Initialize the terminal runner and agent"""
        async with self._agent_environment():
            try:
                # Create OpenAI client - default to empty string if not set
                api_key = os.getenv("OPENAI_API_KEY", "")

                # Determine base agent name
                agent_name = self.config.agent_name
                if self.config.is_parallel and self.config.parallel_config:
                    agent_name = self.config.parallel_config.agent_name

                # Instantiate agent depending on parallel mode
                if self.config.is_parallel and self.config.parallel_config:
                    agent_id = self.config.parallel_config.id or f"P{self.config.terminal_number}"
                    model_override = self.config.parallel_config.model or self.config.model
                    self.agent = get_agent_by_name(
                        agent_name,
                        agent_id=agent_id,
                        model_override=model_override,
                    )

                    if not self.agent:
                        if os.getenv('CAI_BROADCAST_MODE') != 'true':
                            self.terminal.write(f"[red]Agent '{agent_name}' not found[/red]")
                        return

                    actual_p_id = agent_id
                    if self.agent and hasattr(self.agent, 'model') and hasattr(self.agent.model, 'agent_id'):
                        actual_p_id = self.agent.model.agent_id

                    if self.terminal and hasattr(self.terminal, 'state'):
                        self.terminal.state.agent_id = actual_p_id

                    from cai.util import apply_compacted_memory_to_agent

                    apply_compacted_memory_to_agent(self.agent)
                else:
                    base_agent = get_agent_by_name(agent_name)
                    if not base_agent:
                        if os.getenv('CAI_BROADCAST_MODE') != 'true':
                            self.terminal.write(f"[red]Agent '{agent_name}' not found[/red]")
                        return

                    agent_id = f"T{self.config.terminal_number}_{agent_name}"
                    self.agent = get_agent_by_name(
                        agent_name,
                        agent_id=agent_id,
                        model_override=self.config.model,
                    )

                    if not self.agent:
                        if os.getenv('CAI_BROADCAST_MODE') != 'true':
                            self.terminal.write(f"[red]Failed to create isolated agent instance[/red]")
                        return

                    actual_p_id = agent_id
                    if self.agent and hasattr(self.agent, 'model') and hasattr(self.agent.model, 'agent_id'):
                        actual_p_id = self.agent.model.agent_id

                    if self.terminal and hasattr(self.terminal, 'state'):
                        self.terminal.state.agent_id = actual_p_id

                    from cai.util import apply_compacted_memory_to_agent

                    apply_compacted_memory_to_agent(self.agent)

                # Update model/terminal metadata
                if self.agent and hasattr(self.agent, "model"):
                    agent_model = self.agent.model
                    display_override = getattr(self.agent, "name", None)
                    if display_override:
                        if getattr(agent_model, "agent_name", None) != display_override:
                            agent_model.agent_name = display_override
                        if getattr(agent_model, "_display_name", None) != display_override:
                            agent_model._display_name = display_override
                        if hasattr(agent_model, "agent_type") and display_override:
                            normalized_type = display_override.lower().replace(" ", "_")
                            current_type = getattr(agent_model, "agent_type", None)
                            if current_type not in {normalized_type, display_override}:
                                agent_model.agent_type = normalized_type
                    agent_id_to_use = agent_id if 'agent_id' in locals() else f"P{self.config.terminal_number}"

                    if self.terminal and hasattr(self.terminal, 'state'):
                        model_name = self.config.model
                        self.terminal.state.model_name = model_name
                        self.terminal.model_name = model_name

                        if os.getenv('CAI_BROADCAST_MODE') != 'true':
                            self.terminal._update_header()
                            if hasattr(self.terminal, 'refresh'):
                                self.terminal.refresh()

                    if hasattr(agent_model, "_display_context"):
                        from cai.tui.display.handoff_context import DisplayContext

                        agent_model._display_context = DisplayContext(
                            terminal_id=self.config.terminal_id,
                            terminal_number=self.config.terminal_number,
                            agent_name=self.agent.name,
                            agent_id=agent_id_to_use,
                            is_parallel=self.config.is_parallel,
                            is_tui_mode=True,
                            display_manager=self.display_manager,
                        )
                    if hasattr(agent_model, "_terminal_id"):
                        agent_model._terminal_id = self.config.terminal_id
                    if hasattr(agent_model, "_terminal_number"):
                        agent_model._terminal_number = self.config.terminal_number
                    # Do not overwrite an existing unique agent_id assigned by manager.
                    # Only set if missing, and prefer parallel id when present.
                    try:
                        if not getattr(agent_model, 'agent_id', None):
                            computed_agent_id = None
                            if getattr(self.config, 'is_parallel', False) and getattr(self.config, 'parallel_config', None):
                                computed_agent_id = getattr(self.config.parallel_config, 'id', None)
                            agent_model.agent_id = computed_agent_id or getattr(agent_model, 'agent_id', None)
                    except Exception:
                        pass

                    self.display_context.agent_name = self.agent.name
                    self.display_context.agent_id = agent_id_to_use

                self.config.agent_name = agent_name
                self.logger.info(
                    f"Terminal {self.config.terminal_number} initialized with agent: {agent_name}"
                )

            except Exception as e:
                error_msg = f"Error initializing terminal: {str(e)}"
                self.logger.error(error_msg, exc_info=True)
                if os.getenv('CAI_BROADCAST_MODE') != 'true':
                    self.terminal.write(f"[red]{error_msg}[/red]")
                self._report_error_to_info_bar(error_msg)

    async def execute_command(self, user_input: str, show_command: bool = True) -> None:
        """
        Execute a user command in this terminal with proper isolation

        Args:
            user_input: The command or chat message to execute
            show_command: Whether to show the command in the terminal
        """
        # Ensure agent is initialized with proper isolation
        if not self.agent or (
            self.config.is_parallel
            and self.config.parallel_config
            and self.config.agent_name != self.config.parallel_config.agent_name
        ):
            await self.initialize()
            # Don't show banner again if terminal already has content
            if hasattr(self.terminal, 'state') and self.terminal.state.output_buffer:
                # Terminal already has content, don't clear it
                if os.getenv('CAI_BROADCAST_MODE') != 'true':
                    self.terminal.write(f"[dim]Agent '{self.config.agent_name}' ready[/dim]")
            
        # Double-check that this terminal has its own agent instance
        if self.agent and hasattr(self.agent, "model") and hasattr(self.agent.model, "agent_id"):
            expected_id_prefix = f"T{self.config.terminal_number}"
            if not self.agent.model.agent_id.startswith(expected_id_prefix):
                self.logger.warning(f"Agent ID mismatch detected, reinitializing for proper isolation")
                await self.initialize()
        
        # Check if model has changed and update if needed
        if self.agent and hasattr(self.agent, 'model') and hasattr(self.agent.model, 'model'):
            current_model = self.agent.model.model
            if current_model != self.config.model:
                # Model has been updated via /model command, update the agent
                # Use silent=True to avoid duplicate Panel messages
                await self.update_model(self.config.model, silent=True)
            
            # IMPORTANT: Re-register terminal outputs after reinitialization
            # This ensures display manager has the correct terminal outputs for parallel mode
            if self.terminal:
                # Debug logging
                if os.getenv("CAI_DEBUG") == "2":
                    self.terminal.write(f"[yellow]Registering output for Terminal {self.config.terminal_number}[/yellow]")
                    self.terminal.write(f"[yellow]Terminal ID: {self.config.terminal_id}[/yellow]")
                
                # Re-register with display manager - use UniversalTerminal not RichLog
                self.display_manager.set_terminal_output(self.config.terminal_id, self.terminal)
                
                # Also update terminal output mapping for legacy support
                set_terminal_output(self.config.terminal_id, self.terminal)
                
                # Update terminal console
                from cai.tui.core.terminal_console import TerminalConsole, set_terminal_console
                terminal_console = get_terminal_console(self.config.terminal_id)
                if terminal_console and hasattr(terminal_console, "set_terminal_output"):
                    terminal_console.set_terminal_output(self.terminal)

        if not self.agent:
            if os.getenv('CAI_BROADCAST_MODE') != 'true':
                self.terminal.write("[red]Agent not initialized[/red]")
            return

        if (
            os.getenv("CAI_TUI_MODE") == "true"
            and TUI_COMPACTION_MONITOR
            and TUI_COMPACTION_MONITOR.is_active(self.config.terminal_number)
        ):
            if os.getenv('CAI_BROADCAST_MODE') != 'true' and self.terminal:
                self.terminal.write(
                    "[yellow]Compaction in progress. Wait until it finishes before sending new messages.[/yellow]"
                )
            return

        if self.is_running:
            # Don't add to queue here - let session manager handle it
            if os.getenv('CAI_BROADCAST_MODE') != 'true':
                self.terminal.write("[yellow]Terminal is busy. Command rejected.[/yellow]")
            return

        self.is_running = True
        
        # Update terminal widget running state
        if hasattr(self.terminal, 'set_running'):
            self.terminal.set_running(True)

        try:
            async with self._agent_environment():
                # Do NOT call add_to_message_history here and do NOT pass the full history as
                # Runner input. OpenAIChatCompletionsModel.get_response() builds the API payload as
                # shallow_copy(message_history) + items_to_messages(input). Pre-adding the user
                # message and passing the entire history duplicated every prior turn on each call
                # (observed as 2x context on TUI; headless CLI passes only the new user string).

                # Increment interaction counter for display context
                if hasattr(self.display_context, 'interaction_counter'):
                    self.display_context.interaction_counter += 1

                # New user turn only — history is merged inside get_response.
                turn_input: Union[str, List[Dict[str, Any]]] = user_input

                # For parallel execution, we need to await the result directly
                # This allows the session manager to properly wait for all agents
                # Cancel any existing task first to avoid reuse issues
                if self.current_task and not self.current_task.done():
                    self.current_task.cancel()
                    try:
                        await self.current_task
                    except asyncio.CancelledError:
                        pass
                
                # Create new task
                self.current_task = asyncio.create_task(self._run_agent_async(turn_input))
                await self.current_task
        except asyncio.CancelledError:
            # This is expected when cancelling - just log it
            self.logger.info(f"Command execution cancelled in terminal {self.config.terminal_number}")
            if os.getenv('CAI_BROADCAST_MODE') != 'true':
                self.terminal.write("[yellow]Execution cancelled[/yellow]")
        except RuntimeError as e:
            if "cannot reuse already awaited coroutine" in str(e):
                # This error should not happen anymore, but if it does, handle gracefully
                self.logger.warning(f"Coroutine reuse attempted in terminal {self.config.terminal_number}")
            else:
                self.logger.error(f"Runtime error executing command: {e}", exc_info=True)
                if os.getenv('CAI_BROADCAST_MODE') != 'true':
                    self.terminal.write(f"[red]Error: {str(e)}[/red]")
                self._report_error_to_info_bar(str(e))
        except Exception as e:
            # Check if this is the coroutine reuse error
            if "cannot reuse already awaited coroutine" in str(e):
                # This error should be silently ignored
                self.logger.debug(f"Coroutine reuse error (ignoring): {e}")
            else:
                self.logger.error(f"Error executing command: {e}", exc_info=True)
                if os.getenv('CAI_BROADCAST_MODE') != 'true':
                    self.terminal.write(f"[red]Error: {str(e)}[/red]")
                self._report_error_to_info_bar(str(e))
        finally:
            self.is_running = False
            
            # Update terminal widget running state
            if hasattr(self.terminal, 'set_running'):
                self.terminal.set_running(False)

    async def _run_agent_async(
        self, turn_input: Union[str, List[Dict[str, Any]]]
    ) -> None:
        """
        Run agent asynchronously without blocking UI
        """
        # DEBUG: Track recursion depth tracking to file
        
        # DEBUG: Add recursion depth tracking to file
        import datetime
        import traceback
        stack_size = len(traceback.extract_stack())
        with open(f"{_CAI_DEBUG_DIR}/cai_recursion_debug.log", "a") as f:
            f.write(f"\n[{datetime.datetime.now()}] _run_agent_async called - Terminal: {self.config.terminal_id}, Stack size: {stack_size}\n")
            
            # Print stack trace if we're getting deep
            if stack_size > 30:  # Reduced threshold to catch earlier
                f.write(f"[RECURSION DEBUG] Deep stack detected! Size: {stack_size}\n")
                for frame in traceback.extract_stack()[-20:]:  # Last 20 frames
                    f.write(f"  {frame.filename}:{frame.lineno} in {frame.name}\n")
        
        # Import context preservation utilities
        from cai.tui.display.context_preservation import ContextPreservingRunner
        # Use local TOOL_OUTPUT_ROUTER if available; fallback to simple scope
        TOOL_OUTPUT_ROUTER = None
        
        # Import parallel terminal routing fix
        from cai.tui.routing.output_router import set_terminal_context
        from cai.tui.core.terminal_tracking_async import set_current_terminal_id_async
        
        # Set terminal ID for this entire execution context
        set_current_terminal_id(self.config.terminal_id)
        # Also set in context vars for async propagation
        context_token = set_terminal_id_context(self.config.terminal_id)
        # Set async context for subprocess calls
        async_token = set_current_terminal_id_async(self.config.terminal_id)
        
        # Set up display context for handoff propagation
        from cai.tui.display.handoff_context import DisplayContext, set_display_context

        # Get agent_id - use existing if available, otherwise create unique one
        agent_id = f"T{self.config.terminal_number}"
        if self.agent and hasattr(self.agent, 'model') and hasattr(self.agent.model, 'agent_id'):
            agent_id = self.agent.model.agent_id
            
        # Set terminal context for proper routing
        set_terminal_context(
            terminal_id=self.config.terminal_id,
            terminal_number=self.config.terminal_number,
        )

        display_context = DisplayContext(
            terminal_id=self.config.terminal_id,
            terminal_number=self.config.terminal_number,
            agent_name=self.config.agent_name,
            agent_id=agent_id,
            is_parallel=self.config.is_parallel,
            is_tui_mode=True,
            display_manager=self.display_manager,
        )
        display_context_token = set_display_context(display_context)

        # Use tool output router for unique execution context
        _exec_scope = (
            TOOL_OUTPUT_ROUTER.execution_scope(self.config.terminal_id, self.config.terminal_number, agent_id)
            if TOOL_OUTPUT_ROUTER else None
        )
        from contextlib import nullcontext
        _cm = _exec_scope if _exec_scope is not None else nullcontext()
        with _cm:
            # Use context preserving runner to ensure terminal ID is available throughout execution
            async with ContextPreservingRunner(self.config.terminal_id):
                try:
                    result = await self._run_agent(turn_input)

                    # Handle result
                    if result and hasattr(result, "final_output"):
                        # Don't add to message history here - the model already added it during execution
                        # Only display output if not using streaming
                        # (streaming already displayed it)
                        stream = os.getenv("CAI_STREAM", "false").lower() == "true"
                        if not stream:
                            # Create assistant message for display
                            assistant_message = {"role": "assistant", "content": result.final_output}

                            # Get token info from the model directly
                            token_info = None
                            if (
                                self.agent
                                and hasattr(self.agent, "model")
                                and hasattr(self.agent.model, "get_token_info")
                            ):
                                token_info = self.agent.model.get_token_info()

                            # Use display manager to show the agent message with proper formatting
                            # Get the actual model from the agent
                            actual_model = self.config.model
                            if self.agent and hasattr(self.agent, 'model'):
                                if isinstance(self.agent.model, str):
                                    actual_model = self.agent.model
                                elif hasattr(self.agent.model, 'model'):
                                    actual_model = self.agent.model.model
                            
                            self.display_manager.display_agent_messages(
                                terminal_id=self.config.terminal_id,
                                messages=[assistant_message],
                                model=actual_model,
                                max_messages=1,
                                token_info=token_info,
                            )

                except asyncio.CancelledError:
                    # Silently handle cancellation
                    pass
                except RecursionError as e:
                    # DEBUG: Special handling for recursion errors
                    import traceback
                    with open(f"{_CAI_DEBUG_DIR}/cai_recursion_debug.log", "a") as f:
                        f.write(f"\n[RECURSION ERROR CAUGHT] in _run_agent_async\n")
                        f.write(f"  Error: {str(e)}\n")
                        f.write(f"  Stack trace at error:\n")
                        traceback.print_exc(file=f)
                    error_msg = f"Error executing command: maximum recursion depth exceeded"
                    self.logger.error(error_msg, exc_info=True)
                    if os.getenv('CAI_BROADCAST_MODE') != 'true':
                        self.terminal.write(f"[red]{error_msg}[/red]")
                    self._report_error_to_info_bar("maximum recursion depth exceeded")
                except asyncio.CancelledError:
                    # This is normal when cancelling - don't show error
                    raise
                except litellm.exceptions.Timeout as e:
                    # Handle timeout with exponential backoff [F]
                    # The model-level retry in openai_chatcompletions.py should
                    # catch most timeouts; this is a last-resort handler.
                    if not hasattr(self, '_tui_timeout_attempt'):
                        self._tui_timeout_attempt = 0
                    self._tui_timeout_attempt += 1
                    _base, _cap = 5.0, 120.0
                    import random
                    _delay = min(_base * (2 ** (self._tui_timeout_attempt - 1)), _cap)
                    _delay += random.uniform(0, _delay * 0.25)
                    error_msg = f"[yellow]⚠️  Request timed out[/yellow] — retrying in {_delay:.0f}s (attempt {self._tui_timeout_attempt}/3)"
                    self.logger.warning(f"Timeout error in terminal {self.config.terminal_number}: {str(e)}")
                    if os.getenv('CAI_BROADCAST_MODE') != 'true':
                        self.terminal.write(error_msg)
                    
                    if self._tui_timeout_attempt >= 3:
                        self._tui_timeout_attempt = 0
                        if os.getenv('CAI_BROADCAST_MODE') != 'true':
                            self.terminal.write("[red]Max retries reached for timeout.[/red]")
                        self._report_error_to_info_bar("Timeout: max retries reached")
                    else:
                        await asyncio.sleep(_delay)
                        # Clean retry: re-execute the SAME original command
                        # (not "continue") to avoid polluting message history
                        if os.getenv('CAI_BROADCAST_MODE') != 'true':
                            self.terminal.write("[green]Retrying original request...[/green]\n")
                        await self.execute_command(command, show_command=False)
                    
                except litellm.exceptions.RateLimitError as e:
                    # Handle rate-limit with exponential backoff [F]
                    if not hasattr(self, '_tui_ratelimit_attempt'):
                        self._tui_ratelimit_attempt = 0
                    self._tui_ratelimit_attempt += 1
                    _base_rl, _cap_rl = 10.0, 120.0
                    import random
                    _delay_rl = min(_base_rl * (2 ** (self._tui_ratelimit_attempt - 1)), _cap_rl)
                    _delay_rl += random.uniform(0, _delay_rl * 0.25)
                    # Use retry_after from server if available
                    _retry_after = getattr(e, 'retry_after', None)
                    if _retry_after and isinstance(_retry_after, (int, float)):
                        _delay_rl = max(_delay_rl, float(_retry_after))
                    error_msg = f"[yellow]⚠️  Rate limit exceeded[/yellow] — retrying in {_delay_rl:.0f}s (attempt {self._tui_ratelimit_attempt}/3)"
                    self.logger.warning(f"Rate limit error in terminal {self.config.terminal_number}: {str(e)}")
                    if os.getenv('CAI_BROADCAST_MODE') != 'true':
                        self.terminal.write(error_msg)
                    
                    if self._tui_ratelimit_attempt >= 3:
                        self._tui_ratelimit_attempt = 0
                        if os.getenv('CAI_BROADCAST_MODE') != 'true':
                            self.terminal.write("[red]Max retries reached for rate limit.[/red]")
                        self._report_error_to_info_bar("Rate limit: max retries reached")
                    else:
                        await asyncio.sleep(_delay_rl)
                        # Clean retry: re-execute the SAME original command
                        if os.getenv('CAI_BROADCAST_MODE') != 'true':
                            self.terminal.write("[green]Retrying original request...[/green]\n")
                        await self.execute_command(command, show_command=False)
                    
                except RuntimeError as e:
                    if "cannot reuse already awaited coroutine" in str(e):
                        # This error should be silently ignored
                        self.logger.debug(f"Coroutine reuse error (ignoring): {e}")
                    else:
                        error_msg = f"Error executing command: {str(e)}"
                        self.logger.error(error_msg, exc_info=True)
                        if os.getenv('CAI_BROADCAST_MODE') != 'true':
                            self.terminal.write(f"[red]{error_msg}[/red]")
                        self._report_error_to_info_bar(str(e))
                except Exception as e:
                    # Check if this is the coroutine reuse error
                    if "cannot reuse already awaited coroutine" in str(e):
                        # This error should be silently ignored
                        self.logger.debug(f"Coroutine reuse error (ignoring): {e}")
                    else:
                        error_msg = f"Error executing command: {str(e)}"
                        self.logger.error(error_msg, exc_info=True)
                        if os.getenv('CAI_BROADCAST_MODE') != 'true':
                            self.terminal.write(f"[red]{error_msg}[/red]")
                        self._report_error_to_info_bar(str(e))
                    
                    # DEBUG: Log detailed error info
                    import traceback
                    with open(f"{_CAI_DEBUG_DIR}/cai_tui_error.log", "a") as f:
                        f.write(f"\n[{datetime.datetime.now()}] Error in _run_agent_async\n")
                        f.write(f"Terminal: {self.config.terminal_id}\n")
                        f.write(f"Error type: {type(e).__name__}\n")
                        f.write(f"Error: {str(e)}\n")
                        f.write("Full traceback:\n")
                        traceback.print_exc(file=f)
                finally:
                    self.is_running = False
                    
                    # Update terminal widget running state
                    if hasattr(self.terminal, 'set_running'):
                        self.terminal.set_running(False)
                        
                    # Clear terminal ID when done
                    clear_current_terminal_id()
                    # Reset context var
                    reset_terminal_id_context(context_token)
                    # Reset async context
                    from cai.tui.core.terminal_tracking_async import reset_current_terminal_id_async
                    reset_current_terminal_id_async(async_token)
                    # Clear display context
                    from cai.tui.display.handoff_context import clear_display_context

                    clear_display_context(display_context_token)

    async def _run_agent(self, turn_input: Union[str, List[Dict[str, Any]]]) -> Any:
        """Run the agent with conversation context"""
        # DEBUG: Track recursion in _run_agent too
        import traceback
        stack_size = len(traceback.extract_stack())
        with open(f"{_CAI_DEBUG_DIR}/cai_recursion_debug.log", "a") as f:
            f.write(f"[RECURSION DEBUG] _run_agent called - Terminal: {self.config.terminal_id}, Stack size: {stack_size}\n")
        
        # Set terminal ID for this thread
        set_current_terminal_id(self.config.terminal_id)

        try:
            # Use streaming if enabled
            stream = os.getenv("CAI_STREAM", "false").lower() == "true"

            if stream:
                # The model handles its own streaming display
                # Just run the streamed execution
                result = Runner.run_streamed(self.agent, turn_input)

                # Process stream events to collect the final content
                content_buffer = ""
                final_usage = None
                event_count = 0
                try:
                    async for event in result.stream_events():
                        # Yield control to the event loop periodically to keep UI responsive
                        event_count += 1
                        if event_count % 10 == 0:  # Yield every 10 events
                            await asyncio.sleep(0)
                        
                        # Handle different event types
                        if hasattr(event, "name") and event.name == "text_output":
                            # Collect text output
                            if hasattr(event, "item") and hasattr(event.item, "content"):
                                content_buffer += event.item.content
                        elif hasattr(event, "name") and event.name == "final_result":
                            # Capture final usage info from the event
                            if hasattr(event, "item") and hasattr(event.item, "usage"):
                                final_usage = event.item.usage
                except asyncio.CancelledError:
                    # Stream was cancelled, this is expected
                    raise

                # Get final usage info from result or captured from events
                if not final_usage and hasattr(result, "usage"):
                    final_usage = result.usage

                # Create a simple result object with the content
                # Don't set final_output to avoid duplicate display
                class StreamResult:
                    def __init__(self, content, usage=None):
                        self.final_output = content
                        self.usage = usage

                return StreamResult(content_buffer, final_usage)
            else:
                # Non-streaming execution
                return await Runner.run(self.agent, turn_input)
        finally:
            # Clear terminal ID when done
            clear_current_terminal_id()

    async def cancel_current_task(self) -> None:
        """Cancel the currently running task and any running tools"""
        cancelled = False

        # Cancel any running tools first
        if self.agent and hasattr(self.agent, 'model'):
            # Signal cancellation through the model if possible
            if hasattr(self.agent.model, '_current_tool_task'):
                tool_task = getattr(self.agent.model, '_current_tool_task', None)
                if tool_task and not tool_task.done():
                    tool_task.cancel()
                    try:
                        await tool_task
                    except asyncio.CancelledError:
                        pass
                    cancelled = True
                    
        # Cancel the main task
        if self.current_task and not self.current_task.done():
            self.current_task.cancel()
            try:
                # Wait for the task to actually finish cancelling
                await self.current_task
            except asyncio.CancelledError:
                # This is expected
                pass
            finally:
                # Clear the task reference
                self.current_task = None
            cancelled = True
        
        # Update terminal to show cancellation
        if cancelled and self.terminal:
            # Only show cancellation message if not in broadcast mode
            if os.getenv('CAI_BROADCAST_MODE') != 'true':
                self.terminal.write("[yellow]⚠️  Execution cancelled[/yellow]")
            if hasattr(self.terminal, 'action_bar'):
                self.terminal.action_bar.stop_streaming()
        
        # Ensure running state is cleared
        self.is_running = False

    def clear_history(self) -> None:
        """Clear conversation history for this terminal only"""
        # Clear the model's message history if it exists
        if (
            self.agent
            and hasattr(self.agent, "model")
            and hasattr(self.agent.model, "message_history")
        ):
            # Ensure we're only clearing this terminal's history
            self.agent.model.message_history.clear()
            
        self.logger.info(f"Cleared history for terminal {self.config.terminal_number}")

    def get_history(self) -> List[Dict[str, Any]]:
        """Get conversation history"""
        if self.agent and hasattr(self.agent, 'model') and hasattr(self.agent.model, 'message_history'):
            return self.agent.model.message_history.copy()
        return []

    async def switch_agent(self, agent_name: str) -> None:
        """Switch to a different agent"""
        # Cancel any running task
        await self.cancel_current_task()
        
        # In TUI mode, preserve the P-ID and history for this terminal
        preserved_p_id = None
        preserved_history = None
        old_terminal_key = None
        
        if os.getenv("CAI_TUI_MODE") == "true":
            from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
            
            # Get current P-ID for this terminal
            if self.agent and hasattr(self.agent, 'model') and hasattr(self.agent.model, 'agent_id'):
                preserved_p_id = self.agent.model.agent_id
                # Get the history directly from AGENT_MANAGER using P-ID
                preserved_history = AGENT_MANAGER._message_history.get(preserved_p_id, [])
                
                # Find and remove old terminal key
                terminal_num = self.config.terminal_number
                for key, agent_id in list(AGENT_MANAGER._agent_registry.items()):
                    if agent_id == preserved_p_id and key.startswith(f"T{terminal_num}_"):
                        old_terminal_key = key
                        break

        # Update config
        self.config.agent_name = agent_name

        # Reinitialize with new agent
        await self.initialize()
        
        # In TUI mode, ensure the new agent uses the same P-ID and history
        if os.getenv("CAI_TUI_MODE") == "true" and preserved_p_id:
            from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
            
            # The new agent should have been assigned the same P-ID by reuse logic
            # Verify this and fix if needed
            if self.agent and hasattr(self.agent, 'model'):
                actual_new_p_id = self.agent.model.agent_id
                
                # If for some reason we got a different P-ID, we need to fix it
                if actual_new_p_id != preserved_p_id:
                    self.logger.warning(f"P-ID mismatch during agent switch: expected {preserved_p_id}, got {actual_new_p_id}")
                    
                # Ensure history is preserved
                if preserved_history and hasattr(self.agent.model, 'message_history'):
                    # Don't transfer - the model should already be using the P-ID's history
                    # Just verify it's the same reference
                    if self.agent.model.message_history is not preserved_history:
                        # Force the correct history reference
                        self.agent.model.message_history = preserved_history
                        AGENT_MANAGER._message_history[preserved_p_id] = preserved_history
            
            # Clean up old terminal key if exists
            if old_terminal_key and old_terminal_key in AGENT_MANAGER._agent_registry:
                del AGENT_MANAGER._agent_registry[old_terminal_key]
        
        # Don't show switched message - the agent will show its own initialization
        try:
            if os.getenv("CAI_TUI_MODE") == "true":
                from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
                AGENT_MANAGER.cleanup_tui_orphaned_agents()
        except Exception:
            pass

    async def update_model(self, model_name: str, silent: bool = False) -> None:
        """Update the model used by the agent"""
        
        # Debug logging
        if os.getenv("CAI_DEBUG") == "2":
            self.logger.info(f"Terminal {self.config.terminal_number}: Starting model update to {model_name}")
            if self.terminal:
                self.terminal.write(f"[cyan]DEBUG: update_model called for terminal {self.config.terminal_number}[/cyan]")
                self.terminal.write(f"[cyan]DEBUG: New model: {model_name}[/cyan]")
        
        # Update config first
        self.config.model = model_name
        
        # Update terminal's model display immediately
        if self.terminal and hasattr(self.terminal, 'state'):
            self.terminal.state.model_name = model_name
            self.terminal.model_name = model_name  # Update reactive property
            
            # Force UI updates (skip in broadcast mode)
            if os.getenv('CAI_BROADCAST_MODE') != 'true':
                self.terminal._update_header()
                if hasattr(self.terminal, 'refresh'):
                    self.terminal.refresh()
                # Also force info bar update
                if hasattr(self.terminal, 'info_bar') and self.terminal.info_bar:
                    self.terminal.info_bar._update_info()
            
            # Force the reactive property to trigger its watcher
            # This is needed because sometimes the watcher doesn't fire
            if hasattr(self.terminal, 'model_name'):
                # Trigger the watcher by setting to a temp value then back
                temp = self.terminal.model_name
                self.terminal.model_name = ""
                self.terminal.model_name = model_name
                
            # Model update will be shown after completion
        
        # If no agent yet, just return (will use new model on next init)
        if not self.agent:
            if os.getenv('CAI_BROADCAST_MODE') != 'true':
                self.terminal.write(f"[green]Model will be updated to: {model_name} on next run[/green]")
            return

        # Try to update just the model without full reinitialization
        try:
            if self.agent and hasattr(self.agent, 'model'):
                # Update the model directly if possible
                from cai.sdk.agents.models.openai_chatcompletions import OpenAIChatCompletionsModel
                
                # Create new model instance with same config but different model name
                if isinstance(self.agent.model, OpenAIChatCompletionsModel):
                    # Get current agent info
                    agent_name = self.agent.model.agent_name if hasattr(self.agent.model, 'agent_name') else self.agent.name
                    agent_id = self.agent.model.agent_id if hasattr(self.agent.model, 'agent_id') else None
                    agent_type = getattr(self.agent.model, 'agent_type', None)
                    existing_client = getattr(self.agent.model, '_client', None)
                    if existing_client is None:
                        try:
                            from cai.sdk.agents.models import _openai_shared

                            existing_client = _openai_shared.get_default_openai_client()
                        except ImportError:
                            existing_client = None

                    if existing_client is None:
                        existing_client = AsyncOpenAI()

                    # Create new model instance
                    new_model = OpenAIChatCompletionsModel(
                        model=model_name,
                        openai_client=existing_client,
                        agent_name=agent_name,
                        agent_id=agent_id,
                        agent_type=agent_type,
                    )

                    # Update the agent's model
                    self.agent.model = new_model
                    
                    # Update parallel config if needed
                    if self.config.is_parallel and self.config.parallel_config:
                        self.config.parallel_config.model = model_name
                    
                    if not silent and os.getenv('CAI_BROADCAST_MODE') != 'true':
                        self._show_model_update_panel(model_name)
                else:
                    # Fallback to full reinitialization for other model types
                    self.agent = None
                    await self.initialize()
                    if not silent and os.getenv('CAI_BROADCAST_MODE') != 'true':
                        self._show_model_update_panel(model_name, reinitialized=True)
            else:
                # No agent yet, will use new model on next init
                if not silent and os.getenv('CAI_BROADCAST_MODE') != 'true':
                    self._show_model_update_panel(model_name, next_run=True)
        except Exception as e:
            # Fallback to full reinitialization on error
            self.logger.warning(f"Fast model update failed, reinitializing: {e}")
            self.agent = None
            await self.initialize()
            if not silent and os.getenv('CAI_BROADCAST_MODE') != 'true':
                self._show_model_update_panel(model_name, reinitialized=True)

    def _show_model_update_panel(self, model_name: str, reinitialized: bool = False, next_run: bool = False) -> None:
        """Show unified model update panel"""
        # Prevent duplicate panels for the same model within a short time window
        import time
        current_time = time.time()
        
        # Check if we recently showed a panel for this model
        if hasattr(self, '_last_model_panel_time') and hasattr(self, '_last_model_panel_name'):
            time_diff = current_time - self._last_model_panel_time
            if self._last_model_panel_name == model_name and time_diff < 2.0:  # 2 second window
                return  # Skip duplicate panel
        
        # Update tracking variables
        self._last_model_panel_time = current_time
        self._last_model_panel_name = model_name
        
        try:
            from rich.panel import Panel
            
            if next_run:
                message = (
                    f"Model will be updated to: [bold green]{model_name}[/bold green]\n"
                    "[yellow]Note: This will take effect on the next agent interaction[/yellow]"
                )
                title = "Model Queued"
            elif reinitialized:
                message = (
                    f"Model updated to: [bold green]{model_name}[/bold green]\n"
                    "[yellow]Note: Agent was reinitialized with the new model[/yellow]"
                )
                title = "Model Updated"
            else:
                message = (
                    f"Model updated to: [bold green]{model_name}[/bold green]\n"
                    "[yellow]Note: This will take effect on the next agent interaction[/yellow]"
                )
                title = "Model Updated"
            
            panel = Panel(message, border_style="green", title=title)
            if self.terminal:
                self.terminal.write(panel)
        except Exception:
            # Fallback to simple message if Panel fails
            if self.terminal:
                self.terminal.write(f"[green]Model updated to: {model_name}[/green]")

    async def cleanup(self) -> None:
        """Cleanup terminal runner resources"""
        # Cancel any running tasks
        await self.cancel_current_task()

        # Clear terminal ID mapping
        clear_current_terminal_id()
        
        # Destroy the agent instance (capture metadata before nulling reference)
        agent_id_for_cleanup = None
        agent_name_for_cleanup = None
        if self.agent and hasattr(self.agent, "model"):
            agent_id_for_cleanup = getattr(self.agent.model, "agent_id", None)
            agent_name_for_cleanup = getattr(self.agent.model, "agent_name", None)

        if self.agent:
            # If this is a parallel agent, unregister it from AGENT_MANAGER
            if self.config.is_parallel and self.config.parallel_config:
                agent_id = self.config.parallel_config.id or f"P{self.config.terminal_number}"
                # Remove from agent registry
                if hasattr(AGENT_MANAGER, '_agent_registry') and agent_id in AGENT_MANAGER._agent_registry:
                    with AGENT_MANAGER._registry_lock:
                        del AGENT_MANAGER._agent_registry[agent_id]
                # Also remove the history reference
                if hasattr(AGENT_MANAGER, '_message_history') and agent_id in AGENT_MANAGER._message_history:
                    with AGENT_MANAGER._registry_lock:
                        del AGENT_MANAGER._message_history[agent_id]
                if not agent_id_for_cleanup:
                    agent_id_for_cleanup = agent_id
                    agent_name_for_cleanup = self.config.parallel_config.agent_name

            # Clear the agent reference
            self.agent = None

        # In TUI mode, remove registry keys associated with this terminal (Tn_*)
        try:
            if os.getenv("CAI_TUI_MODE") == "true":
                terminal_prefix = f"T{self.config.terminal_number}_"
                with AGENT_MANAGER._registry_lock:
                    touched_p_ids = set()
                    for key, p_id in list(AGENT_MANAGER._agent_registry.items()):
                        if key.startswith(terminal_prefix):
                            touched_p_ids.add(p_id)
                            del AGENT_MANAGER._agent_registry[key]
                    # Prune unreferenced empty histories and name mappings
                    for p_id in touched_p_ids:
                        still_ref = any(v == p_id for v in AGENT_MANAGER._agent_registry.values())
                        if not still_ref:
                            if p_id in AGENT_MANAGER._message_history and not AGENT_MANAGER._message_history[p_id]:
                                del AGENT_MANAGER._message_history[p_id]
                            if p_id in getattr(AGENT_MANAGER, "_p_id_to_agent_name", {}):
                                del AGENT_MANAGER._p_id_to_agent_name[p_id]
        except Exception:
            pass

        # Clear history
        self.clear_history()

        # Remove cost tracking entries tied to this runner
        try:
            from cai.util import COST_TRACKER

            terminal_id_for_cleanup = self.config.terminal_id
            if not agent_name_for_cleanup:
                agent_name_for_cleanup = self.config.agent_name

            COST_TRACKER.remove_agent_tracking(
                agent_id=agent_id_for_cleanup,
                agent_name=agent_name_for_cleanup,
                terminal_id=terminal_id_for_cleanup,
            )
        except Exception:
            pass
        
        # Clear display manager context
        if hasattr(self, 'display_manager'):
            # Remove terminal output mapping
            if hasattr(self.display_manager, '_terminal_outputs'):
                self.display_manager._terminal_outputs.pop(self.config.terminal_id, None)
                # Also remove predictable ID for parallel terminals
                if self.config.is_parallel:
                    predictable_id = f"terminal-{self.config.terminal_number}"
                    self.display_manager._terminal_outputs.pop(predictable_id, None)

        self.logger.info(f"Terminal {self.config.terminal_number} cleaned up")

    def _report_error_to_info_bar(self, error_message: str) -> None:
        """Report error to the info status bar"""
        try:
            # Get the app instance from terminal
            if hasattr(self.terminal, 'app') and self.terminal.app:
                # Try to find the info bar
                from cai.tui.components.info_status_bar import InfoStatusBar
                info_bar = self.terminal.app.query_one("#info-status-bar", InfoStatusBar)
                if info_bar:
                    # Extract just the error message, remove formatting
                    clean_error = error_message.replace("[red]", "").replace("[/red]", "")
                    # Remove "Error: " prefix if present
                    if clean_error.startswith("Error: "):
                        clean_error = clean_error[7:]
                    # Set the error
                    info_bar.set_error(clean_error)
        except Exception:
            # Silently fail if we can't update the info bar
            pass
