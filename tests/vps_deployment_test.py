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
            "Environment=JUBI_DEPLOYMENT_PROFILE=linux_vps",
            "Environment=PYTHONNOUSERSITE=1",
            "ReadWritePaths=@JUBI_PREFIX@/data @JUBI_PREFIX@/workspace @JUBI_PREFIX@/logs",
        ):
            self.assertIn(required, text)
        self.assertNotIn("User=root", text)

    def test_installer_never_pulls_models_or_opens_public_bind(self):
        text = (ROOT / "deploy" / "vps" / "install.sh").read_text(encoding="utf-8")
        self.assertNotIn("ollama pull", text.lower())
        self.assertNotIn("JUBI_HOST=0.0.0.0", text)
        self.assertIn("JUBI_HOST=127.0.0.1", text)
        self.assertIn("--with-hermes", text)
        self.assertIn("--with-voice", text)
        self.assertIn("--with-browser", text)

    def test_full_profile_is_explicit_and_role_complete(self):
        text = (ROOT / "deploy" / "vps" / "full-profile.sh").read_text(encoding="utf-8")
        self.assertIn("--with-hermes", text)
        self.assertIn("--with-browser", text)
        self.assertIn("provision-models.sh", text)
        provision = (ROOT / "deploy" / "vps" / "provision-models.sh").read_text(encoding="utf-8")
        for model in ("qwen3:8b", "qwen2.5vl:3b", "qwen3-embedding:0.6b"):
            self.assertIn(model, provision)

    def test_voice_provisioning_is_explicit_and_local(self):
        installer = (ROOT / "deploy" / "vps" / "install.sh").read_text(encoding="utf-8")
        provision = (ROOT / "deploy" / "vps" / "provision-voice.sh").read_text(encoding="utf-8")
        full = (ROOT / "deploy" / "vps" / "full-profile.sh").read_text(encoding="utf-8")
        self.assertIn("--with-voice", installer)
        self.assertIn("faster-whisper==1.2.1", installer)
        self.assertIn("provision-voice.sh", full)
        self.assertIn("JUBI_WHISPER_MODEL=", provision)
        self.assertNotIn("OPENAI_API_KEY", provision)

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
