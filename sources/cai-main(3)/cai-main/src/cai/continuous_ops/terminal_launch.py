"""Spawn an external terminal for the continuous-ops loop (CLI, no import cycle with cli_headless)."""

from __future__ import annotations

import json
import os
import platform
import shlex
import shutil
import subprocess


def detect_external_terminal_backend() -> tuple[str | None, str]:
    """Return ``(backend, hint)`` — same policy as headless parallel workers."""
    is_wsl = "microsoft" in platform.release().lower() or bool(os.getenv("WSL_DISTRO_NAME"))
    system = platform.system().lower()

    if system == "darwin":
        if shutil.which("osascript"):
            return "osascript", "macOS Terminal (via osascript)"
        return None, "Install/enable AppleScript CLI (osascript)"

    # Debian family first (Ubuntu, Kali, Raspberry Pi OS, etc.); then common X11 terminals.
    for candidate in (
        "x-terminal-emulator",
        "gnome-terminal",
        "konsole",
        "qterminal",
        "xfce4-terminal",
        "xterm",
    ):
        if shutil.which(candidate):
            return candidate, candidate

    if is_wsl:
        return None, "Install a terminal launcher inside WSL (e.g. xterm) or use tmux attach"
    return None, "Install one of: x-terminal-emulator, gnome-terminal, konsole, qterminal, xfce4-terminal, xterm"


def spawn_external_terminal(backend: str, title: str, command: str) -> bool:
    """Open *command* in a new terminal window/tab (best effort)."""
    try:
        if backend == "x-terminal-emulator":
            # Debian alternatives (Raspberry Pi OS, Ubuntu). Many wrappers expect a single argv after ``-e``.
            tail = "; echo; echo '[CAI] Continuous ops worker finished.'; read -r -p 'Press Enter to close...'"
            script = f"{command}{tail}"
            subprocess.Popen(
                ["x-terminal-emulator", "-e", f"bash -lc {shlex.quote(script)}"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            return True
        if backend == "gnome-terminal":
            subprocess.Popen(
                [
                    "gnome-terminal",
                    "--title",
                    title,
                    "--",
                    "bash",
                    "-lc",
                    f"{command}; echo; echo '[CAI] Continuous ops worker finished.'; read -r -p 'Press Enter to close...'",
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            return True
        if backend == "konsole":
            subprocess.Popen(
                [
                    "konsole",
                    "--new-tab",
                    "-p",
                    f"tabtitle={title}",
                    "-e",
                    "bash",
                    "-lc",
                    f"{command}; echo; echo '[CAI] Continuous ops worker finished.'; read -r -p 'Press Enter to close...'",
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            return True
        if backend == "qterminal":
            tail = "; echo; echo '[CAI] Continuous ops worker finished.'; read -r -p 'Press Enter to close...'"
            script = f"{command}{tail}"
            subprocess.Popen(
                ["qterminal", "-e", f"bash -lc {shlex.quote(script)}"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            return True
        if backend == "xfce4-terminal":
            subprocess.Popen(
                [
                    "xfce4-terminal",
                    "--title",
                    title,
                    "--hold",
                    "-e",
                    f"bash -lc {shlex.quote(command)}",
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            return True
        if backend == "xterm":
            subprocess.Popen(
                [
                    "xterm",
                    "-T",
                    title,
                    "-hold",
                    "-e",
                    "bash",
                    "-lc",
                    command,
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            return True
        if backend == "osascript":
            osa_cmd = (
                'tell application "Terminal" to do script '
                + json.dumps(f"{command}; echo; echo '[CAI] Continuous ops worker finished.'")
            )
            subprocess.Popen(
                ["osascript", "-e", osa_cmd],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            return True
    except OSError:
        return False
    return False
