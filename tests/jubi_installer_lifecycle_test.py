from __future__ import annotations

import json
import os
from pathlib import Path
import re
from tempfile import TemporaryDirectory
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from sarus.core.models import OllamaRouter


class JubiInstallerLifecycleTests(unittest.TestCase):
    def test_bootstrap_is_canonical_and_auto_update_enabled(self):
        cfg = json.loads((ROOT / "config" / "bootstrap.json").read_text(encoding="utf-8"))
        self.assertGreaterEqual(cfg["schema_version"], 3)
        update = cfg["auto_update"]
        self.assertTrue(update["enabled"])
        self.assertEqual(update["channel"], "continuous")
        self.assertEqual(update["manifest_asset"], "Jubi-Update-Manifest.json")
        self.assertEqual(update["installer_asset"], "Jubi-Setup.exe")
        self.assertIn("kautukade/JUBI", update["release_api"])
        self.assertTrue(update["require_sha256"])
        self.assertGreaterEqual(int(cfg["background"]["repair_check_seconds"]), 1800)
        ports = cfg["prerequisites"]["ollama"]["candidate_ports"]
        self.assertIn(11434, ports)
        self.assertIn(11500, ports)
        self.assertGreaterEqual(len(ports), 10)
        self.assertEqual(len(ports), len(set(ports)))

    def test_updater_is_hash_verified_and_repo_scoped(self):
        text = (ROOT / "jubi" / "updater.py").read_text(encoding="utf-8")
        self.assertIn('manifest.get("repository") != "kautukade/JUBI"', text)
        self.assertIn("installer SHA-256 mismatch", text)
        self.assertIn('"/UPDATE"', text)
        self.assertIn("https://api.github.com/repos/kautukade/JUBI/", text)
        self.assertNotIn("shell=True", text)

    def test_background_task_is_persistent_but_user_session_scoped(self):
        register = (ROOT / "installer" / "REGISTER-JUBI-BACKGROUND.ps1").read_text(encoding="utf-8")
        self.assertIn("New-ScheduledTaskTrigger -AtLogOn", register)
        self.assertIn("-RestartCount 999", register)
        self.assertIn("-RunLevel Highest", register)
        self.assertIn("Jubi Background Agent", register)
        self.assertIn("JUBI-BACKGROUND.ps1", register)
        background = (ROOT / "jubi" / "background.py").read_text(encoding="utf-8")
        self.assertIn("check_for_update", background)
        self.assertIn("apply_update", background)
        self.assertIn("jubi.server", background)
        self.assertIn("repair_check_seconds", background)
        self.assertIn("_start_repair(full=True)", background)

    def test_background_bootstrap_repairs_private_runtime(self):
        text = (ROOT / "installer" / "JUBI-BACKGROUND.ps1").read_text(encoding="utf-8")
        self.assertIn("Test-PrivateRuntime", text)
        self.assertIn("Repair-CoreRuntime", text)
        self.assertIn("'-Repair'", text)
        self.assertIn("'-RepairMode'", text)
        self.assertIn("consecutiveSupervisorFailures", text)

    def test_prerequisites_are_automatically_provisioned_and_self_healing(self):
        text = (ROOT / "installer" / "JUBI-PREREQUISITES.ps1").read_text(encoding="utf-8")
        bootstrap = (ROOT / "config" / "bootstrap.json").read_text(encoding="utf-8")
        for token in ("Python.Python.3.11", "Ollama.Ollama", "Git.Git", "OpenJS.NodeJS.LTS"):
            self.assertIn(token, bootstrap)
        for token in (
            "Install-WingetPackage",
            "www.python.org",
            "ollama.com",
            "Ensure-Models",
            "Get-AuthenticodeSignature",
            "Normalize-LocalOllamaUrl",
            "OLLAMA_HOST",
            "runtime.json",
            "$request.Proxy = $null",
            "Start-OllamaOnCandidates",
            "Test-LocalPortAvailable",
            "pending_models",
            "ForceRepair",
            "candidate_ports",
            "Stop-StaleOllamaProcesses",
            "Remove-TempFile",
        ):
            self.assertIn(token, text)
        self.assertIsNone(re.search(r"(?im)^\s*\$host\s*=", text), "PowerShell $Host is read-only and must never be assigned")

    def test_real_target_ollama_candidate_list_is_not_nested(self):
        text = (ROOT / "installer" / "JUBI-PREREQUISITES.ps1").read_text(encoding="utf-8")
        self.assertIn("return $result", text)
        self.assertIsNone(re.search(r"(?im)^\s*return\s+,\$result\s*$", text))
        self.assertIn("foreach ($candidateUrl in @($Candidates))", text)
        self.assertIn("11505", text)

    def test_vendor_installer_downloads_use_unique_temp_paths(self):
        text = (ROOT / "installer" / "JUBI-PREREQUISITES.ps1").read_text(encoding="utf-8")
        self.assertIn("Jubi-OllamaSetup-", text)
        self.assertIn("jubi-python-3.11-", text)
        self.assertIn("[guid]::NewGuid().ToString('N')", text)
        self.assertIn(".download", text)
        self.assertNotIn("Join-Path $env:TEMP 'Jubi-OllamaSetup.exe'", text)

    def test_pending_models_do_not_force_reinstall_loop(self):
        text = (ROOT / "sarus" / "acceptance.py").read_text(encoding="utf-8")
        for token in (
            "_runtime_pending_models",
            "Deferred Ollama model provisioning",
            "background_retry",
            "model_required = not (install_mode and model in pending_models)",
            "generation_required = not (install_mode and bool(pending_models))",
            "env['OLLAMA_HOST']",
        ):
            self.assertIn(token, text)

    def test_core_install_retries_transient_work_and_reuses_runtime_on_update(self):
        text = (ROOT / "installer" / "INSTALL-SARUS.ps1").read_text(encoding="utf-8")
        for token in (
            "Invoke-WebDownloadRetry",
            "Invoke-SaraDependencySetup",
            "UpdateMode",
            "RepairMode",
            "Test-PythonRuntime",
            "Existing private Jubi Python runtime is healthy; reusing it safely.",
            "automatic retry",
        ):
            self.assertIn(token, text)
        self.assertIn("Update mode: keeping previously provisioned SARA/Windows dependencies", text)

    def test_ollama_router_respects_local_custom_port(self):
        with TemporaryDirectory() as td:
            config = Path(td) / "models.json"
            config.write_text('{"general":["qwen2.5:7b"]}', encoding="utf-8")
            with patch.dict(os.environ, {"JUBI_OLLAMA_URL": "http://127.0.0.1:11500"}, clear=False):
                router = OllamaRouter(config)
                self.assertEqual(router.base, "http://127.0.0.1:11500")

    def test_ollama_router_rejects_remote_env_endpoint(self):
        with TemporaryDirectory() as td:
            config = Path(td) / "models.json"
            config.write_text('{"general":["qwen2.5:7b"]}', encoding="utf-8")
            env = {"JUBI_OLLAMA_URL": "http://192.168.1.50:11434", "OLLAMA_HOST": ""}
            with patch.dict(os.environ, env, clear=False):
                router = OllamaRouter(config)
                self.assertEqual(router.base, "http://127.0.0.1:11434")

    def test_outer_installer_captures_diagnostics_and_repairs_failed_steps(self):
        text = (ROOT / "installer" / "EXE-INSTALL.ps1").read_text(encoding="utf-8")
        for token in (
            "RedirectStandardOutput",
            "RedirectStandardError",
            "installer-steps",
            "Append-ChildLog",
            "Invoke-WithRetry",
            "automatic repair mode",
            "core-repair-prereq",
            "certify-core-repair",
            "Background task registration",
        ):
            self.assertIn(token, text)

    def test_inno_installer_supports_silent_updates_and_current_repo(self):
        iss = (ROOT / "installer" / "SARUS-Setup.iss").read_text(encoding="utf-8")
        self.assertIn('https://github.com/kautukade/JUBI', iss)
        self.assertIn("CloseApplications=yes", iss)
        self.assertIn("/UPDATE", iss)
        self.assertIn("Jubi-Setup", iss)
        self.assertIn("Jubi.exe", iss)

    def test_uninstall_removes_background_task(self):
        uninstall = (ROOT / "installer" / "UNINSTALL-SARUS.ps1").read_text(encoding="utf-8")
        self.assertIn("UNREGISTER-JUBI-BACKGROUND.ps1", uninstall)
        self.assertIn("jubi\\.(background|server)", uninstall)

    def test_workflow_publishes_continuous_release_manifest(self):
        workflow = (ROOT / ".github" / "workflows" / "build-windows-installer.yml").read_text(encoding="utf-8")
        self.assertIn("config\\build-info.json", workflow)
        self.assertIn("Jubi-Update-Manifest.json", workflow)
        self.assertIn("gh release upload continuous", workflow)


if __name__ == "__main__":
    unittest.main(verbosity=2)
