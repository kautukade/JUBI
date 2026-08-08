"""CAI Terminal Theme System (embedded minimal theme)."""

from dataclasses import dataclass
from typing import Dict


@dataclass
class Theme:
    name: str
    variables: Dict[str, str]


class ThemeManager:
    def __init__(self):
        self._current = DEFAULT_THEME

    def set_theme(self, name: str) -> None:
        theme = THEMES.get(name)
        if theme:
            self._current = theme

    def get_theme_css(self) -> Dict[str, str]:
        return self._current.variables


DEFAULT_THEME = Theme(
    name="nature",
    variables={
        "background": "#001f1a",
        "text": "#e6ffe6",
        "accent": "#00ff9c",
        "panel_bg": "#01342c",
    },
)

THEMES: Dict[str, Theme] = {
    DEFAULT_THEME.name: DEFAULT_THEME,
}

__all__ = ["Theme", "ThemeManager", "THEMES"]
