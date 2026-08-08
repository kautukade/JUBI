"""Simple CLI to interact with the CAI API backend."""

from __future__ import annotations

import json
import os
import sys
from dataclasses import dataclass
from typing import Any, Dict

import requests

API_KEY_HEADER = os.getenv("CAI_API_KEY_HEADER", "X-CAI-API-Key")


@dataclass
class ClientConfig:
    base_url: str
    api_key: str


def _prompt(label: str, default: str | None = None) -> str:
    suffix = f" [{default}]" if default else ""
    value = input(f"{label}{suffix}: ").strip()
    if not value and default is not None:
        return default
    return value


def configure_client() -> ClientConfig:
    host = _prompt("Host", os.getenv("CAI_API_HOST", "127.0.0.1"))
    port = _prompt("Port", os.getenv("CAI_API_PORT", "8000"))
    api_key = _prompt("API key (use ALIAS_API_KEY)", os.getenv("ALIAS_API_KEY") or os.getenv("CAI_API_KEY", ""))
    base_url = f"http://{host}:{port}/api/v1"
    return ClientConfig(base_url=base_url, api_key=api_key)


def request(method: str, path: str, config: ClientConfig, payload: Dict[str, Any] | None = None) -> Dict[str, Any]:
    url = f"{config.base_url}{path}"
    headers = {"Content-Type": "application/json"}
    if config.api_key:
        headers[API_KEY_HEADER] = config.api_key
    response = requests.request(method, url, json=payload, headers=headers, timeout=30)
    if response.status_code >= 400:
        raise RuntimeError(f"{response.status_code}: {response.text}")
    if response.text:
        return response.json()
    return {}


def main() -> None:
    print("=== CAI API Demo CLI ===")
    config = configure_client()

    session_payload = {
        "agent": _prompt("Agent", os.getenv("CAI_AGENT_TYPE", "redteam_agent")),
        "model": _prompt("Model", os.getenv("CAI_MODEL", "alias1")),
        "stateful": True,
    }
    session = request("POST", "/sessions", config, session_payload)
    session_id = session["id"]
    print(f"Session created: {session_id}")

    try:
        while True:
            user_input = input("prompt> ").strip()
            if user_input in {"/exit", "quit", "q"}:
                break
            if not user_input:
                continue
            payload = {"input": user_input}
            result = request("POST", f"/sessions/{session_id}/messages", config, payload)
            text_output = result["result"].get("text_output")
            print("--- Response ---")
            print(text_output or json.dumps(result["result"], indent=2))
    finally:
        try:
            request("DELETE", f"/sessions/{session_id}", config)
        except Exception:  # pragma: no cover - cleanup best effort
            pass


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(1)
