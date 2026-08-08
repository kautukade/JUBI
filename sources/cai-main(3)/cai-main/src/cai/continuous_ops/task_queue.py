"""Discrete task queue for continuous-ops ticks (JSON on disk).

Cadence policy (B2): a task is *due* when it has never run or
``now - last_run_at >= repeat_after_seconds``. Among due tasks we pick the
oldest ``last_run_at`` (or never-run first). If none are due, we pick the task
whose ``last_run_at`` is oldest overall (most stale).

Model appends use delimited JSON in tick logs::

    <<<COPS_TASK_APPEND>>>
    [{"id": "x", "text": "...", "repeat_after_seconds": 3600}]
    <<<END_COPS_TASK_APPEND>>>
"""

from __future__ import annotations

import json
import logging
import re
import uuid
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from cai.continuous_ops.model_parse import MissionPlan, normalized_tasks

_LOG = logging.getLogger(__name__)

QUEUE_REL = Path("state") / "task_queue.json"
SCHEMA_VERSION = 1

MARK_BEGIN = "<<<COPS_TASK_APPEND>>>"
MARK_END = "<<<END_COPS_TASK_APPEND>>>"


def _utcnow() -> datetime:
    return datetime.now(timezone.utc)


def _parse_ts(raw: str | None) -> datetime | None:
    if not raw or not str(raw).strip():
        return None
    s = str(raw).strip()
    try:
        if s.endswith("Z"):
            s = s[:-1] + "+00:00"
        return datetime.fromisoformat(s)
    except ValueError:
        return None


def _iso(dt: datetime | None) -> str | None:
    if dt is None:
        return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(timezone.utc).isoformat()


@dataclass
class TaskItem:
    id: str
    text: str
    repeat_after_seconds: int
    last_run_at: datetime | None
    created_at: datetime

    @staticmethod
    def from_dict(d: dict[str, Any]) -> TaskItem | None:
        try:
            tid = str(d.get("id") or "").strip()
            text = str(d.get("text") or "").strip()
            if not tid or not text:
                return None
            rep = int(d.get("repeat_after_seconds") or 3600)
            rep = max(60, min(86400 * 30, rep))
            lr = _parse_ts(str(d.get("last_run_at") or "") or None)
            cr = _parse_ts(str(d.get("created_at") or "") or None) or _utcnow()
            return TaskItem(id=tid, text=text, repeat_after_seconds=rep, last_run_at=lr, created_at=cr)
        except (TypeError, ValueError):
            return None

    def to_dict(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "text": self.text,
            "repeat_after_seconds": self.repeat_after_seconds,
            "last_run_at": _iso(self.last_run_at),
            "created_at": _iso(self.created_at) or _iso(_utcnow()),
        }


def queue_path(run_dir: Path) -> Path:
    return run_dir.resolve() / QUEUE_REL


def load_queue(run_dir: Path) -> dict[str, Any] | None:
    p = queue_path(run_dir)
    if not p.is_file():
        return None
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        _LOG.warning("task_queue: could not load %s: %s", p, e)
        return None


def save_queue(run_dir: Path, data: dict[str, Any]) -> None:
    p = queue_path(run_dir)
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_suffix(f".tmp.{uuid.uuid4().hex[:8]}")
    tmp.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    tmp.replace(p)


def _items_from_raw(raw: dict[str, Any]) -> list[TaskItem]:
    out: list[TaskItem] = []
    for d in raw.get("tasks") or []:
        if isinstance(d, dict):
            it = TaskItem.from_dict(d)
            if it:
                out.append(it)
    return out


def ensure_task_queue_bootstrapped(run_dir: Path, tick_prompt: str) -> None:
    """If no queue file exists (legacy run), create a single-task queue from *tick_prompt*."""
    if queue_path(run_dir).is_file():
        return
    if not (tick_prompt or "").strip():
        return
    now = _utcnow()
    items = [
        TaskItem(
            id=f"bootstrap_{uuid.uuid4().hex[:8]}",
            text=tick_prompt.strip(),
            repeat_after_seconds=3600,
            last_run_at=None,
            created_at=now,
        )
    ]
    save_queue(
        run_dir,
        {"schema_version": SCHEMA_VERSION, "tasks": [x.to_dict() for x in items], "cursor": 0},
    )


def initialize_from_plan(run_dir: Path, plan: MissionPlan) -> None:
    """E1: structured tasks from planner → queue; else E2 single bootstrap task from tick text."""
    tasks_txt = normalized_tasks(plan)
    base_prompt = (plan.refined_tick_prompt or plan.tasks_markdown or "").strip()
    items: list[TaskItem] = []
    now = _utcnow()
    if tasks_txt:
        for i, line in enumerate(tasks_txt):
            tid = f"t_{i+1}_{uuid.uuid4().hex[:6]}"
            items.append(
                TaskItem(
                    id=tid,
                    text=line.strip(),
                    repeat_after_seconds=3600,
                    last_run_at=None,
                    created_at=now,
                )
            )
    elif base_prompt:
        items.append(
            TaskItem(
                id=f"bootstrap_{uuid.uuid4().hex[:8]}",
                text=base_prompt,
                repeat_after_seconds=3600,
                last_run_at=None,
                created_at=now,
            )
        )
    data = {
        "schema_version": SCHEMA_VERSION,
        "tasks": [x.to_dict() for x in items],
        "cursor": 0,
    }
    save_queue(run_dir, data)


def _seconds_since(last: datetime | None, now: datetime) -> float:
    if last is None:
        return float("inf")
    return max(0.0, (now - last.astimezone(timezone.utc)).total_seconds())


def pick_next_task(run_dir: Path) -> tuple[str | None, str | None]:
    """Return (task_id, task_text) or (None, None) if queue missing/empty."""
    raw = load_queue(run_dir)
    if not raw:
        return None, None
    items = _items_from_raw(raw)
    if not items:
        return None, None
    now = _utcnow()
    due: list[TaskItem] = []
    for it in items:
        age = _seconds_since(it.last_run_at, now)
        if it.last_run_at is None or age >= float(it.repeat_after_seconds):
            due.append(it)
    pick_pool = due if due else items
    # Oldest last_run first; never-run beats epoch
    def sort_key(it: TaskItem) -> tuple[float, str]:
        if it.last_run_at is None:
            return (-1.0, it.id)
        return (it.last_run_at.timestamp(), it.id)

    pick = sorted(pick_pool, key=sort_key)[0]
    return pick.id, pick.text


def mark_task_run(run_dir: Path, task_id: str) -> None:
    raw = load_queue(run_dir)
    if not raw:
        return
    items = _items_from_raw(raw)
    now = _utcnow()
    changed = False
    for it in items:
        if it.id == task_id:
            it.last_run_at = now
            changed = True
            break
    if not changed:
        return
    raw["tasks"] = [x.to_dict() for x in items]
    save_queue(run_dir, raw)


_RE_APPEND = re.compile(
    re.escape(MARK_BEGIN) + r"\s*(.*?)\s*" + re.escape(MARK_END),
    re.DOTALL | re.IGNORECASE,
)


def merge_appends_from_log(log_text: str, run_dir: Path) -> int:
    """Parse ``COPS_TASK_APPEND`` blocks from *log_text* and merge into queue. Returns count added."""
    raw = load_queue(run_dir)
    if not raw:
        raw = {"schema_version": SCHEMA_VERSION, "tasks": [], "cursor": 0}
    items = _items_from_raw(raw)
    existing_ids = {it.id for it in items}
    added = 0
    for m in _RE_APPEND.finditer(log_text):
        chunk = (m.group(1) or "").strip()
        if not chunk:
            continue
        try:
            data = json.loads(chunk)
        except json.JSONDecodeError:
            _LOG.debug("task_queue: skip non-JSON append block")
            continue
        if isinstance(data, dict):
            data = [data]
        if not isinstance(data, list):
            continue
        now = _utcnow()
        for entry in data:
            if not isinstance(entry, dict):
                continue
            tid = str(entry.get("id") or "").strip() or f"model_{uuid.uuid4().hex[:10]}"
            if tid in existing_ids:
                tid = f"{tid}_{uuid.uuid4().hex[:6]}"
            text = str(entry.get("text") or "").strip()
            if not text:
                continue
            try:
                rep = int(entry.get("repeat_after_seconds") or 3600)
            except (TypeError, ValueError):
                rep = 3600
            rep = max(60, min(86400 * 30, rep))
            items.append(
                TaskItem(id=tid, text=text, repeat_after_seconds=rep, last_run_at=None, created_at=now)
            )
            existing_ids.add(tid)
            added += 1
    if added:
        raw["tasks"] = [x.to_dict() for x in items]
        save_queue(run_dir, raw)
    return added


def merge_appends_from_log_file(log_path: Path, run_dir: Path) -> int:
    try:
        text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return 0
    return merge_appends_from_log(text, run_dir)
