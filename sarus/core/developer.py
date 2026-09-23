"""Bounded autonomous development loop for Jubi VPS.

The model never receives a shell. It may request a small set of typed workspace
operations. Files are confined to root/workspace and verification commands are
either non-executing syntax checks or an explicit administrator-authored test
plan stored outside the model-writable project tree.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
from pathlib import Path


_TEXT_EXTENSIONS = {
    ".py", ".js", ".mjs", ".cjs", ".ts", ".tsx", ".jsx", ".json", ".md",
    ".html", ".css", ".scss", ".yml", ".yaml", ".toml", ".ini", ".cfg",
    ".sh", ".sql", ".txt",
}
_SKIP_DIRS = {".git", "node_modules", ".venv", "venv", "__pycache__", "dist", "build"}
_MAX_FILE_BYTES = 200_000
_MAX_MODEL_CONTEXT = 22_000


class VPSDeveloper:
    def __init__(self, app):
        self.app = app
        self.root = app.root.resolve()
        self.workspace = (self.root / "workspace").resolve()
        self.workspace.mkdir(parents=True, exist_ok=True)
        self.test_plan_dir = (self.root / "config" / "vps-test-plans").resolve()
        self.test_plan_dir.mkdir(parents=True, exist_ok=True)

    def _project(self, value: str | None) -> Path:
        rel = str(value or ".").strip() or "."
        candidate = (self.workspace / rel).resolve()
        if candidate != self.workspace and self.workspace not in candidate.parents:
            raise PermissionError("project path escapes Jubi workspace")
        candidate.mkdir(parents=True, exist_ok=True)
        return candidate

    @staticmethod
    def _clean_rel(value: str) -> str:
        rel = str(value or "").replace("\\", "/").strip().lstrip("/")
        if not rel or rel in {".", ".."}:
            raise ValueError("a project-relative file path is required")
        parts = Path(rel).parts
        if any(part in {"", ".", ".."} or part.startswith(".") for part in parts):
            raise PermissionError("hidden and parent paths are not model-writable")
        return rel

    def _file(self, project: Path, value: str, *, must_exist=False) -> Path:
        rel = self._clean_rel(value)
        path = (project / rel).resolve()
        if path != project and project not in path.parents:
            raise PermissionError("file path escapes project")
        if path.suffix.lower() not in _TEXT_EXTENSIONS:
            raise PermissionError("file extension is not in the text-workspace allowlist")
        if must_exist and not path.is_file():
            raise FileNotFoundError(rel)
        return path

    def inventory(self, project: Path) -> list[dict]:
        out = []
        if not project.exists():
            return out
        for base, dirs, files in os.walk(project, followlinks=False):
            dirs[:] = sorted(d for d in dirs if d not in _SKIP_DIRS and not d.startswith("."))
            for name in sorted(files):
                path = Path(base) / name
                if name.startswith(".") or path.suffix.lower() not in _TEXT_EXTENSIONS:
                    continue
                try:
                    size = path.stat().st_size
                except OSError:
                    continue
                out.append({"path": path.relative_to(project).as_posix(), "size": size})
                if len(out) >= 400:
                    return out
        return out

    def read(self, project: Path, rel: str) -> dict:
        path = self._file(project, rel, must_exist=True)
        if path.stat().st_size > _MAX_FILE_BYTES:
            raise ValueError("file exceeds Jubi bounded read limit")
        return {"path": path.relative_to(project).as_posix(),
                "content": path.read_text(encoding="utf-8", errors="replace")}

    def write(self, project: Path, rel: str, content: str) -> dict:
        path = self._file(project, rel)
        raw = str(content)
        if len(raw.encode("utf-8")) > _MAX_FILE_BYTES:
            raise ValueError("file exceeds Jubi bounded write limit")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(raw, encoding="utf-8")
        return {"path": path.relative_to(project).as_posix(), "bytes": path.stat().st_size}

    @staticmethod
    def _run(argv: list[str], project: Path, timeout=90) -> dict:
        env = {
            "PATH": os.environ.get("PATH", ""),
            "LANG": os.environ.get("LANG", "C.UTF-8"),
            "LC_ALL": os.environ.get("LC_ALL", "C.UTF-8"),
            "PYTHONNOUSERSITE": "1",
            "PYTHONDONTWRITEBYTECODE": "1",
            "HOME": str(project),
        }
        try:
            cp = subprocess.run(argv, cwd=project, env=env, capture_output=True, text=True,
                                errors="replace", timeout=timeout, shell=False)
            return {"argv": argv, "exit_code": cp.returncode,
                    "stdout": cp.stdout[-20_000:], "stderr": cp.stderr[-20_000:],
                    "ok": cp.returncode == 0}
        except (OSError, subprocess.TimeoutExpired) as exc:
            return {"argv": argv, "exit_code": -1, "stdout": "", "stderr": str(exc), "ok": False}

    def verify(self, project: Path) -> dict:
        checks = []
        items = self.inventory(project)
        py_files = [x["path"] for x in items if x["path"].endswith(".py")]
        if py_files:
            checks.append(self._run([os.sys.executable, "-m", "compileall", "-q", *py_files], project, 60))
        json_files = [x["path"] for x in items if x["path"].endswith(".json")]
        for rel in json_files[:50]:
            try:
                json.loads((project / rel).read_text(encoding="utf-8"))
                checks.append({"argv": ["json-parse", rel], "exit_code": 0, "stdout": "", "stderr": "", "ok": True})
            except Exception as exc:
                checks.append({"argv": ["json-parse", rel], "exit_code": 1, "stdout": "", "stderr": str(exc), "ok": False})
        node = shutil_which("node")
        js_files = [x["path"] for x in items if Path(x["path"]).suffix.lower() in {".js", ".mjs", ".cjs"}]
        if node:
            for rel in js_files[:40]:
                checks.append(self._run([node, "--check", rel], project, 30))
        ok = all(x.get("ok") for x in checks) if checks else True
        return {"ok": ok, "checks": checks, "mode": "bounded-static-verification"}

    def approved_test(self, project: Path, plan_name: str) -> dict:
        name = re.sub(r"[^A-Za-z0-9_.-]", "", str(plan_name or ""))
        if not name:
            raise ValueError("test plan name is required")
        plan_path = (self.test_plan_dir / (name + ".json")).resolve()
        if self.test_plan_dir not in plan_path.parents or not plan_path.is_file():
            raise PermissionError("administrator-approved test plan not found")
        plan = json.loads(plan_path.read_text(encoding="utf-8"))
        argv = plan.get("argv")
        if not isinstance(argv, list) or not argv or not all(isinstance(x, str) and x for x in argv):
            raise ValueError("invalid approved test plan")
        allowed = {"python", "python3", os.path.basename(os.sys.executable), "node", "npm"}
        if os.path.basename(argv[0]) not in allowed:
            raise PermissionError("approved test executable is outside Jubi allowlist")
        return self._run(argv, project, min(max(int(plan.get("timeout", 120)), 1), 600))

    def diff(self, project: Path) -> dict:
        git = shutil_which("git")
        if not git or not (project / ".git").exists():
            return {"ok": True, "diff": "", "mode": "no-git-repository"}
        result = self._run([git, "diff", "--", "."], project, 30)
        return {"ok": result["ok"], "diff": result["stdout"][-30_000:], "stderr": result["stderr"]}

    @staticmethod
    def _json_object(text: str) -> dict:
        raw = str(text or "").strip()
        if raw.startswith("~~~"):
            raw = raw.strip("~").removeprefix("json").strip()
        try:
            value = json.loads(raw)
        except ValueError:
            match = re.search(r"\{.*\}", raw, flags=re.S)
            if not match:
                raise ValueError("model did not return a JSON action")
            value = json.loads(match.group(0))
        if not isinstance(value, dict):
            raise ValueError("model action must be a JSON object")
        return value

    def _tool(self, project: Path, action: dict) -> dict:
        op = str(action.get("operation", "")).strip().lower()
        if op == "inventory":
            return {"ok": True, "files": self.inventory(project)}
        if op == "read":
            return {"ok": True, **self.read(project, str(action.get("path", "")))}
        if op == "write":
            return {"ok": True, **self.write(project, str(action.get("path", "")), str(action.get("content", "")))}
        if op == "verify":
            return self.verify(project)
        if op == "test":
            return self.approved_test(project, str(action.get("plan", "")))
        if op == "diff":
            return self.diff(project)
        if op == "finish":
            return {"ok": True, "finished": True, "summary": str(action.get("summary", ""))[:4000]}
        raise ValueError("unsupported developer operation")

    def run(self, request: str, project_path=".", model=None, max_iterations=10) -> dict:
        project = self._project(project_path)
        model = model or self.app.models.choose("coding") or self.app.models.choose("general")
        if not model:
            return {"ok": False, "status": "DEPENDENCY_MISSING", "error": "No installed local coding/general model"}
        system = (
            "You are Jubi VPS Developer. You have ONLY a typed project workspace tool. "
            "Return exactly one JSON object each turn. Allowed operations: "
            "inventory; read with path; write with path and content; verify; test with plan; diff; "
            "finish with summary. Never request shell, network, hidden files, parent paths, "
            "credentials, package installation or commands. Inspect before editing. "
            "After every edit run verify. Use test only when an administrator-approved "
            "test plan name is already present in the task/context. Finish only after verification."
        )
        transcript = []
        observation = {"task": str(request), "project": str(project.relative_to(self.workspace)),
                       "files": self.inventory(project)}
        writes = 0
        changed_paths = []
        final_summary = ""
        for index in range(max(1, min(int(max_iterations), 20))):
            prompt = (
                "TASK:\n" + str(request)[:6000] +
                "\n\nCURRENT OBSERVATION:\n" + json.dumps(observation, ensure_ascii=False)[:_MAX_MODEL_CONTEXT] +
                "\n\nRECENT TOOL HISTORY:\n" + json.dumps(transcript[-5:], ensure_ascii=False)[:10_000] +
                "\n\nReturn one JSON action now."
            )
            try:
                response = self.app.models.generate_text(prompt, "coding", system=system, model=model, timeout=180)
                action = self._json_object(response)
                result = self._tool(project, action)
            except Exception as exc:
                transcript.append({"iteration": index + 1, "error": str(exc)[:1000]})
                if index >= 2:
                    break
                observation = {"ok": False, "error": str(exc)[:1000],
                               "instruction": "Return a valid allowed JSON action only."}
                continue
            transcript.append({"iteration": index + 1, "action": action, "result": result})
            observation = result
            if action.get("operation") == "write" and result.get("ok"):
                writes += 1
                written = str(result.get("path", ""))
                if written and written not in changed_paths:
                    changed_paths.append(written)
            if result.get("finished"):
                final_summary = result.get("summary", "")
                break

        verification = self.verify(project)
        diff = self.diff(project)
        review_evidence = diff.get("diff", "")
        if writes and not review_evidence:
            snapshots = []
            for rel in changed_paths[:20]:
                try:
                    current = self.read(project, rel)
                    snapshots.append("FILE " + rel + "\n" + current["content"][:12000])
                except Exception as exc:
                    snapshots.append("FILE " + rel + " unreadable: " + str(exc))
            review_evidence = "\n\n".join(snapshots)
        reviewer = self.review(request, review_evidence, verification, model=model) if writes else {
            "approved": verification["ok"], "reason": "No file writes were performed"
        }
        ok = bool(verification.get("ok") and reviewer.get("approved") is True and (writes > 0 or final_summary))
        return {
            "ok": ok,
            "status": "completed" if ok else "failed",
            "mode": "vps-bounded-developer",
            "model": model,
            "project": str(project.relative_to(self.workspace)),
            "writes": writes,
            "changed_files": changed_paths,
            "verification": verification,
            "review": reviewer,
            "diff": diff.get("diff", ""),
            "summary": final_summary,
            "transcript": transcript,
            "tools_executed": True,
        }

    def review(self, request: str, diff: str, verification: dict, model=None) -> dict:
        if not diff:
            return {"approved": False, "reason": "No changed-file evidence was available for independent review"}
        prompt = (
            "Review this bounded workspace change. Return ONLY JSON with boolean approved and string reason."
            "\nTASK:\n" + str(request)[:4000] +
            "\nDIFF:\n" + diff[-16_000:] +
            "\nVERIFICATION:\n" + json.dumps(verification, ensure_ascii=False)[-5000:]
        )
        try:
            raw = self.app.models.generate_text(
                prompt, "coding",
                system="You are Jubi independent code reviewer. Do not edit files. Judge correctness and whether verification supports completion.",
                model=model, timeout=120,
            )
            value = self._json_object(raw)
            return {"approved": value.get("approved") is True, "reason": str(value.get("reason", ""))[:2000]}
        except Exception as exc:
            return {"approved": False, "reason": "Reviewer failed: " + str(exc)[:1000]}


def shutil_which(name: str):
    import shutil
    return shutil.which(name)
