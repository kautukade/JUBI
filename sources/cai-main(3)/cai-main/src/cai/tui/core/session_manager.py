"""
Session Manager - Manages TUI session state and terminal runners
"""

import os
import asyncio
import logging
import threading
from typing import Dict, Optional, Any, ClassVar
from datetime import datetime

from cai.tui.core.terminal_runner import TerminalRunner, TerminalConfig
from cai.tui.core.agent_executor import AgentExecutor
from cai.repl.commands.parallel import PARALLEL_CONFIGS
from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER
from cai.util import COST_TRACKER


class SessionManager:
    """Manages the overall TUI session including all terminals"""

    _instance_lock: ClassVar[threading.Lock] = threading.Lock()
    _instance: ClassVar[Optional["SessionManager"]] = None

    @classmethod
    def get_instance(cls) -> Optional["SessionManager"]:
        """Return the active session manager if one is running."""
        with cls._instance_lock:
            if cls._instance:
                return cls._instance

        # Fallback to the active CAITerminal if available
        try:
            from cai.tui.cai_terminal import CAITerminal

            app = getattr(CAITerminal, "_instance", None) or getattr(
                CAITerminal, "_current_app", None
            )
            if app and getattr(app, "session_manager", None):
                instance = app.session_manager
                with cls._instance_lock:
                    cls._instance = instance
                return instance
        except Exception:
            return None

        return None

    def __init__(self):
        self.terminal_runners: Dict[int, TerminalRunner] = {}
        self.agent_executor = AgentExecutor()
        self.session_start_time = datetime.now()
        self.is_parallel_mode = False
        self.logger = logging.getLogger("SessionManager")
        self._terminal_creation_lock = asyncio.Lock()  # Serialize terminal creation

        # Reset cost tracking
        COST_TRACKER.reset_agent_costs()

        # Reset agent manager
        AGENT_MANAGER.reset_registry()

        with SessionManager._instance_lock:
            SessionManager._instance = self

    def add_terminal_runner(self, terminal_number: int, terminal_widget: Any) -> TerminalRunner:
        """
        Add a terminal runner for a terminal widget

        Args:
            terminal_number: The terminal number (1, 2, 3, etc.)
            terminal_widget: The UniversalTerminal widget

        Returns:
            The created TerminalRunner
        """
        config = TerminalConfig(
            terminal_id=terminal_widget.terminal_id,
            terminal_number=terminal_number,
            # Always start with redteam_agent in TUI
            agent_name="redteam_agent",
            model=os.getenv("CAI_MODEL", "alias1"),
        )

        runner = TerminalRunner(terminal_widget, config)
        self.terminal_runners[terminal_number] = runner
        
        # Track terminal count in AGENT_MANAGER
        AGENT_MANAGER.increment_terminal_count()

        return runner

    async def initialize_terminal(self, terminal_number: int) -> None:
        """Initialize a specific terminal"""
        if terminal_number in self.terminal_runners:
            async with self._terminal_creation_lock:
                await self.terminal_runners[terminal_number].initialize()
                # Clean up any orphaned agents after initialization
                AGENT_MANAGER.cleanup_orphaned_parallel_agents()

    async def update_terminal_agent(self, terminal_number: int, agent_name: str) -> None:
        """Update the agent for a specific terminal"""
        self.logger.info(f"update_terminal_agent called for terminal {terminal_number} with agent {agent_name}")
        
        if terminal_number in self.terminal_runners:
            runner = self.terminal_runners[terminal_number]
            try:
                # Use the terminal runner's switch_agent method
                await runner.switch_agent(agent_name)
                
                # Update the terminal's UI state
                if hasattr(runner.terminal, 'state') and runner.agent:
                    runner.terminal.state.agent_name = agent_name
                    # Update reactive property
                    runner.terminal.agent_name = agent_name
                    
                    # Get the actual agent ID and model from the runner's agent
                    if hasattr(runner.agent, 'model'):
                        if hasattr(runner.agent.model, 'agent_id'):
                            runner.terminal.state.agent_id = runner.agent.model.agent_id
                        if hasattr(runner.agent.model, 'model'):
                            runner.terminal.state.model_name = runner.agent.model.model
                            runner.terminal.model_name = runner.agent.model.model
                    
                    # Force header update and refresh (skip in broadcast mode)
                    if os.getenv('CAI_BROADCAST_MODE') != 'true':
                        if hasattr(runner.terminal, '_update_header'):
                            runner.terminal._update_header()
                        if hasattr(runner.terminal, 'refresh'):
                            runner.terminal.refresh()
                    
                    # Force the reactive properties to trigger their watchers
                    if hasattr(runner.terminal, 'agent_name'):
                        # Trigger the watcher by setting to a temp value then back
                        temp = runner.terminal.agent_name
                        runner.terminal.agent_name = ""
                        runner.terminal.agent_name = agent_name
                    if hasattr(runner.terminal, 'model_name') and runner.agent and hasattr(runner.agent.model, 'model'):
                        temp = runner.terminal.model_name
                        runner.terminal.model_name = ""
                        runner.terminal.model_name = runner.agent.model.model
                
                # Log the change
                self.logger.info(f"Terminal {terminal_number} agent changed to: {agent_name}")
                
                # Clean up any orphaned parallel agents
                AGENT_MANAGER.cleanup_orphaned_parallel_agents()
                
            except Exception as e:
                self.logger.error(f"Error updating terminal {terminal_number} agent: {e}")
                if runner.terminal:
                    runner.terminal.write(f"[red]Error changing agent: {e}[/red]")
    
    async def execute_command(self, command: str, terminal_number: int = None) -> None:
        """
        Execute a command in a specific terminal or all terminals

        Args:
            command: The command to execute
            terminal_number: Terminal number (None for broadcast to all)
        """
        # Debug logging
        if os.getenv("CAI_DEBUG") == "2":
            self.logger.info(f"Execute command called: terminal_number={terminal_number}, command={command[:50]}...")
        # If terminal_number is None, broadcast to all terminals
        if terminal_number is None:
            # Check if we're in parallel mode (TUI mode checks for multiple terminals)
            tui_parallel_mode = os.getenv("CAI_TUI_MODE") == "true" and len(self.terminal_runners) > 1
            if (self.is_parallel_mode and len(PARALLEL_CONFIGS) > 0) or tui_parallel_mode:
                # In parallel mode: write command to main terminal only ONCE
                if 1 in self.terminal_runners:
                    self.terminal_runners[1].terminal.write_command(command)

                # Execute in parallel agents (terminals 2+)
                # Note: _execute_parallel_command will write to each terminal
                await self._execute_parallel_command(command)
            else:
                # In single mode: write to all terminals and execute in main
                for runner in self.terminal_runners.values():
                    runner.terminal.write_command(command)

                # Execute only in main terminal (terminal 1)
                if 1 in self.terminal_runners:
                    # Check if runner is busy
                    runner = self.terminal_runners[1]
                    from cai.tui.core.terminal_queue import TERMINAL_QUEUE_MANAGER
                    
                    if runner.is_running:
                        # Add to main terminal's queue
                        await TERMINAL_QUEUE_MANAGER.add_prompt(command, 1)
                        queue = await TERMINAL_QUEUE_MANAGER.get_queue(1)
                        queue_size = len(queue._queue)
                        runner.terminal.write(f"[yellow]Main terminal is busy. Added to queue (position {queue_size}).[/yellow]")
                        return
                    if os.getenv("CAI_DEBUG") == "2":
                        self.logger.info(f"Executing in main terminal (broadcast): {command[:50]}...")
                    await runner.execute_command(command, show_command=False)
                    # Process any queued prompts after execution
                    await self._process_terminal_queue(1)
                else:
                    self.logger.warning("Main terminal (1) not found in runners")
        else:
            # Single terminal execution
            if terminal_number in self.terminal_runners:
                runner = self.terminal_runners[terminal_number]
                # Debug log
                if os.getenv("CAI_DEBUG") == "2":
                    self.logger.info(f"Executing in terminal {terminal_number}: {command[:50]}...")
                    self.logger.info(f"Terminal runners available: {list(self.terminal_runners.keys())}")
                    self.logger.info(f"Runner agent name: {runner.config.agent_name if runner else 'No runner'}")
                    # Also write to the terminal for visibility
                    runner.terminal.write(f"[bold cyan]>>> SESSION MANAGER: Executing command in Terminal {terminal_number} <<<[/bold cyan]")
                    runner.terminal.write(f"[cyan]Command: {command}[/cyan]")
                    runner.terminal.write(f"[cyan]Agent: {runner.config.agent_name}[/cyan]")
                    runner.terminal.write("")
                # Check if the terminal is busy
                from cai.tui.core.terminal_queue import TERMINAL_QUEUE_MANAGER
                
                # If terminal is not busy, execute directly
                if not runner.is_running:
                    if os.getenv("CAI_DEBUG") == "2":
                        self.logger.info(f"Runner {terminal_number} idle; executing immediately")
                    # Don't write command here - it's already written in the terminal where typed
                    # Execute in that terminal
                    await runner.execute_command(command, show_command=False)
                    
                    # After execution, check if there are queued prompts
                    await self._process_terminal_queue(terminal_number)
                else:
                    if os.getenv("CAI_DEBUG") == "2":
                        self.logger.info(f"Runner {terminal_number} busy; queuing command")
                    # Terminal is busy, add to its queue
                    await TERMINAL_QUEUE_MANAGER.add_prompt(command, terminal_number)
                    queue = await TERMINAL_QUEUE_MANAGER.get_queue(terminal_number)
                    queue_size = len(queue._queue)
                    runner.terminal.write(f"[yellow]Terminal {terminal_number} is busy. Added to queue (position {queue_size}).[/yellow]")
            else:
                self.logger.warning(f"Terminal {terminal_number} not found in runners: {list(self.terminal_runners.keys())}")
                # Try to find the main terminal and show error
                if 1 in self.terminal_runners:
                    main_terminal = self.terminal_runners[1].terminal
                    main_terminal.write(f"[red]Error: Terminal {terminal_number} not registered with session manager[/red]")
                    main_terminal.write(f"[red]Available terminals: {list(self.terminal_runners.keys())}[/red]")

    async def _execute_parallel_command(self, command: str) -> None:
        """Execute command in parallel mode"""
        # Setup parallel isolation first
        from cai.sdk.agents.parallel_isolation import PARALLEL_ISOLATION
        
        # In TUI mode, use terminal runners directly
        if os.getenv("CAI_TUI_MODE") == "true":
            # Create tasks for each terminal (except main)
            tasks = []
            for term_num, runner in self.terminal_runners.items():
                if term_num > 1:  # Skip main terminal
                    # Write command to terminal
                    runner.terminal.write_command(command)
                    # Execute command
                    task = asyncio.create_task(runner.execute_command(command, show_command=False))
                    tasks.append((term_num, task))
            
            # Wait for all tasks to complete
            if tasks:
                await asyncio.gather(*[task for _, task in tasks], return_exceptions=True)
            
            return
        
        # Original parallel mode logic (for CLI mode)
        # Get agent IDs
        agent_ids = [
            config.id or f"P{idx}" for idx, config in enumerate(PARALLEL_CONFIGS, 1)
        ]
        
        # Check if we already have isolated histories
        already_has_histories = False
        if PARALLEL_ISOLATION.is_parallel_mode():
            for agent_id in agent_ids:
                if PARALLEL_ISOLATION.get_isolated_history(agent_id):
                    already_has_histories = True
                    break
        
        if not already_has_histories:
            # Get current history from main terminal's agent
            current_history = []
            if 1 in self.terminal_runners:
                runner = self.terminal_runners[1]
                if runner.agent and hasattr(runner.agent, "model") and hasattr(runner.agent.model, "message_history"):
                    current_history = runner.agent.model.message_history
            
            # Transfer history to parallel agents
            pattern_description = os.getenv("CAI_PATTERN_DESCRIPTION", "")
            if "different contexts" in pattern_description.lower():
                # Only transfer to first agent
                PARALLEL_ISOLATION._parallel_mode = True
                if current_history and agent_ids:
                    PARALLEL_ISOLATION.clear_all_histories()
                    PARALLEL_ISOLATION.replace_isolated_history(agent_ids[0], current_history.copy())
                    for agent_id in agent_ids[1:]:
                        PARALLEL_ISOLATION.replace_isolated_history(agent_id, [])
            else:
                # Transfer to all agents
                PARALLEL_ISOLATION.transfer_to_parallel(current_history, len(PARALLEL_CONFIGS), agent_ids)
        else:
            PARALLEL_ISOLATION._parallel_mode = True
        
        # Create tasks for parallel execution in each terminal runner
        tasks = []

        # Write parallel execution status to main terminal
        if 1 in self.terminal_runners:
            main_terminal = self.terminal_runners[1].terminal
            main_terminal.write(f"[bold cyan]╭{'─' * 68}╮[/bold cyan]")
            main_terminal.write(
                f"[bold cyan]│ PARALLEL EXECUTION: {len(PARALLEL_CONFIGS)} agents {'  ' * (44 - len(str(len(PARALLEL_CONFIGS))))}│[/bold cyan]"
            )
            main_terminal.write(f"[bold cyan]╰{'─' * 68}╯[/bold cyan]")
            main_terminal.write(f"[cyan]Command: {command}[/cyan]")
            main_terminal.write("")

        # In parallel mode, each terminal runs an agent from PARALLEL_CONFIGS
        # Terminal 1 runs the first agent, Terminal 2 runs the second, etc.
        for idx, config in enumerate(PARALLEL_CONFIGS):
            terminal_number = idx + 1  # Terminal numbers: 1, 2, 3...

            if terminal_number in self.terminal_runners:
                # Get the runner for this terminal
                runner = self.terminal_runners[terminal_number]

                # Update runner config to match parallel config
                runner.config.agent_name = config.agent_name
                runner.config.is_parallel = True
                runner.config.parallel_config = config

                # Ensure the runner is reinitialized with the new config
                # This is important for parallel agents to get the right instance
                runner.agent = None  # Force reinitialization
                
                # Don't write command here - it was already written to main terminal
                # Only write to agent terminals if they are different from main
                if terminal_number > 1:
                    runner.terminal.write_command(command)

                # Determine the actual input to use
                actual_input = config.prompt if config.prompt else command

                # Execute command in this terminal using its runner
                # This ensures proper Rich panel formatting
                # Create task with context preservation
                from cai.tui.display.context_preservation import ensure_terminal_context
                
                # Create a coroutine that sets its own terminal context
                async def run_with_terminal_context(runner, input_text, terminal_id):
                    """Run command with specific terminal context"""
                    from cai.tui.routing.output_router import route_to_terminal, set_terminal_context
                    
                    # Set routing context (only terminal id/number)
                    set_terminal_context(terminal_id, runner.config.terminal_number)
                    
                    # Use routing context manager
                    with route_to_terminal(terminal_id, runner.config.terminal_number):
                        # Execute the command
                        return await runner.execute_command(input_text, show_command=False)
                
                # Create task for this terminal
                coro = run_with_terminal_context(runner, actual_input, runner.config.terminal_id)
                task = asyncio.create_task(coro, name=f"terminal-{terminal_number}")
                tasks.append((terminal_number, task))

        # Execute all in parallel
        if tasks:
            try:
                results = await asyncio.gather(*[task for _, task in tasks], return_exceptions=True)
                
                # Save histories back to isolation after execution
                for idx, config in enumerate(PARALLEL_CONFIGS):
                    terminal_number = idx + 1
                    if terminal_number in self.terminal_runners:
                        runner = self.terminal_runners[terminal_number]
                        if runner.agent and hasattr(runner.agent, "model") and hasattr(runner.agent.model, "message_history"):
                            agent_id = config.id or f"P{idx + 1}"
                            PARALLEL_ISOLATION.replace_isolated_history(agent_id, runner.agent.model.message_history)
                
                # Report completion to main terminal
                if 1 in self.terminal_runners:
                    main_terminal = self.terminal_runners[1].terminal
                    main_terminal.write(
                        f"[green]Parallel execution completed for {len(tasks)} agents[/green]"
                    )
                    main_terminal.write("")
            except Exception as e:
                self.logger.error(f"Error in parallel execution: {e}", exc_info=True)
                if 1 in self.terminal_runners:
                    main_terminal = self.terminal_runners[1].terminal
                    main_terminal.write(f"[red]Error in parallel execution: {str(e)}[/red]")

        # Log results
        self.logger.info(f"Parallel execution completed: {len(PARALLEL_CONFIGS)} agents")

    def set_parallel_mode(self, enabled: bool) -> None:
        """Enable or disable parallel mode"""
        self.is_parallel_mode = enabled

        if enabled:
            self.logger.info(f"Parallel mode enabled with {len(PARALLEL_CONFIGS)} agents")
        else:
            self.logger.info("Parallel mode disabled")
            # Clean up orphaned parallel agents when exiting parallel mode
            AGENT_MANAGER.cleanup_orphaned_parallel_agents()

    async def switch_agent(self, agent_name: str, terminal_number: int = 1) -> None:
        """Switch agent in a specific terminal"""
        if terminal_number in self.terminal_runners:
            await self.terminal_runners[terminal_number].switch_agent(agent_name)

    async def update_model(self, model_name: str, terminal_number: Optional[int] = None, silent: bool = False) -> None:
        """Update model for specific terminal or all terminals
        
        Args:
            model_name: The model name to set
            terminal_number: Optional terminal number. If None, updates all terminals.
            silent: If True, suppress the confirmation panel (used when command already shows it)
        """
        if terminal_number is not None:
            # Update specific terminal
            if terminal_number in self.terminal_runners:
                await self.terminal_runners[terminal_number].update_model(model_name, silent=silent)
            else:
                self.logger.warning(f"Terminal {terminal_number} not found")
        else:
            # Update all terminals
            tasks = []
            for runner in self.terminal_runners.values():
                tasks.append(runner.update_model(model_name, silent=silent))

            if tasks:
                await asyncio.gather(*tasks)

    def clear_terminal_history(self, terminal_number: int) -> None:
        """Clear history for a specific terminal"""
        if terminal_number in self.terminal_runners:
            self.terminal_runners[terminal_number].clear_history()

    def clear_all_histories(self) -> None:
        """Clear history for all terminals"""
        for runner in self.terminal_runners.values():
            runner.clear_history()

    async def cancel_all_tasks(self) -> None:
        """Cancel all running tasks across all terminals"""
        # Cancel terminal tasks
        tasks = []
        for runner in self.terminal_runners.values():
            tasks.append(runner.cancel_current_task())

        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)

        # Cancel parallel execution tasks
        await self.agent_executor.cancel_all_tasks()
    
    async def _process_terminal_queue(self, terminal_number: int) -> None:
        """Process queued prompts for a terminal after it becomes free"""
        from cai.tui.core.terminal_queue import TERMINAL_QUEUE_MANAGER
        
        # Keep processing while there are queued prompts and terminal is free
        while terminal_number in self.terminal_runners:
            runner = self.terminal_runners[terminal_number]
            
            # Check if terminal is busy
            if runner.is_running:
                break
                
            # Get next prompt from queue
            next_prompt = await TERMINAL_QUEUE_MANAGER.get_next_prompt(terminal_number)
            if not next_prompt:
                # No more prompts, mark queue as completed
                await TERMINAL_QUEUE_MANAGER.mark_completed(terminal_number)
                break
                
            # Process the queued prompt
            runner.terminal.write(f"[cyan]Processing queued prompt: {next_prompt[:50]}...[/cyan]")
            runner.terminal.write_command(next_prompt)
            
            # Execute the command
            await runner.execute_command(next_prompt, show_command=False)
            
            # Mark this prompt as completed
            await TERMINAL_QUEUE_MANAGER.mark_completed(terminal_number)
            
            # Small delay before processing next
            await asyncio.sleep(0.1)

    def get_session_stats(self) -> Dict[str, Any]:
        """Get session statistics"""
        duration = datetime.now() - self.session_start_time

        return {
            "duration": str(duration),
            "total_cost": COST_TRACKER.session_total_cost,
            "terminal_count": len(self.terminal_runners),
            "parallel_mode": self.is_parallel_mode,
            "parallel_agents": len(PARALLEL_CONFIGS) if self.is_parallel_mode else 0,
        }

    def cleanup(self) -> None:
        """Cleanup session resources"""
        # Decrement terminal count for each runner
        for terminal_num in self.terminal_runners:
            AGENT_MANAGER.decrement_terminal_count()
            
        # Clear all runners
        self.terminal_runners.clear()

        # Reset agent manager
        AGENT_MANAGER.reset_registry()

        self.logger.info("Session cleanup completed")
