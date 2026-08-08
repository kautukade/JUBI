"""Tests for REPL /virtualization (VirtualizationCommand)."""

from __future__ import annotations

import os
import sys
from unittest.mock import MagicMock, patch

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src"))

import cai.repl.commands._virtualization_monolith as virt_monolith
from cai.repl.commands._virtualization_monolith import VirtualizationCommand


@pytest.fixture
def virt_cmd() -> VirtualizationCommand:
    return VirtualizationCommand()


def test_handle_info_calls_show_status(virt_cmd: VirtualizationCommand) -> None:
    with patch.object(virt_cmd, "show_virtualization_status", return_value=True) as m:
        assert virt_cmd.handle_info_subcommand() is True
    m.assert_called_once_with()


def test_handle_clear_delegates_to_host(virt_cmd: VirtualizationCommand) -> None:
    with patch.object(virt_cmd, "handle_activate_image", return_value=True) as m:
        assert virt_cmd.handle_clear_subcommand() is True
    m.assert_called_once_with("host")


def test_handle_set_missing_arg(virt_cmd: VirtualizationCommand) -> None:
    with patch.object(virt_cmd, "handle_activate_image") as m:
        assert virt_cmd.handle_set_subcommand(None) is False
        assert virt_cmd.handle_set_subcommand([]) is False
    m.assert_not_called()


def test_handle_set_with_id(virt_cmd: VirtualizationCommand) -> None:
    with patch.object(virt_cmd, "handle_activate_image", return_value=True) as m:
        assert virt_cmd.handle_set_subcommand(["abc123"]) is True
    m.assert_called_once_with("abc123")


def test_containers_matching_id_prefix(virt_cmd: VirtualizationCommand) -> None:
    containers = [
        {"ID": "deadbeef1111", "Image": "a", "Status": "Up", "Names": ""},
        {"ID": "dead99991111", "Image": "b", "Status": "Up", "Names": ""},
    ]
    assert len(virt_cmd._containers_matching_id_prefix(containers, "dead")) == 2
    assert len(virt_cmd._containers_matching_id_prefix(containers, "deadbeef")) == 1
    assert virt_cmd._containers_matching_id_prefix(containers, "nope") == []


def test_handle_run_subcommand_unique_prefix_activates(virt_cmd: VirtualizationCommand) -> None:
    virt_cmd.cached_containers = [
        {"ID": "deadbeef0123456789abcdef0123456789abcdef0123456789abcdef01", "Image": "x", "Status": "Up", "Names": ""}
    ]

    def _noop_refresh() -> None:
        pass

    virt_cmd.refresh_docker_info = _noop_refresh  # type: ignore[method-assign]

    with patch.object(virt_cmd, "handle_activate_image", return_value=True) as act:
        with patch("cai.repl.commands._virtualization_monolith.DockerManager") as DM:
            dm = DM.return_value
            dm.is_docker_installed.return_value = True
            dm.is_docker_running.return_value = True
            assert virt_cmd.handle_run_subcommand(["deadbeef"]) is True
    act.assert_called_once_with("deadbeef")


def test_handle_run_subcommand_ambiguous_prefix(virt_cmd: VirtualizationCommand) -> None:
    virt_cmd.cached_containers = [
        {"ID": "aa1111111111111111111111111111111111111111111111111111111111", "Image": "x", "Status": "Up", "Names": ""},
        {"ID": "aa2222222222222222222222222222222222222222222222222222222222", "Image": "y", "Status": "Up", "Names": ""},
    ]

    def _noop_refresh() -> None:
        pass

    virt_cmd.refresh_docker_info = _noop_refresh  # type: ignore[method-assign]

    with patch.object(virt_cmd, "handle_activate_image") as act:
        with patch("cai.repl.commands._virtualization_monolith.DockerManager") as DM:
            dm = DM.return_value
            dm.is_docker_installed.return_value = True
            dm.is_docker_running.return_value = True
            assert virt_cmd.handle_run_subcommand(["aa"]) is False
    act.assert_not_called()


def test_handle_run_subcommand_no_container_match_runs_image(virt_cmd: VirtualizationCommand) -> None:
    virt_cmd.cached_containers = []

    def _noop_refresh() -> None:
        pass

    virt_cmd.refresh_docker_info = _noop_refresh  # type: ignore[method-assign]

    with patch.object(virt_cmd, "handle_activate_image") as act:
        with patch("cai.repl.commands._virtualization_monolith.DockerManager") as DM:
            dm = DM.return_value
            dm.is_docker_installed.return_value = True
            dm.is_docker_running.return_value = True
            dm.run_container.return_value = (True, "Successfully started container with ID: abcdef123456")
            dm.set_active_container = MagicMock()
            assert virt_cmd.handle_run_subcommand(["kalilinux/kali-rolling"]) is True
    act.assert_not_called()
    dm.run_container.assert_called_once()


def test_set_active_container_missing_cai_workspace_no_workspace_setup_error() -> None:
    """CAI_WORKSPACE unset must not trigger NoneType when validating workspace name."""
    recorded: list[str] = []

    def _capture(msg: object, **_kwargs: object) -> None:
        recorded.append(str(msg))

    def _subprocess_run(cmd: list[str], **_kwargs: object) -> MagicMock:
        out = MagicMock()
        out.returncode = 0
        if "inspect" in cmd:
            out.stdout = "true\n"
        else:
            out.stdout = ""
        return out

    prior_ws = os.environ.pop("CAI_WORKSPACE", None)
    prior_ac = os.environ.pop("CAI_ACTIVE_CONTAINER", None)
    try:
        with patch.object(virt_monolith.console, "print", side_effect=_capture):
            with patch.object(virt_monolith.subprocess, "run", side_effect=_subprocess_run):
                with patch.object(virt_monolith, "_sync_tui_container_selection"):
                    virt_monolith.DockerManager.set_active_container("abc123deadbeef")

        assert os.environ.get("CAI_ACTIVE_CONTAINER") == "abc123deadbeef"
        assert not any("NoneType" in msg for msg in recorded), recorded
        assert not any("Failed to setup workspace in container" in msg for msg in recorded), recorded
    finally:
        if prior_ws is not None:
            os.environ["CAI_WORKSPACE"] = prior_ws
        else:
            os.environ.pop("CAI_WORKSPACE", None)
        if prior_ac is not None:
            os.environ["CAI_ACTIVE_CONTAINER"] = prior_ac
        else:
            os.environ.pop("CAI_ACTIVE_CONTAINER", None)
