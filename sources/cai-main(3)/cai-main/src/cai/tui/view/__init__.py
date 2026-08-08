"""
TUI View layer -- layout composition and CSS extracted from cai_terminal.py.

Part of the MVC extraction from the original 4,500+ LOC monolith.
"""

from cai.tui.view.main_view import (
    CAI_TERMINAL_CSS,
    compose_main_layout,
    register_cai_themes,
    get_help_basic_content,
    get_help_advanced_content,
    get_help_protips_content,
    update_tab_appearance,
)

__all__ = [
    "CAI_TERMINAL_CSS",
    "compose_main_layout",
    "register_cai_themes",
    "get_help_basic_content",
    "get_help_advanced_content",
    "get_help_protips_content",
    "update_tab_appearance",
]
