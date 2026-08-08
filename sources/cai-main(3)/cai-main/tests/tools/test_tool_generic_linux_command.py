import os
import pytest
import json
import asyncio
from unittest.mock import MagicMock, AsyncMock, patch

# Set test environment variables to avoid OpenAI client initialization errors
os.environ["OPENAI_API_KEY"] = "test_key_for_ci_environment"

from cai.tools.evidence.capture_notice import packet_capture_failure_notice
from cai.tools.reconnaissance.generic_linux_command import generic_linux_command
from cai.sdk.agents import RunContextWrapper


# ---------------------------------------------------------------------------
# Tests that execute real commands are mocked to avoid Docker/dind dependency
# in CI pipelines.  We mock run_command_async / run_command at the module
# where they are *used* (generic_linux_command module).
# ---------------------------------------------------------------------------

_CMD_MODULE = "cai.tools.reconnaissance.generic_linux_command"


@pytest.mark.asyncio
@patch(f"{_CMD_MODULE}.run_command_async", new_callable=AsyncMock, return_value="hello")
async def test_generic_linux_command_echo(mock_run):
    """Test the execution of echo command using generic_linux_command."""
    args = {"command": "echo 'hello'"}
    result = await generic_linux_command.on_invoke_tool(RunContextWrapper(None), json.dumps(args))
    assert "hello" in result
    mock_run.assert_called_once()
    # Verify the command was passed correctly
    call_args = mock_run.call_args
    assert call_args[0][0] == "echo 'hello'" or call_args[1].get("command") == "echo 'hello'"


@pytest.mark.asyncio
@patch(f"{_CMD_MODULE}.run_command_async", new_callable=AsyncMock, return_value="total 32\ndrwxr-xr-x 2 root root 4096 Jan  1 00:00 bin\n-rw-r--r-- 1 root root  123 Jan  1 00:00 file.txt")
async def test_generic_linux_command_ls(mock_run):
    """Test the execution of ls command using generic_linux_command."""
    args = {"command": "ls -l"}
    result = await generic_linux_command.on_invoke_tool(RunContextWrapper(None), json.dumps(args))
    # Check that the output contains typical ls -l indicators
    assert "total" in result or "drwx" in result or "-rw" in result
    mock_run.assert_called_once()


@pytest.mark.asyncio
@patch(f"{_CMD_MODULE}.run_command_async", new_callable=AsyncMock, return_value="bash: invalid_command_xyz123: command not found")
async def test_generic_linux_command_invalid_command(mock_run):
    """Test handling of invalid command using generic_linux_command."""
    args = {"command": "invalid_command_xyz123"}
    result = await generic_linux_command.on_invoke_tool(RunContextWrapper(None), json.dumps(args))
    # Check for common error indicators
    assert (
        "not found" in result.lower()
        or "command not found" in result.lower()
        or "no such file" in result.lower()
    )
    mock_run.assert_called_once()


@pytest.mark.asyncio
async def test_generic_linux_command_empty_command():
    """Test handling of empty command using generic_linux_command."""
    args = {"command": ""}
    result = await generic_linux_command.on_invoke_tool(RunContextWrapper(None), json.dumps(args))
    assert "Error: No command provided" in result


@pytest.mark.asyncio
async def test_generic_linux_command_session_list():
    """Test session list functionality using generic_linux_command."""
    args = {"command": "session list"}
    result = await generic_linux_command.on_invoke_tool(RunContextWrapper(None), json.dumps(args))
    assert "No active sessions" in result or "Active sessions:" in result


@pytest.mark.asyncio
async def test_generic_linux_command_env_info():
    """Test environment info functionality using generic_linux_command."""
    args = {"command": "env info"}
    result = await generic_linux_command.on_invoke_tool(RunContextWrapper(None), json.dumps(args))
    assert "Current Environment:" in result
    assert "CTF Environment:" in result
    assert "Container:" in result
    assert "SSH:" in result
    assert "Workspace:" in result


@pytest.mark.asyncio
@patch(f"{_CMD_MODULE}.run_command_async", new_callable=AsyncMock, return_value="test")
async def test_generic_linux_command_interactive_flag(mock_run):
    """Test interactive flag functionality using generic_linux_command."""
    # Test with interactive=True but a simple command (not truly interactive)
    args = {"command": "echo 'test'", "interactive": True}
    result = await generic_linux_command.on_invoke_tool(RunContextWrapper(None), json.dumps(args))
    # Should still work, just might have different session handling
    assert "test" in result


@pytest.mark.asyncio
@patch(f"{_CMD_MODULE}.run_command_async", new_callable=AsyncMock, return_value="session output")
async def test_generic_linux_command_with_session_id(mock_run):
    """Test session_id parameter using generic_linux_command."""
    # Test with a non-existent session_id
    args = {"command": "echo 'test'", "session_id": "nonexistent123"}
    result = await generic_linux_command.on_invoke_tool(RunContextWrapper(None), json.dumps(args))
    # Should handle gracefully - either execute or give session error
    assert isinstance(result, str)


def test_packet_capture_failure_notice_detects_tcpdump_error():
    cmd = "tcpdump -i any -w out.pcap host 10.0.0.1"
    out = "tcpdump: any: You don't have permission to perform this capture\nCAP_NET_RAW may be required"
    notice = packet_capture_failure_notice(cmd, out)
    assert notice is not None
    assert "PACKET-CAPTURE FAILURE" in notice


def test_packet_capture_failure_notice_skips_tshark_read_only():
    cmd = "tshark -r capture.pcap -Y http -w filtered.pcap"
    out = "Running as user"
    assert packet_capture_failure_notice(cmd, out) is None


@pytest.mark.asyncio
@patch("cai.util.user_prompts.prompt_sudo_elevation", return_value=None)
@patch(f"{_CMD_MODULE}._get_workspace_dir", return_value="/tmp/cai-test-workspace")
@patch(f"{_CMD_MODULE}.run_command_async", new_callable=AsyncMock)
async def test_generic_linux_command_prepends_capture_notice(
    mock_run, _mock_workspace, _mock_sudo_prompt,
):
    mock_run.return_value = (
        "tcpdump: any: You don't have permission to perform this capture "
        "(CAP_NET_RAW may be required)"
    )
    args = {"command": "tcpdump -i any -w /tmp/x.pcap -c 5"}
    result = await generic_linux_command.on_invoke_tool(
        RunContextWrapper(None), json.dumps(args)
    )
    assert "PACKET-CAPTURE FAILURE" in result
    assert "permission to perform this capture" in result
    _mock_sudo_prompt.assert_not_called()
