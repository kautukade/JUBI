from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from sarus.core.memory import MemoryStore


class VPSMemoryTests(unittest.TestCase):
    def test_add_search_delete_lifecycle(self):
        with tempfile.TemporaryDirectory() as td:
            store = MemoryStore(Path(td) / "memory.db")
            created = store.add("vps-memory-marker", "cert", "vps-cert")
            found = store.search("vps-memory-marker", "vps-cert", 10)
            self.assertTrue(any(x["id"] == created["id"] for x in found))
            deleted = store.delete(created["id"])
            self.assertTrue(deleted["ok"])
            self.assertEqual(store.search("vps-memory-marker", "vps-cert", 10), [])
            with self.assertRaises(KeyError):
                store.delete(created["id"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
