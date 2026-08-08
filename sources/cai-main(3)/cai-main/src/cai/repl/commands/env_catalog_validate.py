"""
Resolve and validate /env catalog variable specs and values.
"""

from __future__ import annotations

import functools
import ipaddress
import json
import re
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

# Catalog dict type: int -> {name, description, default}
EnvVarEntry = Dict[str, object]

# Extras merged from ``env_info_catalog.EXTRA_ENV_VARS`` (bool-like, not model/IP/port rules).
CATALOG_BOOL_VAR_NAMES = frozenset(
    {
        "CAI_YOLO",
        "CAI_AVOID_SUDO",
        "CAI_SENSITIVE_GUARD",
        "CAI_UNRESTRICTED",
        "CAI_UNRESTRICTED_LOG",
        "CAI_TOOL_LIVE_SHOW_PRICING",
        "CAI_DISABLE_TOOL_WAIT_HINTS",
        "CAI_TOOL_OUTPUT_MARKDOWN",
        "CAI_SKIP_UPDATE_CHECK",
        "CAI_AUTO_UPDATE",
        "CAI_VERBOSE_HTTP_RETRY",
        "CAI_HTTP_ERROR_BODY",
    }
)


def resolve_catalog_spec(spec: str, env_vars: dict[int, EnvVarEntry]) -> Optional[Tuple[int, EnvVarEntry, str]]:
    """Resolve ``#`` or variable name (case-insensitive) to (num, entry, canonical_name)."""
    s = (spec or "").strip()
    if not s:
        return None
    if s.isdigit():
        n = int(s)
        if n not in env_vars:
            return None
        info = env_vars[n]
        name = str(info["name"])
        return n, info, name
    su = s.upper()
    for num, info in env_vars.items():
        if str(info["name"]).upper() == su:
            return num, info, str(info["name"])
    return None


def is_model_catalog_var(var_name: str) -> bool:
    """True if this env var holds an LLM model id."""
    if var_name in ("CAI_MODEL", "CTF_MODEL"):
        return True
    if var_name.endswith("_MODEL"):
        return True
    return False


@functools.lru_cache(maxsize=1)
def get_caibench_ctf_names_for_completion_cached() -> tuple[str, ...]:
    """Unique canonical CTF names from CAIBench, sorted case-insensitively (REPL completer + validation)."""
    try:
        import cai.caibench as cb_mod

        path = Path(cb_mod.__file__).resolve().parent / "ctf-jsons" / "ctf_configs.jsonl"
        if not path.is_file():
            return ()
        with path.open(encoding="utf-8") as fh:
            configs = json.load(fh)
        seen: Set[str] = set()
        out: List[str] = []
        for cfg in configs:
            n = cfg.get("name")
            if not isinstance(n, str):
                continue
            s = n.strip()
            if not s:
                continue
            low = s.lower()
            if low not in seen:
                seen.add(low)
                out.append(s)
        out.sort(key=str.lower)
        return tuple(out)
    except Exception:  # pylint: disable=broad-except
        return ()


def _load_caibench_ctf_names_lowercase() -> Optional[Set[str]]:
    """Return lowercase CTF ids from CAIBench config, or None if unavailable."""
    names = get_caibench_ctf_names_for_completion_cached()
    if not names:
        return None
    return {n.lower() for n in names}


def _first_caibench_ctf_name_canonical() -> Optional[str]:
    """First CTF ``name`` string from CAIBench config (for tests / samples)."""
    names = get_caibench_ctf_names_for_completion_cached()
    return names[0] if names else None


def _validate_ctf_name(value: str) -> Optional[str]:
    v = (value or "").strip()
    if not v:
        return "CTF_NAME cannot be empty."
    try:
        from cai import is_pentestperf_available

        if not is_pentestperf_available():
            return None
    except Exception:  # pylint: disable=broad-except
        return None
    known = _load_caibench_ctf_names_lowercase()
    if not known:
        return None
    if v.lower() not in known:
        return (
            f"No CAIBench CTF named '{v}'. Use an id from the CAIBench catalog "
            "(same names as in cai.caibench ctf_configs.jsonl)."
        )
    return None


def _validate_model_value(value: str) -> Optional[str]:
    """Return error message if model is not known and not verifiable."""
    v = (value or "").strip()
    if not v:
        return "Model value cannot be empty."

    from cai.repl.commands.model import get_predefined_model_names, load_all_available_models

    if v in get_predefined_model_names():
        return None

    try:
        all_names, _ = load_all_available_models()
    except Exception:  # pylint: disable=broad-except
        all_names = []

    if v in all_names:
        return None

    return (
        f"Unknown model '{v}'. Use a name from /model or /model show, or ensure Ollama/LiteLLM "
        "includes this model."
    )


def _validate_ipv4(value: str) -> Optional[str]:
    s = (value or "").strip()
    try:
        addr = ipaddress.ip_address(s)
        if addr.version != 4:
            return f"Expected an IPv4 address, got '{s}'."
    except ValueError:
        return f"Invalid IPv4 address: '{s}'."
    return None


def _validate_cidr_or_subnet(value: str) -> Optional[str]:
    s = (value or "").strip()
    try:
        ipaddress.ip_network(s, strict=False)
    except ValueError:
        return f"Invalid network/CIDR: '{s}'."
    return None


def _validate_int_range(value: str, low: int, high: int, label: str) -> Optional[str]:
    s = (value or "").strip()
    try:
        n = int(s, 10)
    except ValueError:
        return f"{label} must be an integer, got '{s}'."
    if n < low or n > high:
        return f"{label} must be between {low} and {high}, got {n}."
    return None


def _validate_float_range(value: str, low: float, high: float, label: str) -> Optional[str]:
    s = (value or "").strip()
    try:
        x = float(s)
    except ValueError:
        return f"{label} must be a number, got '{s}'."
    if x < low or x > high:
        return f"{label} must be between {low} and {high}, got {x}."
    return None


def _validate_bool_string(value: str) -> Optional[str]:
    s = (value or "").strip().lower()
    if s not in ("true", "false", "0", "1"):
        return f"Expected true/false (or 0/1), got '{value}'."
    return None


def _validate_inf_or_float(value: str, label: str) -> Optional[str]:
    s = (value or "").strip().lower()
    if s == "inf":
        return None
    try:
        float(s)
    except ValueError:
        return f"{label} must be a number or 'inf', got '{value}'."
    return None


def validate_catalog_value(var_name: str, value: str, var_info: EnvVarEntry) -> Optional[str]:
    """Return an error string, or None if the value is acceptable."""

    # Model-family variables
    if is_model_catalog_var(var_name):
        return _validate_model_value(value)

    if var_name == "CTF_NAME":
        return _validate_ctf_name(value)

    # IPv4 host addresses
    if var_name.endswith("_IP") and "API" not in var_name:
        return _validate_ipv4(value)

    # Subnets
    if var_name.endswith("_SUBNET") or var_name.endswith("_CIDR"):
        return _validate_cidr_or_subnet(value)

    # Ports
    if var_name.endswith("_PORT") or var_name in ("CAI_AUTH_DEVICE_PORT",):
        return _validate_int_range(value, 1, 65535, "Port")

    if var_name == "CAI_PARALLEL":
        return _validate_int_range(value, 1, 20, "CAI_PARALLEL")

    if var_name == "CAI_DEBUG":
        return _validate_int_range(value, 0, 2, "CAI_DEBUG")

    if var_name == "CAI_COMPACT_REPL":
        return _validate_bool_string(value)

    if var_name in ("CAI_TEMPERATURE",):
        return _validate_float_range(value, 0.0, 2.0, "CAI_TEMPERATURE")

    if var_name in ("CAI_TOP_P",):
        return _validate_float_range(value, 0.0, 1.0, "CAI_TOP_P")

    if var_name in ("CAI_MAX_TURNS", "CAI_MAX_INTERACTIONS"):
        return _validate_inf_or_float(value, var_name)

    if var_name == "CAI_PRICE_LIMIT":
        s = (value or "").strip().lower()
        if s == "inf":
            return None
        return _validate_float_range(value, 0.0, 1e9, "CAI_PRICE_LIMIT")

    if var_name in CATALOG_BOOL_VAR_NAMES:
        return _validate_bool_string(value)

    if var_name == "CAI_PARALLEL_EXTERNAL_TIMEOUT":
        s = (value or "").strip()
        try:
            x = float(s)
        except ValueError:
            return f"CAI_PARALLEL_EXTERNAL_TIMEOUT must be a number, got '{value}'."
        if x <= 0:
            return "CAI_PARALLEL_EXTERNAL_TIMEOUT must be positive."
        return None

    if var_name == "CAI_TASK_RESET_PENDING":
        return _validate_int_range(value, 0, 1, "CAI_TASK_RESET_PENDING")

    if var_name == "CAI_MERGE_SUMMARIZE_PER_WORKER":
        return _validate_int_range(value, 0, 1, "CAI_MERGE_SUMMARIZE_PER_WORKER")

    if var_name == "CAI_MERGE_SUMMARIZE_MIN_MESSAGES":
        return _validate_int_range(value, 1, 10_000_000, "CAI_MERGE_SUMMARIZE_MIN_MESSAGES")

    # Boolean-like when catalog default is true/false
    default = var_info.get("default")
    if isinstance(default, str) and default.lower() in ("true", "false"):
        if re.search(r"\b(enable|disable|flag|mode)\b", str(var_info.get("description", "")), re.I):
            return _validate_bool_string(value)

    return None


def sample_valid_test_value(var_name: str, var_info: EnvVarEntry) -> str:
    """Return a value expected to pass ``validate_catalog_value`` (for pytest only)."""
    if is_model_catalog_var(var_name):
        return "alias1"
    if var_name == "CTF_NAME":
        sample = _first_caibench_ctf_name_canonical()
        if sample:
            return sample
    if var_name.endswith("_IP") and "API" not in var_name:
        return "10.11.12.13"
    if "SUBNET" in var_name or var_name.endswith("_CIDR"):
        return "192.168.99.0/24"
    if var_name.endswith("_PORT") or var_name == "CAI_AUTH_DEVICE_PORT":
        return "9090"
    if var_name == "CAI_PARALLEL":
        return "2"
    if var_name == "CAI_DEBUG":
        return "1"
    if var_name == "CAI_TEMPERATURE":
        return "0.5"
    if var_name == "CAI_TOP_P":
        return "0.9"
    if var_name in ("CAI_MAX_TURNS", "CAI_MAX_INTERACTIONS"):
        return "inf"
    if var_name == "CAI_PRICE_LIMIT":
        return "2"
    if var_name in CATALOG_BOOL_VAR_NAMES:
        return "false"
    if var_name in ("ALIAS_API_KEY", "OPENAI_API_KEY", "OLLAMA_API_KEY"):
        return "test-key-placeholder"
    if var_name == "CAI_PARALLEL_EXTERNAL_TIMEOUT":
        return "900"
    if var_name == "CAI_TASK_RESET_PENDING":
        return "0"
    if var_name == "CAI_MERGE_SUMMARIZE_PER_WORKER":
        return "1"
    if var_name == "CAI_MERGE_SUMMARIZE_MIN_MESSAGES":
        return "20"
    d = var_info.get("default")
    if d is not None and str(d).strip() != "":
        return str(d)
    return "z_test_ok"
