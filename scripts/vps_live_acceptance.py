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
        rows.append(check("local embedding", lambda: _embed(app)))
        rows.append(check("local vision", lambda: _vision(app)))
        rows.append(check("public research fetch", lambda: _research(app)))
        rows.append(check("headless Chromium", lambda: _browser(app)))
        rows.append(check("bounded developer edit-review", lambda: _developer(app)))
        rows.append(check("swarm orchestration", lambda: _swarm(app)))
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


def _receipts(app):
    result = app.receipts.verify_chain()
    ok = result is True or (isinstance(result, dict) and result.get("ok") is True)
    if not ok:
        raise RuntimeError("receipt chain verification failed: " + str(result))
    return result


if __name__ == "__main__":
    raise SystemExit(main())
