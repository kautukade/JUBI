"""Live acceptance suite for Jubi's headless Linux VPS profile.

This suite is intentionally separate from CI: it exercises the actual installed
Ollama models and network on the target VPS. It never provisions a model,
changes security settings, or exposes the HTTP server.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import shutil
import struct
import time
import uuid
import zlib
from pathlib import Path

from .core.app import Jubi


def _solid_png(width: int = 32, height: int = 32) -> bytes:
    width = max(1, min(int(width), 128))
    height = max(1, min(int(height), 128))
    raw = b"".join(b"\x00" + (b"\xff\x40\x20" * width) for _ in range(height))

    def chunk(kind: bytes, payload: bytes) -> bytes:
        body = kind + payload
        return (
            struct.pack(">I", len(payload))
            + body
            + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
        )

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def run_vps_acceptance(
    root: Path,
    *,
    full: bool = True,
    autonomy: bool = True,
    save_evidence: bool = True,
    app: Jubi | None = None,
) -> dict:
    root = Path(root).resolve()
    owned_app = app is None
    app = app or Jubi(root)
    checks: list[dict] = []
    acceptance_project: Path | None = None

    def check(name, fn, *, required=True):
        started = time.perf_counter()
        try:
            detail = fn()
            if isinstance(detail, dict) and "ok" in detail:
                ok = bool(detail["ok"])
            else:
                ok = detail is not False and detail is not None
            checks.append({
                "name": name,
                "ok": bool(ok),
                "required": bool(required),
                "latency_ms": round((time.perf_counter() - started) * 1000, 2),
                "detail": detail,
            })
            return detail
        except Exception as exc:
            checks.append({
                "name": name,
                "ok": False,
                "required": bool(required),
                "latency_ms": round((time.perf_counter() - started) * 1000, 2),
                "detail": {"error": str(exc)[:4000], "type": type(exc).__name__},
            })
            return None

    try:
        readiness = check(
            "full_readiness",
            lambda: {
                **app.vps_readiness.run(full=full),
                "ok": app.vps_readiness.run(full=full)["ready"],
            },
        )

        def general_inference():
            model = app.models.choose("general")
            if not model:
                raise RuntimeError("no local general model")
            response = app.models.generate_text(
                "Reply with a short sentence containing the token JUBI_VPS_OK.",
                "general",
                model=model,
                timeout=180,
            )
            return {"ok": bool(response.strip()), "model": model, "response": response[:500]}
        check("general_inference", general_inference)

        def embedding_roundtrip():
            model = app.models.choose("embedding")
            if not model:
                raise RuntimeError("no local embedding model")
            vector = app.models.embed("Jubi VPS semantic acceptance token", model=model)
            if not isinstance(vector, list) or not vector:
                raise RuntimeError("embedding response was empty")
            return {
                "ok": all(isinstance(x, (int, float)) for x in vector[:32]),
                "model": model,
                "dimensions": len(vector),
            }
        check("embedding_inference", embedding_roundtrip, required=full)

        def knowledge_roundtrip():
            namespace = "vps-acceptance-" + uuid.uuid4().hex[:10]
            token = "JUBI-KNOWLEDGE-" + uuid.uuid4().hex[:10]
            doc = app.knowledge.ingest(
                "The acceptance token is " + token + ".",
                title="VPS Acceptance",
                namespace=namespace,
                source="vps-acceptance",
            )
            matches = app.knowledge.search(token, namespace=namespace, limit=3)
            answer = app.knowledge.answer(
                "What is the acceptance token?",
                namespace=namespace,
                limit=3,
                provider="ollama",
            )
            return {
                "ok": bool(matches and token in matches[0].get("text", "") and answer.get("answer")),
                "document_id": doc.get("id"),
                "match_count": len(matches),
                "answer": str(answer.get("answer", ""))[:1000],
            }
        check("knowledge_rag_roundtrip", knowledge_roundtrip, required=full)

        def vision_inference():
            model = app.models.choose("vision")
            if not model:
                raise RuntimeError("no local vision model")
            image = base64.b64encode(_solid_png()).decode("ascii")
            out = app.vision.analyze(
                image,
                "Describe the dominant color of this simple test image in one short sentence.",
                model=model,
                timeout=240,
            )
            response = str(out.get("response") or out.get("output") or "")
            return {"ok": bool(response.strip()), "model": model, "response": response[:500]}
        check("vision_inference", vision_inference, required=full)

        def hermes_analysis():
            status = app.hermes.status()
            if not status.get("ready"):
                raise RuntimeError("Hermes runtime is not ready: " + json.dumps(status))
            model = app.models.choose("coding") or app.models.choose("general")
            if not model:
                raise RuntimeError("no local model for Hermes")
            out = app.hermes.analyze(
                "Analyze this acceptance statement and reply briefly: Jubi is running on a Linux VPS.",
                model=model,
                timeout=240,
            )
            delegation = out.get("delegation") or {}
            return {
                "ok": bool(out.get("inference_receipts") and delegation),
                "model": model,
                "parent_session": out.get("parent_session"),
                "delegation": delegation,
            }
        check("hermes_analysis", hermes_analysis, required=full)

        def council_roundtrip():
            out = app.council.run(
                "Give one practical reason to keep an AI service bound to localhost.",
                task_type="general",
                max_members=2,
                judge_provider="ollama",
            )
            return {
                "ok": bool(out.get("members") and out.get("final")),
                "member_count": len(out.get("members") or []),
                "final": str(out.get("final") or "")[:1000],
            }
        check("ai_council", council_roundtrip, required=full)

        def supervisor_roundtrip():
            out = app.supervisor.run(
                "Create a two-step reasoning plan to verify a local web service health endpoint.",
                task_type="planning",
                provider="ollama",
            )
            return {
                "ok": out.get("status") == "completed" and bool(out.get("final")),
                "status": out.get("status"),
                "steps": len(out.get("results") or []),
                "final": str(out.get("final") or "")[:1000],
            }
        check("multi_agent_supervisor", supervisor_roundtrip, required=full)

        def public_web_fetch():
            page = app.research.fetch("https://example.com", timeout=20)
            return {
                "ok": page.get("url", "").startswith("https://") and page.get("chars", 0) > 50,
                "url": page.get("url"),
                "title": page.get("title"),
                "chars": page.get("chars"),
            }
        check("public_web_fetch_ssrf_boundary", public_web_fetch)

        def memory_roundtrip():
            token = "JUBI-MEMORY-" + uuid.uuid4().hex
            saved = app.memory.add(token, "VPS acceptance", "vps-acceptance")
            found = app.memory.search(token, "vps-acceptance", 5)
            return {"ok": any(x.get("id") == saved.get("id") for x in found), "id": saved.get("id")}
        check("memory_roundtrip", memory_roundtrip)

        def automation_roundtrip():
            name = "VPS acceptance " + uuid.uuid4().hex[:8]
            created = app.scheduler.add(
                name,
                "VPS acceptance disabled task",
                3600,
                enabled=False,
                metadata={"acceptance": True},
            )
            listed = app.scheduler.list()
            return {
                "ok": any(x.get("id") == created.get("id") and not x.get("enabled") for x in listed),
                "id": created.get("id"),
                "scheduler_alive": bool(app.scheduler.thread and app.scheduler.thread.is_alive()),
            }
        check("automation_scheduler", automation_roundtrip)

        def receipt_roundtrip():
            receipt = app.receipts.create(
                "vps-acceptance",
                "receipt",
                "vps-acceptance",
                "verified",
                {"purpose": "VPS live acceptance"},
            )
            chain = app.receipts.verify_chain()
            return {"ok": bool(receipt.get("signature") and chain.get("ok")), "chain": chain}
        check("signed_receipt_chain", receipt_roundtrip)

        def operator_roundtrip():
            processes = app.windows.execute_typed("system.processes.list", {}, {})
            services = app.windows.execute_typed("system.services.list", {}, {})
            return {
                "ok": bool(processes.get("ok") and services.get("ok")),
                "process_sample": str(processes.get("stdout") or "")[:500],
                "service_sample": str(services.get("stdout") or "")[:500],
            }
        check("linux_typed_operator", operator_roundtrip)

        def network_roundtrip():
            status = app.network.status()
            result = app.network.passive_discover()
            return {
                "ok": bool(status.get("passive_neighbor_available") and result.get("ok")),
                "status": status,
                "observed": len(result.get("devices") or []),
            }
        check("authorized_lan_passive", network_roundtrip)

        def source_catalogs():
            states = app.adapters.connect()
            return {
                "ok": bool(states and all(x.connected for x in states)),
                "states": [x.__dict__ for x in states],
            }
        check("source_catalogs", source_catalogs, required=full)

        if autonomy:
            def autonomous_development():
                nonlocal acceptance_project
                acceptance_name = "vps-acceptance-" + uuid.uuid4().hex[:10]
                acceptance_project = root / "workspace" / acceptance_name
                acceptance_project.mkdir(parents=True, exist_ok=False)
                (acceptance_project / "calc.py").write_text(
                    "def add(a, b):\n    return a - b\n", encoding="utf-8"
                )
                (acceptance_project / "test_calc.py").write_text(
                    "import unittest\n"
                    "from calc import add\n\n"
                    "class Acceptance(unittest.TestCase):\n"
                    "    def test_add(self):\n"
                    "        self.assertEqual(add(2, 3), 5)\n\n"
                    "if __name__ == '__main__': unittest.main()\n",
                    encoding="utf-8",
                )
                result = app.development.run(
                    "Fix calc.py so add(a, b) correctly returns the sum. Inspect files, run the failing test, make the minimal edit, rerun tests, inspect the diff, and verify.",
                    project=acceptance_name,
                    max_iterations=12,
                )
                return {
                    "ok": bool(result.get("ok") and result.get("verification", {}).get("ok")),
                    "model": result.get("model"),
                    "iterations": result.get("iterations"),
                    "changed_files": result.get("changed_files"),
                    "verification": result.get("verification"),
                    "reviewer": result.get("reviewer"),
                }
            check("autonomous_code_edit_test_verify", autonomous_development, required=full)

        fable = app.fable.status()
        check(
            "fable_integration",
            lambda: {
                "ok": bool(fable.get("integrated")),
                "integrated": fable.get("integrated"),
                "source": fable.get("source"),
            },
            required=False,
        )

        required = [x for x in checks if x["required"]]
        result = {
            "name": "Jubi VPS Live Acceptance",
            "version": app.VERSION,
            "profile": "linux_vps",
            "full": bool(full),
            "autonomy": bool(autonomy),
            "ok": all(x["ok"] for x in required),
            "passed": sum(1 for x in required if x["ok"]),
            "required": len(required),
            "timestamp": time.time(),
            "checks": checks,
            "static_readiness": readiness,
        }

        if save_evidence:
            evidence_dir = root / "data" / "evidence"
            evidence_dir.mkdir(parents=True, exist_ok=True)
            stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
            path = evidence_dir / ("vps-live-acceptance-" + stamp + ".json")
            path.write_text(json.dumps(result, ensure_ascii=False, indent=2, default=str) + "\n", encoding="utf-8")
            result["evidence_path"] = str(path)
        return result
    finally:
        if acceptance_project and acceptance_project.is_dir() and acceptance_project.name.startswith("vps-acceptance-"):
            shutil.rmtree(acceptance_project, ignore_errors=True)
        if owned_app:
            app.shutdown()


def main():
    parser = argparse.ArgumentParser(description="Run live Jubi Linux VPS acceptance")
    parser.add_argument("--core", action="store_true", help="Do not require model-heavy optional/full features")
    parser.add_argument("--no-autonomy", action="store_true", help="Skip the live autonomous coding challenge")
    parser.add_argument("--no-save", action="store_true", help="Do not save evidence under data/evidence")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    result = run_vps_acceptance(
        root,
        full=not args.core,
        autonomy=not args.no_autonomy,
        save_evidence=not args.no_save,
    )
    print(json.dumps(result, ensure_ascii=False, indent=2, default=str))
    raise SystemExit(0 if result["ok"] else 2)


if __name__ == "__main__":
    main()
