"""Agent manager component for CAI TUI"""

import asyncio
from typing import Optional
from textual.widgets import RichLog

from cai.agents import get_agent_by_name
from cai.sdk.agents import Runner


class AgentManager:
    """Manages agent interactions and lifecycle"""

    def __init__(self, output: RichLog):
        self.output = output
        self.agent = None
        # Default to redteam_agent in TUI context
        self.current_agent_name = "redteam_agent"
        self._initializing = False

    async def initialize_agent(self, agent_name: Optional[str] = None) -> None:
        """Initialize the default agent"""
        if self._initializing:
            return

        self._initializing = True
        await asyncio.sleep(0.1)

        try:
            if agent_name:
                self.current_agent_name = agent_name
            from cai.sdk.agents.simple_agent_manager import DEFAULT_SESSION_AGENT_ID

            self.agent = get_agent_by_name(self.current_agent_name, agent_id=DEFAULT_SESSION_AGENT_ID)
        except Exception as e:
            if self.output:
                self.output.write(f"[red]✗ Failed to initialize agent: {e}[/red]")
                self.output.write("")
        finally:
            self._initializing = False

    def switch_agent(self, agent_name: str) -> bool:
        """Switch to a different agent"""
        try:
            from cai.sdk.agents.simple_agent_manager import DEFAULT_SESSION_AGENT_ID

            self.agent = get_agent_by_name(agent_name, agent_id=DEFAULT_SESSION_AGENT_ID)
            self.current_agent_name = agent_name
            return True
        except Exception:
            return False

    async def chat_with_agent(self, message: str) -> None:
        """Process a message with the current agent"""
        if not self.agent:
            self.output.write("[red]No agent available[/red]")
            self.output.write("")
            return

        try:
            # Run agent without any status messages
            result = await Runner.run(self.agent, message)

            # Don't output anything - let the model streaming handle the output

            self.output.write("")

        except Exception as e:
            self.output.write(f"[red]Error: {e}[/red]")
            self.output.write("")
