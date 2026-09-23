from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from sarus.core.workflows import WorkflowScheduler


class Bus:
    def __init__(self):
        self.events = []

    def emit(self, kind, payload):
        self.events.append((kind, payload))


class VPSAutomationTests(unittest.TestCase):
    def test_create_pause_delete_lifecycle(self):
        with tempfile.TemporaryDirectory() as td:
            bus = Bus()
            scheduler = WorkflowScheduler(
                Path(td) / "automation.db",
                lambda prompt, source="automation": {"status": "completed", "task_id": "T1"},
                event_bus=bus,
            )
            created = scheduler.add("cert", "do nothing", 3600, enabled=True)
            self.assertEqual(len(scheduler.list()), 1)
            scheduler.set_enabled(created["id"], False)
            self.assertFalse(scheduler.list()[0]["enabled"])
            deleted = scheduler.delete(created["id"])
            self.assertTrue(deleted["ok"])
            self.assertEqual(scheduler.list(), [])
            self.assertTrue(any(k == "AUTOMATION_DELETED" for k, _ in bus.events))
            with self.assertRaises(KeyError):
                scheduler.delete(created["id"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
