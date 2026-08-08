"""Bounded session snapshot (D2) for continuous-ops tick subprocesses.

Exports the tail of ``agent.model.message_history`` to JSON so the next tick
can rehydrate before sending the new user prompt. Best-effort: skips entries
that are not JSON-serializable after coercion.
"""

from __future__ import annotations

import json
import logging
import os
from copy import deepcopy
from pathlib import Path
from typing import Any

_LOG = logging.getLogger(__name__)


def _env_int(name: str, default: int, lo: int, hi: int) -> int:
    try:
        v = int((os.getenv(name) or str(default)).strip())
    except ValueError:
        return default
    return max(lo, min(hi, v))


def _env_int_content(name: str, default: int, lo: int, hi: int) -> int:
    try:
        v = int((os.getenv(name) or str(default)).strip())
    except ValueError:
        return default
    return max(lo, min(hi, v))


def _truncate_msg(msg: dict[str, Any], max_content: int) -> dict[str, Any]:
    m = deepcopy(msg)
    c = m.get("content")
    if isinstance(c, str) and len(c) > max_content:
        m["content"] = c[:max_content] + "\n[… truncated for snapshot …]"
    return m


def export_snapshot(path: Path, message_history: list[dict[str, Any]] | None) -> bool:
    if not message_history:
        return False
    max_msg = _env_int("CAI_COPS_SNAPSHOT_MAX_MESSAGES", 40, 1, 200)
    max_content = _env_int_content("CAI_COPS_SNAPSHOT_MAX_CONTENT_CHARS", 12_000, 500, 100_000)
    tail = message_history[-max_msg:]
    serializable: list[dict[str, Any]] = []
    for msg in tail:
        if not isinstance(msg, dict):
            continue
        try:
            cleaned = _truncate_msg(msg, max_content)
            json.dumps(cleaned, default=str)
            serializable.append(cleaned)
        except (TypeError, ValueError):
            continue
    payload = {"schema_version": 1, "messages": serializable}
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(path.suffix + f".tmp.{os.getpid()}")
        tmp.write_text(json.dumps(payload, ensure_ascii=False, default=str), encoding="utf-8")
        tmp.replace(path)
        return True
    except OSError as e:
        _LOG.debug("session_snapshot export failed: %s", e)
        return False


def load_snapshot_messages(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        return []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        _LOG.debug("session_snapshot load failed: %s", e)
        return []
    msgs = data.get("messages")
    if not isinstance(msgs, list):
        return []
    out: list[dict[str, Any]] = []
    for m in msgs:
        if isinstance(m, dict) and m.get("role"):
            out.append(m)
    return out


def apply_snapshot_to_agent(
    agent, messages: list[dict[str, Any]], *, history_key: str | None = None
) -> None:
    """Replace shared message history for *agent* with *messages* (same list ref as manager)."""
    if not messages or agent is None or not hasattr(agent, "model"):
        return
    model = agent.model
    if not hasattr(model, "message_history"):
        return
    from cai.sdk.agents.simple_agent_manager import AGENT_MANAGER

    name = (history_key or "").strip() or getattr(model, "agent_name", None) or getattr(agent, "name", None)
    if not name:
        return
    # ``get_message_history`` returns a fresh ``[]`` when the key is missing — mutate the canonical dict.
    if name not in AGENT_MANAGER._message_history:
        AGENT_MANAGER._message_history[name] = []
    hist = AGENT_MANAGER._message_history[name]
    hist.clear()
    hist.extend(messages)
    model.message_history = hist
