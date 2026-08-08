"""
Environment command for CAI REPL.
This module provides commands for displaying and configuring environment variables.
"""

from typing import List, Optional

from cai.repl.commands.base import Command, register_command
from cai.repl.commands.env_catalog import (
    handle_env_catalog_default,
    handle_env_catalog_get,
    handle_env_catalog_list,
    handle_env_catalog_set,
    print_bare_env_session_view,
)


class EnvCommand(Command):
    """Command for displaying and configuring environment variables (REPL session)."""

    def __init__(self):
        """Initialize the env command."""
        super().__init__(
            name="/env",
            description="Display and configure environment variables",
            aliases=["/e"],
        )
        self.add_subcommand(
            "list",
            "List all catalog environment variables and their values",
            handle_env_catalog_list,
        )
        self.add_subcommand(
            "get",
            "Get a catalog variable by number or name",
            handle_env_catalog_get,
        )
        self.add_subcommand(
            "set",
            "Set a catalog variable: /env set <#|NAME> <value...>",
            handle_env_catalog_set,
        )
        self.add_subcommand(
            "default",
            "Restore all catalog variables to registered defaults",
            handle_env_catalog_default,
        )

    def handle(self, args: Optional[List[str]] = None) -> bool:
        """Route: no args -> session CAI_/CTF_ in os.environ; else subcommands only."""
        if not args:
            return print_bare_env_session_view()

        first_arg = args[0]
        if first_arg in self.subcommands:
            handler = self.subcommands[first_arg]["handler"]
            return handler(args[1:] if len(args) > 1 else None)

        return self.handle_unknown_subcommand(first_arg)

    def handle_no_args(self) -> bool:
        """Satisfy base contract if invoked without args via default handler."""
        return print_bare_env_session_view()


register_command(EnvCommand())
