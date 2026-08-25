# Jubi v0.1.0 Phase 0 Fix Report

## Scope

Jubi Phase 0 is a stabilization and product-migration release built from the SARUS v1.3.1 foundation. It intentionally repairs confirmed foundation bugs before adding new cloud providers, LAN control, advanced planner logic or semantic self-learning.

## Confirmed fixes

### 1. SQLite writes were not consistently committed

**Problem**  
Several SARUS-era stores executed SQLite `INSERT` / `UPDATE` statements and then closed the connection without an explicit commit.

**Risk**  
Memory, events, tasks, approvals or automations could appear to work in-process but fail to persist reliably across connection/application restarts.

**Fix**  
Added `sarus/core/database.py` with explicit transaction and read helpers. The helper configures WAL mode, busy timeout and foreign keys. Memory, events, execution state and workflow automation writes now use explicit commit/rollback transactions.

**Tests**  
`tests/jubi_phase0_test.py` reopens memory/event/automation stores and verifies the records remain available.

---

### 2. SQLite concurrency was fragile

**Problem**  
The HTTP server, scheduler, Fable agenda and event system can all touch SQLite from different threads.

**Risk**  
Short bursts of concurrent work could cause avoidable `database is locked` errors.

**Fix**  
Connections now use WAL, a 5-second SQLite busy timeout and short transactions. Every operation gets a separate connection.

---

### 3. Approval-required task state was not resumable

**Problem**  
The old execution engine could create a pending approval, but changing approval status alone did not persist a complete execution cursor that could resume the exact paused step.

**Fix**  
Added persistent `task_state` with:

- original request
- source
- capability ID
- serialized plan
- current step index
- completed results
- execution context
- task status
- update time

The task now enters `waiting_approval`, survives a process restart, and resumes the exact persisted step after approval.

**Safety**  
Approval is matched to task ID + step ID. A later step cannot inherit the earlier approval. Rejected approvals terminate the waiting task without running the action. A resolved approval cannot be replayed through the execution API.

**Tests**  
Phase 0 tests simulate a process restart between `waiting_approval` and approval.

---

### 4. Task/Fable ID compatibility mismatch

**Problem**  
The execution engine's canonical identifier is `task_id`, while some SARUS-era code expects `id`.

**Fix**  
Jubi returns canonical `task_id` and a temporary `id` compatibility alias with the same value. This prevents loss of Fable execution trace linkage during Phase 0 while allowing later code to standardize on `task_id`.

---

### 5. Ollama router could return a model that was not installed

**Problem**  
The previous fallback could return the first configured candidate even when it was absent from Ollama.

**Fix**  
Jubi now selects only from the actual `/api/tags` model list. If no compatible model exists, it returns `None` and generation produces a useful error rather than pretending an unavailable model can run.

The model response also exposes basic metadata and role classification (`general`, `coding`, `vision`, `embedding`, `cloud-through-ollama`).

---

### 6. Doctor duplicated required model configuration

**Problem**  
Required model names existed both in `config/production.json` and hard-coded in Doctor.

**Fix**  
Doctor now reads the canonical production profile for required models and minimum Python version.

---

### 7. Background scheduler lifecycle was incomplete

**Problem**  
The scheduler had start behavior but no explicit clean stop/restart lifecycle and silently swallowed loop errors.

**Fix**  
Added `stop()`, thread reset, event reporting and committed automation updates. Jubi application shutdown stops both the workflow scheduler and Fable agenda thread.

---

### 8. Normal HTTP errors exposed raw traceback text

**Problem**  
Generic server failures returned Python traceback output to the UI.

**Fix**  
Normal mode returns safe error messages and records a local event. Tracebacks are returned only when `JUBI_DEBUG=1` (legacy `SARUS_DEBUG` is accepted during Phase 0).

---

### 9. User-facing product identity was SARUS

**Fix**  
The runtime identity, status API, dashboard, README, build metadata and installer artifact now identify the product as **Jubi v0.1.0**.

New Python entry points:

```powershell
python -m jubi.server
python -m jubi.acceptance
```

The canonical core remains under `sarus/` during Phase 0 to avoid a high-risk blind package/driver/installer rename. This is explicitly documented as a compatibility layer.

---

### 10. Windows installer artifact remained SARUS-branded

**Fix**  
The existing Inno Setup definition now emits:

```text
Jubi-Setup.exe
```

with target path:

```text
C:\Program Files\Jubi
```

and launcher name:

```text
Jubi.exe
```

The verified legacy launcher bytes are copied to `Jubi.exe` after SHA-256 equality verification during the compatibility release. Existing SARUS installs use a different AppId and are not intentionally overwritten.

---

## Security boundaries intentionally preserved

Phase 0 does not add unrestricted shell or kernel access. The following remain blocked or unavailable to model-facing privileged actions:

- arbitrary PowerShell/cmd
- arbitrary executable path invocation
- arbitrary IOCTL
- kernel-memory read/write
- raw driver access
- security-control disabling
- audit disabling

The controlled legacy Ring0 bridge remains limited to its existing fixed status/ping surface.

## Validation status

GitHub CI runs the new Phase 0 persistence/approval tests plus the existing production, Fable, integration, broker and Ring0 safety gates. Physical Windows validation is still required for real Ollama inference, SARA, installer clean-install behavior, signed driver activation, audio/camera/GPU and code-signing evidence.

## Deferred to later Jubi phases

- semantic vector memory / RAG
- experience-scored self-learning router
- advanced LLM planner and task DAG
- NVIDIA / OpenRouter / Hugging Face
- LAN / SSH / SMB / NAS
- expanded browser research tooling
- AI Council
- voice/screen upgrades
- broader typed Windows tools
