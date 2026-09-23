from __future__ import annotations

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class VPSLiveAcceptanceContractTests(unittest.TestCase):
    def test_live_acceptance_uses_real_components_and_is_bounded(self):
        text = (ROOT / "scripts" / "vps_live_acceptance.py").read_text(encoding="utf-8")
        for required in (
            "mocked_model_output",
            "app.models.generate_text",
            "app.models.embed",
            "app.vision.analyze",
            "app.voice.synthesize",
            "app.voice.transcribe",
            'app.research.fetch("https://example.com/"',
            'app.browser.read("https://example.com/"',
            'project_path=rel',
            "app.swarm.run",
            "app.conversations.send",
            "app.knowledge.ingest",
            "app.council.run",
            "app.supervisor.run",
            "app.hermes.analyze",
            "app.execution.run",
            "app.scheduler.add",
            "app.windows.execute_typed",
            "app.privileged.handle",
            "app.fable.status",
            "app.receipts.verify_chain",
        ):
            self.assertIn(required, text)
        self.assertNotIn("0.0.0.0", text)
        self.assertNotIn("subprocess.run(", text)
        self.assertIn('.jubi-certification', text)

    def test_full_profile_runs_live_acceptance(self):
        text = (ROOT / "deploy" / "vps" / "full-profile.sh").read_text(encoding="utf-8")
        self.assertIn("scripts/vps_live_acceptance.py", text)

if __name__ == "__main__":
    unittest.main()
