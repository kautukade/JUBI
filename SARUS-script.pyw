"""Legacy native-launcher compatibility entry for Jubi v0.1.0.

The verified SARUS-era PE launcher may still look for ``SARUS-script.pyw``.
Rather than relying on an installer-time rewrite, keep this tracked script
functional and route it to the canonical Jubi server entry point.
"""

from __future__ import annotations

import os
from pathlib import Path
import socket
import subprocess
import sys
import time
import webbrowser


ROOT = Path(__file__).resolve().parent
HOST = os.environ.get('JUBI_HOST') or os.environ.get('SARUS_HOST') or '127.0.0.1'
PORT = int(os.environ.get('JUBI_PORT') or os.environ.get('SARUS_PORT') or '8877')
URL = f'http://{HOST}:{PORT}'


def _listening(timeout: float = 0.35) -> bool:
    try:
        with socket.create_connection((HOST, PORT), timeout=timeout):
            return True
    except OSError:
        return False


def _runtime() -> Path:
    candidates = [
        ROOT / '.sarus-venv' / 'Scripts' / 'pythonw.exe',
        ROOT / '.sarus-venv' / 'Scripts' / 'python.exe',
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise SystemExit(
        'Jubi private Python runtime is missing. Run Jubi-Setup.exe before launching Jubi.exe.'
    )


def main() -> None:
    if HOST == '0.0.0.0':
        raise SystemExit('Jubi Phase 0 is localhost-only; JUBI_HOST=0.0.0.0 is not allowed.')

    if not _listening():
        runtime = _runtime()
        creationflags = 0
        if os.name == 'nt':
            creationflags = (
                getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)
                | getattr(subprocess, 'CREATE_NO_WINDOW', 0)
            )
        subprocess.Popen(
            [str(runtime), '-m', 'jubi.server'],
            cwd=str(ROOT),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            creationflags=creationflags,
        )
        deadline = time.time() + 20
        while time.time() < deadline and not _listening():
            time.sleep(0.25)

    webbrowser.open(URL)


if __name__ == '__main__':
    main()
