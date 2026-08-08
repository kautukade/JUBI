"""Mechanical tick log summary (A2) for continuous-ops.

Writes ``state/mechanical_summary.txt``: tail of the log plus lines matching
high-signal patterns (status tags, numbered steps). Redacts obvious API key
fragments. Capped by bytes.
"""

from __future__ import annotations

import os
import re
from pathlib import Path

SUMMARY_REL = Path("state") / "mechanical_summary.txt"

# Lines matching any of these (substring) are kept in addition to the tail window.
_KEEP_SUBSTR = (
    "[STATUS:",
    "[status:",
    "### Step",
    "### step",
    "Decision Log",
    "tick END",
    "tick START",
    "[TICK_COMPLETE]",
)

_RE_SK = re.compile(r"sk-[A-Za-z0-9]{8,}")
_RE_BEARER = re.compile(r"Bearer\s+[A-Za-z0-9._\-+/=]{10,}", re.I)


def _env_int(name: str, default: int, lo: int, hi: int) -> int:
    try:
        v = int((os.getenv(name) or str(default)).strip())
    except ValueError:
        return default
    return max(lo, min(hi, v))


def _redact(text: str) -> str:
    text = _RE_SK.sub("sk-[REDACTED]", text)
    text = _RE_BEARER.sub("Bearer [REDACTED]", text)
    return text


def build_mechanical_summary(log_text: str) -> str:
    tail_lines = _env_int("CAI_COPS_MECHANICAL_SUMMARY_TAIL_LINES", 80, 10, 500)
    max_bytes = _env_int("CAI_COPS_MECHANICAL_SUMMARY_MAX_BYTES", 24_000, 2048, 200_000)
    lines = log_text.splitlines()
    tail = lines[-tail_lines:] if len(lines) > tail_lines else lines
    keep_extra: list[str] = []
    for ln in lines:
        low = ln.lower()
        if any(s.lower() in low for s in _KEEP_SUBSTR):
            keep_extra.append(ln)
    merged: list[str] = []
    seen = set()
    for ln in keep_extra + ["--- tail ---"] + tail:
        if ln in seen and ln != "--- tail ---":
            continue
        seen.add(ln)
        merged.append(ln)
    body = _redact("\n".join(merged)).strip() + "\n"
    if len(body.encode("utf-8")) > max_bytes:
        enc = body.encode("utf-8")
        body = enc[-max_bytes:].decode("utf-8", errors="replace")
        body = "[… truncated mechanical summary …]\n" + body
    return body


def write_mechanical_summary_from_log(log_path: Path, run_dir: Path) -> Path | None:
    try:
        raw = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    out = run_dir.resolve() / SUMMARY_REL
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(build_mechanical_summary(raw), encoding="utf-8")
    return out
