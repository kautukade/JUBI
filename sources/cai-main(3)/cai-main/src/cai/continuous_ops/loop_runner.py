"""Cross-platform continuous-ops worker loop (replaces generated bash).

Runs on Linux (incl. Kali), macOS, and WSL. Native Windows is not supported — use WSL.
Suspension/hibernation still freeze the CPU: no script can advance work while the machine sleeps.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from cai.continuous_ops.mechanical_summary import SUMMARY_REL, write_mechanical_summary_from_log
from cai.continuous_ops.task_queue import (
    ensure_task_queue_bootstrapped,
    mark_task_run,
    merge_appends_from_log_file,
    pick_next_task,
)
from cai.config import DEFAULT_AGENT_TYPE
from cai.continuous_ops.tick_context import (
    extract_tick_context_from_file,
    persist_tick_context,
    rolling_context_for_prompt,
)

CONFIG_NAME = "run_loop_config.json"

# Set only in wizard-generated systemd user units; loop_runner then forces plain text in tick/recovery logs.
_PLAIN_LOG_FLAG = "CAI_CONTINUOUS_OPS_SYSTEMD_PLAIN_LOG"


def _cai_subprocess_env_from_os_environ() -> dict[str, str]:
    """Copy of the process environment for ``cai`` ticks; plain logs when *_PLAIN_LOG_FLAG* is set (systemd unit)."""
    env = os.environ.copy()
    if os.environ.get(_PLAIN_LOG_FLAG, "").strip().lower() in ("1", "true", "yes", "on"):
        env["NO_COLOR"] = "1"
        env["FORCE_COLOR"] = "0"
        env["CLICOLOR"] = "0"
    return env

# Exit code when the tick subprocess is killed for exceeding the wall clock (similar to GNU ``timeout``).
CAI_TICK_WALL_RC = 124


def tick_wall_timeout_seconds(tick_seconds: int) -> float:
    """Maximum wall time for one ``cai`` subprocess: ``max(120, 2 * TICK)`` seconds."""
    return float(max(120, 2 * int(tick_seconds)))


def effective_privileged_worker(cfg: LoopConfig) -> bool:
    """True only when config asks for privileges and CAI_AVOID_SUDO is not forcing a non-root shell policy."""
    avoid = os.environ.get("CAI_AVOID_SUDO", "").strip().lower() in ("1", "true", "yes", "on")
    return bool(cfg.privileged) and not avoid


@dataclass(frozen=True)
class LoopConfig:
    tick_seconds: int
    tick_prompt: str
    privileged: bool
    cai_argv: list[str]
    python_interpreter: str
    log_full_days: int
    log_delete_after_days: int
    entry_script: str
    worker_agent_type: str

    @staticmethod
    def load(run_dir: Path) -> LoopConfig:
        raw = json.loads((run_dir / CONFIG_NAME).read_text(encoding="utf-8"))
        _wt = str(raw.get("worker_agent_type", "") or "").strip()
        return LoopConfig(
            tick_seconds=int(raw["tick_seconds"]),
            tick_prompt=str(raw["tick_prompt"]),
            privileged=bool(raw["privileged"]),
            cai_argv=[str(x) for x in raw["cai_argv"]],
            python_interpreter=str(raw["python_interpreter"]),
            log_full_days=int(raw.get("log_full_days", 7)),
            log_delete_after_days=int(raw.get("log_delete_after_days", 15)),
            entry_script=str(raw.get("entry_script", "run_loop.py")),
            worker_agent_type=_wt or "blueteam_agent",
        )


def _ts_human() -> str:
    try:
        return datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")
    except Exception:
        return datetime.now(timezone.utc).isoformat(timespec="seconds")


def _ignore_hup() -> None:
    if hasattr(signal, "SIGHUP"):
        try:
            signal.signal(signal.SIGHUP, signal.SIG_IGN)
        except (ValueError, OSError):
            pass


def _pause_if_requested(run_dir: Path) -> None:
    pause = run_dir / "state" / "PAUSE"
    while pause.is_file():
        time.sleep(2)


def _stop_requested(run_dir: Path, entry_path: Path | None) -> bool:
    if not (run_dir / "state" / "STOP").is_file():
        return False
    if entry_path and entry_path.is_file():
        try:
            entry_path.unlink()
        except OSError:
            pass
    return True


def _infer_framework_dotenv_paths(cfg: LoopConfig) -> list[Path]:
    """``run_loop`` ``chdir``s into *run_dir* (often no ``.env``); discover repo ``.env`` from venv layout."""
    out: list[Path] = []
    for seed in (cfg.cai_argv[0] if cfg.cai_argv else "", cfg.python_interpreter):
        s = (seed or "").strip()
        if not s:
            continue
        try:
            p = Path(s).expanduser().resolve()
        except (OSError, ValueError):
            continue
        try:
            if p.is_file() and p.parent.name == "bin" and p.parent.parent.name == "cai_env":
                envf = p.parent.parent.parent / ".env"
                if envf.is_file():
                    out.append(envf)
        except (OSError, ValueError):
            continue
    return out


def _load_dotenv_for_loop(cfg: LoopConfig, run_dir: Path) -> None:
    """Load API keys before ``chdir`` so recovery pings match the ``cai`` subprocess."""
    try:
        from dotenv import load_dotenv
    except ImportError:
        return
    seen: set[Path] = set()
    candidates = [
        *_infer_framework_dotenv_paths(cfg),
        run_dir / ".env",
        Path.home() / ".cai" / ".env",
    ]
    for envf in candidates:
        try:
            rp = envf.resolve()
        except OSError:
            continue
        if rp in seen or not envf.is_file():
            continue
        seen.add(rp)
        load_dotenv(dotenv_path=rp, verbose=False)


def _ping_model(py: str) -> bool:
    """Lightweight reachability check; Alias models (``alias1``/``2``/``3``) use ``ALIAS_API_KEY`` only."""
    # Match ``httpx_client.direct_httpx_completion`` URL + auth (not ``OpenAI()`` defaults).
    code = (
        "import os,sys\n"
        "try:\n"
        " import httpx\n"
        " key=(os.getenv('ALIAS_API_KEY') or '').strip()\n"
        " if not key:\n"
        "  sys.exit(1)\n"
        " from cai.util.llm_api_base import resolve_llm_openai_compatible_base\n"
        " m=os.environ.get('CAI_MODEL','alias1')\n"
        " base=resolve_llm_openai_compatible_base(m).rstrip('/')\n"
        " url=f\"{base}/chat/completions\"\n"
        " body={\"model\":m,\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}],\"max_tokens\":1}\n"
        " h={\"Authorization\":f\"Bearer {key}\",\"Content-Type\":\"application/json\"}\n"
        " r=httpx.post(url,headers=h,json=body,timeout=60.0)\n"
        " sys.exit(0 if r.status_code==200 else 1)\n"
        "except Exception:\n"
        " sys.exit(1)\n"
    )
    try:
        r = subprocess.run([py, "-c", code], cwd=os.getcwd(), capture_output=True, timeout=120)
        return r.returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def _recover_until_api_ready(py: str) -> None:
    print(
        "[CAI continuous ops] Tick failed or API unreachable — pausing; "
        "will ping every 60s until the model responds.",
        file=sys.stderr,
    )
    while True:
        if _ping_model(py):
            print("[CAI continuous ops] Model reachable again — resuming iterations.", file=sys.stderr)
            return
        time.sleep(60)


def _tail_file_text(path: Path, max_lines: int) -> str:
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return "(no log tail)"
    if len(lines) <= max_lines:
        return "\n".join(lines)
    return "\n".join(lines[-max_lines:])


def _write_tick_prompt_file(run_dir: Path, cfg: LoopConfig) -> str | None:
    """Write ``current_tick_prompt.txt``; return task id picked for this tick (if any)."""
    cur = run_dir / "state" / "current_tick_prompt.txt"
    cur.parent.mkdir(parents=True, exist_ok=True)
    hint = run_dir / "state" / "model_recovery_hint.txt"
    tick_prompt = cfg.tick_prompt.rstrip("\n")
    task_id, task_text = pick_next_task(run_dir)
    parts: list[str] = []
    if task_text:
        parts.append(
            "[Continuous ops — single task for this tick]\n"
            "Execute the following sub-task fully within this tick (including tool rounds). "
            "The full mission text for reference is below this block.\n\n"
            f"{task_text.strip()}\n"
        )
        parts.append("\n--- Full mission (reference) ---\n")
    parts.append(tick_prompt)
    mech = run_dir.resolve() / SUMMARY_REL
    if mech.is_file() and mech.stat().st_size > 0:
        parts.append("\n--- Mechanical summary (prior tick, redacted) ---\n")
        parts.append(mech.read_text(encoding="utf-8", errors="replace").rstrip("\n"))
    prior = rolling_context_for_prompt(run_dir)
    if prior:
        parts.append(prior.rstrip("\n"))
    if hint.is_file() and hint.stat().st_size > 0:
        parts.append("")
        parts.append(
            "[Model auto-recovery — context from a previous failed tick; fold into this tick if still relevant]"
        )
        parts.append(hint.read_text(encoding="utf-8", errors="replace").rstrip("\n"))
    cur.write_text("\n".join(parts) + "\n", encoding="utf-8")
    os.environ["CAI_SINGLE_SHOT_STDIN_PROMPT_FILE"] = str(cur.resolve())
    return task_id


def _link_latest_log(logs_full: Path, log_file: Path) -> None:
    latest = logs_full / "latest.log"
    try:
        if latest.is_symlink() or latest.exists():
            latest.unlink()
    except OSError:
        pass
    try:
        latest.symlink_to(log_file.name)
    except OSError:
        try:
            shutil.copyfile(log_file, latest)
        except OSError:
            pass


def _run_cai_with_tee(
    argv: list[str],
    log_file: Path,
    env: dict[str, str],
    cwd: Path,
    *,
    timeout_sec: float,
) -> int:
    """Run CAI; stream stdout+stderr to *log_file* and this process; kill after *timeout_sec* wall clock."""
    log_file.parent.mkdir(parents=True, exist_ok=True)
    stdbuf = shutil.which("stdbuf")
    if stdbuf:
        argv = [stdbuf, "-oL", "-eL", *argv]
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0) if sys.platform == "win32" else 0
    try:
        p = subprocess.Popen(
            argv,
            cwd=str(cwd),
            env=env,
            stdin=sys.stdin,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            creationflags=creationflags,
        )
    except OSError as e:
        print(f"[CAI continuous ops] failed to spawn cai: {e}", file=sys.stderr)
        return 127
    assert p.stdout

    def _pump() -> None:
        try:
            with log_file.open("ab") as lf:
                while True:
                    chunk = p.stdout.read(8192)
                    if not chunk:
                        break
                    lf.write(chunk)
                    lf.flush()
                    try:
                        sys.stdout.buffer.write(chunk)
                        sys.stdout.buffer.flush()
                    except (BrokenPipeError, ValueError):
                        pass
        except OSError:
            pass

    pump = threading.Thread(target=_pump, daemon=True)
    pump.start()
    pump.join(timeout=timeout_sec)
    if pump.is_alive():
        print(
            f"[CAI continuous ops] tick wall limit ({timeout_sec:.0f}s = max(120, 2×TICK)) exceeded — "
            "terminating cai; next iteration will run after the usual sleep.",
            file=sys.stderr,
        )
        try:
            p.kill()
        except OSError:
            pass
        try:
            pump.join(timeout=30.0)
        except RuntimeError:
            pass
        try:
            p.wait(timeout=20)
        except subprocess.TimeoutExpired:
            pass
        return CAI_TICK_WALL_RC

    try:
        return int(p.wait(timeout=5))
    except subprocess.TimeoutExpired:
        return int(p.poll() or 0)


def _maintain_logs(py: str, logs_dir: Path) -> None:
    try:
        subprocess.run(
            [py, "-m", "cai.continuous_ops.log_maintenance", str(logs_dir)],
            check=False,
            timeout=600,
            capture_output=True,
        )
    except (OSError, subprocess.TimeoutExpired):
        pass


def _ensure_latest_log_placeholder(run_dir: Path) -> None:
    """So ``less +F …/logs/full/latest.log`` works before the first tick opens its log file."""
    logs_full = run_dir / "logs" / "full"
    logs_full.mkdir(parents=True, exist_ok=True)
    boot = logs_full / "_bootstrap_placeholder.log"
    try:
        if not boot.is_file():
            boot.write_text(
                "[CAI continuous ops] Placeholder — first tick log not created yet.\n",
                encoding="utf-8",
            )
        _link_latest_log(logs_full, boot)
    except OSError:
        pass


def _write_preloop_summary_stub(
    run_dir: Path, start_ts: float, start_human: str, tick_seconds: int, tick_wall: float
) -> None:
    """Create ``summary.txt`` before the first tick so ``watch cat …/summary.txt`` works immediately."""
    summary_dir = run_dir / "logs" / "summary"
    summary_dir.mkdir(parents=True, exist_ok=True)
    lines = [
        "=== Continuous ops summary ===",
        f"process_start_human: {start_human}",
        f"last_update_human: {_ts_human()}",
        f"elapsed_seconds: {int(time.time() - start_ts)}",
        "model_available_seconds: 0",
        "model_available_pct: 0",
        "last_tick_run_seconds: 0",
        "iterations_total: 0",
        "anomalies_total: 0",
        "last_exit_code: -1",
        "session_note: Worker process reached the main loop; the first CAI subprocess tick has not finished yet. "
        "If this line never changes to show iterations_total >= 1, inspect stderr/journalctl — the tick may be "
        "failing immediately (e.g. missing API keys, wrong python on PATH under systemd).",
        "tick_note: Wall period is max(TICK, last tick duration) + recovery; TICK is minimum idle after each tick completes. "
        f"Per-tick hard wall: max(120, 2×TICK) = {tick_wall:.0f}s; overrun kills the cai subprocess (exit {CAI_TICK_WALL_RC}).",
        "live_log: Summary: watch -n2 cat logs/summary/summary.txt  |  Current tick: less +F logs/full/latest.log",
        "last_10_iterations:",
        "",
    ]
    body = "\n".join(lines) + "\n"
    tmp = summary_dir / f"summary.new.{os.getpid()}.tmp"
    dst = summary_dir / "summary.txt"
    tmp.write_text(body, encoding="utf-8")
    tmp.replace(dst)


def _update_summary(
    run_dir: Path,
    start_ts: float,
    start_human: str,
    iteration: int,
    last_rc: int,
    last_iter_sec: int,
    agg_model_up: int,
    anomalies: int,
) -> None:
    summary_dir = run_dir / "logs" / "summary"
    summary_dir.mkdir(parents=True, exist_ok=True)
    iter_lines = summary_dir / "iter_lines.txt"
    now_ts = time.time()
    elapsed = int(now_ts - start_ts)
    pct = 0
    if elapsed > 0:
        pct = min(100, int(100 * agg_model_up / elapsed))
    tail_lines = ""
    if iter_lines.is_file():
        raw = iter_lines.read_text(encoding="utf-8", errors="replace").splitlines()
        tail_lines = "\n".join(raw[-10:])
    lines = [
        "=== Continuous ops summary ===",
        f"process_start_human: {start_human}",
        f"last_update_human: {_ts_human()}",
        f"elapsed_seconds: {elapsed}",
        f"model_available_seconds: {agg_model_up}",
        f"model_available_pct: {pct}",
        f"last_tick_run_seconds: {last_iter_sec}",
        f"iterations_total: {iteration}",
        f"anomalies_total: {anomalies}",
        f"last_exit_code: {last_rc}",
        "session_note: Suspend/hibernate freeze all ticks until wake. GUI logout can kill desktop terminals; "
        "prefer SSH+tmux detach, systemd --user, or loginctl enable-linger.",
        "tick_note: Wall period is max(TICK, last tick duration) + recovery; TICK is minimum idle after each tick completes. "
        f"Per-tick hard wall: max(120, 2×TICK) seconds; overrun kills the cai subprocess (exit {CAI_TICK_WALL_RC}).",
        "live_log: Summary: watch -n2 cat logs/summary/summary.txt  |  Current tick: less +F logs/full/latest.log",
        "last_10_iterations:",
        tail_lines,
    ]
    body = "\n".join(lines) + "\n"
    tmp = summary_dir / f"summary.new.{os.getpid()}.tmp"
    dst = summary_dir / "summary.txt"
    tmp.write_text(body, encoding="utf-8")
    tmp.replace(dst)


def _model_recovery_turn(
    cfg: LoopConfig,
    run_dir: Path,
    iteration: int,
    last_rc: int,
    logfull: Path,
    *,
    tick_timeout_sec: float,
) -> None:
    recf = run_dir / "state" / "recovery_prompt_once.txt"
    reclog = run_dir / "logs" / "full" / f"recovery_iter_{iteration}.log"
    reclog.parent.mkdir(parents=True, exist_ok=True)
    logtail = _tail_file_text(logfull, 120)
    recf.write_text(
        "[Continuous ops auto-recovery — single diagnostic turn]\n"
        f"The scheduled monitoring tick (iter {iteration}) exited with code {last_rc}.\n"
        "Respect the same operator policies as normal ticks (including NO_SUDO when configured).\n"
        "Tasks: (1) Likely root cause in one short paragraph. (2) Concrete adjustments for the NEXT tick.\n"
        "(3) Optional read-only verification commands. Plain text, at most ~800 words.\n\n"
        "--- tail of iteration log (last 120 lines) ---\n"
        f"{logtail}\n",
        encoding="utf-8",
    )
    os.environ["CAI_SINGLE_SHOT_STDIN_PROMPT_FILE"] = str(recf.resolve())
    env = os.environ.copy()
    env["CAI_SINGLE_SHOT_STDIN_PROMPT_FILE"] = str(recf.resolve())
    env["PYTHONUNBUFFERED"] = "1"
    print("[CAI continuous ops] Running model auto-recovery turn…", file=sys.stderr)
    rc = _run_cai_with_tee(
        [*cfg.cai_argv, "--prompt", "recovery"],
        reclog,
        env,
        run_dir,
        timeout_sec=tick_timeout_sec,
    )
    try:
        tail = reclog.read_bytes()
        if len(tail) > 20_000:
            tail = tail[-20_000:]
        (run_dir / "state" / "model_recovery_hint.txt").write_bytes(tail)
    except OSError:
        pass
    print("[CAI continuous ops] Auto-recovery turn finished (hint saved for next tick).", file=sys.stderr)
    if rc != 0:
        print(f"[CAI continuous ops] recovery turn exit={rc}", file=sys.stderr)


def _sudo_refresh() -> None:
    try:
        subprocess.run(["sudo", "-n", "-v"], check=False, timeout=30, capture_output=True)
    except (OSError, subprocess.TimeoutExpired):
        pass


def main(run_dir: Path, *, entry_path: Path | None = None) -> int:
    run_dir = run_dir.resolve()
    cfg = LoopConfig.load(run_dir)
    if entry_path is None:
        entry_path = run_dir / cfg.entry_script
    _load_dotenv_for_loop(cfg, run_dir)
    os.chdir(run_dir)

    _ignore_hup()
    os.environ.setdefault("PYTHONUNBUFFERED", "1")
    os.environ["CAI_COPS_LOG_FULL_DAYS"] = str(cfg.log_full_days)
    os.environ["CAI_COPS_LOG_DELETE_AFTER_DAYS"] = str(cfg.log_delete_after_days)
    os.environ["CAI_CONTINUOUS_OPS_TICK_SECONDS"] = str(cfg.tick_seconds)
    os.environ.setdefault("FORCE_COLOR", os.environ.get("FORCE_COLOR", "1"))
    os.environ.setdefault("TERM", os.environ.get("TERM", "xterm-256color"))
    os.environ.pop("NO_COLOR", None)
    # Config may still say privileged=true from older wizards; CAI_AVOID_SUDO (e.g. systemd unit) means no sudo/TTY.
    effective_privileged = effective_privileged_worker(cfg)
    if os.environ.get("CAI_AVOID_SUDO", "").strip().lower() in ("1", "true", "yes", "on") and cfg.privileged:
        print(
            "[CAI continuous ops] run_loop_config has privileged=true but CAI_AVOID_SUDO is set — "
            "skipping sudo bootstrap; ticks run without elevation (typical under systemd --user).",
            file=sys.stderr,
        )

    if not effective_privileged:
        os.environ["CAI_CONTINUOUS_OPS_NO_SUDO"] = "true"
    else:
        os.environ.pop("CAI_CONTINUOUS_OPS_NO_SUDO", None)

    # Worker agent is configurable via env or run config. Default: selection_agent (handoff router).
    # Selection/orchestration agents need 'auto' routing; pinned workers (e.g. blueteam_agent)
    # run their own tools per tick without handoff noise.
    wt = (os.environ.get("CAI_CONTINUOUS_OPS_WORKER_AGENT_TYPE") or "").strip() or cfg.worker_agent_type
    wt = (wt or DEFAULT_AGENT_TYPE).strip() or DEFAULT_AGENT_TYPE
    os.environ["CAI_AGENT_TYPE"] = wt
    os.environ["CAI_AGENT_ROUTE_MODE"] = "auto" if wt in ("selection_agent", "orchestration_agent") else "pinned"
    os.environ["CAI_SINGLE_SHOT_CLI"] = "true"
    os.environ["CAI_CONTINUOUS_OPS_LOOP_CHILD"] = "1"

    py = cfg.python_interpreter
    for d in ("logs/full", "logs/summary", "state"):
        (run_dir / d).mkdir(parents=True, exist_ok=True)

    ensure_task_queue_bootstrapped(run_dir, cfg.tick_prompt)

    start_ts = time.time()
    start_human = _ts_human()

    if effective_privileged:
        print("", file=sys.stderr)
        print(
            "[CAI continuous ops] Privileged worker: validate sudo once for this terminal session.",
            file=sys.stderr,
        )
        try:
            r = subprocess.run(["sudo", "-v"], timeout=300)
            if r.returncode != 0:
                print("[CAI continuous ops] sudo -v failed — cannot run a privileged worker without sudo.", file=sys.stderr)
                return 1
        except (OSError, subprocess.TimeoutExpired):
            print("[CAI continuous ops] sudo -v failed — cannot run a privileged worker without sudo.", file=sys.stderr)
            return 1

    iteration = 0
    anomalies = 0
    agg_model_up = 0
    tick_wall = tick_wall_timeout_seconds(cfg.tick_seconds)
    _write_preloop_summary_stub(run_dir, start_ts, start_human, cfg.tick_seconds, tick_wall)
    _ensure_latest_log_placeholder(run_dir)

    while True:
        if _stop_requested(run_dir, entry_path):
            return 0
        _pause_if_requested(run_dir)
        if _stop_requested(run_dir, entry_path):
            return 0

        iteration += 1
        ts_file = datetime.now().strftime("%Y%m%d_%H%M%S")
        logs_full = run_dir / "logs" / "full"
        logfull = logs_full / f"iter_{iteration}_{ts_file}.log"

        iter_start = time.time()

        snap_path = run_dir / "state" / "session_snapshot.json"
        if snap_path.is_file():
            os.environ["CAI_COPS_SNAPSHOT_IN"] = str(snap_path.resolve())
        else:
            os.environ.pop("CAI_COPS_SNAPSHOT_IN", None)
        os.environ["CAI_COPS_SNAPSHOT_OUT"] = str(snap_path.resolve())

        active_task_id = _write_tick_prompt_file(run_dir, cfg)
        if effective_privileged:
            _sudo_refresh()

        _link_latest_log(logs_full, logfull)
        print(
            f"[CAI continuous ops] tick START iter={iteration} ts={_ts_human()} "
            f"log=logs/full/{logfull.name}  wall_limit={tick_wall:.0f}s  "
            "(watch summary: watch -n2 cat logs/summary/summary.txt)",
            file=sys.stderr,
        )

        env = _cai_subprocess_env_from_os_environ()
        env["CAI_SINGLE_SHOT_STDIN_PROMPT_FILE"] = os.environ.get("CAI_SINGLE_SHOT_STDIN_PROMPT_FILE", "")
        env["PYTHONUNBUFFERED"] = "1"
        argv = [*cfg.cai_argv, "--prompt", cfg.tick_prompt]
        last_rc = _run_cai_with_tee(argv, logfull, env, run_dir, timeout_sec=tick_wall)

        print(f"[CAI continuous ops] tick END   iter={iteration} ts={_ts_human()} exit={last_rc}", file=sys.stderr)

        iter_end = time.time()
        last_iter_sec = int(iter_end - iter_start)

        try:
            write_mechanical_summary_from_log(logfull, run_dir)
        except OSError:
            pass
        try:
            merge_appends_from_log_file(logfull, run_dir)
        except OSError:
            pass

        if last_rc == 0:
            agg_model_up += last_iter_sec
            if active_task_id:
                try:
                    mark_task_run(run_dir, active_task_id)
                except OSError:
                    pass
            hint = run_dir / "state" / "model_recovery_hint.txt"
            try:
                hint.unlink()
            except OSError:
                pass
            ctx_block = extract_tick_context_from_file(logfull)
            if ctx_block:
                try:
                    persist_tick_context(run_dir, ctx_block)
                except OSError:
                    pass
        else:
            anomalies += 1

        # Persist summary before API recovery / recovery-model turns so operators always see
        # iter_lines + summary.txt (e.g. ``watch cat logs/summary/summary.txt``) even when
        # ``_recover_until_api_ready`` blocks for a long time on failed pings.
        with (run_dir / "logs" / "summary" / "iter_lines.txt").open("a", encoding="utf-8") as il:
            dur = last_iter_sec
            il.write(f"iter={iteration} ts={_ts_human()} rc={last_rc} dur_s={dur}\n")

        _maintain_logs(py, run_dir / "logs")
        _update_summary(run_dir, start_ts, start_human, iteration, last_rc, last_iter_sec, agg_model_up, anomalies)

        if last_rc != 0:
            _recover_until_api_ready(py)
            _model_recovery_turn(cfg, run_dir, iteration, last_rc, logfull, tick_timeout_sec=tick_wall)

        spent = int(time.time() - iter_start)
        wait_s = max(1, int(cfg.tick_seconds) - spent)
        time.sleep(wait_s)


def _cli() -> int:
    ap = argparse.ArgumentParser(description="CAI Continuous Ops worker loop")
    ap.add_argument("--run-dir", type=Path, required=True, help="Run directory containing run_loop_config.json")
    args = ap.parse_args()
    try:
        return main(args.run_dir.resolve(), entry_path=None)
    except Exception:  # pylint: disable=broad-except
        import traceback

        print("[CAI continuous ops] Fatal error — traceback follows.", file=sys.stderr)
        traceback.print_exc(file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(_cli())
