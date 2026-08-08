"""REPL /exit command."""

from typing import List, Optional

from cai.repl.commands.base import Command, register_command

REPL_EXIT_REQUESTED = False


class ExitCommand(Command):
    def __init__(self):
        super().__init__(name="/exit", description="Exit the CAI REPL", aliases=["/q", "/quit"])

    def handle(self, args: Optional[List[str]] = None) -> bool:
        if args:
            return super().handle(args)
        global REPL_EXIT_REQUESTED
        REPL_EXIT_REQUESTED = True
        return True


register_command(ExitCommand())
