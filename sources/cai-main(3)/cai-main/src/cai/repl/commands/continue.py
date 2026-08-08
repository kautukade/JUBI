"""
Continue command implementation for enabling automatic continuation mode.
"""

import os
import asyncio
from cai.repl.commands.base import Command, register_command
from rich.console import Console

console = Console()

# Global variable to track continue mode state
_continue_mode_enabled = False


def get_continue_mode():
    """Get the current continue mode state."""
    global _continue_mode_enabled
    return _continue_mode_enabled


def set_continue_mode(enabled):
    """Set the continue mode state."""
    global _continue_mode_enabled
    _continue_mode_enabled = enabled


class ContinueCommand(Command):
    """Enable or disable automatic continuation mode."""
    
    def __init__(self):
        """Initialize the continue command."""
        super().__init__(
            name="continue",
            description="Enable continuation mode and continue current task",
            aliases=["/continue"]
        )
        
        # Add subcommands
        self.add_subcommand("on", "Enable continuation mode and continue task", self.handle_on)
        self.add_subcommand("off", "Disable automatic continuation mode", self.handle_off)
        self.add_subcommand("status", "Check current continuation mode status", self.handle_status)
    
    def handle_no_args(self):
        """
        Enable automatic continuation mode and immediately continue the current task.
        
        This both enables continuation mode AND triggers immediate continuation
        of the current conversation using AI-generated continuation prompts.
        """
        # Always enable continuation mode when /continue is called without args
        set_continue_mode(True)
        os.environ["CAI_CONTINUE_MODE"] = "true"
        
        console.print("[green]✓ Automatic continuation mode ENABLED[/green]")
        console.print("[dim]The agent will automatically continue when it stops.[/dim]")
        
        # Trigger immediate continuation
        self._trigger_immediate_continuation()
        
        return True
    
    def _trigger_immediate_continuation(self):
        """Generate and queue a continuation prompt to continue the current task."""
        try:
            # Try to get the current agent's message history
            # This is a bit tricky since we're in the command context
            # We'll need to check if there's an active agent with history
            from cai.continuation import generate_continuation_advice
            
            # Try to find the message history from the current context
            # This is hacky but necessary since commands don't have direct access to the agent
            import sys
            agent = None
            message_history = None
            
            # Look for the agent in the call stack's locals
            frame = sys._getframe()
            while frame:
                if 'agent' in frame.f_locals and hasattr(frame.f_locals['agent'], 'model'):
                    agent = frame.f_locals['agent']
                    if hasattr(agent.model, 'message_history'):
                        message_history = agent.model.message_history
                        break
                frame = frame.f_back
            
            if message_history:
                # Generate continuation advice
                console.print("[cyan]🤖 Generating continuation prompt...[/cyan]")
                continuation_prompt = asyncio.run(generate_continuation_advice(
                    agent_name=getattr(agent, "name", "Agent") if agent else "Agent",
                    message_history=message_history,
                    console=console
                ))
                
                # Queue the continuation prompt
                from cai.repl.commands.queue import add_to_queue
                add_to_queue(continuation_prompt)
                
                # Set auto-run queue flag
                os.environ["CAI_AUTO_RUN_QUEUE"] = "1"
                
                console.print("[cyan]✓ Continuation prompt queued. The agent will continue automatically.[/cyan]")
            else:
                # If no active conversation, just inform the user
                console.print("[yellow]No active conversation to continue. Continuation mode is now enabled for future tasks.[/yellow]")
                
        except Exception as e:
            # If anything goes wrong, just enable the mode without immediate continuation
            console.print(f"[yellow]Could not generate immediate continuation prompt: {str(e)}[/yellow]")
            console.print("[green]Continuation mode is enabled for future tasks.[/green]")
    
    def handle_on(self, args=None):
        """Enable automatic continuation mode and immediately continue."""
        set_continue_mode(True)
        os.environ["CAI_CONTINUE_MODE"] = "true"
        console.print("[green]✓ Automatic continuation mode ENABLED[/green]")
        
        # Also trigger immediate continuation
        self._trigger_immediate_continuation()
        
        return True
    
    def handle_off(self, args=None):
        """Disable automatic continuation mode."""
        set_continue_mode(False)
        os.environ["CAI_CONTINUE_MODE"] = "false"
        console.print("[yellow]✗ Automatic continuation mode DISABLED[/yellow]")
        return True
    
    def handle_status(self, args=None):
        """Check current continuation mode status."""
        current_state = get_continue_mode()
        if current_state:
            console.print("[green]Automatic continuation mode is ENABLED[/green]")
        else:
            console.print("[yellow]Automatic continuation mode is DISABLED[/yellow]")
        return True


# Register the command
register_command(ContinueCommand())