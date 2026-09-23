"""Consolidated non-destructive readiness report for the Linux VPS profile."""
from __future__ import annotations

import os
import platform


class VPSReadiness:
    def __init__(self, app):
        self.app = app

    def run(self) -> dict:
        checks = []

        def add(name, ok, detail="", required=True):
            checks.append({
                "name": name,
                "ok": bool(ok),
                "required": bool(required),
                "detail": detail,
            })

        linux = platform.system() == "Linux"
        add("Linux host", linux, platform.platform())

        model_status = self.app.models.list_models()
        add("Local Ollama", model_status.get("online", False), model_status.get("error", "online"))

        full_models = os.environ.get("JUBI_REQUIRE_FULL_MODELS", "0").lower() in {"1", "true", "yes", "on"}
        for role in ("general", "coding", "vision", "embedding"):
            model = self.app.models.choose(role)
            add("Model role " + role, bool(model), model or "missing", required=full_models or role in {"general", "coding"})

        hermes = self.app.hermes.status()
        require_hermes = os.environ.get("JUBI_REQUIRE_HERMES", "0").lower() in {"1", "true", "yes", "on"}
        add("Hermes runtime", hermes.get("ready", False), str(hermes), required=require_hermes)

        browser = self.app.browser.status()
        require_browser = os.environ.get("JUBI_REQUIRE_BROWSER", "0").lower() in {"1", "true", "yes", "on"}
        add("Read-only JS browser", browser.get("ready", False), str(browser), required=require_browser)

        voice = self.app.voice.status()
        require_voice = os.environ.get("JUBI_REQUIRE_VOICE", "0").lower() in {"1", "true", "yes", "on"}
        add("Offline clip STT/TTS", voice.get("ready", False), str(voice), required=require_voice)

        add("Bounded VPS developer", self.app.developer.workspace.is_dir(), str(self.app.developer.workspace))
        swarm = self.app.swarm.status()
        add("Executable VPS swarm", swarm.get("ready", False), str(swarm))
        add("Research runtime", self.app.research is not None, "public HTTP/HTTPS research")
        add("Vision runtime", self.app.vision is not None, "local Ollama vision")
        add("Memory/RAG", self.app.knowledge is not None and self.app.memory is not None, "SQLite + local embeddings")
        add("Persistent tasks", self.app.execution is not None, "SQLite execution state")
        add("Automations", self.app.scheduler is not None, "persistent scheduler")
        add("Approval/receipts", self.app.privileged is not None and self.app.receipts is not None, "typed broker + receipts")

        required = [row for row in checks if row["required"]]
        return {
            "profile": "linux_vps",
            "ready": bool(required) and all(row["ok"] for row in required),
            "full_profile_required": {
                "hermes": require_hermes,
                "browser": require_browser,
                "voice": require_voice,
                "all_model_roles": full_models,
            },
            "checks": checks,
            "failed_required": [row["name"] for row in required if not row["ok"]],
            "windows_only_excluded": [
                "Ring0 kernel bridge",
                "Windows desktop application launch",
                "native SARA desktop mouse/keyboard control",
                "direct VPS microphone capture (audio must come from an authenticated client)",
                "Windows EXE installer runtime",
            ],
        }
