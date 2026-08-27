from __future__ import annotations

import json
import os
from pathlib import Path
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
        update = cfg["auto_update"]
        self.assertTrue(update["enabled"])
        self.assertEqual(update["channel"], "continuous")
        self.assertEqual(update["manifest_asset"], "Jubi-Update-Manifest.json")
        self.assertEqual(update["installer_asset"], "Jubi-Setup.exe")
        self.assertIn("kautukade/JUBI", update["release_api"])
        self.assertTrue(update["require_sha256"])

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

    def test_prerequisites_are_automatically_provisioned(self):
        text = (ROOT / "installer" / "JUBI-PREREQUISITES.ps1").read_text(encoding="utf-8")
        for token in ("Python.Python.3.11", "Ollama.Ollama", "Git.Git", "OpenJS.NodeJS.LTS"):
            self.assertIn(token, (ROOT / "config" / "bootstrap.json").read_text(encoding="utf-8"))
        self.assertIn("Install-WingetPackage", text)
        self.assertIn("www.python.org", text)
        self.assertIn("ollama.com", text)
        self.assertIn("Ensure-Models", text)
        self.assertIn("Get-AuthenticodeSignature", text)
        self.assertIn("Normalize-LocalOllamaUrl", text)
        self.assertIn("OLLAMA_HOST", text)
        self.assertIn("runtime.json", text)
        self.assertIn("$request.Proxy = $null", text)
        self.assertIn("ollama-serve.stderr.log", text)
        self.assertIn("$hostName = $uri.Host.ToLowerInvariant()", text)
        self.assertNotRegex(text, r"(?im)^\s*\$host\s*=")

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

    def test_outer_installer_captures_child_diagnostics(self):
        text = (ROOT / "installer" / "EXE-INSTALL.ps1").read_text(encoding="utf-8")
        self.assertIn("RedirectStandardOutput", text)
        self.assertIn("RedirectStandardError", text)
        self.assertIn("installer-steps", text)
        self.assertIn("Append-ChildLog", text)

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
