"""Set terminal window/tab title (cross-platform best effort).

Uses xterm-style OSC sequences on TTYs (Linux, macOS, WSL + Windows Terminal, most VTE
terminals). On native Windows console, also tries SetConsoleTitleW when available.

Captures the previous title once before applying CAI branding and restores it on exit
(atexit, explicit restore on Ctrl+C, and after TUI).
"""

from __future__ import annotations

import atexit
import os
import re
import sys

# Branding in tab/window title (not cwd). « » and ® are Unicode; avoid OSC control chars.
CAI_DEFAULT_TERMINAL_WINDOW_TITLE = (
    "\u00abCAI\u00bb CyberSecurity AI framework supported by Alias Robotics S.L. \u00ae"
)

_snapshot_before_brand: str | None = None
_snapshot_taken: bool = False
_atexit_registered: bool = False


def _title_disabled() -> bool:
    return os.environ.get("CAI_DISABLE_TERMINAL_TITLE", "").lower() in (
        "1",
        "true",
        "yes",
        "on",
    )


def _sanitize_title(title: str) -> str:
    # BEL ends OSC; newlines break the sequence
    t = title.replace("\x07", " ").replace("\n", " ").replace("\r", " ").strip()
    if len(t) > 240:
        t = t[:237] + "..."
    return t


def _write_osc0(safe: str, stream) -> None:
    if stream is None or not getattr(stream, "isatty", lambda: False)():
        return
    try:
        stream.write(f"\033]0;{safe}\007")
        stream.flush()
    except (OSError, BrokenPipeError, ValueError, TypeError):
        pass


def _read_title_windows() -> str | None:
    try:
        import ctypes

        buf = ctypes.create_unicode_buffer(8192)
        n = ctypes.windll.kernel32.GetConsoleTitleW(buf, 8192)
        if n <= 0:
            return None
        t = buf.value.strip()
        return t if t else None
    except Exception:
        return None


def _parse_title_report_response(raw: bytes) -> str | None:
    if not raw:
        return None
    s = raw.decode("utf-8", errors="replace")
    for pattern in (
        r"\x1b\]l([^\x07\x1b]+)",  # xterm DECRQTSR-style report (icon label)
        r"\x1b\]L([^\x07\x1b]+)",
        r"\x1b\]0;([^\x07]+)\x07",
    ):
        m = re.search(pattern, s)
        if m:
            t = m.group(1).strip().rstrip("\033\\")
            if t:
                return _sanitize_title(t)
    return None


def _read_title_posix() -> str | None:
    """Best-effort: CSI 21 t report (xterm-compatible). Fails quietly on unsupported TTYs."""
    if os.name == "nt":
        return None
    try:
        import select
        import termios
        import tty as tty_mod
    except ImportError:
        return None
    if not sys.stdin.isatty():
        return None
    fd = None
    old = None
    try:
        fd = os.open("/dev/tty", os.O_RDWR | os.O_NOCTTY)
        old = termios.tcgetattr(fd)
        tty_mod.setcbreak(fd)
        os.write(fd, b"\033[21t")
        ready, _, _ = select.select([fd], [], [], 0.15)
        if not ready:
            return None
        chunks: list[bytes] = []
        for _ in range(8):
            data = os.read(fd, 4096)
            if not data:
                break
            chunks.append(data)
            ready, _, _ = select.select([fd], [], [], 0.03)
            if not ready:
                break
        raw = b"".join(chunks)
    except (OSError, termios.error, ValueError, AttributeError):
        return None
    finally:
        if old is not None and fd is not None:
            try:
                termios.tcsetattr(fd, termios.TCSADRAIN, old)
            except termios.error:
                pass
        if fd is not None:
            try:
                os.close(fd)
            except OSError:
                pass
    return _parse_title_report_response(raw)


def _take_snapshot_once() -> None:
    global _snapshot_taken, _snapshot_before_brand
    if _snapshot_taken or _title_disabled():
        return
    _snapshot_taken = True
    if os.name == "nt":
        _snapshot_before_brand = _read_title_windows()
    else:
        _snapshot_before_brand = _read_title_posix()


def _register_restore_atexit() -> None:
    global _atexit_registered
    if _atexit_registered or _title_disabled():
        return
    _atexit_registered = True
    atexit.register(restore_terminal_window_title)


def _apply_title_string(safe: str) -> None:
    if not safe:
        return
    if os.name == "nt":
        if sys.stdout.isatty():
            try:
                import ctypes

                ctypes.windll.kernel32.SetConsoleTitleW(safe)
            except Exception:
                pass
        _write_osc0(safe, sys.stdout)
        _write_osc0(safe, sys.stderr)
        return
    _write_osc0(safe, sys.stdout)
    _write_osc0(safe, sys.stderr)


def set_terminal_window_title(title: str | None = None) -> None:
    """Set the terminal window/tab title when stdout/stderr is a TTY.

    On first call, saves the current title (Windows API or xterm CSI 21 t) so
    :func:`restore_terminal_window_title` can put it back later.
    """
    if _title_disabled():
        return
    _take_snapshot_once()
    _register_restore_atexit()
    raw = CAI_DEFAULT_TERMINAL_WINDOW_TITLE if title is None else title
    safe = _sanitize_title(raw)
    if not safe:
        return
    _apply_title_string(safe)


def restore_terminal_window_title() -> None:
    """Restore the title captured before CAI branding (safe to call multiple times)."""
    if _title_disabled():
        return
    global _snapshot_before_brand, _snapshot_taken
    saved = _snapshot_before_brand
    if saved is None:
        _snapshot_taken = False
        return
    _snapshot_before_brand = None
    _snapshot_taken = False
    safe = _sanitize_title(saved)
    if safe:
        _apply_title_string(safe)
