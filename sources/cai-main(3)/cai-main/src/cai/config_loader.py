"""Utilities for loading agent configuration files."""

from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, Iterable, Optional, Tuple

import os
import yaml


class AgentsConfigError(RuntimeError):
    """Raised when the agents configuration file cannot be loaded."""


def _default_config_paths() -> Iterable[Path]:
    """Yield default locations to search for agents.yml."""
    cwd = Path.cwd()
    yield cwd / "agents.yml"

    package_root = Path(__file__).resolve().parent
    # src/cai -> want src/cai/agents/patterns/configs/agents.yml
    yield package_root / "agents" / "patterns" / "configs" / "agents.yml"


def resolve_agents_path(path: Optional[str | Path] = None) -> Optional[Path]:
    """Resolve the path to agents.yml.

    Args:
        path: Optional explicit path provided by the user.

    Returns:
        The resolved configuration path if it exists, otherwise ``None``.
    """
    candidates: Iterable[Path]

    if path:
        candidates = [Path(path).expanduser().resolve()]
    else:
        candidates = _default_config_paths()

    for candidate in candidates:
        if candidate.exists() and candidate.is_file():
            return candidate
    return None


def load_agents_config(path: Optional[str | Path] = None) -> Tuple[Dict[str, Any], Optional[Path]]:
    """Load agent configuration data from YAML.

    Args:
        path: Optional explicit path to ``agents.yml``. When ``None``, the
            default search paths are used.

    Returns:
        A tuple ``(data, resolved_path)``. ``data`` will be an empty dict if the
        file is not found. ``resolved_path`` is ``None`` when no configuration
        file could be resolved.

    Raises:
        AgentsConfigError: If the file exists but cannot be parsed.
    """
    resolved_path = resolve_agents_path(path)
    if not resolved_path:
        return {}, None

    try:
        with resolved_path.open("r", encoding="utf-8") as handle:
            data = yaml.safe_load(handle) or {}
    except Exception as exc:  # noqa: BLE001 - surface parsing errors clearly
        raise AgentsConfigError(f"Failed to load agents config at {resolved_path}: {exc}") from exc

    if not isinstance(data, dict):
        raise AgentsConfigError(
            f"Agents config at {resolved_path} must be a mapping, got {type(data).__name__}"
        )

    return data, resolved_path


def _normalize_bool(value: Any) -> Optional[bool]:
    """Best-effort normalization of truthy strings and integers to booleans."""

    if value is None:
        return None
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return bool(value)
    if isinstance(value, str):
        lowered = value.strip().lower()
        if lowered in {"true", "1", "yes", "y", "on"}:
            return True
        if lowered in {"false", "0", "no", "n", "off"}:
            return False
    return None


def extract_agent_definitions(
    data: Dict[str, Any]
) -> Tuple[list[Dict[str, Any]], Dict[str, Any], Optional[str]]:
    """Return normalized agent entries plus shared metadata.

    The function understands both the legacy ``tui_startup`` structure and the
    modern ``parallel_agents`` block.  Each returned agent dictionary contains
    the keys ``agent_name``, ``prompt``, ``team``, ``model``, ``env``,
    ``auto_run`` and ``unified_context``.  ``metadata`` carries configuration
    defaults such as ``shared_prompt`` and ``auto_run``.
    """

    agents: list[Dict[str, Any]] = []
    metadata: Dict[str, Any] = {
        "shared_prompt": None,
        "auto_run": True,
        "description": None,
    }
    origin: Optional[str] = None

    shared_block = data.get("shared") if isinstance(data.get("shared"), dict) else {}

    # ========================================================================
    # BACKWARD COMPATIBILITY: Support documentation format (agents -> parallel_agents)
    # ========================================================================
    # The documentation shows "agents:" but the code expects "parallel_agents:"
    # This normalizes the old format to the new one for backward compatibility
    if "agents" in data and "parallel_agents" not in data:
        raw_agents = data.get("agents")
        if isinstance(raw_agents, list):
            # Normalize the documentation format to the expected format
            normalized_agents = []
            for entry in raw_agents:
                if not isinstance(entry, dict):
                    continue
                
                normalized_entry = {}
                
                # Map "agent_type" to "name" (agent_type was used in docs but isn't correct)
                if "agent_type" in entry:
                    normalized_entry["name"] = entry["agent_type"]
                elif "name" in entry:
                    # If there's a "name" field, use it as description
                    # and keep looking for the actual agent name
                    if "name" in entry and "agent_type" not in entry:
                        # In this case, "name" might be the agent name
                        normalized_entry["name"] = entry["name"]
                    normalized_entry["description"] = entry.get("name")
                
                # Map "initial_prompt" to "prompt" (documentation used initial_prompt)
                if "initial_prompt" in entry:
                    normalized_entry["prompt"] = entry["initial_prompt"]
                elif "prompt" in entry:
                    normalized_entry["prompt"] = entry["prompt"]
                
                # Copy other fields as-is
                for key in ["model", "team", "group", "label", "auto_run", "unified_context", "env"]:
                    if key in entry:
                        normalized_entry[key] = entry[key]
                
                # If we still don't have a name, skip this entry
                if "name" in normalized_entry:
                    normalized_agents.append(normalized_entry)
            
            # Replace "agents" with "parallel_agents" in the data dict
            data["parallel_agents"] = normalized_agents
    # ========================================================================

    # Preferred modern structure: parallel_agents
    raw_parallel = data.get("parallel_agents")
    if isinstance(raw_parallel, list):
        origin = "parallel_agents"

        shared_prompt = data.get("shared_prompt")
        if not isinstance(shared_prompt, str):
            shared_prompt = shared_block.get("prompt")
        if isinstance(shared_prompt, str):
            shared_prompt = shared_prompt.strip()

        auto_run_default = _normalize_bool(data.get("auto_run"))
        if auto_run_default is None:
            auto_run_default = _normalize_bool(shared_block.get("auto_run"))
        if auto_run_default is None:
            auto_run_default = True

        metadata["shared_prompt"] = shared_prompt
        metadata["auto_run"] = bool(auto_run_default)
        metadata["description"] = data.get("description") or shared_block.get("description")

        team_indices: Dict[str, int] = {}

        for idx, entry in enumerate(raw_parallel, start=1):
            if not isinstance(entry, dict):
                continue

            agent_name = entry.get("name") or entry.get("agent")
            if not agent_name:
                continue

            prompt = entry.get("prompt")
            if isinstance(prompt, str):
                prompt = prompt.strip()
            elif shared_prompt:
                prompt = shared_prompt
            else:
                prompt = None

            team_name = entry.get("team") or entry.get("group") or entry.get("label")
            if isinstance(team_name, str):
                team_name = team_name.strip()
            if team_name:
                team_indices.setdefault(team_name, len(team_indices) + 1)

            agent_auto = _normalize_bool(entry.get("auto_run"))
            if agent_auto is None:
                agent_auto = metadata["auto_run"]

            agents.append(
                {
                    "agent_name": agent_name,
                    "prompt": prompt,
                    "team": team_name,
                    "model": entry.get("model"),
                    "env": entry.get("env") if isinstance(entry.get("env"), dict) else {},
                    "auto_run": bool(agent_auto),
                    "unified_context": bool(_normalize_bool(entry.get("unified_context")) or False),
                    "description": entry.get("description"),
                    "index": idx,
                }
            )

    # Legacy structure: tui_startup
    if not agents:
        startup_cfg = data.get("tui_startup")
        if isinstance(startup_cfg, dict):
            origin = "tui_startup"

            shared_prompt = startup_cfg.get("shared_prompt")
            if isinstance(shared_prompt, str):
                shared_prompt = shared_prompt.strip()

            auto_run_default = _normalize_bool(startup_cfg.get("auto_run"))
            if auto_run_default is None:
                auto_run_default = True

            metadata["shared_prompt"] = shared_prompt
            metadata["auto_run"] = bool(auto_run_default)
            metadata["description"] = startup_cfg.get("description")

            teams = startup_cfg.get("teams")
            if isinstance(teams, list):
                for team_index, team in enumerate(teams, start=1):
                    if not isinstance(team, dict):
                        continue

                    team_name = team.get("name") or f"Team {team_index}"
                    team_prompt = team.get("prompt")
                    if isinstance(team_prompt, str):
                        team_prompt = team_prompt.strip()

                    team_auto = _normalize_bool(team.get("auto_run"))

                    agents_cfg = team.get("agents")
                    if not isinstance(agents_cfg, list):
                        continue

                    for agent_index, agent_cfg in enumerate(agents_cfg, start=1):
                        if not isinstance(agent_cfg, dict):
                            continue

                        agent_name = agent_cfg.get("name") or agent_cfg.get("agent")
                        if not agent_name:
                            continue

                        prompt = agent_cfg.get("prompt")
                        if isinstance(prompt, str):
                            prompt = prompt.strip()
                        else:
                            prompt = team_prompt or shared_prompt

                        agent_auto = _normalize_bool(agent_cfg.get("auto_run"))
                        if agent_auto is None:
                            agent_auto = team_auto if team_auto is not None else metadata["auto_run"]

                        agents.append(
                            {
                                "agent_name": agent_name,
                                "prompt": prompt,
                                "team": team_name,
                                "model": agent_cfg.get("model"),
                                "env": agent_cfg.get("env") if isinstance(agent_cfg.get("env"), dict) else {},
                                "auto_run": bool(agent_auto),
                                "unified_context": bool(_normalize_bool(agent_cfg.get("unified_context")) or False),
                                "description": agent_cfg.get("description"),
                                "team_index": team_index,
                                "agent_index": agent_index,
                            }
                        )

    return agents, metadata, origin


__all__ = [
    "AgentsConfigError",
    "load_agents_config",
    "resolve_agents_path",
    "extract_agent_definitions",
]
