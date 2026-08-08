"""Wizard picks venv interpreter for worker when VIRTUAL_ENV is set."""

from __future__ import annotations

import sys
from pathlib import Path

from cai.continuous_ops import wizard


def test_probe_cai_import_succeeds_on_current_interpreter() -> None:
    import sys

    ok, err = wizard._probe_cai_import_on_python(sys.executable)
    assert ok is True
    assert err == ""


def test_worker_python_bin_prefers_venv(monkeypatch, tmp_path: Path) -> None:
    venv = tmp_path / "venv"
    (venv / "bin").mkdir(parents=True)
    py = venv / "bin" / "python3"
    py.write_text("#!/bin/sh\necho\n", encoding="utf-8")
    py.chmod(0o755)
    monkeypatch.setenv("VIRTUAL_ENV", str(venv))
    out = wizard._worker_python_bin()
    assert out == str(py.resolve())


def test_worker_python_bin_fallback(monkeypatch) -> None:
    monkeypatch.delenv("VIRTUAL_ENV", raising=False)
    out = wizard._worker_python_bin()
    assert Path(out).name.startswith("python") or "python" in out.lower()


def test_loop_python_for_run_script_follows_cai_launcher_venv(monkeypatch, tmp_path: Path) -> None:
    """When VIRTUAL_ENV is unset but ``cai`` is ``.../env/bin/cai``, use that env's python3."""
    venv = tmp_path / "myenv"
    bin_dir = venv / "bin"
    bin_dir.mkdir(parents=True)
    cai_sh = bin_dir / "cai"
    cai_sh.write_text("#!/bin/sh\necho\n", encoding="utf-8")
    cai_sh.chmod(0o755)
    py3 = bin_dir / "python3"
    py3.write_text("#!/bin/sh\necho\n", encoding="utf-8")
    py3.chmod(0o755)
    monkeypatch.delenv("VIRTUAL_ENV", raising=False)

    py3_res = str(py3.resolve())

    def _probe(py: str) -> tuple[bool, str]:
        if py == py3_res:
            return True, ""
        return wizard._probe_cai_import_on_python(py)

    monkeypatch.setattr(wizard, "_probe_cai_import_on_python", _probe)
    out = wizard._loop_python_for_run_script([str(cai_sh.resolve())])
    assert out == py3_res


def test_loop_python_falls_back_to_checkout_cai_env_when_shim_python_lacks_cai(
    monkeypatch, tmp_path: Path
) -> None:
    """``*/bin/cai`` with a sibling ``python3`` that cannot import ``cai`` must not win over ``cai_env``."""
    usr_bin = tmp_path / "fake_usr" / "bin"
    usr_bin.mkdir(parents=True)
    cai_sh = usr_bin / "cai"
    cai_sh.write_text("#!/bin/sh\necho shim\n", encoding="utf-8")
    cai_sh.chmod(0o755)
    bad_py = usr_bin / "python3"
    bad_py.write_text("#!/bin/sh\necho bad\n", encoding="utf-8")
    bad_py.chmod(0o755)

    venv = tmp_path / "cai_env"
    (venv / "bin").mkdir(parents=True)
    good_py = venv / "bin" / "python3"
    good_py.write_text("#!/bin/sh\necho good\n", encoding="utf-8")
    good_py.chmod(0o755)

    good_res = str(good_py.resolve())

    def _probe(py: str) -> tuple[bool, str]:
        if py == good_res:
            return True, ""
        return False, "no module named cai"

    monkeypatch.setattr(wizard, "_probe_cai_import_on_python", _probe)
    monkeypatch.setattr(wizard, "_wizard_checkout_root_for_loop_python", lambda: tmp_path)
    monkeypatch.setattr(wizard, "_worker_python_bin", lambda: str(bad_py.resolve()))
    monkeypatch.delenv("VIRTUAL_ENV", raising=False)

    out = wizard._loop_python_for_run_script([str(cai_sh.resolve())])
    assert out == good_res


def test_loop_python_returns_venv_bin_path_when_python3_symlinks_to_system(
    monkeypatch, tmp_path: Path
) -> None:
    """``venv/bin/python3`` → ``/usr/bin/python3*``: must not collapse shebang to the system path."""
    bin_dir = tmp_path / "myvenv" / "bin"
    bin_dir.mkdir(parents=True)
    py = bin_dir / "python3"
    try:
        py.symlink_to(Path(sys.executable).resolve())
    except OSError:
        import pytest

        pytest.skip("cannot create symlink for test")
    cai_sh = bin_dir / "cai"
    cai_sh.write_text("#!/bin/sh\necho\n", encoding="utf-8")
    cai_sh.chmod(0o755)
    want = str(py.absolute())
    assert Path(want).resolve() == Path(sys.executable).resolve()

    def _probe(py: str) -> tuple[bool, str]:
        if py == want:
            return True, ""
        return False, "no cai"

    monkeypatch.setattr(wizard, "_probe_cai_import_on_python", _probe)
    monkeypatch.delenv("VIRTUAL_ENV", raising=False)
    out = wizard._loop_python_for_run_script([str(cai_sh.resolve())])
    assert out == want
