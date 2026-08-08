"""Convenience launcher for the CAI API server."""

from __future__ import annotations

import os
import socket
from typing import Any, Dict

import uvicorn

from .app import create_cai_api_app


def _is_port_free(host: str, port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(0.5)
        result = s.connect_ex((host, port))
        return result != 0


def _pick_available_port(host: str, preferred: int, attempts: int = 25) -> int:
    if preferred == 0:
        # Let the OS pick an ephemeral port
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.bind((host, 0))
            return s.getsockname()[1]
    if _is_port_free(host, preferred):
        return preferred
    start = int(os.getenv("CAI_API_PORT_FALLBACK_START", preferred + 1))
    end = int(os.getenv("CAI_API_PORT_FALLBACK_END", preferred + attempts))
    for p in range(start, end + 1):
        if _is_port_free(host, p):
            return p
    return preferred  # Fallback to preferred even if busy; uvicorn will raise


def run_api_server(
    *,
    host: str | None = None,
    port: int | None = None,
    reload: bool = False,
    workers: int = 1,
) -> None:
    """Start the CAI API backend using uvicorn."""
    host = host or os.getenv("CAI_API_HOST", "127.0.0.1")
    port = port or int(os.getenv("CAI_API_PORT", "8000"))
    reload = reload or os.getenv("CAI_API_RELOAD", "false").lower() == "true"
    workers = workers or int(os.getenv("CAI_API_WORKERS", "1"))

    if reload and workers != 1:
        workers = 1  # uvicorn does not allow reload with multiple workers

    # Choose a free port if the preferred one is busy
    chosen_port = _pick_available_port(host, port)
    if chosen_port != port:
        print(f"[CAI API] Port {port} busy. Using {chosen_port} instead.")
    app = create_cai_api_app()
    log_level = os.getenv("CAI_API_LOG_LEVEL", "info")

    config: Dict[str, Any] = {
        "app": app,
        "host": host,
        "port": chosen_port,
        "reload": reload,
        "log_level": log_level,
    }
    if not reload:
        config["workers"] = workers

    uvicorn.run(**config)
