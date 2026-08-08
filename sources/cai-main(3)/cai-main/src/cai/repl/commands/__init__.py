"""
Commands module for CAI REPL.
This module exports all commands available
in the CAI REPL.
"""

from typing import (
    Dict,
    List,
)
import importlib

# Define command modules for lazy loading
COMMAND_MODULES = [
    'agent',
    'api',
    'auth',
    'compact',
    'config',
    'continue',
    'context',
    'cost',
    'ctr',  # Heavy module - will be loaded on demand
    'env',
    'exit',
    'flush',
    'graph',
    'help',
    'history',
    'load',
    'mcp',
    'memory',
    'merge',
    'meta_debug',
    'model',
    'parallel',
    'replay',
    'queue',
    'quickstart',
    'resume',
    'save',
    'settings',
    'shell',
    'shortcuts',
    'temperature',
    'virtualization',
    'workspace',
]

# Track which modules have been loaded
_loaded_modules = set()

def _ensure_command_loaded(module_name: str):
    """Lazily load a command module if not already loaded."""
    if module_name not in _loaded_modules:
        try:
            importlib.import_module(f'cai.repl.commands.{module_name}')
            _loaded_modules.add(module_name)
        except ImportError:
            pass  # Module doesn't exist or has errors

def _ensure_all_commands_loaded():
    """Load all command modules (used for help, completions, etc.)."""
    for module in COMMAND_MODULES:
        _ensure_command_loaded(module)

# Import base command structure
from cai.repl.commands.base import (
    COMMAND_ALIASES,
    COMMANDS,
    Command,
    get_command as _base_get_command,
    handle_command as _base_handle_command,
    handle_command_with_autocorrect as _base_handle_command_with_autocorrect,
    find_closest_command as _base_find_closest_command,
    register_command,
)

# Lazy loading wrappers
def get_command(name: str):
    """Get a command by name with lazy loading support."""
    # First try to get the command without loading everything
    cmd = _base_get_command(name)
    if cmd:
        return cmd

    # Command not found, try loading the specific module
    # Check if name matches any known command module
    if name == "?":
        _ensure_command_loaded("shortcuts")
        return _base_get_command(name)

    name_clean = COMMAND_ALIASES.get(name, name).lstrip('/')
    if name_clean in COMMAND_MODULES:
        _ensure_command_loaded(name_clean)
        return _base_get_command(name)

    # Still not found, load all commands (for help/completions)
    _ensure_all_commands_loaded()
    return _base_get_command(name)

def handle_command(command: str, args=None):
    """Handle a command with lazy loading support."""
    # Ensure the specific command is loaded
    cmd_name = command.lstrip('/')
    if cmd_name in COMMAND_MODULES:
        _ensure_command_loaded(cmd_name)
    elif command == "?":
        _ensure_command_loaded("shortcuts")
    return _base_handle_command(command, args)

def handle_command_with_autocorrect(command: str, args=None, auto_correct=True):
    """Handle a command with autocorrect and lazy loading support."""
    # Try to load the specific command first
    cmd_name = command.lstrip('/')
    if cmd_name in COMMAND_MODULES:
        _ensure_command_loaded(cmd_name)
    elif command == "?":
        _ensure_command_loaded("shortcuts")
    elif command.startswith("/"):
        # Aliases like /virt (for /virtualization) may not map to module names.
        # Load command registry once before first dispatch to avoid duplicate
        # execution/error output from a failed first lookup + retry.
        _ensure_all_commands_loaded()

    # First attempt with currently loaded commands.
    result = _base_handle_command_with_autocorrect(command, args, auto_correct)

    # Only retry after loading all commands when the command is truly unknown.
    # If the command already exists but returned False (validation/usage error),
    # re-running it would duplicate error output.
    if command == "?":
        normalized = "?"
        known_now = _base_get_command("?") is not None
    else:
        normalized = command if command.startswith("/") else f"/{command}"
        known_now = _base_get_command(normalized) is not None

    if result[0] is False and result[1] is None and not known_now:
        _ensure_all_commands_loaded()
        result = _base_handle_command_with_autocorrect(command, args, auto_correct)
    return result

def find_closest_command(command: str):
    """Find closest command with lazy loading support."""
    # Need all commands loaded for fuzzy matching
    _ensure_all_commands_loaded()
    return _base_find_closest_command(command)

# Defer completer import for faster startup
FuzzyCommandCompleter = None

def get_fuzzy_completer():
    """Get the fuzzy command completer with lazy loading."""
    global FuzzyCommandCompleter
    if FuzzyCommandCompleter is None:
        from cai.repl.commands.completer import FuzzyCommandCompleter as _FuzzyCompleter
        FuzzyCommandCompleter = _FuzzyCompleter
    return FuzzyCommandCompleter

# Define helper functions


def get_command_descriptions() -> Dict[str, str]:
    """Get descriptions for all commands.

    Returns:
        A dictionary mapping command names to descriptions
    """
    _ensure_all_commands_loaded()  # Load all commands for complete list
    return {cmd.name: cmd.description for cmd in COMMANDS.values()}


def get_subcommand_descriptions() -> Dict[str, str]:
    """Get descriptions for all subcommands.

    Returns:
        A dictionary mapping command paths to descriptions
    """
    _ensure_all_commands_loaded()  # Load all commands for complete list
    descriptions = {}
    for cmd in COMMANDS.values():
        for subcmd in cmd.get_subcommands():
            key = f"{cmd.name} {subcmd}"
            descriptions[key] = cmd.get_subcommand_description(subcmd)
    return descriptions


def get_all_commands() -> Dict[str, List[str]]:
    """Get all commands and their subcommands.

    Returns:
        A dictionary mapping command names to lists of subcommand names
    """
    _ensure_all_commands_loaded()  # Load all commands for complete list
    return {cmd.name: cmd.get_subcommands() for cmd in COMMANDS.values()}


# Import the command completer after defining the helper functions

# Export command registry
__all__ = [
    "Command",
    "COMMANDS",
    "COMMAND_ALIASES",
    "register_command",
    "get_command",
    "handle_command",
    "handle_command_with_autocorrect",
    "find_closest_command",
    "get_command_descriptions",
    "get_subcommand_descriptions",
    "get_all_commands",
    "get_fuzzy_completer",
    "FuzzyCommandCompleter",  # Will be None until loaded
]
