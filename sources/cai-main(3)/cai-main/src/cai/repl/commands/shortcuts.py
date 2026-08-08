"""REPL input shortcuts — bare ``?`` (CLI headless)."""

from cai.repl.commands.base import Command, console, register_command
from cai.repl.ui.repl_input_shortcuts import print_repl_input_shortcuts


class ShortcutsCommand(Command):
    """Show minimal key / prefix help; distinct from ``/?`` (alias of ``/help``)."""

    def __init__(self) -> None:
        super().__init__(
            name="?",
            description="Show REPL input shortcuts (CLI headless)",
        )

    def handle(self, args=None):
        if args:
            console.print("[yellow]Usage: ?[/yellow] — no arguments.")
            return False
        return self.handle_no_args()

    def handle_no_args(self) -> bool:
        print_repl_input_shortcuts(console)
        return True


register_command(ShortcutsCommand())
