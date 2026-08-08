"""Tests for false-positive suppression in the sensitive command guard.

Verifies that tool names appearing in file paths or arguments do NOT
trigger the guard, while actual tool invocations still do.
"""
from __future__ import annotations

from unittest.mock import patch

import pytest

from cai.util.user_prompts import (
    _extract_all_binaries,
    detect_sensitive_command,
)


@pytest.fixture(autouse=True)
def _guard_enabled():
    """Ensure the guard is always enabled during tests."""
    with patch.dict("os.environ", {"CAI_SENSITIVE_GUARD": "true", "CAI_TUI_MODE": "false"}):
        yield


# ── _extract_all_binaries unit tests ──────────────────────────────────

class TestExtractAllBinaries:
    def test_simple_command(self):
        assert _extract_all_binaries("ls -la /tmp") == {"ls"}

    def test_sudo_prefix(self):
        assert _extract_all_binaries("sudo nmap -sS 10.0.0.1") == {"nmap"}

    def test_pipeline(self):
        assert _extract_all_binaries("nmap -sS 10.0.0.1 | grep open") == {"nmap", "grep"}

    def test_chain_and(self):
        assert _extract_all_binaries("whoami && nmap -sS 10.0.0.1") == {"whoami", "nmap"}

    def test_chain_or(self):
        assert _extract_all_binaries("false || nmap 10.0.0.1") == {"false", "nmap"}

    def test_semicolon(self):
        assert _extract_all_binaries("cd /tmp; nmap 10.0.0.1") == {"cd", "nmap"}

    def test_env_assignment_prefix(self):
        assert _extract_all_binaries("FOO=bar nmap -sS 10.0.0.1") == {"nmap"}

    def test_full_path_binary(self):
        assert _extract_all_binaries("/usr/bin/nmap -sS 10.0.0.1") == {"nmap"}

    def test_tool_in_path_only(self):
        assert _extract_all_binaries("ls -la /home/kali/nmap/") == {"ls"}

    def test_multiple_prefixes(self):
        assert _extract_all_binaries("sudo env nmap -sS 10.0.0.1") == {"nmap"}


# ── False positives (should NOT trigger the guard) ────────────────────

class TestFalsePositives:
    def test_ls_nmap_directory(self):
        is_sensitive, _, _ = detect_sensitive_command("ls -la /home/kali/nmap/")
        assert not is_sensitive

    def test_cat_masscan_config(self):
        is_sensitive, _, _ = detect_sensitive_command("cat /opt/masscan/config.txt")
        assert not is_sensitive

    def test_find_shred_backup(self):
        is_sensitive, _, _ = detect_sensitive_command("find / -name shred_backup")
        assert not is_sensitive

    def test_echo_nmap_string(self):
        is_sensitive, _, _ = detect_sensitive_command("echo 'installed nmap yesterday'")
        assert not is_sensitive

    def test_grep_in_hydra_log(self):
        is_sensitive, _, _ = detect_sensitive_command("grep error /var/log/hydra.log")
        assert not is_sensitive

    def test_grep_alternation_with_sudo_literal_not_sudo_privilege(self):
        cmd = (
            r"tail -100 /var/log/auth.log 2>/dev/null | "
            r"grep -E \"(Failed|Accepted|Invalid|session opened|session closed|sudo|su:)\" | tail -50"
        )
        is_sensitive, _, category = detect_sensitive_command(cmd)
        assert not is_sensitive
        assert category == ""

    def test_mkdir_nikto_output(self):
        is_sensitive, _, _ = detect_sensitive_command("mkdir -p /tmp/nikto_results")
        assert not is_sensitive

    def test_cp_fdisk_backup(self):
        is_sensitive, _, _ = detect_sensitive_command("cp /tmp/fdisk_output.txt /backup/")
        assert not is_sensitive


# ── True positives (MUST trigger the guard) ───────────────────────────

class TestTruePositives:
    def test_nmap_direct(self):
        is_sensitive, reason, category = detect_sensitive_command("nmap -sS 10.0.0.1")
        assert is_sensitive
        assert category == "recon_tool"

    def test_sudo_nmap(self):
        is_sensitive, _, category = detect_sensitive_command("sudo nmap -sV 10.0.0.1")
        assert is_sensitive
        assert category == "sudo"

    def test_nmap_in_pipeline(self):
        is_sensitive, _, category = detect_sensitive_command("cat hosts.txt | nmap -iL -")
        assert is_sensitive
        assert category == "recon_tool"

    def test_env_prefix_nmap(self):
        is_sensitive, _, category = detect_sensitive_command("FOO=bar nmap -sS 10.0.0.1")
        assert is_sensitive
        assert category == "recon_tool"

    def test_hydra_direct(self):
        is_sensitive, _, category = detect_sensitive_command("hydra -l admin ssh://10.0.0.1")
        assert is_sensitive
        assert category == "recon_tool"

    def test_shred_direct(self):
        is_sensitive, _, category = detect_sensitive_command("shred -u /tmp/secret.txt")
        assert is_sensitive
        assert category == "destructive"

    def test_fdisk_direct(self):
        is_sensitive, _, category = detect_sensitive_command("fdisk /dev/sda")
        assert is_sensitive
        assert category == "destructive"

    def test_sudo_rm_rf(self):
        """sudo category is NOT subject to binary post-check."""
        is_sensitive, _, category = detect_sensitive_command("sudo rm -rf /")
        assert is_sensitive
        assert category == "sudo"

    def test_full_path_nmap(self):
        is_sensitive, _, category = detect_sensitive_command("/usr/bin/nmap -sS 10.0.0.1")
        assert is_sensitive
        assert category == "recon_tool"

    def test_msfconsole(self):
        is_sensitive, _, category = detect_sensitive_command("msfconsole -q")
        assert is_sensitive
        assert category == "recon_tool"
