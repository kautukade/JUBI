# Jubi recursive architecture and implementation audit

Audit date: 2026-09-12. Inspected Jubi commit: `231739f9eb373ba0d6ef3a2ab01eb0507dc70c15`.

The audit began at `C:/Users/hp/Desktop/JUBI` and was transferred intact to `C:/Users/hp/.codex/worktrees/89f1/JUBI` before final review, at the same commit. Historical absolute paths in logs record where tests actually ran; they have not been rewritten to imply a new test run. Reports and relative source references apply to this worktree.

**Verdict: a useful local application foundation, not yet a production personal operating agent.** Text chat, local model ranking, persistence, public research, image analysis, receipts and typed Windows operations have real code. The main task planner still dispatches mostly prompt fragments. Hermes swarm execution, native offline voice, hardware admission, a unified lifecycle and enforced cyber containment are absent from the product path.

This phase produced audit/design artifacts only. Implementation awaits [source-map approval](SOURCE-TO-PIPELINE-MAP.md). No package/model/dataset/image installation, cyber execution, release, host configuration change or feature integration was performed.

## Coverage and evidence

- Recursive inventory attempted **17,383 files** outside `.git` and this audit directory: **17,381 readable files, 559,992,068 bytes**, with SHA-256, path, size and extension recorded in [file-inventory.csv](file-inventory.csv). Hidden source folders are included. See [inventory-summary.json](inventory-summary.json).
- Ten source roots contain **17,127 readable files**. The manifest expects 17,129. During inventory two CAI files returned read errors and subsequently appeared deleted in Git. Initial status was clean; no assistant deletion or execution of those files occurred. Cause is unestablished. They were not restored, and their errors are preserved. They are `src/cai/caibench/cyber_ranges/CobaltGroupRansomware/c2/payload.py` and `src/cai/tools/web/webshell_suit.py` under the CAI root.
- Actual Jubi dispatch, planner, adapters, model/provider routing, memory, state, policy/broker, receipts, server routes, Windows operations, background/update/install code and relevant tests were read. [core-symbols.json](core-symbols.json) inventories Python classes/functions and syntax results.
- Each source received recursive structural inspection plus semantic review of integration-critical code, manifests and tests. All Python files in CAI and the five SARA/Second Brain/Awesome/Fable/AutoResearch roots were syntax-read without import. Hermes/ECC/Superpowers/Agency received focused runtime/workflow/API review. This is **not a claim of manual line-by-line review of every vendored library, translated document, binary, fixture or demo**. Inventory or AST success does not mean executable readiness.
- Four unique additional upstream repositories were verified through public GitHub trees and actual API/launch/license files. CyberGym's duplicate URL was deduplicated. Their reviewed tree SHAs and limitations are in [cyber-optional-sources.md](cyber-optional-sources.md); no local integration exists.
- Detailed source findings: [swarm/workflows](swarm-workflows.md), [other capability providers](capability-sources.md), [cyber/optional providers](cyber-optional-sources.md).

## Source inventory

| Source | Readable files | Current Jubi relationship |
|---|---:|---|
| Hermes | 8,515 | Source text adapter; real upstream swarm/queue unconnected |
| ECC | 3,437 | Source text adapter; useful workflows and competing runtime upstream |
| Awesome LLM Apps | 1,802 | Example catalogue; selected text generation |
| CAI | 1,533 | Defensive prompt adapter; restricted source license; no isolation manager |
| Fable | 1,194 | Text adapter plus Jubi-written trace/capability/agenda/lab facade |
| Agency Agents | 343 | Persona catalogue; upstream lazy Hermes plugin available |
| Superpowers | 180 | Workflow text; no workflow-stage runtime binding |
| Second Brain | 97 | Skills/patterns; no memory backend |
| SARA | 16 | Incomplete proxy fragments; required computer runtime missing |
| AutoResearch | 10 | Experiment proposal only; actual source is CUDA training |

The source count is not a capability count. `sarus/core/capabilities.py:8` indexes files by name/path; assets, docs and source code all contribute. The manifest's 4,803 “capability-like files” is historical metadata, not proof of 4,803 executable actions. `summary()` also pluralizes `code` to `codes` and `doc` to `docs`, but its accumulator uses `code`/`docs`; the code subtotal is consequently wrong. There is no content-hash invalidation when an existing registry cache is loaded.

## Capability matrix

Status describes this checkout, not the intended design. “Implemented” still needs the stated runtime/target validation.

| Target capability | Status | Evidence and practical limit |
|---|---|---|
| Local HTTP dashboard/text chat | Implemented | `sarus/server.py:18,154,461`; `conversations.py:50`. Persistent bounded conversation context. Ten live HTTP fixture tests pass; inference is a named test double. |
| Natural-language goal planning | Partial | `orchestrator.py:17` uses keywords to construct fixed steps; separate `council.py:218` model planner feeds reasoning only. Chat and task APIs use different pipelines. |
| Installed Ollama role routing | Implemented, incomplete safety | `models.py:94,122`; `brain.py:264`. Installed model discovery, type/preferences/history/fallback exist; types are name heuristics; no hardware budget or tool support probe. Explicit cloud override flaw reproduced. |
| Hermes swarm/parallel/nesting | Indexed only in Jubi | `adapters/hermes.py` inherits prompt execution. Upstream implements real delegation, nested roles and Kanban; defaults/Windows limitations detailed in swarm report. |
| Coding workflow | Partial | Superpowers/ECC fragments loaded; SARA expected to perform implementation. No normal native edit→test→review loop is connected to Hermes. |
| Progressive agent loading | Partial | One file excerpt is selected by generic adapter. No unified executable manifest, dependency/trust/permission contract; adapters all imported on first get. Agency's upstream lazy loader is reusable. |
| Computer operation | Narrow implementation | `windows.py:93` implements bounded filesystem, Git inspection, apps/process/service/URL actions. No Playwright/UIA interaction or observe/act/verify loop in core. SARA natural-language control blocked without missing runtime. |
| Guarded PowerShell | Missing worker capability | Runtime explicitly blocks arbitrary shell. Installer uses PowerShell, but that is not a model-facing typed automation worker. Add fixed, validated PowerShell operations through the broker, not raw model scripts. |
| Public research | Implemented network capability | `research.py:103,195,245,274,289`: public-only HTTP/DNS/redirect restrictions, extraction/search/synthesis. No central network-consent record; source citations are not independently checked for entailment. Requires approved online access. |
| Local vision | Implemented | `vision.py:80` checks local installed vision model and image formats. No native screen capture/action loop; tests use fake models. |
| Local STT/TTS / Hey Jubi | Missing | `web/assets/vision.js:14,39,44` uses browser speech APIs. Offline STT is not guaranteed; no wake-word service. |
| Memory/RAG | Implemented but fragmented | `memory.py`, `knowledge.py`, `experience.py`, `conversations.py` use SQLite, but separate APIs and retention rules. Fable/Hermes/ECC add other potential stores. Experience embedding compatibility bug reproduced. |
| Reviewer / verifier | Partial / missing outcome gate | ECC steps and Supervisor review call models; `_execute_step` considers `out.ok` sufficient for completed. Receipt authentication proves recorded data integrity, not completion of requested work. |
| Approvals/restart | Partial | `execution.py:30,175` persists plan/approval resume. Other task systems are separate; no general crash recovery/idempotency for in-flight side effects. Broker replay memory resets on restart. |
| Unified registry/task state | Missing | File registry, Fable capabilities, task_state/tasks, Supervisor/Council, automations/Fable agenda, native Hermes/ECC stores overlap. |
| Installer/background/update | Implemented skeleton, partial production | Inno/C#/PowerShell pipeline, local env, Ollama provisioning, task registration and SHA checks. Fixed model list; whole-tree packaging; background runs Highest; no end-to-end install/update certification here. |
| Cyber Lab Manager | Missing | Policy string/metadata only. Fable lab is not a substitute. Additional benchmark repos only inspected upstream. |
| Experiential/HF requested roles | Not integrated / conflicting legacy HF role | Experiential absent locally. Existing HF inference/key UI contradicts the requested acquisition-only boundary. |

## Priority findings

Priorities are for the proposed local-first integration: P0 blocks admitting real autonomous tools; P1 blocks release; P2 reduces reliability/maintainability.

### P0 — local-only and free-only are not enforced end to end

`brain.py:275` validates that an explicit model is installed and not an embedding model, but does not reject `cloud-through-ollama`. `providers.py:650` invokes Brain and then labels the result `cloud=False`. An audit double advertised `example:cloud`; `ProviderManager.generate(..., model='example:cloud')` accepted it in `local_only` and returned a false local label. This was **reproduced without a network/model call**, in [probe-results.json](probe-results.json).

`providers.py:547` accepts explicit model IDs and ranks discovered paid/nonfree IDs after free ones rather than excluding them. Optional cloud mode therefore has no hard zero-cost guarantee. Default configuration is local-only, but merely selecting another mode plus a credential can enable routes outside the new requirements. Cloud model/health calls (`models`, `validate`, `status(validate=True)`) lack the generation mode gate. HF is configured as an inference provider. Automatic updates and repair/download code also have no central user consent policy. Implement one enforceable network/free/local boundary across inference, tools, auxiliary calls, assets, telemetry and background jobs.

### P0 — isolation is a label, and direct capability execution bypasses task policy

`execution.py:252,311` converts every CAI step to `defensive_readonly`. At `:260` any non-denied decision invokes the adapter; `:272` only attaches `policy_isolation=True`. A harmless autoresearch double was invoked in-process and marked completed under `isolated` policy. There is no container, no network denial and no resource containment.

`server.py`'s `/api/capability/run` branch calls `adapter.execute` directly, outside `ExecutionEngine`, without its approval/policy path. Several other endpoint families likewise have separate execution paths. Current prompt-only providers limit immediate effects, but attaching real tools here would create inconsistent authorization. Admission must be enforced at a shared dispatch boundary for every frontend and worker, including inherited Hermes tools. Cyber tools must be inaccessible to ordinary workers even by capability ID.

### P0 — restricted/unverified source material is distributed and selected as normal capability content

`installer/SARUS-Setup.iss:45` packages the repository recursively, including sources and vendor blobs. Local CAI LICENSE prohibits production use of restricted additions without commercial licensing. Normal audit/security keyword routing selects CAI source text today. SARA, Fable, AutoResearch and most Second Brain content lack a verified root license in these snapshots. Remove restricted/uncleared content from the production manifest and normal prompt catalogue after approval; retain source notices and component-level license evidence for approved reuse. Missing rights must not be “solved” by buying a required license under the zero-subscription design.

### P1 — multiple planners/state stores produce incompatible meanings of completion

`app.py:34` constructs Council/Supervisor, `:47` keyword Orchestrator, `:52` ExecutionEngine, `:53` Fable, `:56` WorkflowScheduler. Fable starts its own boot/periodic agenda during construction. `/api/chat`, `/api/task`, `/api/supervisor/run`, direct capabilities and Fable routes have different lifecycles. The current execution engine continues after failed steps and aggregates partial status; it has no dependency graph beyond sequence. The separate Supervisor sorts dependencies but performs reasoning only.

Use Hermes Kanban as the sole durable lifecycle backend through Jubi's Task API. Retain session/transcript and evidence stores for their own data, not independent task authorities. Retire old scheduler/state writers through an explicit migration. Workers submit results for review; only a trusted verifier can approve final completion.

### P1 — verification and learning trust generation success

`execution.py:281` marks `out.ok` completed. Generic adapters return `tools_executed=False`, yet their success can complete a task. `council.py:315` judges success from specialist model returns; `brain.py:407` records model success on nonempty generation. `server.py` records successful chat into experience. These measure availability/completion of inference, not correctness or verified action success.

Keep inference transport success separate from outcome quality. Require typed tool receipts, observed postconditions, relevant tests and a distinct review verdict. Persist verified/failed/unknown outcomes; do not promote self-reported prose into trusted memories or reusable enabled capabilities. Fable stored permissions are metadata currently ignored by `run_capability` (`fable.py:580`).

### P1 — memory contracts diverge

Knowledge ingestion checks model/dimension stability and finite vectors. Experience retrieval (`experience.py:138`) discards the query embedding model identity and reads vectors without filtering their stored model. Same-dimension vectors from different models can match; the audit double produced a spurious **0.94** score after changing embedding model. Experience ingestion also accepts nonfinite float vectors. Unified Memory API must preserve model digest/dimensions, provenance, namespace ACL, confidence, retention/export/deletion and lexical fallback. A shared SQLite file alone is not a unified memory contract.

### P1 — installation does not adapt to hardware or preserve least privilege

`JUBI-PREREQUISITES.ps1:317` provisions all `Production.required_models`; no CPU/RAM/GPU/VRAM/storage sizing implementation exists there. Core/full acceptance still depends on fixed models and full acceptance on SARA. Existing launcher repair is valuable, but not hardware-aware setup.

The install defaults to Program Files while mutable `data/sarus.db` lives below the application root. `REGISTER-JUBI-BACKGROUND.ps1:35` registers the full user-session supervisor at `RunLevel Highest`. Move mutable state to per-user application data and run Brain/Hermes/voice/desktop workers unelevated. Use the narrow privileged broker only when a concrete operation requires elevation; no kernel driver is needed for ordinary agent operation.

### P1 — background release trust and failure recovery are incomplete

`bootstrap.json` enables automatic continuous updates. `updater.py` checks expected repository/URLs, SHA-256 and epochs, but the hash and installer originate in the same mutable release; no independently trusted signed manifest verification is implemented. `build-windows-installer.yml` builds and publishes without invoking the available release-signing helper. `background.py` triggers full prerequisite repair on its interval and restarts processes; transactional data migration, running-job drain/checkpoint, rollback and signed update authorization are not demonstrated. The public release needs an offline-verifiable publisher manifest and safe rollback. Public Authenticode is a separate release-distribution decision; no recurring paid certificate/service can become a runtime prerequisite.

### P1 — restart safety stops at a subset of approvals

Task/task_state writes are separate transactions around side effects. There is no universal action-idempotency key or in-flight reconciliation. Broker `_seen` at `privileged_broker.py:39,104` is per-process only. Two new broker instances accepted the same harmless test request; signed action proof replay protection therefore does not survive process recreation. Store durable request consumption and reconcile observable side effects before retrying. Do not claim exactly-once execution where an app cannot provide it.

### P2 — readiness labels, memory growth and runtime dependencies need cleanup

Directory existence or unconditionally true values report several providers as native/ready (`native.py`, source probes). Registry caching has no source version invalidation, runtime availability or permission schema. Chat locks and append-only stores have no complete per-user retention policy; semantic retrieval uses bounded scans rather than scalable search. Source-bundle counters and checksum artifacts need an actual component manifest. The root has no tracked top-level LICENSE; this is a release/governance gap, not evidence that all sources can be redistributed under one license.

## Verification results

Existing tests were inspected, then executed in a disposable copy using bundled Python. Original application/test code was copied unchanged. The fixture config used absolute read-only provider paths for source-dependent tests, original relative paths for the HTTP fixture; all mutable app state stayed temporary. External credentials were removed from the child environment; Ollama was pointed at an unavailable loopback port except where tests create explicit doubles. No existing user data or credentials were used.

Results: [original logs/results](test-results.json), [corrected-fixture retries](test-retries.json), [test-logs](test-logs/), [audit reproductions](probe-results.json).

| Check | Result / qualification |
|---|---|
| 20 existing Python suites | Final: **180/184 cases pass; 17 suites pass and 3 retain failures/errors**. This counts the latest corrected broker/HTTP runs, not retry duplicates. The HTTP suite initially failed because of an audit fixture configuration error; after correction all 10 tests pass. |
| Broker security | **17/18 pass** after correcting audit test-secret length. `tasklist` fails Access denied in this sandbox; no host-control success claim. |
| Foundation integration | **22/24 pass**. Source count 17,127 vs 17,129 fails; `/api/status` times out with unavailable dependencies/probes. The all-adapters execution test passes with model/SARA/research doubles and does not prove native integration. |
| Provider manager | **10/11 pass**; DPAPI round-trip fails WinError 2 under sandbox identity. Requires validation as the real installed user. |
| JavaScript syntax | All **8** dashboard scripts pass `node --check`. |
| Dashboard DOM execution | Blocked by missing `jsdom`; no dependency installation. Initial fixture-path problem corrected first. |
| PowerShell syntax | All **14** first-party PS1 files parse without errors. No installer/service/driver script was executed. |
| Core Python syntax | All **70** inventoried first-party Python files parse; function/class index saved. |
| Additional audit probes | Four confirmed gaps: cloud override in Local Only, cross-model experience scoring, in-process isolated adapter, replay acceptance across broker instances. All used harmless doubles. |
| Upstream focused tests | ECC 26 behavior tests; Superpowers 32 protocol tests plus bootstrap-cache check; Agency lazy-plugin schema/search/inspect check pass. Hermes runner blocked before collection by missing pytest environment. |
| Other source checks | 561/562 Python files parse across five roots (Second Brain quote-slide syntax error); 1,077 accessible CAI Python files parse. Fable formatter lint: 136 files clean; Awesome ranker: 6 tests pass; Second Brain transport classifier: 4 smoke assertions pass. |

Target hardware probes for CPU/RAM/GPU returned Access denied; [host-probe.json](host-probe.json) records this and approximately 24 GiB free on C at observation time. This is not a certified hardware profile. Do not attempt huge benchmark/model provisioning from this audit. No real local-model generation, microphone/STT/TTS test, UIA/Playwright action, installer lifecycle, crash/upgrade soak, WSL/container escape test or cyber benchmark was executed.

Passing static “production readiness” tests mostly check source strings/configuration. They cannot establish production readiness. In particular, isolation tests currently assert a decision string, and model/SARA doubles underpin the all-ten-adapters test.

## Decision and next work

The recommended ownership and boundaries are in [SOURCE-TO-PIPELINE-MAP.md](SOURCE-TO-PIPELINE-MAP.md). The [pipeline design](PIPELINE-DESIGN.md) and [implementation plan](IMPLEMENTATION-PLAN.md) define concrete first integration gates, migrations and release checks. Complete remaining dependency/Windows/live tests during those gates; do not weaken requirements merely to make existing source-count or status tests green.

The only intended workspace additions are this audit directory. Two source disappearances are separately documented above; product files were not intentionally modified. **Await mapping approval before integration.**
