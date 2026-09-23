# M0/M1 stabilization evidence

Last validation: 2026-09-15, Windows laptop, worktree `89f1/JUBI`, HEAD `231739f9eb373ba0d6ef3a2ab01eb0507dc70c15`.

**M0: FAIL. M1: FAIL. Not ready to merge.** The real local development acceptance did not repair the project. The final core suite has one unresolved source-integrity failure. No merge or release was performed.

This report supersedes the implementation-status statements in the historical September 12 audit. The attached source-map approval authorized this tranche; it did not authorize model downloads, cloud inference or Cyber Lab execution. The original audit and failed attempts remain preserved.

## Files changed since the checkpoint

Exactly **23 first-party files** differ from the saved SHA-256 checkpoint. [Machine-readable changes](changes-since-checkpoint.json) includes additions/modifications and both hashes. The checkpoint excluded documentation, source providers and ignored local runtime data; it is not a count of every dirty file relative to HEAD.

| File | Change in this stabilization phase |
|---|---|
| `sarus/adapters/sara.py` | Disable unattested natural-language relay; restrict health requests to literal loopback without redirects/proxies |
| `sarus/core/brain.py` | Reject explicit cloud aliases before catalogue I/O |
| `sarus/core/capabilities.py` | Validate enum inputs; exclude generated caches; prune source traversal early |
| `sarus/core/hardware.py` | Windows native/registry/CIM fallbacks, devices, tooling, resource admission and UTF-16 probe output |
| `sarus/core/hermes.py` | Accurate context admission, bounded retries, normalized status and durable transcript paths |
| `sarus/core/models.py` | Short, locked status-catalogue cache; inference preflight remains fresh |
| `sarus/core/provider_policy.py` | Literal loopback only and malformed model metadata denial |
| `sarus/integrations/acceptance_workspace.py` | Restricted arithmetic edit capability, immutable tests, real commands, phase guards |
| `sarus/integrations/development_acceptance.py` | Actual Brain/Hermes/Kanban/SessionDB acceptance, independent verification and failure persistence |
| `sarus/integrations/hermes_transport.py` | Native Ollama bridge, exact Windows command authorization, network guard, budgets and failure circuit |
| `sarus/integrations/hermes_worker.py` | Persist parent session and close session storage |
| `scripts/run_core_regressions.py` | Complete disposable core suite with exact results/logs |
| `scripts/run_development_acceptance.py` | Disposable project controller, Windows job termination and cancellation recovery |
| `tests/acceptance_workspace_test.py` | Actual fail/edit/pass checks; write, harness, phase and cancellation denials |
| `tests/hardware_test.py` | RAM/concurrency invariants, explicit paging admission and unavailable/UTF-16 probes |
| `tests/hermes_policy_probe.py` | Real Hermes constructor/delegation fixture, exact command and raw-network checks |
| `tests/hermes_policy_test.py` | Assertions over that isolated subprocess |
| `tests/integration_test.py` | Require blocked execution through disabled SARA relay; preserve exact source-count assertion |
| `tests/jubi_functional_regression_test.py` | Seed legacy persisted cloud preference while preserving privacy assertions |
| `tests/jubi_http_functional_test.py` | Explicit strict SARA certification fixture despite optional production default |
| `tests/production_readiness_test.py` | Test installed local chat discovery and reject cloud-only/embedding-only inventories |
| `tests/provider_paths_test.py` | Council, Supervisor, fallback, persisted automation/task and direct-adapter denials |
| `tests/trust_foundation_test.py` | Validate all six registry states and executable-state gates |

New validation artifacts are under this directory. Ignored disposable acceptance workspaces and Hermes SQLite databases remain under `data/development-acceptance/`; they are evidence, not installed capabilities. The pre-existing two missing CAI files were not restored or changed by this phase.

## Exact regression results

| Run | Passed | Failed | Skipped | Scope |
|---|---:|---:|---:|---|
| [Initial checkpoint](baseline/results.json) | 194 | 7 | 0 | 201 core unittest cases; DOM runtime separately failed because jsdom was missing |
| [Intermediate sandbox](final/results.json) | 210 | 5 | 0 | 215 core cases; diagnostic run before final fixes |
| [Actual Windows user](final-host/results.json) | 215 | 1 | 0 | 216 core cases |
| [Final, after real acceptance](post-acceptance/results.json) | **215** | **1** | **0** | **216 core cases across 26 unittest files; no suite-level execution errors** |

The separate dashboard runtime suite passed: **18 pages and 7 interaction flows**. It uses controlled inference fixtures and is not live-model acceptance. [Dashboard log](post-acceptance/dashboard_runtime_test.cjs.log). Python syntax for all 23 changed Python files, the modified installer PowerShell parser check, and `git diff --check` passed. [Syntax evidence](syntax-check.json).

The sole remaining core failure is `integration_test.py::T.test_02_registry_exact_original_file_count`: **17,127 present versus 17,129 expected**. [Failure log](post-acceptance/integration_test.py.log). The manifest and exact assertion were retained. Missing paths:

- `sources/cai-main(3)/cai-main/src/cai/caibench/cyber_ranges/CobaltGroupRansomware/c2/payload.py`
- `sources/cai-main(3)/cai-main/src/cai/tools/web/webshell_suit.py`

A read-only Defender history query found successful detections of those same files in the Desktop checkout on September 12 and 13. That corroborates a security-product interaction, but does not establish the precise deletion event in this worktree. No exclusions, protection changes or restorations were made.

Windows DPAPI and process enumeration failed under the restricted sandbox, then passed unchanged under the actual user account. Status loading was a real performance defect: first indexing spent about 11 seconds traversing and repeatedly normalizing paths. Pruned traversal removed that timeout; the original HTTP timeout assertion passed unchanged.

No security assertion was relaxed to obtain green results. Changed expectations reflect authorized behavior: legacy cloud preferences cannot grant authority, SARA certification is optional unless explicitly requested, installed chat models replace a mandatory fixed download list, and an unattested SARA relay cannot report task completion. Fifteen core test cases were added relative to the checkpoint run.

## Local Only bypass results

All **16 dedicated policy/path tests passed**, in addition to the existing provider-manager suite.

| Requested bypass | Result and evidence |
|---|---|
| Explicit cloud model/provider override | PASS: rejected before prompt/credential release; cloud alias rejected before catalogue I/O |
| Ollama cloud aliases | PASS: names blocked; renamed aliases rejected by fresh installed-model and `/api/show` metadata |
| Hermes workers | PASS: actual AIAgent constructor and delegated child cloud overrides fail at Jubi client creation |
| Nested workers | PASS: actual Hermes depth gate refuses nested orchestration; thread recreation does not change inference authority |
| Brain fallback | PASS: local failure cannot choose cloud, including saved legacy cloud settings |
| Council/Supervisor | PASS: local members remain local; external judge/provider overrides are denied |
| Automations | PASS: reopened persisted automation with cloud preference records failure without inference release |
| Resumed tasks | PASS: reopened approval/task cannot bypass transport or report completion after cloud denial |
| Direct source adapters | PASS: catalogue adapters use guarded ProviderManager; SARA agent relay is disabled even with a token; direct remote provider clients denied |

[Transport tests](post-acceptance/provider_policy_test.py.log), [entrypoint tests](post-acceptance/provider_paths_test.py.log), [real Hermes boundary tests](post-acceptance/hermes_policy_test.py.log). Proxies and redirects cannot redirect inference. Cloud keys and saved settings are not permission grants. These tests establish the exercised application paths; they are not an OS sandbox for arbitrary imported Python or a system-wide network audit.

## Real Hermes coding acceptance

[Complete attempt evidence](development-acceptance-final.json), [independently reopened storage and file hashes](acceptance-storage-check.json), [earlier minimal native-tool probe](native-tool-probe.json).

| Observation | Actual result |
|---|---|
| Brain | Classified the supplied goal as coding, complexity 2/5; initially ranked `qwen2.5-coder:7b` |
| Model compatibility | Qwen's installed 32,768-token capacity is below this Hermes version's 64K minimum. Jubi admitted local `glm4:latest` using metadata and an explicit single-worker CPU-paging allowance |
| Parent session | `20260915_095333_e5590c` |
| Actual child session | `20260915_095334_44a54e`, persisted with that parent |
| Hermes task | `t_077523b1`, persisted as `blocked` |
| Model endpoint | `glm4:latest` at `http://127.0.0.1:11500`; one actual inference request, timed out at 240.125 seconds |
| Worker tools | Only `jubi_workspace` exposed; **zero actual worker tool calls** |
| Files changed | **None**; `pricing.py` still adds instead of multiplying |
| Worker test command | **Not run by the child**; prose/error summaries were not treated as execution |
| Reviewer | **NOT_RUN**, no actual change to review; no second child spawned |
| Verifier | Real `python.exe -I -B <run>/run_tests.py <run>/project verify`; **exit 1**, one unittest method with five failing subcases |
| Process versus task | Worker process exited 0 after recording failure; controller exited 1; task acceptance is false |
| Experience | `364da917-c531-4bcf-be96-edc00c6aca64`, `success=0`, text persisted, not embedded |
| Remote providers | **0 recorded/allowed in the instrumented worker**; Python socket I/O outside guarded local inference denied. This is not a system-wide packet capture |

The earlier minimal GLM native tool probe also returned prose without a tool call. Successful chat or upstream Hermes `status=completed` does not establish development success. This version of Hermes returned `completed` with `exit_reason=max_iterations` and an API error; Jubi correctly refused acceptance and task completion.

The fixture capability permits edits only to a small, AST-constrained arithmetic function. It protects the harness/test hashes and executes exact fixed argv without a shell. It is deliberately not an arbitrary-project Python sandbox. Contract tests prove actual failing tests, an actual edit, passing tests and independent verification, but those tests do **not** substitute for the failed model-driven workflow.

Limits: at most two sequential children (coder and reviewer), depth 1, concurrency 1, child timeout 420 seconds, overall timeout 600 seconds, SDK retries 0, inference failure circuit prevents a second network attempt, at most 12 inference calls, worker iterations 8 and reviewer iterations 3. Cancellation uses a per-run sentinel and Windows job termination; timed-out/cancelled tasks cannot be completed. No general terminal, browser, native Kanban tool or nested delegation tool is exposed to the model.

## Windows hardware results

[Host probe](hardware-host.json) and [restricted-sandbox fallback probe](hardware-final.json) demonstrate both discovery paths.

| Item | Observed |
|---|---|
| CPU | AMD Ryzen 5 7530U; 6 physical / 12 logical cores |
| RAM | 16,474,791,936 bytes, approximately 15.34 GiB usable; free RAM varied strongly with local inference |
| GPU | AMD Radeon (TM) Graphics, driver 31.0.21910.4002 |
| VRAM | 536,870,912 bytes (512 MiB reported dedicated); shared RAM is not counted as dedicated VRAM |
| Disk | 26,767,872,000 bytes free at host capture, approximately 24.93 GiB; NVMe SSD enumerated, plus USB storage |
| Windows | Windows 11 25H2, build 26200.9168 |
| Devices | Realtek speakers, AMD microphone array, HP Wide Vision HD Camera; enumeration only, no recording or playback |
| Ollama | Online at `127.0.0.1:11500`; installed coding, general, vision and embedding models discovered |
| Model inventory | `qwen2.5-coder:7b`, `qwen2.5:7b`, `qwen2.5vl:3b`, `glm4:latest`, `nomic-embed-text-v2-moe:latest`; `qwen3-coder:480b-cloud` discovered but prohibited |
| WSL | Available, default version 2, default distribution `docker-desktop` |
| Docker | CLI present; daemon unavailable (named pipe absent) |

Missing CIM permission degraded to registry/native information without asking for hardware settings. Hardware probes do not start services, download models or capture devices. Admission remains an estimate; the slow/paging GLM attempt is not a performance qualification.

## Registry and remaining merge gates

The explicit registry distinguishes **AVAILABLE, PARTIAL, DEPENDENCY_MISSING, DISABLED, EXPERIMENTAL, FAILED**. Tests assert all six, deny execution in the four unavailable states, reject unknown states/inputs/source IDs, and prohibit cyber registration in the normal registry. Source indexing never registers an executor. `hermes.analysis` and the disposable development capability remain experimental.

Remaining defects/gates:

1. Resolve source distribution/manifest integrity through a reviewed provider manifest and quarantine/package decision. Do not restore flagged CAI files merely to satisfy a count.
2. Establish a compatible installed local model/tool protocol that actually passes inspect → failing test → edit → passing test → reviewer → independent verifier within laptop resource limits. No model was downloaded and no cloud fallback was used.
3. The acceptance is an explicit CLI integration harness. The ordinary task/UI route is not yet the canonical Hermes pipeline; legacy execution/task writers, readiness labels and memory APIs remain to be unified under the approved plan.
4. Generic workspace execution, cross-process resource leases, crash/fault recovery coverage and clean-install Hermes dependency provisioning are not certified by this narrow fixture. Full upstream Hermes regression/Windows installer certification also remain before production approval.
5. Re-run the real acceptance and all relevant regression gates after those blockers are resolved. M0/M1 must remain FAIL until the required model-driven development acceptance succeeds.

CyberGym, ExploitGym and CyberGym-E2E were not connected or run. No Cyber Lab Manager or full voice runtime was started. Hugging Face remains outside inference; no model/dataset download occurred. No merge to main.
