# Integration plan after source-map approval

**Current phase:** audit/design complete to the documented evidence limits. **Implementation has not started.** The user's explicit mapping approval is the next gate. It authorizes development within the agreed boundaries; it does not authorize optional external services, asset downloads, lab runs or publishing.

## Milestones, in dependency order

| Milestone | Concrete change | Completion evidence |
|---|---|---|
| M0 — truthful local foundation | Enforce local/free/network consent boundary across all inference entrypoints; reject explicit Ollama cloud aliases; remove HF inference and permanently exclude paid/subscription inference routes. Optional external services require verified free access, explicit consent and no recurring/manual key-rotation workflow. Replace native-ready labels with evidence-backed status; production manifest excludes restricted/uncleared sources and missing SARA payloads. | Tests reproduce and then close Local Only bypass; no-network core run; optional absence doesn't break startup; package manifest/license tests. |
| M1 — canonical contracts + Hermes vertical slice | One capability manifest model and Jubi Task API over pinned Hermes Kanban; local profile/runtime; Brain plans and chooses installed model; real child uses one harmless scoped tool through protected common dispatch/completion. Deny raw tool/CLI/board-DB access and protect policy/receipt state from that worker from the outset. Reviewer/verifier gate completes it. | Actual installed Ollama → Hermes child → workspace operation → independent verified result, no external key/service. One task identity across UI/API/session/receipts. Bypass-denial tests pass before real tools are exposed. |
| M2 — state migration + completion authority | Import legacy task/approval/automation/Fable records once; stop old writers/schedulers; normalize direct endpoints; adapt Hermes review/completion path. Protect board/policy/receipt state from workers. | Restart/reclaim/duplicate callback/approval replay/cancel tests; no raw completion or filesystem bypass; historical data retained; reversible migration. |
| M3 — hardware admission and dynamic roles | CPU/RAM/GPU/VRAM/storage probes, installed model capability tests, global resource leases, model residency/context tuning, bounded depth/time and pause/cancel hooks around Hermes. | CPU-only/low-memory/GPU tests; nested concurrency cannot exceed global allocation; parent waiting doesn't deadlock children; OOM recovery bounded; no cloud/download fallback. |
| M4 — one coding workflow | Load Superpowers stages and selected ECC reviewers through registry; Agency search/inspect/load; contained workspace edits/test processes; independent review/verification. | One observed failing regression → real edit → passing relevant tests → review → verified diff. Lazy prompt loading, model metadata translation and error handling proved. |
| M5 — real Windows operation | Browser Playwright and native UIA workers; typed PowerShell operations through common broker; observe/act/verify, scoped targets, receipts, stop control and restricted worker execution. | End-to-end local browser form/file task and native app task; uncertain UI/missing target yields blocked; locked/UAC desktop tested; no SARA dependency. |
| M6 — unified memory + research | Facade over existing memory/knowledge/experience/transcripts; compatible vector identities, namespace access, provenance/retention/export/forget; selected local research helpers and consented public reader. | Restart retrieval, cross-model mismatch prevention, no false-success promotion, deletion/namespace tests; untrusted evidence cannot grant tools. |
| M7 — native local voice | Capture/VAD + approved local STT; installed local TTS; push-to-talk/mute/interruption; optional measured Hey Jubi provider. | Air-gapped STT/TTS, speech-to-canonical-task completion, low-resource profile, wake false-positive/stop/permission tests. |
| M8 — one-click production lifecycle | Hardware-aware acquisition plan, approved component bundle, per-user state and unelevated login task, integrity-checked updates/migrations/rollback, actionable diagnostics. | Clean Windows install/update/rollback/uninstall matrix; no keys/subscriptions; offline bundle; selected profile replaces fixed-model/SARA acceptance. |
| M9 — optional providers/labs | Benign Cyber Lab Manager first; manual preapproved offline CyberGym subset, E2E then compatible ExploitGym subsets; CAI only if licensing allows research use. Separate optional Fable/AutoResearch labs. Experiential compatibility trial optional. | No-network/no-host-control/quota/manual-start tests before real benchmark; quarantine verification; absent optional providers leave core functional. |

M1 depends on M0's policy boundary and includes protected dispatch/completion for every route reaching its minimal worker. M2 extends that proven boundary across the full legacy migration; it does not postpone protection of real tools. M2/M3 must pass before broad autonomous tool deployment. M4/M5 can proceed independently after the contracts/containment foundation. M6 can migrate retrieval modules in parallel with worker development, but outcome promotion waits for verifier evidence. M7 can be developed separately after the input contract. M8 packaging follows validated runtime requirements. M9 never blocks the ordinary assistant or runs automatically.

## First implementation tranche, made concrete for approval

After approval, begin **M0 then M1**, not a whole-repository rewrite:

1. Record the accepted source map and snapshot identities; open a scoped `codex/` integration branch if Git permissions permit. Preserve current user work and the two unavailable CAI files; do not restore security-sensitive content automatically.
2. Promote the four audit reproductions into meaningful regression tests where relevant, then repair local-mode model admission, persistent broker replay and experience vector identity. Replace isolation labels with a hard unavailable result until an actual isolated executor exists. Normalize direct capability dispatch through the common authority path.
3. Design approved provider descriptors and truthful status responses while keeping the file index for browsing. Block production use of uncleared source content; remove SARA from core requirements and HF from inference routes. Preserve compatibility response fields with deprecation metadata where useful.
4. Pin the existing Hermes snapshot and run its prescribed tests in an approved prepared dependency environment. Bind only necessary runtime surfaces: `AIAgent`, `delegate_task`, registry, SessionDB, Kanban. No second queue, ECC executor or broad plugin auto-discovery.
5. Select one installed local model that actually supports the required tool protocol. If none is available, produce a concrete local-model setup requirement; do not silently download or call cloud.
6. Implement the narrow task/dispatch/completion adapter and one scoped file-read/write worker with verifiable postconditions. Adapt Hermes's PR-specific review path and fail-open completion assumptions before advertising autonomous completion. Show one real local end-to-end result and relevant failure tests.

M0/M1 changes should be individually reviewable, with tested diffs and no release/deployment in this phase. Tests must check observable behavior rather than merely asserting method names, labels or source counts.

## Expected code ownership

Proposed modules may be added under `jubi/` while `sarus/` remains a compatibility facade. Exact names can change during implementation without changing approved responsibilities.

| Area | Existing files to adapt | Intended result |
|---|---|---|
| Input/dispatch | `sarus/server.py`, `core/app.py`, `orchestrator.py`, `execution.py`, `council.py` | One Task API; old routes delegate; no competing planner or task writer |
| Model boundary | `core/models.py`, `brain.py`, `providers.py`, `config/brain.json`, `models.json`, `providers.json` | Installed local capability-based selection; paid/subscription inference excluded; any optional external service is verified free, consented and free of recurring/manual key rotation |
| Swarm adapter | `adapters/hermes.py`, pinned Hermes runtime | Native delegation and Kanban integration with Jubi admission/completion wrapper |
| Registry | `core/capabilities.py`, `adapters.py`, `native.py`, source descriptors | One capability index and truthful runtime evidence |
| State/consent | `core/database.py`, `privileged_broker.py`, `workflows.py`, `fable.py` | Durable consumption/migrations; legacy lifecycle writers retired; scoped user authority |
| Memory | `memory.py`, `knowledge.py`, `experience.py`, `conversations.py`, Fable capability metadata | Unified API, compatible embeddings, evidence/retention policy |
| Workers | `core/windows.py`, broker + new browser/UIA/coding/voice providers | Real bounded operations and verified postconditions |
| Lifecycle | Installer/C#/PowerShell/background/updater + release manifests | Selected signed/hash-verified components, per-user runtime, hardware configuration and rollback |
| Optional labs | New Cyber Lab Manager and optional provider descriptors | No automatic invocation; runtime-enforced containment; artifact-only return path |

## Test strategy and release gates

Retain passing behavioral tests. Correct fixture shortcomings without weakening assertions. Reclassify existing static readiness tests as static checks and all mocked inference/tool tests as contract tests. Replace the exact source-file-count requirement with verified provider manifests/hashes; report missing/quarantined sources honestly.

Prepare an offline dependency cache/test environment for Hermes and jsdom; installing those dependencies was not done during this audit. Run upstream Hermes delegation, tool-scope, Kanban-isolation, SessionDB and Kanban claim/recovery tests using its required wrapper. Investigate actual failures before native integration. Run Windows DPAPI/process tests under the intended unelevated installed identity; sandbox errors are not certification.

Use failure injection for interrupted side effects, timed-out models, full disk, removed model, embedding changes, stale/duplicate claims, replay after restart, policy revocation, malformed tool responses, prompt injection and inaccessible desktop. Require bounded resource/cancel behavior at every level. Run real local-model and actual app workflows separately from mocks and record hardware/runtime/model/digest/test outputs.

For lab containment tests use benign fixtures first. A network flag in metadata, missing docker command or blocked model response is not a passing no-network test. Inspect effective runtime settings and test prohibited channels, mounts, quotas and automatic startup before admitting optional benchmark code. Restricted/uncleared licenses and unavailable datasets mean the provider stays disabled.

Public release requires reproducible component inventory, license/notice review, local-only/no-key certification, upgrade/rollback and target-user security validation. Independent manifest signing can use project-managed offline trust; public Authenticode distribution remains a separately resolved release concern and cannot introduce a recurring paid runtime dependency.

## Outstanding choices resolved by measurement, not user friction

Keep defaults conservative: direct Ollama, serial generation until measured, leaf workers, no online service, no lab, text/push-to-talk, installed local voices, one memory API and Hermes Kanban. During implementation choose exact model sizes/context, native voice/wake binaries, Windows worker isolation details and container packaging from verified hardware/dependency/license evidence. These are engineering decisions inside the approved design; ask only when an actual user preference, asset acquisition or external/system permission is needed.

No delivery dates are claimed before the Hermes/Windows compatibility spike. Each milestone is accepted by the evidence above, not by file count or the number of named agents.
