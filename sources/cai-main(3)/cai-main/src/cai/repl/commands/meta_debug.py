"""
Meta Agent debug command for TUI
"""

import os
from typing import Optional, List
from rich.table import Table
from rich.console import Console
from rich.panel import Panel
from rich.json import JSON

from cai.repl.commands.base import Command, register_command

console = Console()


class MetaDebugCommand(Command):
    """Show Meta Agent debug information"""

    def __init__(self):
        """Initialize the meta debug command."""
        super().__init__(
            name="/metadebug",
            description="Show Meta Agent debug information",
            aliases=["/md"],
        )

    def handle(self, args: Optional[List[str]] = None) -> bool:
        """Handle the meta debug command"""
        
        # Check if Meta Agent is enabled
        if os.getenv("CAI_META_AGENT", "false").lower() != "true":
            console.print("[yellow]Meta Agent is not enabled. Set CAI_META_AGENT=True to enable.[/yellow]")
            return True
        
        # Lazy import to avoid circular dependency
        try:
            from cai.tui.meta_agent_controller import get_meta_agent_controller
        except ImportError as e:
            console.print(f"[red]Error importing Meta Agent controller: {e}[/red]")
            return True
        
        # Get controller
        controller = get_meta_agent_controller()
        if not controller:
            console.print("[red]Meta Agent controller not initialized[/red]")
            return True
        
        # Get debug info
        debug_info = controller.get_debug_info()
        
        # Create debug panel
        table = Table(title="Meta Agent Debug Info", show_header=True)
        table.add_column("Property", style="cyan")
        table.add_column("Value", style="white")
        
        # Basic info
        table.add_row("Enabled", str(debug_info.get("enabled", False)))
        table.add_row("Model", debug_info.get("model", "Unknown"))
        table.add_row("Workers Started", str(debug_info.get("workers_started", False)))
        table.add_row("Currently Processing", str(debug_info.get("processing", False)))
        table.add_row("Command Queue Size", str(debug_info.get("command_queue_size", 0)))
        
        console.print(table)
        
        # LiteLLM debug info
        litellm_debug = debug_info.get("last_litellm_debug", {})
        if litellm_debug:
            console.print("\n[bold]Last LiteLLM Call:[/bold]")
            
            litellm_table = Table(show_header=False)
            litellm_table.add_column("Property", style="dim cyan")
            litellm_table.add_column("Value", style="white")
            
            for key, value in litellm_debug.items():
                litellm_table.add_row(key.replace("_", " ").title(), str(value))
            
            console.print(litellm_table)
        
        # Show environment
        console.print("\n[bold]Environment:[/bold]")
        env_table = Table(show_header=False)
        env_table.add_column("Variable", style="dim cyan")
        env_table.add_column("Value", style="white")
        
        env_vars = ["CAI_META_AGENT", "CAI_META_MODEL", "CAI_MODEL", "OPENAI_API_KEY", "ANTHROPIC_API_KEY"]
        for var in env_vars:
            value = os.getenv(var)
            if var.endswith("_KEY") and value:
                # Mask API keys
                value = value[:8] + "..." + value[-4:] if len(value) > 12 else "***"
            env_table.add_row(var, value or "Not set")
        
        console.print(env_table)
        
        # Quick tips
        console.print("\n[dim]Tips:[/dim]")
        console.print("[dim]- Meta Agent intercepts ALL user prompts when enabled[/dim]")
        console.print("[dim]- Debug messages appear inline in cyan[/dim]")
        console.print("[dim]- Check API keys if seeing connection errors[/dim]")
        
        return True


# Register command
register_command(MetaDebugCommand())