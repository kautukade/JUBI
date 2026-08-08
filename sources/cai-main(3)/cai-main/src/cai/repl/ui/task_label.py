"""Deterministic task labels for the compact REPL.

A task label is the human-readable single-line description rendered on the
live row (and stored in :class:`cai.output.TaskRecord`). Today it is inferred
deterministically from ``tool_name`` + ``args``; in the future a planner agent
will be able to override the label via :func:`register_task_label_provider`.

Reuses :func:`cai.util.terminal._tool_command_line_display` and
:func:`cai.util.terminal._format_tool_args` to keep a single source of truth.
"""

from __future__ import annotations

import json
from typing import Any, Callable, Optional

from cai.util.terminal import _format_tool_args, _tool_command_line_display

LabelProvider = Callable[[str, Any, Optional[str]], Optional[str]]

_DEFAULT_MAX_LEN = 88

# Optional override hook for the future planner / orchestrator.
_label_provider: LabelProvider | None = None


def register_task_label_provider(provider: LabelProvider | None) -> None:
    """Install (or clear) a custom label provider.

    The provider receives ``(tool_name, args, agent_name)`` and returns either
    a string label or ``None`` to fall back to the deterministic inference.
    Designed for the upcoming orchestration feature.
    """
    global _label_provider
    _label_provider = provider


def _coerce_args(args: Any) -> Any:
    """Accept either a dict or a JSON-encoded string (common from hooks/runtime)."""
    if isinstance(args, str):
        stripped = args.strip()
        if stripped.startswith("{") and stripped.endswith("}"):
            try:
                return json.loads(stripped)
            except (json.JSONDecodeError, ValueError):
                return args
    return args


def _humanize_handoff(tool_name: str) -> str:
    """``transfer_to_red_teamer`` -> ``→ Red Teamer``."""
    raw = tool_name[len("transfer_to_") :]
    return "→ " + " ".join(part.capitalize() for part in raw.split("_") if part)


def _truncate(text: str, max_len: int) -> str:
    text = text.replace("\n", " ").strip()
    if len(text) <= max_len or max_len <= 4:
        return text
    return text[: max_len - 1].rstrip() + "…"


def infer_task_label(
    tool_name: str,
    args: Any,
    agent_name: str | None = None,
    *,
    max_len: int = _DEFAULT_MAX_LEN,
) -> str:
    """Return a single-line label describing the task.

    Resolution order:

    1. Custom ``LabelProvider`` (if registered and returns non-empty).
    2. Handoff tools (``transfer_to_*``) → ``→ Specialist``.
    3. ``generic_linux_command`` → reuses ``_tool_command_line_display``.
    4. ``execute_code`` → ``execute_code: <first line>``.
    5. Generic fallback → ``tool_name(arg=val, ...)``.
    """
    tool_name = tool_name or "tool"

    if _label_provider is not None:
        try:
            override = _label_provider(tool_name, args, agent_name)
            if override:
                return _truncate(override, max_len)
        except Exception:
            # Provider must not break the renderer
            pass

    if tool_name.startswith("transfer_to_"):
        return _truncate(_humanize_handoff(tool_name), max_len)

    coerced = _coerce_args(args)

    if tool_name == "execute_code" and isinstance(coerced, dict):
        code = (coerced.get("code") or "").strip()
        if code:
            first = code.split("\n", 1)[0].strip()
            return _truncate(f"execute_code: {first}", max_len)
        return _truncate("execute_code", max_len)

    if tool_name == "generic_linux_command":
        try:
            cmd = _tool_command_line_display(tool_name, coerced)
        except Exception:
            cmd = ""
        if cmd and cmd.strip():
            return _truncate(cmd, max_len)

    try:
        formatted_args = _format_tool_args(coerced, tool_name=tool_name)
    except Exception:
        formatted_args = str(coerced) if coerced else ""

    if formatted_args:
        return _truncate(f"{tool_name}({formatted_args})", max_len)
    return _truncate(tool_name, max_len)


__all__ = [
    "LabelProvider",
    "infer_task_label",
    "register_task_label_provider",
]
