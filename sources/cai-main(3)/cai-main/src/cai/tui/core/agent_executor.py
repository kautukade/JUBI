"""
Agent Executor - Handles parallel agent execution
"""

import asyncio
import os
import logging
from typing import List, Dict, Any, Optional, Tuple
from dataclasses import dataclass

from cai.agents import get_agent_by_name, get_available_agents
from cai.sdk.agents import Agent, Runner
from cai.sdk.agents.parallel_isolation import PARALLEL_ISOLATION
from cai.repl.commands.parallel import PARALLEL_CONFIGS, ParallelConfig, PARALLEL_AGENT_INSTANCES
from cai.util import update_agent_models_recursively


@dataclass
class ParallelResult:
    """Result from parallel agent execution"""

    config: ParallelConfig
    agent_name: str
    instance_number: int
    result: Any
    error: Optional[str] = None


class AgentExecutor:
    """Handles execution of agents including parallel configurations"""

    def __init__(self):
        self.logger = logging.getLogger("AgentExecutor")
        self.running_tasks: Dict[str, asyncio.Task] = {}

    async def execute_parallel(
        self, user_input: str, terminal_outputs: Dict[int, Any]
    ) -> List[ParallelResult]:
        """
        Execute parallel agents

        Args:
            user_input: The user's input
            terminal_outputs: Dictionary mapping terminal numbers to output widgets

        Returns:
            List of ParallelResult objects
        """
        if not PARALLEL_CONFIGS:
            return []

        # Prepare agent IDs
        agent_ids = [config.id or f"P{idx}" for idx, config in enumerate(PARALLEL_CONFIGS, 1)]

        # Setup parallel isolation
        if not PARALLEL_ISOLATION.is_parallel_mode():
            # Transfer current history to parallel agents
            current_history = self._get_current_history()

            # Check if pattern requires different contexts
            pattern_description = os.getenv("CAI_PATTERN_DESCRIPTION", "")
            if "different contexts" in pattern_description.lower():
                # Only transfer to first agent
                PARALLEL_ISOLATION._parallel_mode = True
                PARALLEL_ISOLATION.clear_all_histories()
                if current_history and agent_ids:
                    PARALLEL_ISOLATION.replace_isolated_history(
                        agent_ids[0], current_history.copy()
                    )
                    for agent_id in agent_ids[1:]:
                        PARALLEL_ISOLATION.replace_isolated_history(agent_id, [])
            else:
                # Transfer to all agents
                PARALLEL_ISOLATION.transfer_to_parallel(
                    current_history, len(PARALLEL_CONFIGS), agent_ids
                )

        # Create tasks for parallel execution
        tasks = []
        for idx, config in enumerate(PARALLEL_CONFIGS, 1):
            terminal_output = terminal_outputs.get(
                idx + 1
            )  # Terminal 1 is main, so agents start at 2
            task = self._execute_agent_instance(config, idx, user_input, terminal_output)
            tasks.append(task)

        # Execute all tasks
        results = await asyncio.gather(*tasks, return_exceptions=True)

        # Process results
        parallel_results = []
        for result in results:
            if isinstance(result, ParallelResult):
                parallel_results.append(result)
            elif isinstance(result, Exception):
                self.logger.error(f"Parallel execution error: {result}")

        return parallel_results

    async def _execute_agent_instance(
        self,
        config: ParallelConfig,
        instance_number: int,
        user_input: str,
        terminal_output: Optional[Any] = None,
    ) -> ParallelResult:
        """Execute a single agent instance"""
        agent_id = config.id or f"P{instance_number}"

        try:
            # Get or create agent instance
            instance_key = (config.agent_name, instance_number)
            instance_agent = PARALLEL_AGENT_INSTANCES.get(instance_key)

            if not instance_agent:
                # Create new instance
                available_agents = get_available_agents()
                base_agent = available_agents.get(config.agent_name.lower())

                if not base_agent:
                    return ParallelResult(
                        config=config,
                        agent_name=config.agent_name,
                        instance_number=instance_number,
                        result=None,
                        error=f"Agent '{config.agent_name}' not found",
                    )

                agent_display_name = getattr(base_agent, "name", config.agent_name)
                custom_name = f"{agent_display_name} #{instance_number}"

                # Determine model
                model_to_use = config.model or os.getenv("CAI_MODEL", "alias1")

                # Create agent instance
                instance_agent = get_agent_by_name(
                    config.agent_name,
                    custom_name=custom_name,
                    model_override=model_to_use,
                    agent_id=agent_id,
                )

                PARALLEL_AGENT_INSTANCES[instance_key] = instance_agent

            # Update model if needed
            model_to_use = config.model or os.getenv("CAI_MODEL", "alias1")
            update_agent_models_recursively(instance_agent, model_to_use)

            # Determine input
            instance_input = config.prompt if config.prompt else user_input

            # Write to terminal if available
            if terminal_output:
                terminal_output.write(f"[cyan]Executing: {instance_input}[/cyan]")
                terminal_output.write("")

            # Run agent
            result = await Runner.run(instance_agent, instance_input)

            # Display result in terminal
            if terminal_output and result and hasattr(result, "final_output"):
                terminal_output.write(result.final_output)
                terminal_output.write("")

            # Save history
            if hasattr(instance_agent, "model") and hasattr(
                instance_agent.model, "message_history"
            ):
                PARALLEL_ISOLATION.replace_isolated_history(
                    agent_id, instance_agent.model.message_history
                )

            return ParallelResult(
                config=config,
                agent_name=config.agent_name,
                instance_number=instance_number,
                result=result,
                error=None,
            )

        except Exception as e:
            error_msg = f"Error in {config.agent_name} #{instance_number}: {str(e)}"
            self.logger.error(error_msg, exc_info=True)

            if terminal_output:
                terminal_output.write(f"[red]{error_msg}[/red]")

            return ParallelResult(
                config=config,
                agent_name=config.agent_name,
                instance_number=instance_number,
                result=None,
                error=error_msg,
            )

    def _get_current_history(self) -> List[Dict[str, Any]]:
        """Get current conversation history"""
        # Try to get from agent manager
        from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER

        # Get history from active agent
        active_agents = AGENT_MANAGER.get_active_agents()
        if active_agents:
            for agent_name in active_agents:
                hist = AGENT_MANAGER.get_message_history(agent_name)
                if hist:
                    return hist

        # Check all message histories
        for agent_name, hist in AGENT_MANAGER._message_history.items():
            if hist:
                return hist

        return []

    async def cancel_all_tasks(self) -> None:
        """Cancel all running tasks"""
        for task_id, task in self.running_tasks.items():
            if not task.done():
                task.cancel()

        # Wait for cancellation
        if self.running_tasks:
            await asyncio.gather(*self.running_tasks.values(), return_exceptions=True)

        self.running_tasks.clear()
