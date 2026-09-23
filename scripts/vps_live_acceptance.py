#!/usr/bin/env python3
"""Live, non-destructive acceptance for the Jubi Linux VPS full profile.

This suite intentionally uses the *real* local Ollama endpoint and the installed
Jubi runtime. It does not mock model output. It is safe to run repeatedly: all
development work is confined to workspace/.jubi-certification and no external
actions are performed.
"""
from __future__ import annotations

import base64
import json
import os
import shutil
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from sarus.core.app import Jubi

# 1x1 PNG, used only to prove that bytes reach the local vision model.
PNG_1X1 = (
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/"
    "x8AAusB9Y9ZQMcAAAAASUVORK5CYII="
)


def check(name, fn, required=True):
    started = time.perf_counter()
    try:
        detail = fn()
        return {
            "name": name, "ok": True, "required": required,
            "latency_ms": round((time.perf_counter() - started) * 1000, 2),
            "detail": detail,
        }
    except Exception as exc:
        return {
            "name": name, "ok": False, "required": required,
            "latency_ms": round((time.perf_counter() - started) * 1000, 2),
            "error": str(exc)[:2000],
        }


def main():
    if os.environ.get("JUBI_DEPLOYMENT_PROFILE", "linux_vps") != "linux_vps":
        raise SystemExit("Live VPS acceptance requires JUBI_DEPLOYMENT_PROFILE=linux_vps")

    app = Jubi(ROOT)
    rows = []
    try:
        rows.append(check("consolidated readiness", lambda: _assert_ready(app)))
        rows.append(check("general local inference", lambda: _infer(app, "general")))
        rows.append(check("coding local inference", lambda: _infer(app, "coding")))
        rows.append(check("persistent chat", lambda: _chat(app)))
        rows.append(check("knowledge RAG", lambda: _knowledge(app)))
        rows.append(check("AI Council", lambda: _council(app)))
        rows.append(check("Supervisor", lambda: _supervisor(app)))
        rows.append(check("Hermes child analysis", lambda: _hermes(app)))
        rows.append(check("local embedding", lambda: _embed(app)))
        rows.append(check("local vision", lambda: _vision(app)))
        rows.append(check("offline voice STT/TTS", lambda: _voice(app)))
        rows.append(check("public research fetch", lambda: _research(app)))
        rows.append(check("headless Chromium", lambda: _browser(app)))
        rows.append(check("bounded developer edit-review", lambda: _developer(app)))
        rows.append(check("swarm orchestration", lambda: _swarm(app)))
        rows.append(check("persistent task execution", lambda: _task(app)))
        rows.append(check("automation persistence", lambda: _automation(app)))
        rows.append(check("Linux host operator", lambda: _operator(app)))
        rows.append(check("approval proof round-trip", lambda: _approval(app)))
        rows.append(check("Fable core integration", lambda: _fable(app)))
        rows.append(check("receipt chain", lambda: _receipts(app)))
    finally:
        app.shutdown()

    failed = [row["name"] for row in rows if row["required"] and not row["ok"]]
    report = {
        "profile": "linux_vps",
        "live": True,
        "mocked_model_output": False,
        "passed": not failed,
        "failed_required": failed,
        "checks": rows,
    }
    print(json.dumps(report, indent=2, ensure_ascii=False))
    return 0 if report["passed"] else 1


def _assert_ready(app):
    result = app.vps_readiness.run()
    if not result.get("ready"):
        raise RuntimeError("readiness failed: " + ", ".join(result.get("failed_required", [])))
    return {"failed_required": [], "checks": len(result.get("checks", []))}


def _infer(app, role):
    model = app.models.choose(role)
    if not model:
        raise RuntimeError("no model selected for " + role)
    response = app.models.generate_text(
        'Reply with exactly: JUBI_OK',
        role,
        system='This is a runtime health check. Reply only with JUBI_OK.',
        model=model,
        timeout=180,
    )
    if "JUBI_OK" not in response.upper().replace(" ", "_"):
        raise RuntimeError(f"unexpected {role} response: {response[:300]}")
    return {"model": model, "response": response[:120]}


def _chat(app):
    first = app.conversations.send(
        "Remember this certification token: ORBIT-731.",
        task_type="general",
        provider="ollama",
    )
    cid = first.get("conversation_id")
    if not cid:
        raise RuntimeError("chat did not create a conversation")
    second = app.conversations.send(
        "Reply with the certification token I gave you.",
        conversation_id=cid,
        task_type="general",
        provider="ollama",
    )
    history = app.conversations.history(cid)
    if len(history.get("messages", [])) < 4:
        raise RuntimeError("conversation history did not persist two turns")
    return {"conversation_id": cid, "messages": len(history["messages"]), "response": str(second.get("response", ""))[:160]}


def _knowledge(app):
    namespace = "vps-certification"
    doc = app.knowledge.ingest(
        "Project Comet uses port 4321. Its certification keyword is sapphire.",
        title="Jubi VPS certification fact",
        namespace=namespace,
        source="live-acceptance",
    )
    try:
        matches = app.knowledge.search("What port does Project Comet use?", namespace=namespace, limit=3)
        if not matches:
            raise RuntimeError("knowledge search returned no matches")
        answer = app.knowledge.answer("Which port does Project Comet use?", namespace=namespace, limit=3, provider="ollama")
        if not str(answer.get("answer", "")).strip():
            raise RuntimeError("knowledge answer is empty")
        return {"document_id": doc["id"], "matches": len(matches), "answer": str(answer["answer"])[:240]}
    finally:
        app.knowledge.delete_document(doc["id"])


def _council(app):
    result = app.council.run(
        "Give two concise checks for a healthy local AI service.",
        task_type="general",
        max_members=2,
        judge_provider="ollama",
    )
    if not result.get("members") or not str(result.get("final", "")).strip():
        raise RuntimeError("Council produced no verified result")
    return {"members": len(result["members"]), "final": str(result["final"])[:240]}


def _supervisor(app):
    result = app.supervisor.run(
        "Prepare a three-point checklist for testing a Python health endpoint.",
        task_type="planning",
        provider="ollama",
    )
    final = str(result.get("final") or result.get("review") or "")
    if not final.strip():
        raise RuntimeError("Supervisor produced no final review")
    return {"steps": len(result.get("results", [])), "final": final[:240]}


def _hermes(app):
    model = app.models.choose("general") or app.models.choose("coding")
    result = app.hermes.analyze(
        "Return a concise analysis of why health checks matter.",
        model=model,
        context="Jubi live VPS certification; no tools are required.",
        timeout=180,
    )
    if result.get("status") in {"FAILED", "DEPENDENCY_MISSING"}:
        raise RuntimeError("Hermes analysis failed: " + json.dumps(result, default=str)[-1500:])
    if not result.get("parent_session") or not result.get("session_archive"):
        raise RuntimeError("Hermes did not persist real session evidence")
    return {"status": result.get("status"), "parent_session": result.get("parent_session"), "session_archive": result.get("session_archive")}


def _embed(app):
    model = app.models.choose("embedding")
    vector = app.models.embed("Jubi VPS live acceptance", model=model)
    if not isinstance(vector, list) or len(vector) < 8:
        raise RuntimeError("embedding vector missing or unexpectedly short")
    return {"model": model, "dimensions": len(vector)}


def _vision(app):
    model = app.models.choose("vision")
    result = app.vision.analyze(
        "data:image/png;base64," + PNG_1X1,
        "This is a health-check image. Briefly confirm that an image was received.",
        model=model,
        timeout=240,
    )
    text = str(result.get("response") or "")
    if not text.strip():
        raise RuntimeError("vision model returned no response")
    return {"model": model, "response": text[:240]}


def _voice(app):
    status = app.voice.status()
    if not status.get("ready"):
        raise RuntimeError("offline voice runtime is not ready: " + str(status))
    tts = app.voice.synthesize("This is a Jubi speech test.", "en", 160)
    if not tts.get("ok") or not str(tts.get("audio", "")).startswith("data:audio/wav;base64,"):
        raise RuntimeError("local TTS failed")
    stt = app.voice.transcribe(tts["audio"], language="en")
    if not stt.get("ok") or not str(stt.get("text", "")).strip():
        raise RuntimeError("local STT returned no text")
    return {
        "tts_mode": tts.get("mode"),
        "stt_mode": stt.get("mode"),
        "transcript": str(stt.get("text", ""))[:240],
        "cloud_speech": status.get("cloud_speech"),
        "microphone_capture": status.get("microphone_capture"),
    }


def _research(app):
    result = app.research.fetch("https://example.com/", timeout=20)
    if not result.get("ok") or "Example Domain" not in (result.get("title", "") + result.get("text", "")):
        raise RuntimeError("public research fetch did not return expected page")
    return {"url": result.get("url"), "title": result.get("title")}


def _browser(app):
    status = app.browser.status()
    if not status.get("ready"):
        raise RuntimeError("Playwright Chromium is not ready")
    result = app.browser.read("https://example.com/", timeout=30, wait_ms=0)
    if not result.get("ok") or "Example Domain" not in (result.get("title", "") + result.get("text", "")):
        raise RuntimeError("Chromium did not render expected public page")
    return {"engine": status.get("engine"), "title": result.get("title")}


def _developer(app):
    rel = ".jubi-certification"
    project = app.developer._project(rel)
    shutil.rmtree(project, ignore_errors=True)
    project.mkdir(parents=True)
    (project / "calc.py").write_text("def add(a, b):\n    return a - b\n", encoding="utf-8")

    # Keep the task extremely small so this is a capability acceptance, not a
    # benchmark. Two attempts reduce model-format flakiness without hiding a
    # repeatable failure.
    last = None
    for _ in range(2):
        last = app.developer.run(
            "Open calc.py. Fix add(a,b) so it returns a + b. Verify the file, then finish.",
            project_path=rel,
            max_iterations=10,
        )
        if last.get("ok") and "return a + b" in (project / "calc.py").read_text(encoding="utf-8"):
            return {
                "model": last.get("model"),
                "writes": last.get("writes"),
                "changed_files": last.get("changed_files"),
                "review": last.get("review"),
            }
    raise RuntimeError("bounded developer failed live acceptance: " + json.dumps(last, default=str)[-2000:])


def _swarm(app):
    # A reasoning-only task proves planner/specialist/reviewer plumbing without
    # causing a second file mutation or external side effect.
    result = app.swarm.run(
        "Create a concise implementation checklist for adding a health endpoint to a small Python service.",
        project=".jubi-certification",
        max_sources=1,
    )
    if result.get("status") not in {"completed", "partial"} or not str(result.get("final", "")).strip():
        raise RuntimeError("swarm produced no usable final result")
    return {
        "status": result.get("status"),
        "steps": len(result.get("results", [])),
        "tool_backed_steps": result.get("tool_backed_steps"),
    }


def _task(app):
    result = app.execution.run(
        "Give one short sentence explaining what a health check does.",
        source="vps-live-acceptance",
    )
    if result.get("status") not in {"completed", "partial"}:
        raise RuntimeError("persistent task did not complete: " + json.dumps(result, default=str)[-1600:])
    loaded = app.execution.get_task(result["task_id"])
    if loaded.get("status") != result.get("status"):
        raise RuntimeError("persisted task status does not match")
    return {"task_id": result["task_id"], "status": result["status"], "steps": len(result.get("steps", []))}


def _automation(app):
    row = app.scheduler.add(
        "VPS certification disabled automation",
        "This automation must remain disabled during certification.",
        3600,
        enabled=False,
        metadata={"certification": True},
    )
    items = {x["id"]: x for x in app.scheduler.list()}
    if row["id"] not in items or items[row["id"]]["enabled"]:
        raise RuntimeError("disabled automation did not persist correctly")
    app.scheduler.set_enabled(row["id"], False)
    return {"id": row["id"], "enabled": False}


def _operator(app):
    processes = app.windows.execute_typed("system.processes.list", {}, {})
    services = app.windows.execute_typed("system.services.list", {}, {})
    if not processes.get("ok"):
        raise RuntimeError("process inventory failed: " + str(processes.get("stderr") or processes.get("error")))
    if not services.get("ok"):
        raise RuntimeError("service inventory failed: " + str(services.get("stderr") or services.get("error")))
    return {"process_inventory": True, "service_inventory": True, "platform": app.windows.platform_capabilities().get("platform")}


def _approval(app):
    rel = "workspace/.jubi-certification/approval-delete.txt"
    app.windows.execute_typed("workspace.file.write", {"path": rel, "content": "approval-test"}, {})
    request = {"action_id": "workspace.file.delete", "parameters": {"path": rel}}
    first = app.privileged.handle(request, source="vps-live-acceptance")
    if first.get("status") != "approval_required" or not first.get("request_id"):
        raise RuntimeError("delete did not require approval: " + str(first))
    proof = app.privileged.create_approval_proof(first["request_id"], request["action_id"], request["parameters"])
    approved = app.privileged.handle(
        {"request_id": first["request_id"], **request},
        source="vps-live-acceptance",
        approval_proof=proof,
    )
    if not approved.get("ok"):
        raise RuntimeError("approved delete failed: " + str(approved))
    return {"approval_required": True, "approved": True, "request_id": first["request_id"]}


def _fable(app):
    result = app.fable.status()
    if not result.get("integrated"):
        raise RuntimeError("Fable core is not integrated")
    trace = result.get("trace") or {}
    if not trace.get("verified_receipt_chain"):
        raise RuntimeError("Fable receipt-chain status is not verified")
    return {"integrated": True, "learned_capabilities": result.get("learned_capabilities"), "lab_runtime_ready": (result.get("source") or {}).get("runtime_ready")}


def _receipts(app):
    result = app.receipts.verify_chain()
    ok = result is True or (isinstance(result, dict) and result.get("ok") is True)
    if not ok:
        raise RuntimeError("receipt chain verification failed: " + str(result))
    return result


if __name__ == "__main__":
    raise SystemExit(main())
