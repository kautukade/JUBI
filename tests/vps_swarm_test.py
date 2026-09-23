from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from sarus.core.swarm import VPSSwarm


class FakeSupervisor:
    def plan(self, request, task_type="auto", provider="auto"):
        return {
            "classification": {"task_type": "coding"},
            "plan": {
                "goal": request,
                "steps": [
                    {"id": "S1", "role": "research", "task": "Find requirements", "depends_on": []},
                    {"id": "S2", "role": "coding", "task": "Implement fix", "depends_on": ["S1"]},
                    {"id": "S3", "role": "business", "task": "Summarize impact", "depends_on": ["S2"]},
                ],
                "verification": ["review"],
            },
            "planner_route": {},
        }


class FakeDeveloper:
    def __init__(self):
        self.calls = []

    def run(self, request, project_path="."):
        self.calls.append((request, project_path))
        return {
            "ok": True,
            "summary": "implemented",
            "diff": "+ fixed",
            "verification": {"ok": True},
            "review": {"approved": True},
        }


class FakeResearch:
    def research(self, query, max_sources=4, provider="auto"):
        return {
            "answer": "requirement evidence",
            "sources": [{"ref": "W1", "url": "https://example.com"}],
        }


class FakeProviders:
    def __init__(self):
        self.calls = []

    def mode(self):
        return "local_only"

    def generate(self, prompt, task_type="general", provider="auto", system="", **kwargs):
        self.calls.append((task_type, prompt, system))
        if "Swarm Reviewer" in system:
            return {"response": "final verified result", "model": "local"}
        return {"response": "business impact", "model": "local"}


class FakeExperience:
    def record(self, *args, **kwargs):
        return {"id": "exp"}


class FakeBus:
    def __init__(self):
        self.events = []

    def emit(self, kind, payload):
        self.events.append((kind, payload))


class App:
    def __init__(self):
        self.supervisor = FakeSupervisor()
        self.developer = FakeDeveloper()
        self.research = FakeResearch()
        self.providers = FakeProviders()
        self.experience = FakeExperience()
        self.bus = FakeBus()


class VPSSwarmTests(unittest.TestCase):
    def test_tool_backed_roles_execute_and_review(self):
        app = App()
        swarm = VPSSwarm(app)
        result = swarm.run("research and fix the project", project="demo")
        self.assertEqual(result["status"], "completed", result)
        self.assertEqual(result["tool_backed_steps"], 2)
        self.assertEqual([x["status"] for x in result["results"]], ["success", "success", "success"])
        self.assertEqual(result["final"], "final verified result")
        self.assertEqual(app.developer.calls[0][1], "demo")
        self.assertTrue(any(kind == "VPS_SWARM_COMPLETED" for kind, _ in app.bus.events))

    def test_failed_dependency_is_not_executed(self):
        app = App()

        class BadResearch:
            def research(self, *args, **kwargs):
                raise RuntimeError("research unavailable")

        app.research = BadResearch()
        result = VPSSwarm(app).run("research and fix the project")
        self.assertEqual(result["status"], "partial")
        self.assertEqual(result["results"][0]["status"], "failed")
        self.assertEqual(result["results"][1]["status"], "skipped")
        self.assertEqual(result["results"][2]["status"], "skipped")
        self.assertEqual(app.developer.calls, [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
