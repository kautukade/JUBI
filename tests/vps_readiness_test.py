from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from sarus.core.vps_readiness import VPSReadiness


class Models:
    def list_models(self):
        return {"online": True, "models": ["qwen3:8b", "qwen2.5vl:3b", "qwen3-embedding:0.6b"]}

    def choose(self, role):
        return {
            "general": "qwen3:8b",
            "coding": "qwen3:8b",
            "vision": "qwen2.5vl:3b",
            "embedding": "qwen3-embedding:0.6b",
        }.get(role)


class Ready:
    def status(self):
        return {"ready": True}


class App:
    def __init__(self, root):
        self.models = Models()
        self.hermes = Ready()
        self.browser = Ready()
        self.voice = Ready()
        self.developer = type("D", (), {"workspace": Path(root)})()
        self.swarm = Ready()
        self.research = object()
        self.vision = object()
        self.knowledge = object()
        self.memory = object()
        self.execution = object()
        self.scheduler = object()
        self.privileged = type("P", (), {"status": lambda self: {"approval_secret_configured": True}})()
        self.receipts = object()


class VPSReadinessTests(unittest.TestCase):
    @unittest.skipIf(os.name == "nt", "Linux VPS readiness contract")
    def test_full_profile_reports_ready_when_every_required_component_exists(self):
        with tempfile.TemporaryDirectory() as td, patch.dict(os.environ, {
            "JUBI_REQUIRE_FULL_MODELS": "1",
            "JUBI_REQUIRE_HERMES": "1",
            "JUBI_REQUIRE_BROWSER": "1",
            "JUBI_REQUIRE_VOICE": "1",
        }, clear=False):
            result = VPSReadiness(App(td)).run()
        self.assertTrue(result["ready"], result)
        self.assertEqual(result["failed_required"], [])
        self.assertIn("Ring0 kernel bridge", result["windows_only_excluded"])

    @unittest.skipIf(os.name == "nt", "Linux VPS readiness contract")
    def test_missing_required_browser_fails_readiness(self):
        with tempfile.TemporaryDirectory() as td, patch.dict(os.environ, {
            "JUBI_REQUIRE_BROWSER": "1",
        }, clear=False):
            app = App(td)
            app.browser = type("B", (), {"status": lambda self: {"ready": False}})()
            result = VPSReadiness(app).run()
        self.assertFalse(result["ready"])
        self.assertIn("Read-only JS browser", result["failed_required"])

    @unittest.skipIf(os.name == "nt", "Linux VPS readiness contract")
    def test_missing_required_voice_fails_readiness(self):
        with tempfile.TemporaryDirectory() as td, patch.dict(os.environ, {
            "JUBI_REQUIRE_VOICE": "1",
        }, clear=False):
            app = App(td)
            app.voice = type("V", (), {"status": lambda self: {"ready": False}})()
            result = VPSReadiness(app).run()
        self.assertFalse(result["ready"])
        self.assertIn("Offline clip STT/TTS", result["failed_required"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
