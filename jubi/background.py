"""Always-on user-session supervisor for Jubi.

This process is launched by the Windows scheduled task created by the one-click
installer. It keeps the localhost Jubi server alive, periodically checks the
verified continuous release channel, and starts bounded local self-repair when
installed prerequisites or model provisioning need attention.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

from . import updater


ROOT = Path(__file__).resolve().parents[1]
HOST = "127.0.0.1"
PORT = int(os.environ.get("JUBI_PORT") or "8877")


def _load_bootstrap() -> dict:
    try:
        return json.loads((ROOT / "config" / "bootstrap.json").read_text(encoding="utf-8"))
    except Exception:
        return {}


def _state_root() -> Path:
    root = updater.state_root()
    (root / "logs").mkdir(parents=True, exist_ok=True)
    return root


def _log(message: str) -> None:
    line = f"[{time.strftime('%Y-%m-%dT%H:%M:%S')}] {message}"
    try:
        with (_state_root() / "logs" / "background.log").open("a", encoding="utf-8") as handle:
            handle.write(line + "\n")
    except OSError:
        pass


def _listening(timeout: float = 0.4) -> bool:
    try:
        with socket.create_connection((HOST, PORT), timeout=timeout):
            return True
    except OSError:
        return False


def _start_server() -> subprocess.Popen:
    flags = 0
    if os.name == "nt":
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    _log("Starting localhost Jubi server.")
    return subprocess.Popen(
        [sys.executable, "-m", "jubi.server"],
        cwd=str(ROOT),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        creationflags=flags,
    )


def _stop_server(process: subprocess.Popen | None) -> None:
    if process is None or process.poll() is not None:
        return
    _log("Stopping supervised Jubi server for maintenance/update.")
    try:
        process.terminate()
        process.wait(timeout=12)
    except Exception:
        try:
            process.kill()
            process.wait(timeout=5)
        except Exception:
            pass


def _powershell() -> Path | None:
    if os.name != "nt":
        return None
    candidate = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"
    return candidate if candidate.is_file() else None


def _start_repair(*, full: bool) -> subprocess.Popen | None:
    ps = _powershell()
    script = ROOT / "installer" / "JUBI-PREREQUISITES.ps1"
    if ps is None or not script.is_file():
        return None
    args = [
        str(ps),
        "-NoLogo",
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(script),
        "-Repair" if full else "-Fast",
    ]
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    try:
        process = subprocess.Popen(
            args,
            cwd=str(ROOT),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            creationflags=flags,
        )
        _log("Started full background self-repair." if full else "Started fast background health repair.")
        return process
    except Exception as exc:
        _log(f"Could not start background self-repair: {exc}")
        return None


def main() -> int:
    if os.environ.get("JUBI_HOST") == "0.0.0.0":
        _log("Refusing unsafe JUBI_HOST=0.0.0.0; Jubi remains localhost-only.")
        return 4

    cfg = _load_bootstrap().get("background") or {}
    poll = max(2, int(cfg.get("health_poll_seconds") or 5))
    update_interval = max(3600, int(cfg.get("update_check_seconds") or 21600))
    repair_interval = max(1800, int(cfg.get("repair_check_seconds") or 21600))
    child: subprocess.Popen | None = None
    repair_process: subprocess.Popen | None = None
    last_update_check = 0.0
    last_repair_check = 0.0
    consecutive_server_failures = 0
    _log(f"Jubi background supervisor started with runtime {sys.executable}.")

    try:
        while True:
            if repair_process is not None and repair_process.poll() is not None:
                code = int(repair_process.returncode or 0)
                _log(f"Background self-repair completed with exit code {code}.")
                repair_process = None

            if child is not None and child.poll() is not None:
                code = child.returncode
                child = None
                consecutive_server_failures += 1
                _log(f"Jubi server exited unexpectedly with code {code}; failure count={consecutive_server_failures}.")
                if consecutive_server_failures >= 3 and repair_process is None:
                    repair_process = _start_repair(full=True)
                    consecutive_server_failures = 0
                time.sleep(min(30, 2 + consecutive_server_failures * 3))

            if child is None and not _listening():
                child = _start_server()
                deadline = time.time() + 25
                while time.time() < deadline:
                    if child.poll() is not None or _listening():
                        break
                    time.sleep(0.5)
                if _listening():
                    consecutive_server_failures = 0
                    _log("Jubi server health check passed.")

            now = time.time()
            if now - last_repair_check >= repair_interval and repair_process is None:
                last_repair_check = now
                repair_process = _start_repair(full=True)

            if now - last_update_check >= update_interval:
                last_update_check = now
                try:
                    info = updater.check_for_update()
                    if info.get("available"):
                        _log(f"Verified Jubi update found: {info.get('remote_commit')}.")
                        _stop_server(child)
                        child = None
                        if repair_process is not None and repair_process.poll() is None:
                            try:
                                repair_process.terminate()
                                repair_process.wait(timeout=10)
                            except Exception:
                                pass
                            repair_process = None
                        result = updater.apply_update(info)
                        if result == 0:
                            _log("Jubi update installed successfully; restarting supervisor through bootstrap loop.")
                            return 75
                        _log(f"Jubi update installer returned exit code {result}; keeping current installation active.")
                    else:
                        _log(f"Update check complete: {info.get('reason', 'current')}.")
                except Exception as exc:
                    _log(f"Update check/apply failed closed: {exc}")

            time.sleep(poll)
    except KeyboardInterrupt:
        _log("Jubi background supervisor stopping on request.")
        return 0
    finally:
        _stop_server(child)
        if repair_process is not None and repair_process.poll() is None:
            try:
                repair_process.terminate()
            except Exception:
                pass


if __name__ == "__main__":
    raise SystemExit(main())
