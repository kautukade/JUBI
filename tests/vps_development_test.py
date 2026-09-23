from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from sarus.core.development import DevelopmentWorkspace, VPSDevelopmentAgent


class _Models:
    base = "http://127.0.0.1:11434"

    def list_models(self):
        return {
            "online": True,
            "models": ["coder:latest"],
            "items": [{"name": "coder:latest", "kind": "coding", "size": 1024}],
        }

    def choose(self, task_type="general"):
        return "coder:latest"


class _Brain:
    def route(self, goal, task_type="coding"):
        return {"candidates": [{"model": "coder:latest", "score": 1.0}], "task_type": task_type}


class _Hermes:
    def status(self):
        return {"ready": False}


class DevelopmentWorkspaceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.project = self.root / "workspace" / "demo"
        self.project.mkdir(parents=True)
        (self.project / "calc.py").write_text(
            "def add(a, b):\n    return a - b\n", encoding="utf-8"
        )
        (self.project / "test_calc.py").write_text(
            "import unittest\n"
            "from calc import add\n\n"
            "class T(unittest.TestCase):\n"
            "    def test_add(self):\n"
            "        self.assertEqual(add(2, 3), 5)\n\n"
            "if __name__ == '__main__': unittest.main()\n",
            encoding="utf-8",
        )

    def test_workspace_is_scoped_and_secret_files_are_denied(self):
        ws = DevelopmentWorkspace(self.root, "demo")
        with self.assertRaises(PermissionError):
            ws.read("../../outside.py")
        with self.assertRaises(PermissionError):
            ws.read(".env")
        with self.assertRaises(PermissionError):
            ws.write(".jubi/project.json", "{}")
        with self.assertRaises(PermissionError):
            ws.write("image.png", "not really an image")

    def test_real_edit_test_diff_and_verify(self):
        ws = DevelopmentWorkspace(self.root, "demo")
        with patch.object(DevelopmentWorkspace, "_sandbox_argv", lambda self, argv: argv):
            self.assertFalse(ws.test("python_unittest")["ok"])
            ws.write("calc.py", "def add(a, b):\n    return a + b\n")
            self.assertTrue(ws.test("python_unittest")["ok"])
            diff = ws.diff()
            self.assertIn("+    return a + b", diff["diff"])
            self.assertTrue(ws.verify()["ok"])

    def test_local_agent_observes_tools_edits_and_verifies(self):
        app = SimpleNamespace(root=self.root, models=_Models(), brain=_Brain(), hermes=_Hermes())
        agent = VPSDevelopmentAgent(app)

        calls = [
            {"operation": "list", "path": "."},
            {"operation": "read", "path": "calc.py"},
            {"operation": "read", "path": "test_calc.py"},
            {"operation": "test", "recipe": "python_unittest"},
            {"operation": "write", "path": "calc.py", "content": "def add(a, b):\n    return a + b\n"},
            {"operation": "test", "recipe": "python_unittest"},
            {"operation": "diff"},
            {"operation": "verify"},
        ]
        state = {"index": 0}

        def fake_json(self, path, body=None, timeout=10):
            if path == "/api/show":
                return {
                    "details": {"format": "gguf"},
                    "capabilities": ["completion", "tools"],
                    "model_info": {"test.context_length": 32768},
                }
            if path == "/api/chat":
                index = state["index"]
                state["index"] += 1
                if index < len(calls):
                    return {
                        "message": {
                            "role": "assistant",
                            "content": "",
                            "tool_calls": [{
                                "function": {
                                    "name": "jubi_workspace",
                                    "arguments": calls[index],
                                }
                            }],
                        },
                        "prompt_eval_count": 100,
                        "eval_count": 20,
                    }
                return {
                    "message": {"role": "assistant", "content": "Implemented and verified the fix."},
                    "prompt_eval_count": 100,
                    "eval_count": 20,
                }
            raise AssertionError(path)

        with patch("sarus.core.development.InferenceTransport.json", new=fake_json), \
             patch("sarus.core.development.memory_snapshot", return_value={
                 "total_bytes": 16 * 1024**3,
                 "available_bytes": 12 * 1024**3,
                 "available_commit_bytes": 12 * 1024**3,
             }), \
             patch("sarus.core.development.admission", return_value={
                 "admitted": True, "max_workers": 1, "mode": "physical_ram"
             }), \
             patch.object(DevelopmentWorkspace, "_sandbox_argv", lambda self, argv: argv):
            result = agent.run("Fix calc.add so addition works", project="demo")

        self.assertTrue(result["ok"], result)
        self.assertEqual(result["status"], "completed")
        self.assertEqual(result["changed_files"], ["calc.py"])
        self.assertTrue(result["verification"]["ok"])
        self.assertIn("return a + b", (self.project / "calc.py").read_text(encoding="utf-8"))
        operations = [x["operation"] for x in result["events"]]
        self.assertIn("write", operations)
        self.assertIn("test", operations)
        self.assertIn("verify", [result["verification"]["operation"]])

    def test_text_encoded_allowlisted_tool_call_is_supported(self):
        app = SimpleNamespace(root=self.root, models=_Models(), brain=_Brain(), hermes=_Hermes())
        agent = VPSDevelopmentAgent(app)
        state = {"index": 0}

        def fake_json(self, path, body=None, timeout=10):
            if path == "/api/show":
                return {
                    "capabilities": ["completion", "tools"],
                    "model_info": {"test.context_length": 32768},
                }
            state["index"] += 1
            if state["index"] == 1:
                return {
                    "message": {
                        "role": "assistant",
                        "content": '{"name":"read","arguments":{"path":"calc.py"}}',
                    }
                }
            return {"message": {"role": "assistant", "content": "No code change requested."}}

        with patch("sarus.core.development.InferenceTransport.json", new=fake_json), \
             patch("sarus.core.development.memory_snapshot", return_value={
                 "total_bytes": 16 * 1024**3,
                 "available_bytes": 12 * 1024**3,
                 "available_commit_bytes": 12 * 1024**3,
             }), \
             patch("sarus.core.development.admission", return_value={"admitted": True}), \
             patch.object(DevelopmentWorkspace, "_sandbox_argv", lambda self, argv: argv):
            result = agent.run("Review calc.py", project="demo", max_iterations=3)

        self.assertFalse(result["ok"])
        self.assertGreaterEqual(result["iterations"], 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
