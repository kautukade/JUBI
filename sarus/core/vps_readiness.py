"""Machine-readable readiness for the headless Linux VPS deployment."""
from __future__ import annotations

import os
import platform
from pathlib import Path


class VPSReadiness:
    def __init__(self, app):
        self.app = app

    def run(self, full: bool = False) -> dict:
        checks = []

        def add(name, ok, detail="", required=True):
            checks.append({
                "name": str(name),
                "ok": bool(ok),
                "required": bool(required),
                "detail": detail,
            })

        prod = self.app.doctor._production()
        profile = prod.get("deployment_profiles", {}).get("linux_vps", {})
        is_linux = platform.system() == "Linux"
        add("linux_vps_profile", is_linux and profile.get("supported") is True,
            {"platform": platform.platform(), "profile": profile})

        provider_mode = self.app.providers.mode()
        add("local_only_provider", provider_mode == "local_only", {"mode": provider_mode})

        models = self.app.models.list_models()
        add("ollama", models.get("online") is True,
            {"endpoint": self.app.models.base, "error": models.get("error")})

        items = models.get("items", [])
        kinds = {}
        for item in items:
            kinds.setdefault(item.get("kind"), []).append(item.get("name"))

        general = self.app.models.choose("general") if models.get("online") else None
        coding = self.app.models.choose("coding") if models.get("online") else None
        vision = self.app.models.choose("vision") if models.get("online") else None
        embedding = self.app.models.choose("embedding") if models.get("online") else None

        add("general_model", bool(general), {"selected": general})
        add("coding_model", bool(coding), {"selected": coding})
        add("vision_model", bool(vision), {"selected": vision, "available": kinds.get("vision", [])},
            required=full)
        add("embedding_model", bool(embedding),
            {"selected": embedding, "available": kinds.get("embedding", [])}, required=full)

        development = self.app.development.status()
        development_ok = bool(development.get("available") and coding)
        development_detail = dict(development)
        if full and development_ok:
            try:
                selected, compact, routing = self.app.development._select_model(
                    "VPS readiness: inspect, edit, test and verify a local coding project"
                )
                development_detail.update({
                    "admitted_model": selected,
                    "compact_profile": compact,
                    "routing": routing,
                })
            except Exception as exc:
                development_ok = False
                development_detail["error"] = str(exc)
        add("autonomous_development", development_ok, development_detail, required=full)

        hermes = self.app.hermes.status()
        add("hermes_runtime", hermes.get("ready") is True, hermes, required=full)

        scheduler = self.app.scheduler
        scheduler_ok = bool(scheduler.thread and scheduler.thread.is_alive())
        add("automation_scheduler", scheduler_ok, {"thread_alive": scheduler_ok})

        receipt = self.app.receipts.verify_chain()
        add("receipt_chain", receipt.get("ok") is True, receipt)

        knowledge = self.app.knowledge.status()
        add("knowledge_rag", bool(knowledge.get("embedding_model")),
            knowledge, required=full)

        vision_status = self.app.vision.status()
        add("vision", bool(vision_status.get("selected_model")), vision_status, required=full)

        add("web_research", hasattr(self.app.research, "research"),
            {"network_scope": "public-http-https-only", "ssrf_guard": True})
        add("typed_operator", self.app.windows.available(),
            {"headless_linux": os.name != "nt", "arbitrary_shell": False})

        network = self.app.network.status()
        add("authorized_network_manager",
            network.get("active_scan") is False and network.get("credential_bruteforce") is False,
            network)

        source_states = self.app.adapters.connect()
        connected = [s.name for s in source_states if s.connected]
        missing = [s.name for s in source_states if not s.connected]
        add("source_catalogs", not missing, {"connected": connected, "missing": missing}, required=full)

        fable = self.app.fable.status()
        add("fable_integration", bool(fable.get("integrated")), fable, required=False)

        data_dir = self.app.root / "data"
        add("persistent_state", data_dir.is_dir() and os.access(data_dir, os.W_OK),
            {"path": str(data_dir), "database": str(self.app.db_path)})

        add("remote_access_boundary", os.environ.get("JUBI_HOST", "127.0.0.1") in {
            "127.0.0.1", "localhost", "::1"
        }, {"bind": os.environ.get("JUBI_HOST", "127.0.0.1")})

        required_checks = [c for c in checks if c["required"]]
        return {
            "profile": "linux_vps",
            "full": bool(full),
            "ready": all(c["ok"] for c in required_checks),
            "passed": sum(1 for c in required_checks if c["ok"]),
            "required": len(required_checks),
            "checks": checks,
            "not_applicable": {
                "windows_ring0": "Windows-only",
                "windows_sara_desktop_control": "Windows desktop/session feature",
                "server_microphone_camera": "Headless VPS has no local user audio/video device requirement",
                "browser_speech": "Runs in the user's browser, not in the VPS service",
            },
        }
