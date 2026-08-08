"""Shared Rich renderables for CAI CLI status lines (startup, model/tool waits, retries).

Badge: ``CAI`` / ``Ctrl+C`` on light grey pill, bold very dark text (no side accent blocks).
"""

from __future__ import annotations

import shutil
from typing import TYPE_CHECKING

from rich.text import Text

from cai.util.cli_palette import GREY_TEXT

if TYPE_CHECKING:
    from rich.console import RenderableType

# Light grey pill + bold near-black label (contrast on pale badge)
CAI_BADGE_BG = "#b8b8c4"
CAI_BADGE_FG = "#0a0a0c"

_PIPE_FRAMES = ("|", "/", "—", "\\")
# Same frames as ``rich.status.Status(..., spinner="dots")`` (startup / license check).
_BRAILLE_DOTS_FRAMES = ("⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏")
STARTUP_HINT_SPINNER_HZ = 8


def terminal_columns() -> int:
    try:
        return max(40, shutil.get_terminal_size((80, 24)).columns)
    except Exception:
        return 80


def _cai_brand_badge() -> Text:
    return Text("CAI", style=f"bold {CAI_BADGE_FG} on {CAI_BADGE_BG}")


def cai_brand_badge_text() -> Text:
    """Public alias: grey pill ``CAI`` (same style as startup / model-tool wait hints)."""
    return _cai_brand_badge()


def build_cai_markup_line(markup: str) -> Text:
    """Grey CAI pill + Rich-markup body. Do not put ``[CAI]`` inside *markup*."""
    line = Text()
    line.append_text(_cai_brand_badge())
    line.append(" ")
    try:
        line.append(Text.from_markup(markup))
    except Exception:
        line.append(markup, style=GREY_TEXT)
    return line


def _ctrl_c_badge() -> Text:
    return Text("Ctrl+C", style=f"bold {CAI_BADGE_FG} on {CAI_BADGE_BG}")


def _truncate_body(s: str, max_len: int) -> str:
    s = s.replace("\n", " ").strip()
    if max_len <= 8 or len(s) <= max_len:
        return s
    return s[: max_len - 1].rstrip() + "…"


def braille_dots_frame(tick: int) -> str:
    """One frame of the startup ``dots`` spinner (braille cycle)."""
    return _BRAILLE_DOTS_FRAMES[tick % len(_BRAILLE_DOTS_FRAMES)]


def build_startup_hint_renderable(message: str) -> RenderableType:
    """Startup line: badge + static `` | `` + dim italic message (no interrupt suffix)."""
    msg = _truncate_body(message, max(20, terminal_columns() - 24))
    line = Text()
    line.append_text(_cai_brand_badge())
    line.append(" | ", style="dim")
    line.append(msg, style="italic dim")
    return line


def build_compact_live_wait_hint_row(body: str, *, frame_tick: int) -> Text:
    """Wait row inside the compact Live block — matches startup chrome + braille spinner.

    ``StartupHints`` paints the spinner via ``Status`` on stderr; here we advance the
    same braille frames manually so the compact ``Live`` can own the only cursor.
    """
    msg = _truncate_body(body, max(20, terminal_columns() - 28))
    line = Text()
    line.append(braille_dots_frame(frame_tick), style="dim")
    line.append(" ", style="")
    line.append_text(_cai_brand_badge())
    line.append(" | ", style="dim")
    line.append(msg, style="italic dim")
    return line


def build_wait_hint_renderable(
    body: str,
    pipe_char: str,
    *,
    include_suffix: bool,
    reserve_for_suffix: int = 36,
) -> RenderableType:
    """Wait line: badge + rotating pipe + dim italic body; optional bold suffix + Ctrl+C badge."""
    cols = terminal_columns()
    budget = cols - 18 - (reserve_for_suffix if include_suffix else 0)
    body = _truncate_body(body, max(24, budget))
    line = Text()
    line.append_text(_cai_brand_badge())
    line.append(f" {pipe_char} ", style="dim")
    line.append(body, style="italic dim")
    if include_suffix:
        line.append("  —  ", style="bold")
        line.append_text(_ctrl_c_badge())
        line.append(" to interrupt", style="bold")
    return line


def pipe_frame(tick: int) -> str:
    return _PIPE_FRAMES[tick % 4]
