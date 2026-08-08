"""Optional systemd --user unit files for continuous-ops workers (wizard-generated only)."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
from pathlib import Path


def _systemd_extra_env_lines(
    *,
    venv_root: str | None,
    pythonpath_extra: str | None,
) -> str:
    """Extra ``Environment=`` lines for user units (venv PATH + editable PYTHONPATH)."""
    lines: list[str] = []
    if venv_root:
        vr = Path(venv_root).expanduser().resolve()
        bd = vr / "bin"
        path_line = f"{bd}:/usr/local/bin:/usr/bin:/bin"
        lines.append(f"Environment=VIRTUAL_ENV={shlex.quote(str(vr))}")
        lines.append(f"Environment=PATH={shlex.quote(path_line)}")
    if pythonpath_extra:
        lines.append(f"Environment=PYTHONPATH={shlex.quote(pythonpath_extra)}")
    return ("\n".join(lines) + "\n") if lines else ""


def write_systemd_user_unit(
    *,
    run_dir: Path,
    service_stem: str,
    python_bin: str,
    avoid_sudo: bool = False,
    venv_root: str | None = None,
    pythonpath_extra: str | None = None,
) -> Path:
    """Write ``{service_stem}.service`` into *run_dir*.

    The wizard may then call :func:`install_user_unit` to copy it to
    ``~/.config/systemd/user/`` and run ``systemctl --user enable --now …``.

    ExecStart uses ``python -m cai.continuous_ops.loop_runner`` so the unit does not
    depend on a shell or bash-specific scripts (Linux / WSL-friendly).
    """
    run_dir = run_dir.resolve()
    py = Path(python_bin).resolve()
    rd = str(run_dir)
    py_s = str(py)
    # Avoid spaces in unit paths (systemd limitation on first token); rare edge case.
    mod = "cai.continuous_ops.loop_runner"
    env_block = "Environment=PYTHONUNBUFFERED=1\n"
    # Wizard-installed units only: loop_runner strips ANSI from tick logs (child env), not from ad-hoc runs.
    env_block += "Environment=CAI_CONTINUOUS_OPS_SYSTEMD_PLAIN_LOG=1\n"
    if avoid_sudo:
        env_block += "Environment=CAI_AVOID_SUDO=1\n"
    env_block += _systemd_extra_env_lines(venv_root=venv_root, pythonpath_extra=pythonpath_extra)
    body = (
        "[Unit]\n"
        f"Description=CAI Continuous Ops worker ({service_stem})\n"
        "Wants=network-online.target\n"
        "After=network-online.target\n"
        "\n"
        "[Service]\n"
        "Type=simple\n"
        f"WorkingDirectory={rd}\n"
        f"ExecStart={shlex.quote(py_s)} -m {mod} --run-dir {shlex.quote(rd)}\n"
        "Restart=always\n"
        "RestartSec=15\n"
        f"{env_block}"
        "\n"
        "[Install]\n"
        "WantedBy=default.target\n"
    )
    out = run_dir / f"{service_stem}.service"
    out.write_text(body, encoding="utf-8")
    return out


def install_user_unit(unit_src: Path) -> tuple[bool, str]:
    """Copy *unit_src* into ``~/.config/systemd/user``, reload, and ``enable --now``.

    Uses only ``systemctl --user`` — **no sudo** on typical Linux installs (files stay
    under the invoking user's home). Returns ``(True, \"\")`` on success, else
    ``(False, error_message)``.
    """
    unit_src = unit_src.resolve()
    if not unit_src.is_file():
        return False, "unit file is missing"
    dest_dir = Path.home() / ".config" / "systemd" / "user"
    try:
        dest_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(unit_src, dest_dir / unit_src.name)
    except OSError as exc:
        return False, str(exc)
    name = unit_src.name
    for args in (
        ["systemctl", "--user", "daemon-reload"],
        ["systemctl", "--user", "enable", "--now", name],
    ):
        proc = subprocess.run(
            args,
            capture_output=True,
            text=True,
            timeout=180,
        )
        if proc.returncode != 0:
            err = (proc.stderr or proc.stdout or "").strip() or f"exit {proc.returncode}"
            return False, f"{' '.join(args)}: {err}"
    return True, ""


def enable_linger_for_session_user() -> tuple[bool, str]:
    """Run ``loginctl enable-linger`` for the current login name.

    Often succeeds without sudo; on locked-down hosts polkit may prompt or deny.
    """
    user = (os.environ.get("USER") or os.environ.get("LOGNAME") or "").strip()
    if not user:
        return False, "USER/LOGNAME is unset"
    proc = subprocess.run(
        ["loginctl", "enable-linger", user],
        capture_output=True,
        text=True,
        timeout=120,
    )
    if proc.returncode != 0:
        err = (proc.stderr or proc.stdout or "").strip() or f"exit {proc.returncode}"
        return False, err
    return True, ""
