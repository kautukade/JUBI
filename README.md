# Jubi

> **Jubi — Local AI Agent & Windows Automation Platform**  
> Developed for **ITCYBER TECHNOLOGIES PVT LTD**  
> Primary platform: **Windows 10/11 x64**  
> Current migration release: **Jubi v0.1.0**  
> Foundation: **SARUS v1.3.1**

Jubi is the next evolution of the SARUS local AI research and Windows automation platform. The Phase 0 migration deliberately keeps the proven SARUS internals that are expensive and risky to rename blindly — source adapters, installer helper filenames, the controlled Ring0 driver ABI, and selected environment-variable compatibility — while moving the user-facing product, runtime identity, dashboard, installer artifact, tests and new entry points to **Jubi**.

The goal of this release is **stability first**: persistence, task recovery, approval correctness, model discovery, lifecycle cleanup and regression coverage. It does **not** yet add the future NVIDIA/OpenRouter/Hugging Face provider layer, LAN management, semantic vector memory, advanced planner, or AI Council.

---

## 1. What changed in Jubi v0.1.0

The migration fixes several foundation issues before advanced features are added:

- SQLite writes now use explicit transactions instead of relying on connection close behavior.
- SQLite connections use WAL mode, a busy timeout and short per-operation connections to reduce locking under HTTP/background threads.
- Memory, events, tasks, approvals and automations are persistence-safe.
- Task plans are serialized to SQLite so an approval-required task can survive a Jubi restart.
- Approval is bound to the exact persisted task step; approving one step cannot silently approve later steps.
- Rejected approvals do not execute the pending action.
- Ollama routing no longer returns a configured model that is not actually installed.
- Ollama model discovery now exposes useful model metadata and basic role classification.
- Jubi Doctor reads the required model list from `config/production.json` instead of duplicating it in Python.
- The HTTP API uses Jubi branding, safe default error responses and localhost-only binding.
- The dashboard is Jubi-branded and includes real Approve / Reject controls for pending pipeline approvals.
- A Jubi user-facing Python entry package is available (`python -m jubi.server`, `python -m jubi.acceptance`).
- The Windows installer artifact is now `Jubi-Setup.exe` and the installed launcher is exposed as `Jubi.exe`.
- New Jubi Phase 0 tests cover persistence, model fallback and approval/restart behavior.

See `docs/JUBI_PHASE0_FIX_REPORT.md` for the detailed repair record.

---

## 2. Architecture

```text
User / Dashboard
        |
        v
Local HTTP API (127.0.0.1:8877)
        |
        +--> Ollama model router
        |
        +--> Persistent task execution
        |       |
        |       +--> Orchestrator
        |       +--> Source adapters
        |       +--> Approval state
        |       +--> Signed receipts
        |
        +--> Local memory + events + automation state (SQLite)
        |
        +--> Policy engine
                |
                +--> Typed Privileged Broker
                        |
                        +--> allowlisted Windows actions
                        +--> controlled legacy Ring0 status bridge
```

The central security boundary remains:

```text
AI reasoning != unrestricted privileged execution
```

Models can propose work. High-impact operations must go through typed policy/broker controls rather than a model-facing arbitrary shell or raw kernel interface.

---

## 3. Local model layer

Model roles are configured in:

```text
config/models.json
```

Production-required local baseline is configured in:

```text
config/production.json
```

Current required baseline:

```text
qwen2.5:7b
qwen2.5-coder:7b
qwen2.5vl:3b
nomic-embed-text-v2-moe:latest
```

Jubi dynamically queries the Ollama API at:

```text
http://127.0.0.1:11434
```

A configured-but-missing model is no longer returned as if it were installed. If there is no compatible installed model, Jubi returns a clear error.

Cloud-tagged Ollama models are not part of the required local production baseline.

---

## 4. Existing source families preserved from SARUS

Jubi Phase 0 preserves the existing ten pinned source families:

1. SARA
2. NousResearch / hermes-agent
3. ECC
4. agency-agents
5. awesome-llm-apps
6. second-brain-skills
7. superpowers
8. fable-os
9. CAI
10. autoresearch

Their paths are configured in:

```text
config/sources.json
```

Public upstream pins are configured in:

```text
config/online_sources.json
```

Important distinction: a connected source repository is not automatically the same thing as a fully running native upstream runtime. Several integrations act as indexed capability/prompt sources routed through local Ollama; SARA/Hermes/ECC have separate native-runtime detection where available.

---

## 5. Persistent task execution and approvals

Jubi stores the task plan and execution cursor in SQLite.

Typical flow:

```text
Task starts
  -> plan persisted
  -> steps execute
  -> high-risk step requires approval
  -> task state = waiting_approval
  -> Jubi may be restarted
  -> pending approval is still present
  -> approve exact step
  -> execution resumes from that step
  -> remaining steps continue
```

Task states include:

```text
queued
planning
running
waiting_approval
completed
partial
failed
rejected
cancelled
```

Approvals are not a generic `approved=true` flag for privileged execution. The Privileged Broker retains its own request-bound proof mechanism for typed Windows actions.

---

## 6. Local data and SQLite reliability

Jubi Phase 0 intentionally keeps the legacy physical database filename:

```text
data/sarus.db
```

This is a compatibility decision for the first migration release, not the product identity. The database is now owned by the Jubi runtime and contains persistent state for:

- memories
- events
- tasks
- task execution state
- approvals
- automations
- receipts
- Fable traces
- learned capabilities
- Fable agenda

The database helper configures:

```text
WAL mode
busy_timeout
foreign keys
explicit commit / rollback
```

A later target-tested migration may rename the physical database to `jubi.db`; Phase 0 avoids moving user state merely for cosmetic reasons.

---

## 7. Windows privileged broker

The existing zero-trust typed broker is retained.

Core behavior includes:

- default deny
- action IDs instead of arbitrary shell strings
- parameter schemas
- allowlisted resources
- path scoping
- replay protection
- short-lived request-bound approval proofs
- sensitive-value redaction
- signed execution receipts

Current examples include read-only process/service listing, scoped workspace file operations, URL opening and allowlisted service/process actions.

Jubi Phase 0 does **not** expose arbitrary PowerShell/cmd execution through the privileged broker.

---

## 8. Controlled Ring0 compatibility bridge

The legacy driver project remains under:

```text
driver/SarusRing0/
```

This name is intentionally retained during Phase 0 because it is a driver ABI/signing compatibility surface, not ordinary UI branding.

Current fixed capabilities remain narrow:

```text
ring0.ping
ring0.status
```

There is no model-facing arbitrary kernel-memory API, raw IOCTL API or general kernel command executor.

A driver is activated by the installer only when Windows reports the bundled driver signature as valid. The installer does not disable Secure Boot, Code Integrity, HVCI, Defender or driver-signature enforcement.

---

## 9. Dashboard

Default dashboard:

```text
http://127.0.0.1:8877
```

Views currently include:

- Command Center
- AI & Models
- Agent Network
- Development
- Automation
- Computer
- Fable Lab
- Knowledge
- Security
- System Health

Phase 0 remains localhost-only by design.

---

## 10. Start Jubi from source

Requirements:

- Windows 10/11 recommended
- Python 3.11+
- Ollama

Clone and enter the repository, then create/activate a Python environment appropriate for your setup.

Run the user-facing entry point:

```powershell
python -m jubi.server
```

Open:

```text
http://127.0.0.1:8877
```

Check Ollama separately:

```powershell
ollama list
```

---

## 11. Acceptance

User-facing acceptance command:

```powershell
python -m jubi.acceptance
```

Full target-machine validation including a real Ollama generation attempt:

```powershell
python -m jubi.acceptance --full
```

Compatibility command remains available during Phase 0:

```powershell
python -m sarus.acceptance --full
```

Do not treat GitHub CI as a substitute for physical Windows validation of camera, microphone, GPU, SARA native runtime, signed driver activation or installer behavior.

---

## 12. Tests

Core Jubi Phase 0 regression test:

```powershell
python tests/jubi_phase0_test.py
```

Production/static gate:

```powershell
python tests/production_readiness_test.py
```

Foundation integration regression:

```powershell
python tests/integration_test.py
```

Broker security:

```powershell
python tests/broker_security_test.py
python tests/ring0_bridge_test.py
```

Fable integration:

```powershell
python tests/fable_integration_test.py
```

Compile check:

```powershell
python -m compileall -q jubi sarus tests scripts
```

---

## 13. Windows installer

The Inno Setup definition still has the legacy repository filename:

```text
installer/SARUS-Setup.iss
```

but now builds the user-facing artifact:

```text
Jubi-Setup.exe
```

Target installation path:

```text
C:\Program Files\Jubi
```

Target launcher:

```text
Jubi.exe
```

During Phase 0 the verified legacy launcher payload is copied byte-for-byte to `Jubi.exe` after checksum verification. A separately rebuilt native launcher can replace this compatibility step later after Windows validation.

Existing SARUS installations are not deliberately overwritten: Jubi uses a distinct installer AppId and install directory.

---

## 14. Legacy compatibility identifiers

The following SARUS-era names may intentionally remain internally in Phase 0:

- `sarus/` Python implementation package
- `data/sarus.db`
- `SARUS_*` environment-variable fallbacks
- `installer/INSTALL-SARUS.ps1`
- `installer/CERTIFY-SARUS.ps1`
- `installer/UNINSTALL-SARUS.ps1`
- `.sarus-venv`
- `driver/SarusRing0/`
- protected broker key locations used by the legacy installer

They are compatibility surfaces, not the current product name. New UI/status/installer artifact identity is Jubi.

---

## 15. What is deliberately NOT in Phase 0

The following are planned later and should not be represented as already implemented:

- NVIDIA provider
- OpenRouter provider
- Hugging Face provider
- provider quota/free-model manager
- advanced task-classifying model router
- semantic/vector RAG memory
- experience-based self-learning router
- advanced LLM planner / task DAG
- AI Council
- LAN device discovery and management
- SSH / SMB / NAS management
- expanded browser research agent
- voice/screen upgrades beyond existing SARA capabilities
- unrestricted PC or kernel control

---

## 16. Next planned development sequence

After Phase 0 passes GitHub and target Windows validation:

```text
Phase 1  Advanced local brain/router
Phase 2  Semantic + episodic + project memory
Phase 3  Experience learning / skill evaluation
Phase 4  NVIDIA + OpenRouter + Hugging Face provider manager
Phase 5  Internet research/browser layer
Phase 6  Expanded typed computer tools
Phase 7  Authorized LAN / NAS / SSH / local-service layer
Phase 8  Multi-agent supervisor / planner / reviewer
Phase 9  Voice, screen awareness, self-healing and AI Council
```

The project should continue using the rule:

```text
Build -> test -> fix -> commit -> next feature
```

rather than attempting all future capabilities in one generation.
