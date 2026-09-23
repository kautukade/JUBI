from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from sarus.core.windows import WindowsBroker


class VPSOperatorTests(unittest.TestCase):
    def make_root(self, td):
        root = Path(td)
        (root / "config").mkdir(parents=True)
        (root / "workspace").mkdir()
        (root / "config" / "broker_allowlist.json").write_text(json.dumps({
            "path_scopes": {"user_workspace": ["workspace"]},
        }), encoding="utf-8")
        return root

    @unittest.skipIf(os.name == "nt", "Linux VPS contract")
    def test_process_inventory_uses_typed_ps(self):
        with tempfile.TemporaryDirectory() as td:
            broker = WindowsBroker(self.make_root(td))
            with patch.object(broker, "_run", return_value={"ok": True}) as run:
                out = broker.execute_typed("system.processes.list", {}, {})
            self.assertTrue(out["ok"])
            argv = run.call_args.args[0]
            self.assertEqual(argv[0], "ps")
            self.assertIn("pid,ppid,user,comm,args", argv)

    @unittest.skipIf(os.name == "nt", "Linux VPS contract")
    def test_service_inventory_and_allowlisted_unit(self):
        with tempfile.TemporaryDirectory() as td:
            broker = WindowsBroker(self.make_root(td))
            with patch("sarus.core.windows.shutil.which", return_value="/bin/systemctl"),                  patch.object(broker, "_run", return_value={"ok": True}) as run:
                out = broker.execute_typed("system.services.list", {}, {})
                self.assertTrue(out["ok"])
                self.assertEqual(run.call_args.args[0][0], "/bin/systemctl")

                out = broker.execute_typed(
                    "service.query", {"resource_id": "ollama"},
                    {"resource_id": "ollama", "linux_unit": "ollama.service"},
                )
                self.assertTrue(out["ok"])
                self.assertEqual(
                    run.call_args.args[0],
                    ["/bin/systemctl", "status", "ollama.service", "--no-pager"],
                )

    @unittest.skipIf(os.name == "nt", "Linux VPS contract")
    def test_service_mapping_rejects_unreviewed_unit(self):
        with tempfile.TemporaryDirectory() as td:
            broker = WindowsBroker(self.make_root(td))
            with patch("sarus.core.windows.shutil.which", return_value="/bin/systemctl"):
                with self.assertRaises(ValueError):
                    broker.execute_typed(
                        "service.query", {},
                        {"resource_id": "bad", "linux_unit": "../../evil.service"},
                    )

    @unittest.skipIf(os.name == "nt", "Linux VPS contract")
    def test_headless_app_launch_is_explicitly_unavailable(self):
        with tempfile.TemporaryDirectory() as td:
            broker = WindowsBroker(self.make_root(td))
            result = broker.execute_typed(
                "app.launch", {"resource_id": "vscode"},
                {"resource_id": "vscode", "argv": ["code"]},
            )
            self.assertFalse(result["ok"])
            self.assertIn("headless VPS", result["error"])

    def test_canonical_config_has_linux_ollama_resources(self):
        cfg = json.loads((ROOT / "config" / "broker_allowlist.json").read_text(encoding="utf-8"))
        self.assertEqual(cfg["resources"]["services"]["ollama"]["linux_unit"], "ollama.service")
        self.assertEqual(cfg["resources"]["processes"]["ollama"]["linux_name"], "ollama")


if __name__ == "__main__":
    unittest.main(verbosity=2)
