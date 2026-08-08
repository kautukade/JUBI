"""
SessionState -- session-level state management extracted from CAITerminal.

Handles command history persistence, session timing/metrics, and the
session summary displayed at exit.  Operates on plain data and filesystem
I/O so it can be tested without Textual.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple


@dataclass
class SessionState:
    """Persistent session data independent of the Textual App lifecycle."""

    history_file: Optional[Path] = None
    start_time: float = field(default_factory=time.time)
    _summary_displayed: bool = False

    # -- history ---------------------------------------------------------------

    def init_history(self, history_dir: Optional[Path] = None) -> None:
        """Ensure the history directory and file exist."""
        if history_dir is None:
            history_dir = Path.home() / ".cai"
        history_dir.mkdir(exist_ok=True, parents=True)
        self.history_file = history_dir / "history.txt"

    def load_history(self, max_entries: int = 100) -> List[str]:
        """Load unique recent commands from the history file.

        Returns at most *max_entries* unique commands in most-recent-first
        order (suitable for arrow-key navigation).
        """
        if self.history_file is None or not self.history_file.exists():
            return []

        try:
            with open(self.history_file, "r", encoding="utf-8") as fh:
                raw_lines = fh.readlines()
        except Exception:
            return []

        history_commands: List[str] = []
        for line in raw_lines:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("+"):
                line = line[1:]
            if line.strip():
                history_commands.append(line)

        # De-duplicate, keeping most recent first.
        seen: set[str] = set()
        unique: List[str] = []
        for cmd in reversed(history_commands):
            if cmd not in seen:
                seen.add(cmd)
                unique.append(cmd)
                if len(unique) >= max_entries:
                    break
        return unique

    def save_command(self, command: str) -> None:
        """Append a single command to the history file."""
        if self.history_file and command.strip():
            try:
                with open(self.history_file, "a", encoding="utf-8") as fh:
                    fh.write(f"{command}\n")
            except Exception:
                pass

    # -- session timing --------------------------------------------------------

    def elapsed_seconds(self) -> float:
        """Seconds elapsed since session start."""
        return max(time.time() - self.start_time, 0.0)

    @staticmethod
    def compute_session_time(
        active: Optional[float] = None,
        idle: Optional[float] = None,
        fallback_elapsed: float = 0.0,
    ) -> float:
        """Compute total session time with fallbacks."""
        if fallback_elapsed > 0:
            return fallback_elapsed
        total = (active or 0.0) + (idle or 0.0)
        return total if total > 0 else 0.0

    @staticmethod
    def ensure_time_breakdown(
        session_time: float,
        active: Optional[float],
        idle: Optional[float],
    ) -> Tuple[float, float]:
        """Ensure active + idle sums to *session_time*."""
        session_time = max(session_time, 0.0)
        if active is None and idle is None:
            active = session_time * 0.1
            idle = max(session_time - active, 0.0)
        elif active is None:
            idle_val = max(idle or 0.0, 0.0)
            active = max(session_time - idle_val, 0.0)
            idle = idle_val
        elif idle is None:
            active_val = max(active or 0.0, 0.0)
            idle = max(session_time - active_val, 0.0)
            active = active_val
        return max(active or 0.0, 0.0), max(idle or 0.0, 0.0)

    @staticmethod
    def format_time(seconds: float) -> str:
        """HH:MM:SS string from seconds."""
        mins, secs = divmod(int(seconds), 60)
        hours, mins = divmod(mins, 60)
        return f"{hours:02d}:{mins:02d}:{secs:02d}"

    def mark_summary_displayed(self) -> bool:
        """Mark the summary as displayed; return False if already shown."""
        if self._summary_displayed:
            return False
        self._summary_displayed = True
        return True
