from __future__ import annotations

import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from sarus.core.development import DevelopmentWorkspace


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux VPS sandbox smoke")
class VPSSandboxSmoke(unittest.TestCase):
    def test_real_bubblewrap_no_network_test_cycle(self):
        self.assertTrue(shutil.which("bwrap"), "bubblewrap must be installed in the VPS autonomy profile")
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            project = root / "workspace" / "demo"
            project.mkdir(parents=True)
            (project / "calc.py").write_text(
                "def add(a, b):\n    return a - b\n", encoding="utf-8"
            )
            (project / "test_calc.py").write_text(
                "import errno\n"
                "import socket\n"
                "import unittest\n"
                "from calc import add\n\n"
                "class T(unittest.TestCase):\n"
                "    def test_add(self):\n"
                "        self.assertEqual(add(2, 3), 5)\n"
                "    def test_network_is_kernel_denied(self):\n"
                "        with self.assertRaises(OSError) as ctx:\n"
                "            socket.socket()\n"
                "        self.assertEqual(ctx.exception.errno, errno.EPERM)\n"
                "    def test_write_outside_project_is_kernel_denied(self):\n"
                "        with self.assertRaises(PermissionError):\n"
                "            open('/var/tmp/jubi-landlock-denied.txt', 'w').write('blocked')\n\n"
                "if __name__ == '__main__': unittest.main()\n",
                encoding="utf-8",
            )
            ws = DevelopmentWorkspace(root, "demo")
            first = ws.test("python_unittest")
            self.assertFalse(first["ok"], first)
            self.assertEqual(first["sandbox"], "landlock+seccomp-no-network")

            ws.write("calc.py", "def add(a, b):\n    return a + b\n")
            second = ws.test("python_unittest")
            self.assertTrue(second["ok"], second)
            verify = ws.verify()
            self.assertTrue(verify["ok"], verify)
            self.assertIn("calc.py", verify["changed_files"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
