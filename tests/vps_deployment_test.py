from __future__ import annotations

import json
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class VPSDeploymentTest(unittest.TestCase):
    def test_production_profile_preserves_loopback(self):
        cfg = json.loads((ROOT / "config" / "production.json").read_text(encoding="utf-8"))
        self.assertTrue(cfg["localhost_only"])
        vps = cfg["deployment_profiles"]["linux_vps"]
        self.assertTrue(vps["supported"])
        self.assertEqual(vps["backend_bind"], "loopback-only")

    def test_env_example_is_not_publicly_bound(self):
        text = (ROOT / "deploy" / "vps" / "jubi.env.example").read_text(encoding="utf-8")
        self.assertIn("JUBI_HOST=127.0.0.1", text)
        self.assertNotIn("JUBI_HOST=0.0.0.0", text)

    def test_systemd_is_non_root_and_hardened(self):
        text = (ROOT / "deploy" / "vps" / "jubi.service").read_text(encoding="utf-8")
        for required in (
            "User=@JUBI_USER@",
            "NoNewPrivileges=true",
            "ProtectSystem=strict",
            "ProtectKernelModules=true",
            "CapabilityBoundingSet=",
            "ReadWritePaths=@JUBI_PREFIX@/data @JUBI_PREFIX@/workspace @JUBI_PREFIX@/logs",
        ):
            self.assertIn(required, text)
        self.assertNotIn("User=root", text)

    def test_installer_never_pulls_models_or_opens_public_bind(self):
        text = (ROOT / "deploy" / "vps" / "install.sh").read_text(encoding="utf-8")
        self.assertNotIn("ollama pull", text.lower())
        self.assertNotIn("JUBI_HOST=0.0.0.0", text)
        self.assertIn("JUBI_HOST=127.0.0.1", text)

    def test_server_still_rejects_wildcard_binding(self):
        text = (ROOT / "sarus" / "server.py").read_text(encoding="utf-8")
        self.assertIn("def loopback_host", text)
        self.assertIn("is_loopback", text)
        self.assertNotIn("0.0.0.0' is permitted", text)

    def test_vps_runbook_requires_tunnel_or_authenticated_proxy(self):
        text = (ROOT / "deploy" / "vps" / "README.md").read_text(encoding="utf-8")
        self.assertIn("ssh -N -L 8877:127.0.0.1:8877", text)
        self.assertIn("authenticate", text.lower())
        self.assertIn("/api/session", text)


if __name__ == "__main__":
    unittest.main()
