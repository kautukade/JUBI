from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from sarus.core.developer import VPSDeveloper
from sarus.core.orchestrator import Orchestrator


class FakeModels:
    def __init__(self, responses):
        self.responses = list(responses)
        self.calls = []

    def choose(self, kind):
        return "local-coder:7b"

    def generate_text(self, prompt, task_type="general", system="", model=None, timeout=300, **_kwargs):
        self.calls.append({"prompt": prompt, "task_type": task_type, "system": system, "model": model})
        if not self.responses:
            raise AssertionError("unexpected model call")
        return self.responses.pop(0)


class FakeApp:
    def __init__(self, root, responses):
        self.root = Path(root)
        self.models = FakeModels(responses)


class Bus:
    def emit(self, *_args, **_kwargs):
        pass


class Policy:
    def evaluate(self, *_args, **_kwargs):
        return {"decision": "allow", "reason": "test"}


class VPSDeveloperTests(unittest.TestCase):
    def test_real_bounded_edit_verify_and_review(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            project = root / "workspace" / "demo"
            project.mkdir(parents=True)
            (root / "config" / "vps-test-plans").mkdir(parents=True)
            (project / "calc.py").write_text("def add(a, b):\n    return a - b\n", encoding="utf-8")

            subprocess.run(["git", "init"], cwd=project, check=True, capture_output=True)
            subprocess.run(["git", "config", "user.email", "jubi@test.local"], cwd=project, check=True)
            subprocess.run(["git", "config", "user.name", "Jubi Test"], cwd=project, check=True)
            subprocess.run(["git", "add", "calc.py"], cwd=project, check=True)
            subprocess.run(["git", "commit", "-m", "baseline"], cwd=project, check=True, capture_output=True)

            responses = [
                json.dumps({"operation": "read", "path": "calc.py"}),
                json.dumps({"operation": "write", "path": "calc.py",
                            "content": "def add(a, b):\n    return a + b\n"}),
                json.dumps({"operation": "verify"}),
                json.dumps({"operation": "diff"}),
                json.dumps({"operation": "finish", "summary": "Fixed addition and verified syntax."}),
                json.dumps({"approved": True, "reason": "The diff matches the requested arithmetic fix and verification passed."}),
            ]
            app = FakeApp(root, responses)
            result = VPSDeveloper(app).run("Fix add() so it adds two values", project_path="demo")

            self.assertTrue(result["ok"], result)
            self.assertEqual(result["writes"], 1)
            self.assertTrue(result["verification"]["ok"])
            self.assertTrue(result["review"]["approved"])
            self.assertIn("return a + b", (project / "calc.py").read_text(encoding="utf-8"))
            self.assertIn("return a + b", result["diff"])
            self.assertTrue(result["tools_executed"])

    def test_failed_review_rolls_back_workspace(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            project = root / "workspace" / "demo"
            project.mkdir(parents=True)
            (root / "config" / "vps-test-plans").mkdir(parents=True)
            target = project / "calc.py"
            original = "def add(a, b):\n    return a - b\n"
            target.write_text(original, encoding="utf-8")

            responses = [
                json.dumps({"operation": "read", "path": "calc.py"}),
                json.dumps({"operation": "write", "path": "calc.py",
                            "content": "def add(a, b):\n    return a + b\n"}),
                json.dumps({"operation": "verify"}),
                json.dumps({"operation": "finish", "summary": "Changed implementation."}),
                json.dumps({"approved": False, "reason": "Reviewer intentionally rejects this test change."}),
            ]
            result = VPSDeveloper(FakeApp(root, responses)).run(
                "Fix add() but simulate a failed independent review", project_path="demo"
            )

            self.assertFalse(result["ok"], result)
            self.assertTrue(result["rolled_back"], result)
            self.assertEqual(result["changed_files"], [])
            self.assertIn("calc.py", result["attempted_changed_files"])
            self.assertEqual(target.read_text(encoding="utf-8"), original)

    def test_verifier_exception_also_rolls_back(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            project = root / "workspace" / "demo"
            project.mkdir(parents=True)
            (root / "config" / "vps-test-plans").mkdir(parents=True)
            target = project / "calc.py"
            original = "def add(a, b):\n    return a - b\n"
            target.write_text(original, encoding="utf-8")

            responses = [
                json.dumps({"operation": "read", "path": "calc.py"}),
                json.dumps({"operation": "write", "path": "calc.py",
                            "content": "def add(a, b):\n    return a + b\n"}),
                json.dumps({"operation": "finish", "summary": "Changed implementation."}),
            ]
            dev = VPSDeveloper(FakeApp(root, responses))
            def broken_verify(_project):
                raise RuntimeError("simulated verifier failure")
            dev.verify = broken_verify
            result = dev.run("Fix add()", project_path="demo")

            self.assertFalse(result["ok"], result)
            self.assertTrue(result["rolled_back"], result)
            self.assertIn("simulated verifier failure", result["post_error"])
            self.assertEqual(target.read_text(encoding="utf-8"), original)

    def test_workspace_escape_and_hidden_write_are_blocked(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "config" / "vps-test-plans").mkdir(parents=True)
            dev = VPSDeveloper(FakeApp(root, []))
            with self.assertRaises(PermissionError):
                dev._project("../../escape")
            project = dev._project("safe")
            with self.assertRaises(PermissionError):
                dev.write(project, ".env", "SECRET=x")
            with self.assertRaises(PermissionError):
                dev.write(project, "../outside.py", "x=1")

    def test_test_execution_requires_admin_plan(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "config" / "vps-test-plans").mkdir(parents=True)
            dev = VPSDeveloper(FakeApp(root, []))
            project = dev._project("demo")
            with self.assertRaises(PermissionError):
                dev.approved_test(project, "missing")

            plan = root / "config" / "vps-test-plans" / "syntax.json"
            plan.write_text(json.dumps({
                "argv": [sys.executable, "-c", "print('approved-test')"],
                "timeout": 10,
            }), encoding="utf-8")
            result = dev.approved_test(project, "syntax")
            self.assertTrue(result["ok"], result)
            self.assertIn("approved-test", result["stdout"])

    def test_orchestrator_uses_vps_developer_not_sara_for_code(self):
        plan = Orchestrator(Bus(), object(), Policy()).plan("fix this code project")
        coding = [step for step in plan if step.agent == "local-developer"]
        self.assertEqual(len(coding), 1)
        self.assertEqual(coding[0].source, "hermes")


if __name__ == "__main__":
    unittest.main(verbosity=2)
