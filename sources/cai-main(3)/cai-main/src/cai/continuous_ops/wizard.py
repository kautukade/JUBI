"""CLI onboarding wizard for the Continuous Ops agent (English UX, Alias palette)."""

from __future__ import annotations

import math
import os
import shlex
import threading
from dataclasses import replace
import shutil
import subprocess
import sys
import uuid
from pathlib import Path

from rich.console import Console
from rich.markup import escape
from rich.panel import Panel
from rich.text import Text

from cai.config import DEFAULT_AGENT_TYPE
from cai.continuous_ops.model_parse import (
    MissionPlan,
    needs_task_collection,
    normalized_tasks,
    parse_mission_with_planner,
    summary_iteration_tasks,
)
from cai.continuous_ops.rate_plan import resolve_rate_tier
from cai.continuous_ops.scriptgen import default_cai_argv, render_loop_script
from cai.continuous_ops.task_queue import initialize_from_plan
from cai.continuous_ops.terminal_launch import detect_external_terminal_backend, spawn_external_terminal
from cai.util.cli_palette import (
    BANNER_PROMO_YELLOW,
    CAI_GREEN,
    FINAL_PANEL_BG,
    GREY_HINT,
    GREY_TEXT,
    YELLOW_WARN,
)


def _run_dir_as_tilde(run_dir: Path) -> str:
    """``~/.cai/...`` when under the current user's home; else absolute."""
    try:
        rel = run_dir.resolve().relative_to(Path.home().resolve())
        return "~/" + rel.as_posix()
    except (ValueError, OSError):
        return str(run_dir.resolve())


def _probe_cai_import_on_python(python_exe: str) -> tuple[bool, str]:
    """Return (ok, stderr_or_message) after trying to import CAI with the given interpreter."""
    try:
        # Drop inherited PYTHONPATH so a probe of ``/usr/bin/python3`` cannot succeed only because
        # the *wizard* process had ``…/repo/src`` on ``PYTHONPATH`` (common in dev shells). Linux venvs
        # often symlink ``venv/bin/python3`` → ``/usr/bin/python3*``; ``Path.resolve()`` would collapse
        # to the system path and falsely pick the wrong interpreter for ``run_loop.py``.
        env = os.environ.copy()
        env.pop("PYTHONPATH", None)
        proc = subprocess.run(
            [
                python_exe,
                "-c",
                "import cai; import cai.continuous_ops.loop_runner",
            ],
            capture_output=True,
            text=True,
            timeout=45,
            env=env,
        )
        if proc.returncode == 0:
            return True, ""
        err = (proc.stderr or proc.stdout or "").strip() or f"exit {proc.returncode}"
        return False, err
    except (OSError, subprocess.TimeoutExpired) as exc:
        return False, str(exc)


def _infer_pythonpath_for_running_cai() -> str | None:
    """If ``cai`` is loaded from a checkout (not ``site-packages``), return a ``PYTHONPATH`` dir for systemd.

    Typical layout: ``…/repo/src/cai/__init__.py`` → return ``…/repo/src`` so ``/usr/bin/python3 -m cai`` works
    when the operator started CAI from a repo without activating a venv.
    """
    try:
        import cai

        init = Path(cai.__file__).resolve()
    except Exception:
        return None
    low = str(init).lower()
    if "site-packages" in low:
        return None
    pkg = init.parent
    if pkg.name != "cai":
        return None
    if pkg.parent.name == "src":
        return str(pkg.parent.resolve())
    return str(pkg.parent.resolve())


def _probe_cai_import_minimal_env(
    python_exe: str, *, venv_root: str | None, pythonpath: str | None
) -> tuple[bool, str]:
    """Import check with a **small** env similar to ``systemd --user`` (no inherited PYTHONPATH from your shell)."""
    env: dict[str, str] = {
        "HOME": str(Path.home()),
        "USER": (os.environ.get("USER") or os.environ.get("LOGNAME") or "").strip(),
        "LANG": os.environ.get("LANG", "C.UTF-8"),
    }
    lc = (os.environ.get("LC_ALL") or "").strip()
    if lc:
        env["LC_ALL"] = lc
    if venv_root:
        vr = Path(venv_root).expanduser().resolve()
        env["VIRTUAL_ENV"] = str(vr)
        env["PATH"] = f"{vr / 'bin'}:/usr/local/bin:/usr/bin:/bin"
    else:
        env["PATH"] = "/usr/local/bin:/usr/bin:/bin"
    if pythonpath:
        env["PYTHONPATH"] = pythonpath
    try:
        proc = subprocess.run(
            [python_exe, "-c", "import cai; import cai.continuous_ops.loop_runner"],
            capture_output=True,
            text=True,
            timeout=45,
            env=env,
        )
        if proc.returncode == 0:
            return True, ""
        err = (proc.stderr or proc.stdout or "").strip() or f"exit {proc.returncode}"
        return False, err
    except (OSError, subprocess.TimeoutExpired) as exc:
        return False, str(exc)


def _worker_python_bin() -> str:
    """Interpreter for ``loop_runner`` / ``run_loop_config.json`` (prefer active venv over bare ``sys.executable``).

    systemd --user services inherit a minimal environment; if the wizard ran inside a venv, ``sys.executable``
    may still point at the venv, but on some hosts it does not. ``VIRTUAL_ENV`` is the reliable signal to pin
    the same interpreter that can import ``cai``.
    """
    ve = (os.environ.get("VIRTUAL_ENV") or "").strip()
    if ve:
        base = Path(ve).expanduser().resolve()
        for name in ("python", "python3"):
            cand = base / "bin" / name
            try:
                if cand.is_file():
                    return str(cand.expanduser().absolute())
            except OSError:
                continue
    try:
        return str(Path(sys.executable).expanduser().absolute())
    except OSError:
        return sys.executable


def _wizard_checkout_root_for_loop_python() -> Path | None:
    """Repository root when this module is loaded from a checkout (``…/src/cai/continuous_ops/wizard.py``)."""
    try:
        here = Path(__file__).resolve()
        root = here.parents[3]
        if (root / "pyproject.toml").is_file() and (root / "src" / "cai").is_dir():
            return root
    except (IndexError, OSError):
        return None
    return None


def _loop_python_for_run_script(cai_argv: list[str]) -> str:
    """Python that can ``import cai`` for ``run_loop.py`` shebang and ``python_interpreter`` in JSON.

    When the operator launches CAI via ``.../some_env/bin/cai`` but ``VIRTUAL_ENV`` is unset (common in IDEs),
    ``sys.executable`` may still be system Python — then ``run_loop.py`` would die with ``ModuleNotFoundError``
    before creating ``logs/``. Prefer the venv next to the ``cai`` launcher when its path looks like
    ``*/bin/cai`` **and** that interpreter can import ``cai`` (``/usr/bin/cai`` shims must not force
    ``/usr/bin/python3`` if it does not have CAI).

    If ``cai`` resolves to ``python -m cai`` or PATH is wrong, try the checkout's ``cai_env`` / ``.venv``.
    """
    tried: set[str] = set()

    def _consider(py: str) -> str | None:
        try:
            key = os.path.normpath(str(Path(py).expanduser().absolute()))
        except (OSError, ValueError):
            return None
        if key in tried:
            return None
        tried.add(key)
        ok, _err = _probe_cai_import_on_python(key)
        return key if ok else None

    try:
        if cai_argv:
            cai_p = Path(str(cai_argv[0])).expanduser().resolve()
            if cai_p.is_file() and cai_p.name == "cai" and cai_p.parent.name == "bin":
                for name in ("python3", "python"):
                    cand = cai_p.parent / name
                    if cand.is_file():
                        got = _consider(str(cand.expanduser().absolute()))
                        if got:
                            return got
    except (OSError, ValueError):
        pass

    if (
        len(cai_argv) >= 3
        and str(cai_argv[1]) == "-m"
        and str(cai_argv[2]).replace(".__main__", "") == "cai"
    ):
        got = _consider(str(cai_argv[0]))
        if got:
            return got

    got = _consider(_worker_python_bin())
    if got:
        return got

    root = _wizard_checkout_root_for_loop_python()
    if root is not None:
        for rel in (
            "cai_env/bin/python3",
            "cai_env/bin/python",
            ".venv/bin/python3",
            ".venv/bin/python",
        ):
            cand = root / rel
            try:
                if cand.is_file():
                    got = _consider(str(cand.expanduser().absolute()))
                    if got:
                        return got
            except OSError:
                continue

    return _worker_python_bin()


def _shell_activate_prefix() -> str:
    """Prefix for ``bash -lc`` so tmux / external terminals inherit the current venv."""
    ve = (os.environ.get("VIRTUAL_ENV") or "").strip()
    if not ve:
        return ""
    return f"source {shlex.quote(ve.rstrip('/') + '/bin/activate')} && "


def _blank(console: Console) -> None:
    console.print()


def _green_head(console: Console, title: str) -> None:
    console.print(Text(title, style=f"bold {CAI_GREEN}"))


def _promo_section_head(console: Console, title: str) -> None:
    """Section label — same amber accent as banner YOLO / ``--unrestricted`` promos."""
    console.print(Text(title, style=BANNER_PROMO_YELLOW))


def _grey(console: Console, text: str) -> None:
    console.print(Text.from_markup(text, style=GREY_TEXT))


def _hint(console: Console, text: str) -> None:
    console.print(Text.from_markup(text, style=GREY_HINT))


def _attempt_line(console: Console, attempt: int, max_attempts: int) -> None:
    console.print(Text(f"Attempt {attempt}/{max_attempts}", style=f"bold {CAI_GREEN}"))


def _prompt_yes_no(console: Console, question: str, *, max_attempts: int = 3) -> bool | None:
    """Return True/False or ``None`` if the user did not answer acceptably."""
    for a in range(1, max_attempts + 1):
        _attempt_line(console, a, max_attempts)
        raw = input(f"{question} [y/n]: ").strip().lower()
        if raw in ("y", "yes", "1"):
            return True
        if raw in ("n", "no", "0"):
            return False
        console.print(Text("Please answer with y/yes or n/no.", style=YELLOW_WARN))
    return None


def _prompt_seconds(console: Console, *, min_seconds: float, max_attempts: int = 3) -> int | None:
    for a in range(1, max_attempts + 1):
        _attempt_line(console, a, max_attempts)
        raw = input(
            f"Please, insert a higher time in seconds (Attempt {a}/{max_attempts}, "
            f"minimum {math.ceil(min_seconds)}): "
        ).strip()
        try:
            v = int(raw)
        except ValueError:
            console.print(Text("Invalid integer.", style=YELLOW_WARN))
            continue
        if v >= math.ceil(min_seconds):
            return v
        console.print(Text("Value is below the required minimum.", style=YELLOW_WARN))
    return None


def _prompt_positive_int(
    console: Console,
    question: str,
    *,
    minimum: int = 1,
    maximum: int = 3650,
    max_attempts: int = 3,
) -> int | None:
    for a in range(1, max_attempts + 1):
        _attempt_line(console, a, max_attempts)
        raw = input(f"{question} ").strip()
        try:
            v = int(raw)
        except ValueError:
            console.print(Text("Enter a positive integer only.", style=YELLOW_WARN))
            continue
        if minimum <= v <= maximum:
            return v
        console.print(Text(f"Value must be between {minimum} and {maximum}.", style=YELLOW_WARN))
    return None


def _which_tmux() -> str | None:
    return shutil.which("tmux")


def _install_tmux(console: Console) -> bool:
    if shutil.which("brew") and sys.platform == "darwin":
        try:
            subprocess.run(["brew", "install", "tmux"], check=False, timeout=600)
            return _which_tmux() is not None
        except (OSError, subprocess.SubprocessError):
            return False
    if shutil.which("apt-get"):
        try:
            r = subprocess.run(
                ["sudo", "apt-get", "install", "-y", "tmux"],
                check=False,
                timeout=600,
            )
            return r.returncode == 0 and _which_tmux() is not None
        except (OSError, subprocess.SubprocessError):
            return False
    console.print(
        Text(
            "Could not auto-install tmux (no supported package manager). "
            "Install tmux manually, then re-run this agent.",
            style=YELLOW_WARN,
        )
    )
    return False


def _tasks_intro_bullets(plan: MissionPlan) -> str:
    tasks = normalized_tasks(plan)
    if not tasks:
        return "• Task: [italic](not identified)[/italic]"
    if len(tasks) == 1:
        line = tasks[0].strip()
        if len(line) > 160:
            line = line[:157] + "..."
        return f"• Task: {escape(line)}"
    lines = ["• Tasks:"]
    for i, t in enumerate(tasks, 1):
        one = t.strip()
        if len(one) > 140:
            one = one[:137] + "..."
        lines.append(f"  {i}. {escape(one)}")
    return "\n".join(lines)


def _prompt_multiline_tasks(console: Console, *, max_attempts: int = 3) -> str | None:
    for attempt in range(1, max_attempts + 1):
        _attempt_line(console, attempt, max_attempts)
        console.print(
            Text(
                "Enter at least one concrete task the worker should perform on every tick. "
                "Several lines are OK (one task per line, or a short paragraph); finish with a blank line.",
                style=GREY_TEXT,
            )
        )
        lines: list[str] = []
        while True:
            try:
                ln = input()
            except (EOFError, KeyboardInterrupt):
                return None
            if ln == "":
                break
            lines.append(ln)
        blob = "\n".join(lines).strip()
        if blob and not all(len(x.strip()) < 3 for x in blob.splitlines() if x.strip()):
            return blob
        console.print(Text("That input is too short or empty — please describe a real task.", style=YELLOW_WARN))
    return None


def _parse_mission_with_planner_and_hints(console: Console, user_mission: str) -> MissionPlan:
    """Run ``parse_mission_with_planner`` with the same Rich ``StartupHints`` spinner as CLI startup."""
    from cai.repl.ui.startup_hints import StartupHints, startup_hints_disabled

    if startup_hints_disabled():
        return parse_mission_with_planner(user_mission)

    boot = StartupHints(console)
    phases = [
        "Calling the Alias mission planner…",
        "Analyzing your mission for periodic execution…",
        "Deriving concrete tick checklist items…",
        "Estimating workload and tick spacing…",
    ]
    holder: dict[str, MissionPlan] = {}

    def _worker() -> None:
        holder["plan"] = parse_mission_with_planner(user_mission)

    boot.start(phases[0], leading_blank=True)
    th = threading.Thread(target=_worker, daemon=True)
    th.start()
    idx = 0
    while th.is_alive():
        th.join(timeout=1.2)
        if th.is_alive():
            idx = (idx + 1) % len(phases)
            boot.update(phases[idx])
    th.join()
    boot.stop(trailing_blank=False)
    return holder["plan"]


def _ensure_tasks_defined(console: Console, plan: MissionPlan, user_mission: str) -> MissionPlan | None:
    """Block until the mission has at least one actionable task, or abort."""
    original_stripped = user_mission.strip()
    while needs_task_collection(plan):
        _blank(console)
        _green_head(console, "Continuous Ops — at least one task is required")
        _grey(
            console,
            "Periodic execution needs [bold]at least one clear task[/bold] for each tick. Your prompt looks "
            "empty, or we could not derive actionable work (for example if you pressed Enter too early). "
            "Describe what the worker should do on every iteration.",
        )
        text = _prompt_multiline_tasks(console, max_attempts=3)
        if text is None:
            _hint(console, "No tasks captured — aborting setup.")
            return None
        if not original_stripped:
            plan = _parse_mission_with_planner_and_hints(console, text)
        else:
            plan = replace(
                plan,
                tasks_markdown=text.strip(),
                refined_tick_prompt=text.strip() or plan.refined_tick_prompt,
                structured_tasks=None,
            )
        if needs_task_collection(plan):
            _hint(console, "We still could not derive a task — try more specific wording.")
    return plan


def _intro_block(console: Console, plan: MissionPlan) -> None:
    _grey(
        console,
        "This agent is [italic]continuous_ops_agent[/italic]. It prepares unattended periodic "
        "(24/7-style) cybersecurity work from your prompt. You need at least one concrete task, "
        "a safe tick interval, and clear choices about background execution ([italic]tmux[/italic]) "
        "and privilege level. From your prompt we inferred:",
    )
    tick_s = plan.tick_seconds
    tick_disp = f"{tick_s}s" if tick_s is not None else "(not identified)"
    tmux_disp = (
        "yes"
        if plan.use_tmux is True
        else "no"
        if plan.use_tmux is False
        else "(not specified)"
    )
    if plan.auth_required is True:
        priv_disp = "likely required (model)"
    elif plan.auth_required is False:
        priv_disp = "likely not required (model)"
    else:
        priv_disp = "(not specified)"
    bullets = (
        f"{_tasks_intro_bullets(plan)}\n"
        f"• Tick interval: {tick_disp}\n"
        f"• Background-friendly execution ([italic]tmux[/italic] preference): {tmux_disp}\n"
        f"• [italic]sudo[/italic]-style privileges: {priv_disp}\n"
        "• Process end time: [bold]not specified[/bold] (assumed infinite loop until you stop it)"
    )
    _hint(console, bullets)


def _maybe_missing_data_note(console: Console, plan: MissionPlan, tick_ok: bool) -> None:
    gaps: list[str] = []
    if needs_task_collection(plan):
        gaps.append("concrete tasks")
    if plan.tick_seconds is None:
        gaps.append("tick interval")
    elif not tick_ok:
        gaps.append("valid tick interval")
    if plan.use_tmux is None:
        gaps.append("tmux preference")
    if plan.auth_required is None:
        gaps.append("privilege policy")
    if gaps:
        _blank(console)
        _grey(
            console,
            "Some required fields were missing or ambiguous. We will ask a short series of "
            f"questions to collect: {', '.join(gaps)}.",
        )


def _build_tick_prompt(plan: MissionPlan, *, privileged: bool) -> str:
    base = plan.refined_tick_prompt.strip() or plan.tasks_markdown.strip()
    if privileged:
        base = (
            "[Privilege] Operator [bold]granted[/bold] sudo-style privileges for this worker — elevation may be used "
            "where appropriate.\n\n"
            + base
        )
    else:
        base = (
            "[Privilege] Operator [bold]declined[/bold] sudo-style privileges for this worker — "
            "[italic]no[/italic] sudo/su/doas/pkexec; stay unprivileged every tick.\n\n"
            + base
        )
    if not privileged:
        base += (
            "\n\n[Operator policy] Same as the [Privilege] line above (`CAI_CONTINUOUS_OPS_NO_SUDO`). "
            "This iteration must NOT use sudo, su, doas, pkexec, "
            "or other privileged shells — the CLI will not elevate failed commands and must not block "
            "on password prompts. Prefer read-only reconnaissance and user-writable paths under your "
            "home directory. Do not read root-only logs (for example /var/log/auth.log) unless they are "
            "world-readable for your user. Avoid probes that typically require root on Linux without "
            "prior evidence you can run them unprivileged (for example `ufw status`, raw `iptables -L`, "
            "`docker info` security lines, or `/root/**`); if a check needs root, skip it and state that "
            "in prose with [STATUS: OK]. When grepping auth-style events, avoid putting the literal "
            "substring `sudo` inside a `grep -E` alternation (it can trip the shell guard); prefer "
            "patterns like `[s]udo` or uppercase `SUDO` when the file is readable."
        )
    base += (
        "\n\n[Operator policy] Append a one-line status tag at the end of your reply: "
        "[STATUS: OK] or [STATUS: INCIDENT] if you observed a probable security anomaly."
    )
    return base


def run_continuous_ops_wizard(user_mission: str, console: Console) -> bool:
    """Run the full interactive flow. Returns ``True`` if a worker was launched."""
    plan = _parse_mission_with_planner_and_hints(console, user_mission)
    if plan.planner_origin != "planner_api":
        _hint(
            console,
            "[yellow]Mission planner did not obtain parseable JSON from the Alias/CAI API[/yellow][dim] — a local fallback "
            "plan was used (same as typing without planner expansion). The summary may echo your raw phrase. "
            "Check ALIAS_API_KEY, CAI_MODEL (e.g. alias1, alias2-mini, alias3), and network access. "
            "If you need technical details, enable DEBUG logging for ``cai.continuous_ops.model_parse``. "
            "Run setup again if you need refined tasks.[/dim]",
        )
    plan = _ensure_tasks_defined(console, plan, user_mission)
    if plan is None:
        return False
    min_tick = plan.min_tick
    recommended = max(int(math.ceil(min_tick)), int(math.ceil(plan.base_tick)) + 1)

    _intro_block(console, plan)

    tick_ok = plan.tick_seconds is not None and plan.tick_seconds >= math.ceil(min_tick)
    _maybe_missing_data_note(console, plan, tick_ok)

    tick: int | None = plan.tick_seconds
    if tick is None or tick < math.ceil(min_tick):
        _blank(console)
        _green_head(console, "Continuous Ops — parsing your TICK INTERVAL")
        _grey(
            console,
            "You did NOT specify a tick interval, OR it is NOT VALID for your API throughput profile. "
            f"Your configured tier is [bold]{resolve_rate_tier().upper()}[/bold]. "
            f"A safe minimum tick for the estimated workload is about [bold]{math.ceil(min_tick)}[/bold] seconds "
            "(1.75× model-derived base spacing). Shorter intervals are more likely to hit HTTP 429 rate limits "
            "or overload the upstream service. Each tick runs in a subprocess with a wall-clock cap "
            "(at least [bold]120[/bold] seconds, or twice your tick interval, whichever is larger): if the work "
            "for that tick is still running when the cap is reached, that turn is stopped so the loop can "
            "continue, and the next iteration is scheduled after the usual spacing. Choosing very low tick "
            "values can saturate the model and APIs and lead to unstable or misleading behavior. "
            "If you need a higher sustained throughput, contact support at [italic]support@aliasrobotics.com[/italic].",
        )
        console.print(
            Text.assemble(
                ("Model-recommended tick (seconds): ", BANNER_PROMO_YELLOW),
                (str(recommended), f"bold {CAI_GREEN}"),
            )
        )
        choice = _prompt_yes_no(
            console,
            "Do you want to continue with the recommended tick interval?",
            max_attempts=3,
        )
        if choice is True:
            tick = recommended
        elif choice is False:
            manual = _prompt_seconds(console, min_seconds=min_tick, max_attempts=3)
            if manual is None:
                _hint(
                    console,
                    "No valid tick interval after three attempts — returning to the CAI prompt.",
                )
                return False
            tick = manual
        else:
            _hint(console, "No explicit yes/no — aborting setup.")
            return False
    else:
        tick = int(tick)

    use_tmux = plan.use_tmux
    tmux_path = _which_tmux()
    _blank(console)
    _green_head(console, "Continuous Ops — could improve your experience with TMUX")
    if tmux_path:
        _grey(
            console,
            "[italic]tmux[/italic] is installed on this system. We will use [italic]tmux[/italic] so you can "
            "detach and leave the worker running in the background — the recommended pattern for "
            "24/7-style workloads over SSH or on laptops.",
        )
    else:
        _grey(
            console,
            "[italic]tmux[/italic] is NOT installed on this system. Without [italic]tmux[/italic], closing the "
            "terminal window that runs the loop usually STOPS execution. With [italic]tmux[/italic], you can "
            "detach and keep the worker in the background.",
        )
        _blank(console)
        _promo_section_head(console, "Model-recommended tmux on your system")
        ins = _prompt_yes_no(
            console,
            "Do you want the framework to try installing tmux for you (may require sudo on Linux)?",
            max_attempts=3,
        )
        if ins is True:
            if not _install_tmux(console):
                _hint(console, "tmux installation did not succeed — continuing without tmux.")
        elif ins is None:
            _hint(console, "Unclear answer — continuing without tmux.")

    want_systemd_unit = False
    _blank(console)
    _green_head(console, "Continuous Ops — optional systemd user service")
    _promo_section_head(console, "Read before you choose — what systemd needs (and what it does NOT)")
    avoid_glob = (os.environ.get("CAI_AVOID_SUDO", "") or "").strip().lower() in ("1", "true", "yes", "on")
    if avoid_glob:
        _grey(
            console,
            "You already have [bold]CAI_AVOID_SUDO[/bold] enabled in this shell. That is [bold]compatible[/bold] with "
            "systemd here: installing a [italic]user[/italic] unit still uses [bold]systemctl --user[/bold] (no root "
            "sudo). The continuous-ops worker under systemd is [bold]non-privileged by design[/bold] anyway (no "
            "interactive sudo in ticks), and the generated unit also sets [bold]CAI_AVOID_SUDO[/bold] for the "
            "service — you do [bold]not[/bold] need to turn CAI_AVOID_SUDO off to use systemd.",
        )
    _grey(
        console,
        "[bold]Does NOT require[/bold] [italic]sudo[/italic] (root) for the default flow: copying the unit into "
        "[bold]~/.config/systemd/user[/bold], [italic]daemon-reload[/italic], and [italic]enable --now[/italic] run as "
        "your user. The model does [bold]not[/bold] get host root from this step.",
    )
    _grey(
        console,
        "[bold]May require administrator / policy approval[/bold] (still usually [bold]not[/bold] classic "
        "[italic]sudo su[/italic]): optional [italic]loginctl enable-linger[/italic] so the user service survives "
        "[bold]GUI logout[/bold] on some laptops — polkit or org policy can prompt or deny; that is separate from "
        "shell [italic]sudo[/italic] inside CAI ticks.",
    )
    _grey(
        console,
        "[bold]Does require[/bold] a working stack on this machine: [bold]Linux[/bold] with [bold]systemctl[/bold] on "
        "PATH, and a Python that can [italic]import cai[/italic] under a [bold]minimal environment[/bold] (like systemd). "
        "If you use a venv, activate CAI from it so [bold]VIRTUAL_ENV[/bold] is set. If you run from a git checkout "
        "without venv, we try to embed [bold]PYTHONPATH[/bold] pointing at your [italic]src[/italic] tree automatically.",
    )
    _grey(
        console,
        "A [bold]systemd --user[/bold] unit with [italic]Restart=always[/italic] keeps this worker alive across "
        "crashes and reboots (user session / login). It is not tied to a desktop terminal window. "
        "If you answer [bold]yes[/bold] below, CAI will copy the unit to [bold]~/.config/systemd/user[/bold], run "
        "[italic]systemctl --user daemon-reload[/italic], then [italic]enable --now[/italic] so the service "
        "[bold]starts immediately[/bold].",
    )
    _sd_venv = (os.environ.get("VIRTUAL_ENV") or "").strip() or None
    _sd_pp = _infer_pythonpath_for_running_cai()
    py_probe = _loop_python_for_run_script(default_cai_argv())
    console.print(Text(f"Pinned interpreter for systemd + run_loop_config: {py_probe}", style=GREY_HINT))
    if _sd_pp:
        console.print(
            Text(
                f"Editable / checkout layout: will add PYTHONPATH={_sd_pp} to the systemd unit (minimal-env probe).",
                style=GREY_HINT,
            )
        )
    import_ok, import_err = _probe_cai_import_minimal_env(
        py_probe, venv_root=_sd_venv, pythonpath=_sd_pp
    )
    if import_ok:
        console.print(Text("Check: that Python can import cai + loop_runner (OK).", style=f"bold {CAI_GREEN}"))
    else:
        console.print(
            Text(
                "Import check FAILED — systemd would start then exit=1 in a restart loop until this is fixed.",
                style=YELLOW_WARN,
            )
        )
        console.print(Text(import_err[:2000] if import_err else "(no stderr)", style=YELLOW_WARN))
        _grey(
            console,
            "Typical fix: [bold]source your venv[/bold] before launching CAI, or [bold]pip install -e .[/bold] into "
            "that interpreter so [italic]import cai[/italic] works. Avoid relying on [bold]/usr/bin/python3[/bold] if "
            "CAI only exists in a project venv.",
        )

    sd_question = (
        "Generate a systemd user unit and install/start it now (systemctl --user; restart on failure; no tmux duplicate)?"
        if import_ok
        else (
            "Import check failed — still generate and install systemd anyway? "
            "(almost always crashes until Python can import cai)"
        )
    )
    sd_unit = _prompt_yes_no(console, sd_question, max_attempts=3)
    if sd_unit is True:
        want_systemd_unit = True
    elif sd_unit is None:
        _hint(console, "Treating systemd unit file generation as NO after unclear answers.")
        want_systemd_unit = False

    if not want_systemd_unit:
        _blank(console)
        _grey(
            console,
            "Without a systemd user unit, the most reliable pattern on this machine is "
            "[bold]SSH to 127.0.0.1[/bold], start [italic]tmux[/italic] inside that SSH session, and run the worker "
            "there: the process sits under [italic]sshd[/italic], which usually survives GUI logout unlike "
            "[italic]tmux[/italic] launched from a desktop terminal.",
        )

    want_priv = False
    if want_systemd_unit:
        _blank(console)
        _green_head(console, "Continuous Ops — worker privileges with systemd")
        _grey(
            console,
            "Installing the unit with [bold]systemctl --user[/bold] does [bold]not[/bold] require [italic]sudo[/italic]. "
            "User services also have [bold]no TTY[/bold], so interactive [italic]sudo[/italic] prompts during ticks "
            "would hang or fail. This run is therefore locked to [bold]non-privileged ticks[/bold]: the wizard sets "
            "[bold]CAI_AVOID_SUDO=true[/bold] for the worker process and bakes the same flag into the generated "
            "[italic].service[/italic] file.",
        )
        os.environ["CAI_AVOID_SUDO"] = "true"
    else:
        _blank(console)
        _green_head(console, "Continuous Ops — define whether commands use SUDO PRIVILEGES")
        if plan.auth_required is None:
            _grey(
                console,
                "You did not specify whether [italic]sudo[/italic]-style privileges will be granted. "
                "Allowing them lets the model use powerful host commands, but increases risk. If you decline, "
                "the model must stay within unprivileged alternatives for each tick.",
            )
            pr = _prompt_yes_no(
                console,
                "Do you want to allow sudo-style privileges in the worker "
                "(you will be asked for the password once in that terminal)?",
                max_attempts=3,
            )
            want_priv = pr is True
            if pr is None:
                _hint(console, "Treating privilege choice as NO after unclear answers.")
        else:
            if plan.auth_required is True:
                _grey(
                    console,
                    "The planner flagged this mission as likely needing elevated privileges. "
                    "We still need your explicit confirmation for the worker.",
                )
                pr = _prompt_yes_no(
                    console,
                    "Do you want to allow sudo-style privileges in the worker "
                    "(you will be asked for the password once in that terminal)?",
                    max_attempts=3,
                )
                want_priv = pr is True
                if pr is None:
                    _hint(console, "Treating privilege choice as NO after unclear answers.")
            else:
                _grey(
                    console,
                    "The planner indicated privileged commands are unlikely. "
                    "The worker will run without [italic]sudo[/italic] elevation.",
                )
                want_priv = False

    log_full_days = 7
    log_delete_after = 15
    _blank(console)
    _green_head(console, "Continuous Ops — define the SAVED LOGS POLICY")
    _grey(
        console,
        "To protect disk space we rotate logs automatically. Default policy: "
        "[bold]days 1–7[/bold] full files under [italic]logs/full/[/italic], "
        "[bold]days 8–15[/bold] gzip-compressed, older than [bold]15[/bold] days deleted each tick.",
    )
    lp = _prompt_yes_no(console, "Confirm this default log retention policy for this run?", max_attempts=3)
    if lp is False:
        fd = _prompt_positive_int(
            console,
            "Enter how many days (integer only) to keep full, uncompressed logs:",
            minimum=1,
            maximum=365,
            max_attempts=3,
        )
        if fd is None:
            _hint(console, "Could not read full-log days — keeping defaults (7 / 15).")
        else:
            cd = _prompt_positive_int(
                console,
                "Enter how many additional days (integer only) to keep gzip-compressed logs "
                "after the full-log window (total retention = full days + this value):",
                minimum=1,
                maximum=3650,
                max_attempts=3,
            )
            if cd is None:
                log_full_days = fd
                log_delete_after = fd + 8
                _hint(
                    console,
                    "Could not read compressed span — using 8 additional gzip days after your full-log window.",
                )
            else:
                log_full_days = fd
                log_delete_after = fd + cd
    elif lp is None:
        _hint(console, "Proceeding with the default retention policy.")

    run_dir = Path.home() / ".cai" / "continuous_ops" / f"run_{uuid.uuid4().hex[:12]}"
    tick_prompt = _build_tick_prompt(plan, privileged=want_priv)
    _cai_argv = default_cai_argv()
    _py = _loop_python_for_run_script(_cai_argv)
    script_path = render_loop_script(
        run_dir=run_dir,
        tick_seconds=int(tick),
        tick_prompt=tick_prompt,
        privileged=want_priv,
        cai_argv=_cai_argv,
        python_bin=_py,
        log_full_days=log_full_days,
        log_delete_after_days=log_delete_after,
    )
    try:
        initialize_from_plan(run_dir, plan)
    except OSError:
        pass
    if not script_path.is_file():
        console.print(Text("Internal error: run_loop.py was not created on disk.", style=YELLOW_WARN))
        return False
    if not os.access(script_path, os.X_OK):
        script_path.chmod(script_path.stat().st_mode | 0o111)

    systemd_unit_path: Path | None = None
    systemd_user_active = False
    if want_systemd_unit:
        from cai.continuous_ops.systemd_unit import (
            enable_linger_for_session_user,
            install_user_unit,
            write_systemd_user_unit,
        )

        systemd_unit_path = write_systemd_user_unit(
            run_dir=run_dir,
            service_stem=f"cai-cops-{run_dir.name}",
            python_bin=_py,
            avoid_sudo=True,
            venv_root=_sd_venv,
            pythonpath_extra=_sd_pp,
        )
        if systemd_unit_path is not None:
            if sys.platform.startswith("linux") and shutil.which("systemctl"):
                ok, err = install_user_unit(systemd_unit_path)
                if ok:
                    systemd_user_active = True
                    console.print(
                        Text(
                            "systemd --user: unit installed, enabled, and started (Restart=always on failures).",
                            style=f"bold {CAI_GREEN}",
                        )
                    )
                    linger = _prompt_yes_no(
                        console,
                        "Run loginctl enable-linger for this user so the service can survive GUI logout "
                        "(may prompt for policy/admin approval on some systems)?",
                        max_attempts=3,
                    )
                    if linger is True:
                        lg_ok, lg_err = enable_linger_for_session_user()
                        if lg_ok:
                            console.print(
                                Text("loginctl enable-linger succeeded for this user.", style=f"bold {CAI_GREEN}")
                            )
                        else:
                            _hint(
                                console,
                                f"loginctl enable-linger did not succeed ({lg_err}). "
                                "You can retry later as root: [italic]loginctl enable-linger $USER[/italic].",
                            )
                    elif linger is None:
                        _hint(console, "Skipping loginctl enable-linger after unclear answers.")
                else:
                    _hint(
                        console,
                        f"Automatic systemd --user install/start failed ({err}). "
                        "A unit file was still written under the run directory — use the summary commands to install "
                        "manually, or rely on tmux / external terminal below.",
                    )
            else:
                _hint(
                    console,
                    "Automatic systemd install/start is only attempted on Linux when [bold]systemctl[/bold] is on PATH. "
                    "Unit file was written under the run directory — install manually if you use systemd elsewhere.",
                )

    session_name = f"cai-cops-{uuid.uuid4().hex[:8]}"
    tmux_sess: str | None = None
    launched_via = ""
    if systemd_user_active and systemd_unit_path is not None:
        launched_via = (
            "systemd --user: "
            f"[cyan]{systemd_unit_path.name}[/cyan] is enabled and active "
            "(tmux worker was not started — avoids two loops on the same run directory)."
        )
    elif _which_tmux() and (use_tmux is not False):
        try:
            # Do not use ``exec`` here: gnome-terminal / x-terminal-emulator append ``; read`` after this
            # command; ``exec`` would replace the shell so an immediate run_loop exit skips ``read`` and
            # the GUI window flashes closed (regression confused with "release broke tmux").
            inner = f"{_shell_activate_prefix()}cd {shlex.quote(str(run_dir))} && ./{shlex.quote(script_path.name)}"
            subprocess.run(
                [
                    "tmux",
                    "new-session",
                    "-d",
                    "-s",
                    session_name,
                    "bash",
                    "-lc",
                    inner,
                ],
                check=True,
                timeout=30,
            )
            subprocess.run(
                ["tmux", "set-option", "-t", session_name, "history-limit", "100000"],
                check=False,
                timeout=5,
            )
            subprocess.run(
                ["tmux", "set-option", "-t", session_name, "mouse", "on"],
                check=False,
                timeout=5,
            )
            tmux_sess = session_name
            launched_via = f"tmux session [cyan]{session_name}[/cyan]"
            _spawn_optional_tmux_attach_window(console, session_name)
        except (OSError, subprocess.SubprocessError) as exc:
            console.print(Text(f"tmux launch failed ({exc}); falling back to external terminal.", style=YELLOW_WARN))
            launched_via = _launch_external_terminal(console, run_dir, script_path)
    else:
        launched_via = _launch_external_terminal(console, run_dir, script_path)

    if systemd_user_active:
        bg_line = (
            "[bold]systemd --user[/bold] supervises the loop ([italic]Restart=always[/italic] after crashes; "
            "survives reboot when your user session / linger policy allows)."
        )
    elif tmux_sess:
        bg_line = (
            "Permitted ([italic]tmux[/italic] required): detach-safe session is [bold]active[/bold]."
        )
    elif _which_tmux():
        bg_line = "Permitted — [italic]tmux[/italic] is available; worker uses a pseudo-TTY for each tick."
    else:
        bg_line = (
            "Declined or not installed — worker runs in a [bold]foreground[/bold] terminal; "
            "closing that window usually stops the loop."
        )

    priv_line = "permitted" if want_priv else "declined"

    cheatsheet_lines = _cheatsheet_markup_lines(
        run_dir, tmux_sess, systemd_unit_path, systemd_installed=systemd_user_active
    )
    worker_colour_note = (
        "[dim]Worker window: colours match CAI when the terminal reports 256 colours and NO_COLOR is unset. "
        "Plain tmux panes still use your terminal theme for chrome.[/dim]"
    )

    exec_tasks = summary_iteration_tasks(plan)
    task_panel_lines = ["[bold]Tasks to execute:[/bold]"]
    for i, t in enumerate(exec_tasks, 1):
        task_panel_lines.append(f"  [white]{i}.[/white] {escape(t.strip())}")

    systemd_panel: list[str] = []
    if systemd_unit_path is not None:
        un = systemd_unit_path.name
        installed_path = Path.home() / ".config" / "systemd" / "user" / un
        if systemd_user_active:
            systemd_panel = [
                "",
                "[bold]systemd user service[/bold]",
                "• Status: [bold]installed and running[/bold] under your user manager "
                "([italic]enable --now[/italic]; [italic]Restart=always[/italic] on failures).",
                f"• Source copy in run dir: [white]{systemd_unit_path}[/white]",
                f"• Installed unit: [white]{installed_path}[/white]",
            ]
        else:
            systemd_panel = [
                "",
                "[bold]systemd user service (unit file only)[/bold]",
                f"• Unit file: [white]{systemd_unit_path}[/white]",
                "• Manual install (if automatic step failed or was skipped):",
                f"  mkdir -p ~/.config/systemd/user && cp {systemd_unit_path} ~/.config/systemd/user/",
                f"  systemctl --user daemon-reload && systemctl --user enable --now {un}",
            ]
        systemd_panel.extend(
            [
                "",
                "[bold]Manage the service[/bold]",
                f"  systemctl --user status {un}",
                f"  systemctl --user stop {un}",
                f"  systemctl --user start {un}",
                f"  systemctl --user restart {un}",
                f"  journalctl --user -u {un} -f",
                "",
                "[bold]Remove the service completely[/bold]",
                "[dim]Stops ticks, disables autostart, removes the unit from systemd, drops the generated file in the "
                "run directory, then reloads the unit database.[/dim]",
                f"  systemctl --user disable --now {un}",
                f"  rm -f ~/.config/systemd/user/{un}",
                f"  rm -f {systemd_unit_path}",
                "  systemctl --user daemon-reload",
            ]
        )

    running_footer = (
        f"[bold {CAI_GREEN}]Continuous ops worker is running under systemd --user[/bold {CAI_GREEN}] — "
        f"tick every [white]{tick}[/white]s, privileged=[white]{priv_line}[/white]."
        if systemd_user_active
        else (
            f"[bold {CAI_GREEN}]Continuous ops worker is running[/bold {CAI_GREEN}] — tick every [white]{tick}[/white]s, "
            f"privileged=[white]{priv_line}[/white], tmux=[white]{'yes' if tmux_sess else 'no'}[/white]."
        )
    )

    panel_body = "\n".join(
        [
            "[bold]Final configuration[/bold]",
            "",
            "\n".join(task_panel_lines),
            f"• Tick interval: [bold]{tick}[/bold]s",
            f"• Background execution: {bg_line}",
            f"• [italic]sudo[/italic]-style privileges: [bold]{priv_line}[/bold]",
            "• Process end time: [bold]not specified[/bold] (infinite loop until STOP / teardown)",
            "",
            "[bold]Generated worker files[/bold]",
            f"• Run directory: [white]{run_dir}[/white]",
            f"• Same path (tilde): [white]{_run_dir_as_tilde(run_dir)}[/white]",
            f"• Loop script: [white]{script_path}[/white]",
            f"• Loop config: [white]{run_dir / 'run_loop_config.json'}[/white]",
            f"• Log policy: [white]{log_full_days}[/white] days full, delete after [white]{log_delete_after}[/white] days",
            *systemd_panel,
            "",
            f"{launched_via}",
            "",
            worker_colour_note,
            "",
            "\n".join(cheatsheet_lines),
            "",
            running_footer,
            "",
            "[dim]If you do not want long-running iteration jobs, switch away from Continuous Ops. "
            "For general routing we recommend the Selection Agent.[/dim]",
        ]
    )
    _blank(console)
    console.print(
        Panel.fit(
            Text.from_markup(panel_body),
            title=Text("Continuous Ops — summary", style=f"bold {CAI_GREEN}"),
            border_style=CAI_GREEN,
            style=f"on {FINAL_PANEL_BG}",
        )
    )

    os.environ["CAI_CONTINUOUS_OPS_SETUP_DONE"] = "1"
    os.environ["CAI_AGENT_TYPE"] = DEFAULT_AGENT_TYPE
    os.environ["CAI_AGENT_ROUTE_MODE"] = "auto"
    return True


def _cheatsheet_markup_lines(
    run_dir: Path,
    tmux_session: str | None,
    systemd_unit: Path | None = None,
    *,
    systemd_installed: bool = False,
) -> list[str]:
    rd = str(run_dir)
    lines: list[str] = [
        "[bold]Continuous ops — operator cheatsheet[/bold]",
        "",
        "[bold]Pause[/bold]",
        "[dim]Temporarily halts scheduling between ticks. The worker keeps running: it finishes any tick already "
        "started, then waits in a sleep loop until the PAUSE file is removed. Nothing is killed; use [bold]Resume[/bold] "
        "below to continue.[/dim]",
        f"[bold]>> [/bold]mkdir -p {rd}/state",
        f"[bold]>> [/bold]touch {rd}/state/PAUSE",
        "",
        "[bold]Resume[/bold]",
        "[dim]Removes the pause flag so the loop proceeds with the next tick after its normal wait. Safe to run even "
        "if PAUSE was never created (the remove is a no-op).[/dim]",
        f"[bold]>> [/bold]rm -f {rd}/state/PAUSE",
        "",
        "[bold]Stop permanently[/bold]",
        "[dim]Requests a graceful shutdown: the worker sees STOP, exits cleanly, and deletes its generated entry "
        "script ([italic]run_loop.py[/italic]). The second line asks the OS to signal the loop process if it is still "
        "running; if nothing matches, you may see a harmless message from [italic]pkill[/italic].[/dim]",
        f"[bold]>> [/bold]touch {rd}/state/STOP",
        f"[bold]>> [/bold]pkill -f \"cai.continuous_ops.loop_runner --run-dir {rd}\"",
        "",
        "[bold]List full iteration logs[/bold]",
        "[dim]Each completed tick writes its own file under [italic]logs/full/[/italic]. This lists names, sizes, "
        "and timestamps so you can open a specific run in an editor or pager.[/dim]",
        f"[bold]>> [/bold]ls -la {rd}/logs/full",
        "",
        "[bold]Summary dashboard[/bold]",
        "[dim]Refreshes the whole [italic]summary.txt[/italic] every two seconds in place (no endless scroll). Shows "
        "iteration counts, last exit code, and the tail of the iteration index. Press Ctrl+C to leave [italic]watch[/italic].[/dim]",
        f"[bold]>> [/bold]watch -n2 cat {rd}/logs/summary/summary.txt",
        "",
        "[bold]Current tick log (follow mode)[/bold]",
        "[dim]Opens the active tick log ([italic]latest.log[/italic]) in [italic]less[/italic] and follows new bytes as "
        "they are written. Press [bold]q[/bold] to quit follow mode and exit.[/dim]",
        f"[bold]>> [/bold]less +F {rd}/logs/full/latest.log",
        "",
        "[dim]Tick wall clock: each [italic]cai[/italic] subprocess is killed after [bold]max(120s, 2×TICK)[/bold]. "
        "In [italic]summary.txt[/italic], [italic]last_exit_code[/italic] [bold]124[/bold] means that tick hit the wall.[/dim]",
    ]
    if systemd_unit is None:
        lines.extend(
            [
                "",
                "[bold]SSH + tmux (when you skipped systemd)[/bold]",
                "[dim]A common pattern so the worker survives GUI logout: the session belongs to [italic]sshd[/italic], "
                "not the desktop. Replace [bold]USER[/bold] with your login. After the second command you get a shell "
                "inside tmux; then run the [bold]cd[/bold] and [bold]run_loop.py[/bold] lines from this run directory.[/dim]",
                "[bold]>> [/bold]ssh USER@127.0.0.1",
                "[bold]>> [/bold]tmux new -s cai-cops-ssh",
                f"[bold]>> [/bold]cd {rd}",
                "[bold]>> [/bold]./run_loop.py",
            ]
        )
    if systemd_unit is not None:
        un = systemd_unit.name
        su = str(systemd_unit)
        if systemd_installed:
            lines.extend(
                [
                    "",
                    "[bold]systemd --user (already installed by the wizard)[/bold]",
                    "[dim]The service is active; no tmux loop was started for this run.[/dim]",
                    f"[bold]>> [/bold]systemctl --user status {un}",
                    f"[bold]>> [/bold]systemctl --user stop {un}",
                    f"[bold]>> [/bold]systemctl --user start {un}",
                    f"[bold]>> [/bold]systemctl --user restart {un}",
                    f"[bold]>> [/bold]journalctl --user -u {un} -f",
                    "",
                    "[bold]Optional: linger (if you skipped it in the wizard)[/bold]",
                    "[dim]Lets user services survive GUI logout on many systems.[/dim]",
                    "[bold]>> [/bold]loginctl enable-linger \"$USER\"",
                    "",
                    "[bold]Remove the service completely[/bold]",
                    f"[bold]>> [/bold]systemctl --user disable --now {un}",
                    f"[bold]>> [/bold]rm -f ~/.config/systemd/user/{un}",
                    f"[bold]>> [/bold]rm -f {su}",
                    "[bold]>> [/bold]systemctl --user daemon-reload",
                ]
            )
        else:
            lines.extend(
                [
                    "",
                    "[bold]systemd --user (install manually)[/bold]",
                    "[dim]Copy the generated unit, reload, and start so the loop restarts on failure.[/dim]",
                    "[bold]>> [/bold]mkdir -p ~/.config/systemd/user",
                    f"[bold]>> [/bold]cp {su} ~/.config/systemd/user/",
                    "[bold]>> [/bold]systemctl --user daemon-reload",
                    f"[bold]>> [/bold]systemctl --user enable --now {un}",
                    "[dim]Optional — survive GUI logout on many laptops (may require policy approval):[/dim]",
                    "[bold]>> [/bold]loginctl enable-linger \"$USER\"",
                    "",
                    "[bold]Check / control the service[/bold]",
                    f"[bold]>> [/bold]systemctl --user status {un}",
                    f"[bold]>> [/bold]systemctl --user stop {un}",
                    f"[bold]>> [/bold]systemctl --user start {un}",
                    f"[bold]>> [/bold]systemctl --user restart {un}",
                    f"[bold]>> [/bold]systemctl --user disable --now {un}",
                    f"[bold]>> [/bold]journalctl --user -u {un} -f",
                    "",
                    "[bold]Remove the service completely[/bold]",
                    f"[bold]>> [/bold]systemctl --user disable --now {un}",
                    f"[bold]>> [/bold]rm -f ~/.config/systemd/user/{un}",
                    f"[bold]>> [/bold]rm -f {su}",
                    "[bold]>> [/bold]systemctl --user daemon-reload",
                ]
            )
    if tmux_session:
        lines.extend(
            [
                "",
                "[bold]Attach to the existing tmux worker[/bold]",
                "[dim]Joins the session where the loop is already running. Detach with Ctrl+b then d so the worker "
                "keeps going in the background.[/dim]",
                f"[bold]>> [/bold]tmux attach -t {tmux_session}",
                "",
                "[dim]Scrollback: enlarged tmux history — mouse wheel, or Ctrl+b then [[ for copy mode and selection.[/dim]",
            ]
        )
    elif systemd_installed:
        lines.extend(
            [
                "",
                "[bold]Note[/bold]",
                "[dim]This run is supervised by systemd --user only (no tmux session).[/dim]",
            ]
        )
    else:
        lines.extend(
            [
                "",
                "[bold]Note[/bold]",
                "[dim]Without tmux or systemd, the loop is tied to the terminal window that launched it; closing that window "
                "usually sends SIGHUP and stops the worker. Prefer tmux or systemd for unattended operation.[/dim]",
            ]
        )
    return lines


def _likely_embedded_ide_terminal() -> bool:
    """True when stdin is probably an IDE-integrated shell (GUI child often invisible or wrong display)."""
    tp = (os.environ.get("TERM_PROGRAM") or "").strip().lower()
    if tp in ("vscode", "cursor"):
        return True
    # Cursor sometimes omits TERM_PROGRAM; this env is set in Cursor-hosted runs.
    if (os.environ.get("CURSOR_TRACE_ID") or "").strip():
        return True
    return False


def _spawn_optional_tmux_attach_window(console: Console, session_name: str) -> None:
    """Best-effort GUI terminal showing ``tmux attach`` (detached session already exists)."""
    attach_cmd = f"tmux attach -t {session_name}"
    console.print(
        Text(
            f"Worker is running in tmux. To see it, run (any terminal on this machine):\n  {attach_cmd}",
            style=f"bold {CAI_GREEN}",
        )
    )
    backend, hint = detect_external_terminal_backend()
    if not backend:
        console.print(
            Text(
                f"No GUI terminal launcher on PATH to auto-open a window ({hint}). "
                f"Use the command above.",
                style="dim",
            )
        )
        return
    inner = f"{_shell_activate_prefix()}exec tmux attach -t {shlex.quote(session_name)}"
    if spawn_external_terminal(backend, "CAI Continuous Ops — worker", inner):
        console.print(
            Text(
                "Also launched a separate GUI terminal to run that attach command — check your taskbar "
                "or other workspaces if you do not see it immediately.",
                style="dim",
            )
        )
        if _likely_embedded_ide_terminal():
            console.print(
                Text(
                    "Integrated IDE terminals often do not show that GUI window; use the tmux attach "
                    f"line above from a desktop terminal (or SSH with X forwarding).",
                    style=YELLOW_WARN,
                )
            )
    else:
        console.print(
            Text(
                f"Could not auto-open a GUI terminal ({hint}). The tmux session is still running — "
                f"use: {attach_cmd}",
                style="dim",
            )
        )


def _launch_external_terminal(console: Console, run_dir: Path, script_path: Path) -> str:
    backend, hint = detect_external_terminal_backend()
    cmd = f"{_shell_activate_prefix()}cd {shlex.quote(str(run_dir))} && ./{shlex.quote(script_path.name)}"
    if not backend or not spawn_external_terminal(backend, "CAI Continuous Ops", cmd):
        console.print(
            Text(
                f"No external terminal backend available ({hint}). "
                f"Start the worker manually: cd {run_dir} && ./run_loop.py",
                style=YELLOW_WARN,
            )
        )
        return "manual start (see instructions above)"
    return f"external terminal ({backend})"


def maybe_intercept_continuous_ops_turn(user_input: str, console: Console) -> bool:
    """If this turn should run the wizard, do so and return ``True`` (skip model)."""
    if os.environ.get("CAI_CONTINUOUS_OPS_SETUP_DONE") == "1":
        return False
    if os.environ.get("CAI_CONTINUOUS_OPS_LOOP_CHILD") == "1":
        return False
    try:
        return run_continuous_ops_wizard(user_input, console)
    except KeyboardInterrupt:
        console.print(Text("\nContinuous ops setup cancelled.", style=YELLOW_WARN))
        return True
