# SARUS v1.0.0-rc1 — Delivery Report

## Delivered architecture

SARUS now has a local executable orchestration runtime instead of the earlier dry-run-only foundation.

### Unified source layer
- Preserves all 10 supplied repositories under one `sources/` tree.
- Clean release source count: exactly **17,356 original source files**.
- Keeps the exhaustive original named-feature catalog in `docs/ALL_SOURCE_FEATURES.md`.
- Keeps the full file inventory and original scan evidence under `evidence/`.
- Maintains an automatic capability registry and coverage export for agents, skills, tools and commands.

### Runtime pipeline
- Central intent-based orchestrator.
- Ten source adapters.
- Original-capability prompt/runtime selection from the actual source registry.
- Local Ollama model routing.
- Research pipeline: Hermes → Awesome LLM Apps → Agency Agents → SARA live evidence → ECC verification.
- Development pipeline: Hermes → Superpowers → ECC implementation review → SARA local execution → ECC code review.
- Knowledge pipeline: Second Brain → SARUS local persistent memory.
- Defensive security path: CAI analysis-only isolation.
- Evaluation path: Autoresearch bounded/isolated experiment design.
- Trust path: Fable-inspired SARUS native hash-chained execution receipts.

### Local models configured
- General: `qwen2.5:7b` (fallbacks GLM4, Mistral, Llama3).
- Coding: `qwen2.5-coder:7b`.
- Vision: `qwen2.5vl:3b`.
- Embeddings: `nomic-embed-text-v2-moe:latest`.
- Fast tasks: `qwen2:1.5b` / Gemma 2B.
- Cloud-tagged models are excluded from automatic local routing.

### Windows/SARA bridge
- SARUS can automatically discover the SARA-generated local API token from the supplied SARA installation.
- When SARA v7 API is online, SARUS routes browser/Windows/local execution through `/v7/command`.
- If SARA is not yet online, source capabilities can still run through the local Ollama fallback, but Windows action certification remains false until SARA native acceptance passes.
- Direct unrestricted Ring-0 access is intentionally not enabled. Privileged actions remain brokered/approved.

### Dashboard/API
- Local dashboard on `127.0.0.1:8877`.
- Command Center, task plan/run, model view, agent/capability browser, direct capability execution, business templates, development workflow, automation scheduler, Windows computer actions, memory, security approvals, trusted receipts and Doctor/health view.
- API endpoints for status, session, doctor, events, models, capabilities, tasks, approvals, receipts, memory, automations, plan, task, chat, direct capability execution and Windows actions.
- POST API protected by a per-process session token.
- Cross-origin POST requests blocked.
- Server binds to localhost by default.

### Persistence and trust
- SQLite local event/task/memory/automation/approval store.
- SQLite connections explicitly closed (Python 3.13 ResourceWarning issue fixed).
- Trusted receipt store now verifies both chain links and payload hashes.
- Workspace path guard prevents direct file broker access outside the SARUS workspace.

### Automation
- Persistent interval automations with a minimum 60-second interval.
- Background scheduler.
- Task/event history available from dashboard.

## Repairs completed

- Repaired the previously identified Second Brain PPTX quote-slide unterminated string error.
- Fixed blank capability-search API response type that could break dashboard rendering.
- Fixed dashboard event-refresh naming collision risk.
- Added the missing Windows `service_control` broker implementation.
- Added SARA token auto-discovery from its generated `.env.local` / `.env`.
- Added localhost POST session-token and cross-origin protections.
- Hardened receipt creation for concurrent use and payload tamper detection.

## Verification completed in build environment

- **SARUS integration/API/security suite: 17/17 PASS.**
- **SARA supplied v7.1.1 contract/smoke suite: 33/33 PASS.**
- Original archive scan evidence: shell syntax failures **0**, JavaScript syntax failures **0**.
- Current SARUS dashboard JavaScript: syntax PASS.
- Second Brain repaired Python file: AST parse PASS.
- Clean payload ZIP integrity: PASS.
- Clean payload source count: **17,356**.
- Release ZIP integrity: PASS.

## Windows installer behavior

The release contains `SARUS-Setup.exe`. It is a native x64 Windows PE launcher and must stay next to its release companion files. The release is therefore distributed as one ZIP; after extracting it, double-click only `SARUS-Setup.exe`.

The installer:
1. Requests Administrator permission.
2. Verifies the payload SHA256 checksum.
3. Extracts the clean SARUS payload.
4. Preserves the existing SARUS memory/audit DB on upgrades.
5. Runs the supplied SARA verified Windows installer.
6. Lets SARA install/verify Python 3.11, Node, Git, Ollama, required local models, browser/voice/vision dependencies and its runtime.
7. Creates a private SARUS Python runtime.
8. Attempts an isolated native Hermes CLI install.
9. Attempts ECC production Node dependency installation.
10. Generates the installed `SARUS.exe` launcher against the exact private Python runtime.
11. Runs `python -m sarus.acceptance --full`.
12. Requires real Ollama generation, required local models, Windows process broker and SARA v7 API bridge to pass.
13. Creates Desktop/Start Menu shortcuts and a logon startup task.
14. Starts SARUS.

If a required target acceptance check fails, installation fails closed and writes logs instead of falsely reporting success.

## Important certification boundary

This build environment is Linux and cannot physically certify the user's Windows camera, microphone, Windows Hello hardware, real mouse/keyboard desktop control, GPU throughput, browser accounts, LAN peers or external account credentials. The supplied installer therefore performs required target-side acceptance for the core Windows/Ollama/SARA bridge. Hardware/account-specific features remain dependent on the target device and credentials.

CAI active/offensive tooling is not enabled in the normal desktop runtime; only defensive analysis is available through the normal SARUS path. Fable bare-metal/QEMU kernel experiments and Autoresearch GPU training are optional isolated lab capabilities rather than normal production desktop actions.
