"""
Base module for CAI REPL commands.
This module provides the base structure for all commands in the CAI REPL.
"""

from typing import List, Optional, Dict, Any, Callable, Tuple
from rich.console import Console  # pylint: disable=import-error
from difflib import get_close_matches

console = Console()


class Command:
    """Base class for all commands."""

    def __init__(self, name: str, description: str, aliases: List[str] = None):
        """Initialize a command.

        Args:
            name: The name of the command (e.g. "/memory")
            description: A short description of the command
            aliases: Optional list of command aliases
        """
        self.name = name
        self.description = description
        self.aliases = aliases or []
        self.subcommands: Dict[str, Dict[str, Any]] = {}

    def add_subcommand(self, name: str, description: str, handler: Callable):
        """Add a subcommand to this command.

        Args:
            name: The name of the subcommand (e.g. "list")
            description: A short description of the subcommand
            handler: The function to call when the subcommand is invoked
        """
        self.subcommands[name] = {"description": description, "handler": handler}

    def get_subcommands(self) -> List[str]:
        """Get a list of all subcommand names.

        Returns:
            A list of subcommand names
        """
        return list(self.subcommands.keys())

    def get_subcommand_description(self, subcommand: str) -> str:
        """Get the description of a subcommand.

        Args:
            subcommand: The name of the subcommand

        Returns:
            The description of the subcommand
        """
        return self.subcommands.get(subcommand, {}).get("description", "")

    def handle(self, args: Optional[List[str]] = None) -> bool:
        """Handle the command.

        Args:
            args: Optional list of command arguments

        Returns:
            True if the command was handled successfully, False otherwise
        """
        if not args:
            return self.handle_no_args()

        subcommand = args[0]
        if subcommand in self.subcommands:
            handler = self.subcommands[subcommand]["handler"]
            return handler(args[1:] if len(args) > 1 else None)

        return self.handle_unknown_subcommand(subcommand)

    def handle_no_args(self) -> bool:
        """Handle the command when no arguments are provided.

        Returns:
            True if the command was handled successfully, False otherwise
        """
        subcommands = ", ".join(self.get_subcommands())
        console.print(f"[yellow]{self.name} command requires a subcommand: {subcommands}[/yellow]")
        return False

    def handle_unknown_subcommand(self, subcommand: str) -> bool:
        """Handle an unknown subcommand.

        Args:
            subcommand: The unknown subcommand

        Returns:
            True if the command was handled successfully, False otherwise
        """
        console.print(f"[red]Unknown {self.name} subcommand: {subcommand}[/red]")
        return False


# Registry for all commands
COMMANDS: Dict[str, Command] = {}
COMMAND_ALIASES: Dict[str, str] = {}


def register_command(command: Command) -> None:
    """Register a command in the global registry.

    Args:
        command: The command to register
    """
    COMMANDS[command.name] = command

    # Register aliases
    for alias in command.aliases:
        COMMAND_ALIASES[alias] = command.name


def get_command(name: str) -> Optional[Command]:
    """Get a command by name or alias.

    Args:
        name: The name or alias of the command

    Returns:
        The command if found, None otherwise
    """
    # Check if it's an alias
    name = COMMAND_ALIASES.get(name, name)

    return COMMANDS.get(name)


def find_closest_command(command: str) -> Optional[str]:
    """Find the closest matching command for a mistyped command.
    
    Args:
        command: The mistyped command
        
    Returns:
        The closest matching command or None
    """
    # Remove leading slash if present
    command_without_slash = command.lstrip("/")
    
    # Get all commands and aliases
    all_commands = [cmd.lstrip("/") for cmd in COMMANDS.keys()]
    all_commands.extend(list(COMMAND_ALIASES.keys()))
    
    # Find closest match
    matches = get_close_matches(command_without_slash, all_commands, n=1, cutoff=0.6)
    
    if matches:
        match = matches[0]
        # Check if it's an alias
        if match in COMMAND_ALIASES:
            # Get the actual command and ensure single slash
            actual_cmd = COMMAND_ALIASES[match]
            return "/" + actual_cmd.lstrip("/")
        else:
            # Ensure single slash
            return "/" + match.lstrip("/")

    return None


def handle_command(command: str, args: Optional[List[str]] = None) -> bool:
    """Handle a command.

    Args:
        command: The command name or alias
        args: Optional list of command arguments

    Returns:
        True if the command was handled successfully, False otherwise
    """
    cmd = get_command(command)
    if cmd:
        return cmd.handle(args)

    return False


def handle_command_with_autocorrect(command: str, args: Optional[List[str]] = None, auto_correct: bool = True) -> Tuple[bool, Optional[str]]:
    """Handle a command with autocorrection support.

    Args:
        command: The command name or alias
        args: Optional list of command arguments
        auto_correct: Whether to auto-correct and execute mistyped commands

    Returns:
        Tuple of (command_handled, suggested_command)
    """
    cmd = get_command(command)
    if cmd:
        return (cmd.handle(args), None)
    
    # Command not found, try to find closest match
    suggested = find_closest_command(command)
    
    if suggested and auto_correct:
        cmd = get_command(suggested)
        if cmd:
            return (cmd.handle(args), suggested)
    
    return (False, suggested)
