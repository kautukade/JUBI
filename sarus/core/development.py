"""Bounded Linux/VPS development workspace and local coding agent.

The model never receives an arbitrary shell primitive. File mutations are
restricted to Jubi's workspace, test commands are fixed recipes, and on Linux
they run through bubblewrap with no network and a read-only host filesystem.
"""
from __future__ import annotations

import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

from .hardware import admission, memory_snapshot
from .provider_policy import InferenceTransport, LOCAL_ONLY
from .sandbox_exec import landlock_abi, seccomp_library_name
from sarus.integrations.hermes_compact import compact_profile
from sarus.integrations.hermes_tool_compat import promote_text_tool_call


_TEXT_EXTENSIONS = {
    ".py", ".js", ".mjs", ".cjs", ".ts", ".tsx", ".jsx", ".json", ".md",
    ".html", ".css", ".scss", ".yml", ".yaml", ".toml", ".ini", ".cfg",
    ".txt", ".sh", ".sql", ".java", ".go", ".rs", ".c", ".h", ".cpp",
}
_BLOCKED_PARTS = {
    ".git", ".venv", "venv", "node_modules", "__pycache__", ".pytest_cache",
    ".mypy_cache", ".ruff_cache",
}
_BLOCKED_NAMES = {
    ".env", ".env.local", ".env.production", "id_rsa", "id_ed25519",
    "credentials.json", "secrets.json",
}


class DevelopmentWorkspace:
    """A single project view inside the Jubi workspace."""

    def __init__(self, root: Path, project: str | None = None):
        self.root = root.resolve()
        self.workspace_root = (self.root / "workspace").resolve()
        self.workspace_root.mkdir(parents=True, exist_ok=True)
        self.project = self._resolve_project(project)
        self.project.mkdir(parents=True, exist_ok=True)
        self.events: list[dict] = []
        self.baseline: dict[str, str | None] = {}
        self.modified: set[str] = set()
        self.test_runs: list[dict] = []

    def _resolve_project(self, hint: str | None) -> Path:
        if hint:
            raw = Path(str(hint))
            candidate = raw.resolve() if raw.is_absolute() else (self.workspace_root / raw).resolve()
        else:
            dirs = [p for p in self.workspace_root.iterdir() if p.is_dir() and not p.name.startswith(".")]
            candidate = dirs[0].resolve() if len(dirs) == 1 else self.workspace_root
        if candidate != self.workspace_root and self.workspace_root not in candidate.parents:
            raise PermissionError("Development project is outside Jubi workspace")
        return candidate

    def _path(self, value: str | None, *, allow_root: bool = False) -> Path:
        raw = str(value or ".").strip()
        p = (self.project / raw).resolve()
        if p != self.project and self.project not in p.parents:
            raise PermissionError("Path escapes the selected project")
        rel = p.relative_to(self.project)
        if any(part in _BLOCKED_PARTS for part in rel.parts):
            raise PermissionError("Path is inside a blocked runtime directory")
        if p.name in _BLOCKED_NAMES or p.name.startswith(".env"):
            raise PermissionError("Credential/environment files are not exposed to the coding agent")
        if p == self.project and not allow_root:
            raise PermissionError("A file path is required")
        return p

    def _text_file(self, path: Path):
        if path.suffix.lower() not in _TEXT_EXTENSIONS:
            raise PermissionError("Only reviewed text/code file types are writable")
        if path.exists() and path.stat().st_size > 300_000:
            raise ValueError("File exceeds the development read/write limit")

    def list_files(self, path: str = ".") -> dict:
        base = self._path(path, allow_root=True)
        if not base.is_dir():
            raise NotADirectoryError(str(base))
        items = []
        for child in sorted(base.iterdir(), key=lambda x: (not x.is_dir(), x.name.lower())):
            if child.name in _BLOCKED_PARTS or child.name in _BLOCKED_NAMES or child.name.startswith(".env"):
                continue
            rel = child.relative_to(self.project).as_posix()
            items.append({
                "path": rel,
                "is_dir": child.is_dir(),
                "size": child.stat().st_size if child.is_file() else None,
            })
            if len(items) >= 300:
                break
        result = {"ok": True, "operation": "list", "path": base.relative_to(self.project).as_posix() or ".", "items": items}
        self.events.append(result)
        return result

    def read(self, path: str) -> dict:
        p = self._path(path)
        self._text_file(p)
        if not p.is_file():
            raise FileNotFoundError(str(p))
        text = p.read_text(encoding="utf-8", errors="replace")
        if len(text) > 200_000:
            text = text[:200_000]
        result = {"ok": True, "operation": "read", "path": p.relative_to(self.project).as_posix(), "content": text}
        self.events.append({"ok": True, "operation": "read", "path": result["path"], "bytes": len(text.encode("utf-8"))})
        return result

    def write(self, path: str, content: str) -> dict:
        if not isinstance(content, str):
            raise ValueError("content must be text")
        if len(content.encode("utf-8")) > 250_000:
            raise ValueError("Write exceeds 250 KB")
        p = self._path(path)
        self._text_file(p)
        rel = p.relative_to(self.project).as_posix()
        if rel.startswith(".jubi/") or rel == ".jubi":
            raise PermissionError("Project execution policy is immutable to the agent")
        if rel not in self.baseline:
            self.baseline[rel] = p.read_text(encoding="utf-8", errors="replace") if p.exists() else None
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content, encoding="utf-8")
        self.modified.add(rel)
        result = {"ok": True, "operation": "write", "path": rel, "bytes": p.stat().st_size}
        self.events.append(result)
        return result

    def _configured_recipe(self) -> tuple[str, int]:
        cfg_path = self.project / ".jubi" / "project.json"
        if cfg_path.is_file():
            try:
                cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
            except (ValueError, OSError) as exc:
                raise RuntimeError("Invalid .jubi/project.json") from exc
            recipe = str(cfg.get("test_recipe", "auto"))
            timeout = max(5, min(int(cfg.get("test_timeout_seconds", 120)), 600))
            return recipe, timeout
        return "auto", 120

    def _recipe(self, requested: str | None = None) -> tuple[str, list[str], int]:
        configured, timeout = self._configured_recipe()
        recipe = str(requested or configured or "auto")
        if configured not in {"auto", recipe} and requested:
            raise PermissionError("Requested test recipe is not approved by project policy")
        if recipe == "auto":
            if (self.project / "pytest.ini").exists() or (self.project / "conftest.py").exists() or (self.project / "pyproject.toml").exists():
                if importlib.util.find_spec("pytest") is not None:
                    recipe = "pytest"
            if recipe == "auto":
                has_python_tests = (self.project / "tests").is_dir() or any(self.project.glob("test_*.py"))
                if has_python_tests:
                    recipe = "python_unittest"
            if recipe == "auto" and (self.project / "package.json").is_file() and shutil.which("node"):
                recipe = "node_test"
            if recipe == "auto":
                recipe = "python_compile"

        recipes = {
            "python_compile": [sys.executable, "-m", "compileall", "-q", "."],
            "python_unittest": [sys.executable, "-m", "unittest", "discover", "-v"],
            "pytest": [sys.executable, "-m", "pytest", "-q"],
            "node_test": ["node", "--test"],
        }
        argv = recipes.get(recipe)
        if not argv:
            raise PermissionError("Unsupported test recipe")
        if shutil.which(argv[0]) is None and not Path(argv[0]).exists():
            raise RuntimeError("Test runtime is missing: " + argv[0])
        return recipe, argv, timeout

    def _sandbox_argv(self, argv: list[str]) -> list[str]:
        if os.name == "nt":
            return argv
        helper = Path(__file__).with_name("sandbox_exec.py").resolve()
        if not helper.is_file():
            raise RuntimeError("Jubi Linux sandbox helper is missing")
        if landlock_abi() < 1:
            raise RuntimeError("Linux Landlock is required for autonomous VPS test execution")
        return [
            sys.executable,
            str(helper),
            "--project", str(self.project),
            "--",
            *argv,
        ]

    def test(self, recipe: str | None = None) -> dict:
        name, argv, timeout = self._recipe(recipe)
        started = time.monotonic()
        env = {
            "PATH": os.environ.get("PATH", ""),
            "PYTHONUTF8": "1",
            "PYTHONDONTWRITEBYTECODE": "1",
            "PYTHONNOUSERSITE": "1",
            "HOME": "/tmp",
            "TMPDIR": "/tmp",
            "NO_PROXY": "*",
            "no_proxy": "*",
        }
        cp = subprocess.run(
            self._sandbox_argv(argv),
            cwd=self.project,
            env=env,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            shell=False,
        )
        result = {
            "ok": cp.returncode == 0,
            "operation": "test",
            "recipe": name,
            "exit_code": cp.returncode,
            "stdout": cp.stdout[-30_000:],
            "stderr": cp.stderr[-30_000:],
            "latency_ms": round((time.monotonic() - started) * 1000, 2),
            "sandbox": "landlock+seccomp-no-network" if os.name != "nt" else "windows-user-process",
        }
        self.test_runs.append(result)
        self.events.append({k: v for k, v in result.items() if k not in {"stdout", "stderr"}})
        return result

    def diff(self) -> dict:
        git = shutil.which("git")
        if git and (self.project / ".git").exists():
            cp = subprocess.run(
                [git, "diff", "--no-ext-diff", "--", "."],
                cwd=self.project,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=20,
                shell=False,
            )
            text = cp.stdout[-100_000:]
        else:
            import difflib
            chunks = []
            for rel in sorted(self.modified):
                before = self.baseline.get(rel)
                after_path = self.project / rel
                after = after_path.read_text(encoding="utf-8", errors="replace") if after_path.exists() else None
                chunks.extend(difflib.unified_diff(
                    (before or "").splitlines(True),
                    (after or "").splitlines(True),
                    fromfile="a/" + rel,
                    tofile="b/" + rel,
                ))
            text = "".join(chunks)[-100_000:]
        result = {"ok": True, "operation": "diff", "diff": text, "changed_files": sorted(self.modified)}
        self.events.append({"ok": True, "operation": "diff", "changed_files": sorted(self.modified), "bytes": len(text.encode("utf-8"))})
        return result

    def git_status(self) -> dict:
        git = shutil.which("git")
        if not git or not (self.project / ".git").exists():
            return {"ok": True, "operation": "git_status", "tracked": False, "output": ""}
        cp = subprocess.run(
            [git, "status", "--short", "--branch"],
            cwd=self.project,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=20,
            shell=False,
        )
        return {"ok": cp.returncode == 0, "operation": "git_status", "tracked": True, "output": cp.stdout[-30_000:]}

    def verify(self) -> dict:
        test = self.test()
        diff = self.diff()
        return {
            "ok": bool(test["ok"]),
            "operation": "verify",
            "test": test,
            "diff": diff,
            "changed_files": sorted(self.modified),
        }

    def execute(self, operation: str, path: str = "", content: str = "", recipe: str = "") -> dict:
        operation = str(operation)
        if operation == "list":
            return self.list_files(path or ".")
        if operation == "read":
            return self.read(path)
        if operation == "write":
            return self.write(path, content)
        if operation == "test":
            return self.test(recipe or None)
        if operation == "diff":
            return self.diff()
        if operation == "git_status":
            return self.git_status()
        if operation == "verify":
            return self.verify()
        raise ValueError("Unsupported development operation")


class VPSDevelopmentAgent:
    """Local Ollama coding loop with typed tools and deterministic verification."""

    TOOL = {
        "type": "function",
        "function": {
            "name": "jubi_workspace",
            "description": "Operate only on the selected Jubi development project.",
            "parameters": {
                "type": "object",
                "required": ["operation"],
                "additionalProperties": False,
                "properties": {
                    "operation": {
                        "type": "string",
                        "enum": ["list", "read", "write", "test", "diff", "git_status", "verify"],
                    },
                    "path": {"type": "string", "maxLength": 4096},
                    "content": {"type": "string", "maxLength": 250000},
                    "recipe": {
                        "type": "string",
                        "enum": ["", "auto", "python_compile", "python_unittest", "pytest", "node_test"],
                    },
                },
            },
        },
    }

    def __init__(self, app):
        self.app = app
        self.root = app.root
        self.models = app.models

    def status(self) -> dict:
        seccomp = seccomp_library_name() if os.name != "nt" else None
        landlock = landlock_abi() if os.name != "nt" else 0
        return {
            "available": os.name == "nt" or bool(seccomp and landlock >= 1),
            "sandbox": "landlock+seccomp-no-network" if os.name != "nt" else "windows-user-process",
            "landlock_abi": landlock,
            "libseccomp": seccomp,
            "coding_model": self.models.choose("coding"),
        }

    def _select_model(self, goal: str) -> tuple[str, dict, dict]:
        decision = self.app.brain.route(goal, "coding")
        items = {x["name"]: x for x in self.models.list_models().get("items", [])}
        transport = InferenceTransport(self.models.base)
        attempts = []
        for candidate in decision.get("candidates", []):
            name = candidate.get("model")
            item = items.get(name)
            if not item:
                continue
            metadata = transport.json("/api/show", {"model": name})
            LOCAL_ONLY.check_model(name, metadata)
            profile = compact_profile(metadata, require_tools=True)
            resource = admission(item.get("size"), memory_snapshot(), allow_cpu_paging=True)
            attempts.append({"model": name, "profile": profile, "admission": resource})
            if profile.get("eligible") and resource.get("admitted"):
                return name, profile, {"decision": decision, "attempts": attempts}
        raise RuntimeError("No installed local coding model passed tool/context/RAM admission")

    @staticmethod
    def _tool_call(message: dict) -> tuple[str, dict] | None:
        calls = message.get("tool_calls") or []
        if not calls:
            return None
        if len(calls) != 1:
            raise RuntimeError("Coding agent emitted multiple tool calls in one turn")
        fn = calls[0].get("function") or {}
        name = fn.get("name")
        args = fn.get("arguments") or {}
        if isinstance(args, str):
            args = json.loads(args)
        if name != "jubi_workspace" or not isinstance(args, dict):
            raise PermissionError("Coding agent requested an unapproved tool")
        return name, args

    def run(self, goal: str, project: str | None = None, max_iterations: int = 16) -> dict:
        workspace = DevelopmentWorkspace(self.root, project)
        model, profile, routing = self._select_model(goal)
        transport = InferenceTransport(self.models.base)
        system = (
            "You are Jubi's autonomous VPS coding worker. Use only jubi_workspace. "
            "Inspect before editing. Make the smallest necessary changes. Run tests. "
            "Never claim success unless a tool result shows a passing test. "
            "Do not request shell, network, credentials, hidden files, or paths outside the project. "
            "When finished, call diff and then verify. After verify passes, answer with a concise summary."
        )
        messages = [
            {"role": "system", "content": system},
            {"role": "user", "content": str(goal)[:12000]},
        ]
        transcript = []
        final_text = ""
        for iteration in range(max(1, min(int(max_iterations), 24))):
            payload = {
                "model": model,
                "messages": messages,
                "stream": False,
                "tools": [self.TOOL],
                "options": {
                    "temperature": 0,
                    "num_ctx": profile["runtime_context"],
                    "num_predict": 768,
                },
                "keep_alive": 60,
            }
            raw = transport.json("/api/chat", payload, timeout=240)
            message = dict(raw.get("message") or {})
            promote_text_tool_call(message, [self.TOOL])
            transcript.append({
                "iteration": iteration + 1,
                "content": str(message.get("content") or "")[:4000],
                "tool_calls": message.get("tool_calls") or [],
            })
            call = self._tool_call(message)
            messages.append(message)
            if call is None:
                final_text = str(message.get("content") or "").strip()
                break
            _, arguments = call
            result = workspace.execute(**arguments)
            messages.append({
                "role": "tool",
                "tool_name": "jubi_workspace",
                "content": json.dumps(result, ensure_ascii=False, default=str)[:60_000],
            })
        verification = workspace.verify()
        diff = verification["diff"]
        changed = verification["changed_files"]
        requires_change = bool(re.search(r"\b(fix|build|implement|create|add|change|update|refactor|develop)\b", goal, re.I))
        passed = bool(verification["ok"] and (changed or not requires_change))

        reviewer = {"status": "NOT_RUN", "approved": passed, "reason": "Deterministic verifier result"}
        try:
            hstatus = self.app.hermes.status()
            if hstatus.get("ready") and diff.get("diff"):
                review_prompt = (
                    "Review this code diff against the user's goal. Do not execute tools. "
                    "Identify correctness/security/test issues and finish with APPROVED or REJECTED.\n\n"
                    + "GOAL:\n" + goal[:6000] + "\n\nDIFF:\n" + diff["diff"][:12000] + "\n\n"
                    + "TEST RESULT:\n" + json.dumps(verification["test"], ensure_ascii=False)[:6000]
                )
                hreview = self.app.hermes.analyze(review_prompt, model, timeout=180)
                reviewer = {"status": hreview.get("status"), "evidence": hreview}
        except Exception as exc:
            reviewer = {"status": "FAILED", "reason": str(exc)[:1000], "approved": passed}

        return {
            "ok": passed,
            "status": "completed" if passed else "failed",
            "mode": "vps_local_coding",
            "model": model,
            "project": str(workspace.project.relative_to(self.root)),
            "routing": routing,
            "compact_profile": profile,
            "iterations": len(transcript),
            "transcript": transcript,
            "events": workspace.events,
            "changed_files": changed,
            "diff": diff,
            "verification": verification,
            "reviewer": reviewer,
            "output": final_text or ("Verified coding task completed" if passed else "Coding task failed verification"),
        }
