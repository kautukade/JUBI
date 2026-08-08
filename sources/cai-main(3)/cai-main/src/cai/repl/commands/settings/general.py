"""Env helpers used by ``/settings``: ``.env`` read/write, CLI/TUI detection, ``questionary`` style."""

from __future__ import annotations

import os
from pathlib import Path
from typing import Dict, List, Optional

from questionary import Style
from rich.console import Console

# Shared console instance
console = Console()

# Custom style for questionary (Layout 1: active = black on CAI green; question bold white)
custom_style = Style([
    ('qmark', 'fg:#00ff9d bold'),
    ('question', 'bold white'),
    ('answer', 'fg:#000000 bg:#00ff9d bold'),
    ('pointer', 'fg:#000000 bg:#00ff9d bold'),
    ('highlighted', 'fg:#000000 bg:#00ff9d bold'),
    ('selected', 'fg:#000000 bg:#00ff9d'),
    ('separator', 'fg:#6c6c6c'),
    ('instruction', 'fg:#858585'),
    ('text', ''),
    ('disabled', 'fg:#858585 italic'),
])

# ── Variables that only apply to CLI mode (not shown in TUI) ─────────────
CLI_ONLY_VARIABLES = {
    'CAI_API_HOST', 'CAI_API_PORT', 'CAI_API_CORS', 'CAI_API_KEY_HEADER',
    'CAI_API_LOG_AUTH', 'CAI_API_LOG_REQUESTS', 'CAI_API_LOG_LEVEL',
    'CAI_API_RELOAD', 'CAI_API_WORKERS',
}

# ── Variables that only apply to TUI mode (not shown in CLI) ─────────────
TUI_ONLY_VARIABLES = {
    'CAI_TUI_MODE', 'CAI_TUI_STARTUP_YAML', 'CAI_TUI_SHARED_PROMPT',
    'CAI_TUI_MAX_LINES', 'CAI_TUI_MAX_RERENDERS_PER_SEC',
}


# ═══════════════════════════════════════════════════════════════════════════
# TUI / CLI Mode Detection
# ═══════════════════════════════════════════════════════════════════════════

def is_tui_mode() -> bool:
    """Check if we're running in TUI mode."""
    return os.getenv("CAI_TUI_MODE", "").lower() == "true"


def get_current_terminal_id() -> Optional[str]:
    """Get the current terminal ID if in TUI mode."""
    if is_tui_mode():
        return os.getenv("CAI_ACTIVE_COMMAND_TERMINAL_ID")
    return None


# ═══════════════════════════════════════════════════════════════════════════
# Env-file I/O
# ═══════════════════════════════════════════════════════════════════════════

def get_env_file_path() -> Path:
    """Get the path to the .env file in the current directory."""
    return Path.cwd() / '.env'


def read_env_file() -> Dict[str, str]:
    """Read the current .env file and return its contents as a dictionary."""
    env_path = get_env_file_path()
    env_dict: Dict[str, str] = {}

    if env_path.exists():
        with open(env_path, 'r', encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith('#') and '=' in line:
                    key, value = line.split('=', 1)
                    value = value.strip().strip('"').strip("'")
                    env_dict[key.strip()] = value

    return env_dict


def write_env_file(env_dict: Dict[str, str]) -> bool:
    """Write environment variables to the .env file.

    Preserves comments and structure of the existing file.
    """
    try:
        env_path = get_env_file_path()

        existing_lines: list[str] = []
        if env_path.exists():
            with open(env_path, 'r', encoding='utf-8') as f:
                existing_lines = f.readlines()

        updated_vars: set[str] = set()
        new_lines: list[str] = []

        for line in existing_lines:
            stripped = line.strip()
            if not stripped or stripped.startswith('#'):
                new_lines.append(line)
                continue

            if '=' in stripped:
                key = stripped.split('=', 1)[0].strip()
                if key in env_dict:
                    new_lines.append(f'{key}={env_dict[key]}\n')
                    updated_vars.add(key)
                else:
                    new_lines.append(line)
            else:
                new_lines.append(line)

        for key, value in env_dict.items():
            if key not in updated_vars:
                new_lines.append(f'{key}={value}\n')

        with open(env_path, 'w', encoding='utf-8') as f:
            f.writelines(new_lines)

        return True
    except Exception as e:
        console.print(f"[red]Error writing to .env file: {e}[/red]")
        return False


def update_env_file(var_name: str, value: str) -> bool:
    """Update a single variable in the .env file."""
    env_dict = read_env_file()
    env_dict[var_name] = value
    return write_env_file(env_dict)


def delete_env_variable(var_name: str) -> bool:
    """Delete a variable from the .env file."""
    try:
        env_path = get_env_file_path()

        if not env_path.exists():
            return False

        with open(env_path, 'r', encoding='utf-8') as f:
            lines = f.readlines()

        new_lines: list[str] = []
        for line in lines:
            stripped = line.strip()
            if stripped and not stripped.startswith('#') and '=' in stripped:
                key = stripped.split('=', 1)[0].strip()
                if key == var_name:
                    continue
            new_lines.append(line)

        with open(env_path, 'w', encoding='utf-8') as f:
            f.writelines(new_lines)

        if var_name in os.environ:
            del os.environ[var_name]

        return True
    except Exception as e:
        console.print(f"[red]Error deleting variable: {e}[/red]")
        return False


# ═══════════════════════════════════════════════════════════════════════════
# Variable inspection helpers
# ═══════════════════════════════════════════════════════════════════════════

def get_current_value(var_name: str, default: Optional[str]) -> str:
    """Get current value of a variable from environment or .env file."""
    env_value = os.environ.get(var_name)
    if env_value is not None:
        return env_value

    env_file = read_env_file()
    if var_name in env_file:
        return env_file[var_name]

    return default if default else ''


def is_boolean_variable(var_name: str, description: str) -> bool:
    """Determine if a variable is boolean based on its name and description."""
    boolean_keywords = ['enable', 'disable', 'boolean', 'true', 'false']
    desc_lower = description.lower()
    name_lower = var_name.lower()
    return any(kw in desc_lower or kw in name_lower for kw in boolean_keywords)


def filter_variables_for_mode(variables: List[str]) -> List[str]:
    """Filter variables based on current mode (TUI vs CLI)."""
    if is_tui_mode():
        return [v for v in variables if v not in CLI_ONLY_VARIABLES]
    else:
        return [v for v in variables if v not in TUI_ONLY_VARIABLES]
