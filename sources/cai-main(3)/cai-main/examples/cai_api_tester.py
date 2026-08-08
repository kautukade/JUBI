"""Interactive client for the CAI API (defaults to 127.0.0.1:8080).

How to use:
  1) Start the backend:
     ALIAS_API_KEY=your_key cai --api --api-host 0.0.0.0 --api-port 8080

  2) Run this client:
     python examples/cai_api_tester.py

Requires: requests
"""

from __future__ import annotations

import json
import os
import sys
from dataclasses import dataclass
from typing import Any, Dict, List

import requests


API_KEY_HEADER = os.getenv("CAI_API_KEY_HEADER", "X-CAI-API-Key")


@dataclass
class ClientConfig:
    host: str = os.getenv("CAI_API_HOST", "127.0.0.1")
    port: int = int(os.getenv("CAI_API_PORT", "8080"))
    api_key: str = os.getenv("ALIAS_API_KEY", "") or os.getenv("CAI_API_KEY", "")

    @property
    def base_url(self) -> str:
        return f"http://{self.host}:{self.port}/api/v1"


def _prompt(label: str, default: str | None = None) -> str:
    suffix = f" [{default}]" if default else ""
    value = input(f"{label}{suffix}: ").strip()
    return default if (not value and default is not None) else value


def _print_json(data: Any) -> None:
    print(json.dumps(data, indent=2, ensure_ascii=False))


def _request(method: str, path: str, cfg: ClientConfig, payload: Dict[str, Any] | None = None):
    url = f"{cfg.base_url}{path}"
    headers = {"Content-Type": "application/json"}
    if cfg.api_key:
        headers[API_KEY_HEADER] = cfg.api_key
    r = requests.request(method, url, json=payload, headers=headers, timeout=60)
    if r.status_code >= 400:
        raise RuntimeError(f"{r.status_code} {r.reason}: {r.text}")
    return r.json() if r.text else None


def main():
    print("=== CAI API Tester (default 8080) ===")
    cfg = ClientConfig()
    # Quick configuration at startup
    cfg.host = _prompt("Host", cfg.host) or cfg.host
    cfg.port = int(_prompt("Port", str(cfg.port)) or cfg.port)
    cfg.api_key = _prompt("API Key (ALIAS_API_KEY)", cfg.api_key)

    state: dict = {"last_session": None}

    def action_health():
        _print_json(_request("GET", "/health", cfg))

    def action_list_commands():
        _print_json(_request("GET", "/commands", cfg))

    def action_run_command():
        name = _prompt("Command (e.g. memory)")
        args_str = _prompt("Args separated by spaces (optional)")
        args = args_str.split() if args_str else []
        _print_json(
            _request(
                "POST",
                f"/commands/{name}",
                cfg,
                {"args": args, "auto_correct": True},
            )
        )

    def action_create_session():
        agent = _prompt("Agent", os.getenv("CAI_AGENT_TYPE", "redteam_agent"))
        model = _prompt("Model", os.getenv("CAI_MODEL", "alias1"))
        stateful = (_prompt("Stateful [true/false]", "true").lower() != "false")
        resp = _request("POST", "/sessions", cfg, {"agent": agent, "model": model, "stateful": stateful})
        state["last_session"] = resp["id"]
        print(f"Created session: {state['last_session']}")
        _print_json(resp)

    def action_list_sessions():
        _print_json(_request("GET", "/sessions", cfg))

    def action_session_detail():
        _print_json(_request("GET", f"/sessions/{_ensure_session(state['last_session'])}", cfg))

    def action_send_message():
        prompt = _prompt("Prompt")
        _print_json(
            _request(
                "POST",
                f"/sessions/{_ensure_session(state['last_session'])}/messages",
                cfg,
                {"input": prompt},
            )
        )

    def action_history():
        _print_json(_request("GET", f"/sessions/{_ensure_session(state['last_session'])}/history", cfg))

    def action_reset():
        _print_json(_request("POST", f"/sessions/{_ensure_session(state['last_session'])}/reset", cfg))

    def action_delete():
        _request("DELETE", f"/sessions/{_ensure_session(state['last_session'])}", cfg)
        print("Session deleted")
        if state["last_session"]:
            state["last_session"] = None

    menu = {
        "1": ("Healthcheck", action_health),
        "1a": ("List agents", lambda: _print_json(_request("GET", "/agents", cfg))),
        "1b": ("List models", lambda: _print_json(_request("GET", "/models", cfg))),
        "2": ("List commands", action_list_commands),
        "3": ("Run command", action_run_command),
        "4": ("Create session", action_create_session),
        "5": ("List sessions", action_list_sessions),
        "6": ("Session detail", action_session_detail),
        "7": ("Send message", action_send_message),
        "8": ("History", action_history),
        "9": ("Reset session", action_reset),
        "10": ("Delete session", action_delete),
        "0": ("Exit", None),
    }

    while True:
        try:
            print("\nOptions:")
            for k in sorted(menu.keys(), key=lambda x: int(x) if x.isdigit() else 99):
                print(f"  {k}) {menu[k][0]}")
            choice = input("> ").strip()
            if choice == "0":
                break
            action = menu.get(choice)
            if not action:
                print("Invalid option")
                continue
            result = action[1]()
            # If an action returned data (and it wasn't printed already), pretty-print it
            if result is not None and not isinstance(result, bool):
                _print_json(result)
        except KeyboardInterrupt:
            print("\nExiting...")
            break
        except Exception as e:
            print(f"Error: {e}")


def _ensure_session(current: str | None) -> str:
    if current:
        return current
    sid = input("Session ID: ").strip()
    if not sid:
        raise RuntimeError("A session ID is required. Create a session first.")
    return sid


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(1)
