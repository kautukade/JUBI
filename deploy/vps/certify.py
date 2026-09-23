#!/usr/bin/env python3
"""Live end-to-end certification for the Jubi Linux VPS full profile."""
from __future__ import annotations

import argparse
import json
import shutil
import sys
import time
import uuid
from pathlib import Path
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


def _json_request(url: str, *, token: str = "", body=None, timeout=30):
    data = None if body is None else json.dumps(body).encode("utf-8")
    headers = {"Accept": "application/json"}
    if data is not None:
        headers["Content-Type"] = "application/json"
    if token:
        headers["X-JUBI-Token"] = token
    request = Request(url, data=data, headers=headers, method="POST" if data is not None else "GET")
    with urlopen(request, timeout=timeout) as response:
        raw = response.read()
        return json.loads(raw or b"{}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="http://127.0.0.1:8877")
    parser.add_argument("--browser-url", default="https://example.com/")
    parser.add_argument("--keep-workspace", action="store_true")
    parser.add_argument("--json-output", default="")
    args = parser.parse_args()

    base = args.base.rstrip("/")
    root = Path(__file__).resolve().parents[2]
    checks = []
    cleanup = []
    token = ""

    def record(name, fn, *, required=True):
        started = time.perf_counter()
        try:
            detail = fn()
            ok = detail.get("ok", True) is not False if isinstance(detail, dict) else True
            checks.append({
                "name": name, "ok": bool(ok), "required": required,
                "latency_ms": round((time.perf_counter() - started) * 1000, 2),
                "detail": detail,
            })
            return detail
        except Exception as exc:
            checks.append({
                "name": name, "ok": False, "required": required,
                "latency_ms": round((time.perf_counter() - started) * 1000, 2),
                "error": str(exc),
            })
            return None

    health = record("HTTP health", lambda: _json_request(base + "/api/health"))
    session = record("Session bootstrap", lambda: _json_request(base + "/api/session"))
    if session:
        token = str(session.get("token") or "")

    readiness = record("Consolidated readiness", lambda: _json_request(base + "/api/vps/readiness"))
    if readiness and not readiness.get("ready"):
        checks[-1]["ok"] = False

    models = record("Local model catalogue", lambda: _json_request(base + "/api/models"))
    if models and (not models.get("online") or not models.get("models")):
        checks[-1]["ok"] = False

    providers = record("Provider policy", lambda: _json_request(base + "/api/providers"))
    if providers and providers.get("mode") != "local_only":
        checks[-1]["ok"] = False
        checks[-1]["detail"]["certification_error"] = "Full VPS certification requires Local Only provider mode"

    def brain_route():
        result = _json_request(
            base + "/api/brain/route", token=token,
            body={"text": "Write a small Python addition function", "task_type": "coding"},
            timeout=60,
        )
        result["ok"] = bool(result.get("selected_model"))
        return result
    record("Brain model routing", brain_route)

    def live_chat():
        result = _json_request(
            base + "/api/chat", token=token,
            body={"text": "Reply with exactly JUBI-LIVE-OK", "provider": "ollama", "task_type": "general"},
            timeout=300,
        )
        text = str(result.get("response") or result.get("output") or "").strip()
        result["certification_match"] = "JUBI-LIVE-OK" in text
        result["ok"] = bool(result["certification_match"])
        return result
    record("Local model inference", live_chat)

    def council_check():
        result = _json_request(
            base + "/api/council/run", token=token,
            body={"text": "What is 2 + 2? Answer concisely.", "task_type": "general", "max_members": 2, "judge_provider": "ollama"},
            timeout=600,
        )
        result["ok"] = bool(result.get("final") and result.get("members"))
        return result
    record("AI Council live deliberation", council_check)

    def supervisor_check():
        result = _json_request(
            base + "/api/supervisor/run", token=token,
            body={"text": "Create a two-step plan to verify a small Python function.", "task_type": "planning", "provider": "ollama"},
            timeout=600,
        )
        result["ok"] = bool(result.get("plan") and result.get("review"))
        return result
    record("Supervisor planner/reviewer", supervisor_check)

    def hermes_check():
        coding_model = None
        for item in (models or {}).get("items", []):
            if item.get("kind") == "coding":
                coding_model = item.get("name")
                break
        coding_model = coding_model or "qwen3:8b"
        result = _json_request(
            base + "/api/capability/run", token=token,
            body={
                "id": "hermes.analysis",
                "parameters": {
                    "prompt": "Analyze the statement: 2 + 2 = 4. Reply with a concise confirmation.",
                    "model": coding_model,
                    "context": "VPS live certification; analysis only.",
                },
            },
            timeout=420,
        )
        delegated = result.get("result") or {}
        result["ok"] = bool(delegated and delegated.get("tools_executed") is False)
        return result
    record("Hermes delegated analysis", hermes_check)

    namespace = "vps-cert-" + uuid.uuid4().hex[:12]
    doc_id = ""
    def rag_ingest():
        nonlocal doc_id
        marker = "ORBIT-" + uuid.uuid4().hex[:10]
        result = _json_request(
            base + "/api/knowledge/ingest", token=token,
            body={
                "title": "VPS certification marker",
                "content": "The private certification marker is " + marker + ".",
                "namespace": namespace,
                "source": "vps-certification",
            },
            timeout=180,
        )
        doc_id = str(result.get("id") or result.get("document_id") or "")
        cleanup.append(("knowledge", doc_id))
        result["marker"] = marker
        result["ok"] = bool(doc_id)
        return result

    ingested = record("RAG embedding ingest", rag_ingest)
    if ingested:
        marker = ingested.get("marker", "")
        def rag_search():
            result = _json_request(
                base + "/api/knowledge/search", token=token,
                body={"query": marker, "namespace": namespace, "limit": 5},
                timeout=180,
            )
            serialized = json.dumps(result, ensure_ascii=False)
            return {"ok": marker in serialized, "marker": marker, "result": result}
        record("RAG semantic retrieval", rag_search)

    def browser_check():
        result = _json_request(
            base + "/api/browser/read", token=token,
            body={"url": args.browser_url, "timeout": 30, "wait_ms": 0},
            timeout=60,
        )
        result["ok"] = bool(result.get("ok") and result.get("text"))
        return result
    record("Headless public browser", browser_check)

    def research_fetch():
        result = _json_request(
            base + "/api/research/fetch", token=token,
            body={"url": args.browser_url, "timeout": 20},
            timeout=40,
        )
        serialized = json.dumps(result, ensure_ascii=False)
        return {"ok": bool(result and "Example Domain" in serialized), "result": result}
    record("Public web research fetch", research_fetch)

    def operator_check():
        result = _json_request(
            base + "/api/system/action", token=token,
            body={"action_id": "system.processes.list", "parameters": {}},
            timeout=40,
        )
        payload = result.get("result") or {}
        result["ok"] = bool(result.get("ok") and payload.get("ok"))
        return result
    record("Typed Linux Computer Operator", operator_check)

    tiny_png = (
        "data:image/png;base64,"
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Y9ZQmcAAAAASUVORK5CYII="
    )
    def vision_check():
        result = _json_request(
            base + "/api/vision/analyze", token=token,
            body={"image": tiny_png, "prompt": "Describe what is visible in this tiny image in one short sentence.", "timeout": 300},
            timeout=360,
        )
        result["ok"] = bool(str(result.get("response") or "").strip() and result.get("provider") == "ollama-local")
        return result
    record("Local vision inference", vision_check)

    project_name = "vps-certification-" + uuid.uuid4().hex[:8]
    project = root / "workspace" / project_name
    project.mkdir(parents=True, exist_ok=True)
    cleanup.append(("project", str(project)))
    target = project / "calc.py"
    target.write_text("def add(a, b):\n    return a - b\n", encoding="utf-8")

    def developer_check():
        result = _json_request(
            base + "/api/developer/run", token=token,
            body={
                "text": "Inspect calc.py and fix add(a, b) so it returns a + b. Change only what is necessary. Verify before finishing.",
                "project": project_name,
                "max_iterations": 12,
            },
            timeout=600,
        )
        current = target.read_text(encoding="utf-8", errors="replace")
        result["observed_file_correct"] = "return a + b" in current
        result["ok"] = bool(result.get("ok") and result["observed_file_correct"]
                            and result.get("verification", {}).get("ok")
                            and result.get("review", {}).get("approved") is True)
        return result
    record("Autonomous developer edit/verify/review", developer_check)

    swarm_project_name = "vps-swarm-cert-" + uuid.uuid4().hex[:8]
    swarm_project = root / "workspace" / swarm_project_name
    swarm_project.mkdir(parents=True, exist_ok=True)
    cleanup.append(("project", str(swarm_project)))
    (swarm_project / "mathutil.py").write_text(
        "def multiply(a, b):\n    return a + b\n", encoding="utf-8"
    )

    def swarm_check():
        result = _json_request(
            base + "/api/swarm/run", token=token,
            body={
                "text": (
                    "Use the coding specialist to inspect this project and fix mathutil.py so "
                    "multiply(a, b) returns a * b. Verify the code and then summarize the result."
                ),
                "project": swarm_project_name,
                "provider": "ollama",
                "max_sources": 2,
            },
            timeout=900,
        )
        current = (swarm_project / "mathutil.py").read_text(encoding="utf-8", errors="replace")
        result["observed_file_correct"] = "return a * b" in current
        result["ok"] = bool(
            result.get("status") == "completed"
            and int(result.get("tool_backed_steps") or 0) >= 1
            and result["observed_file_correct"]
        )
        return result
    record("Executable multi-agent swarm", swarm_check)

    def automation_check():
        name = "VPS certification " + uuid.uuid4().hex[:8]
        created = _json_request(
            base + "/api/automation", token=token,
            body={"name": name, "prompt": "Certification no-op task", "interval_seconds": 86400, "enabled": False},
        )
        automation_id = str(created.get("id") or "")
        if automation_id:
            cleanup.append(("automation", automation_id))
        rows = _json_request(base + "/api/automations")
        found = any(str(x.get("name")) == name for x in rows) if isinstance(rows, list) else False
        return {"ok": bool(automation_id and found), "created": created, "found": found}
    record("Persistent automation scheduler", automation_check)

    def host_header_boundary():
        request = Request(base + "/api/session", headers={"Host": "attacker.invalid"})
        try:
            urlopen(request, timeout=10)
        except HTTPError as exc:
            return {"ok": exc.code == 403, "status": exc.code}
        return {"ok": False, "status": 200}
    record("Host-header security boundary", host_header_boundary)

    for kind, value in cleanup:
        try:
            if kind == "knowledge" and value:
                _json_request(base + "/api/knowledge/delete", token=token, body={"id": value})
            elif kind == "automation" and value:
                _json_request(base + "/api/automation/delete", token=token, body={"id": value})
            elif kind == "project" and not args.keep_workspace:
                shutil.rmtree(value, ignore_errors=True)
        except Exception:
            pass

    failed = [x["name"] for x in checks if x["required"] and not x["ok"]]
    report = {
        "schema": "jubi.vps.live-certification.v1",
        "status": "PASS" if not failed else "FAIL",
        "base": base,
        "failed_required": failed,
        "checks": checks,
    }
    output = json.dumps(report, indent=2, ensure_ascii=False, default=str)
    print(output)
    if args.json_output:
        Path(args.json_output).write_text(output + "\n", encoding="utf-8")
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())
