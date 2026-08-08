"""
Merge command for CAI CLI - alias for /parallel merge.

Provides a shortcut to merge agent message histories without
typing the full /parallel merge command.
"""

from typing import List, Optional

from rich.console import Console

from cai.repl.commands.base import Command, register_command
from cai.repl.commands.parallel import ParallelCommand
from cai.util.hint_renderables import build_cai_markup_line

console = Console()


class MergeCommand(Command):
    """Command to merge agent message histories - alias for /parallel merge."""

    def __init__(self):
        """Initialize the merge command."""
        super().__init__(
            name="/merge",
            description="Merge all agents' message histories by default (alias for /parallel merge all)",
            aliases=["/mrg"],
        )
        # Create a ParallelCommand instance to delegate to
        self._parallel_cmd = ParallelCommand()

    def handle(self, args: Optional[List[str]] = None) -> bool:
        """Handle the merge command by delegating to /parallel merge.

        Args:
            args: Arguments to pass to the merge subcommand

        Returns:
            True if successful
        """
        if not args:
            # No arguments - merge all by default
            return self.handle_no_args()

        # Delegate to ParallelCommand's handle_merge method
        return self._parallel_cmd.handle_merge(args)

    def handle_no_args(self) -> bool:
        """Handle command with no arguments - merge all agents (clean output)."""
        console.print(build_cai_markup_line("[#9aa0a6]Merging all agents by default...[/]"))
        console.print()
        return self._parallel_cmd.handle_merge(["all"])


# Register the command
register_command(MergeCommand())
