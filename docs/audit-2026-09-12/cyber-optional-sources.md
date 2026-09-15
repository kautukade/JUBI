# Cyber and optional source audit — 2026-09-12

Status: audit and proposed design only. No product implementation, source installation, model/data/image download, cyber workload, container launch, remote inference, or external account setup was performed. Integration must wait for the user's review and approval of the source-to-pipeline map.

## Findings that change integration decisions

1. **CAI is present as source, but its Jubi adapter is model reasoning only.** It does not operate CAI tools or enforce a container boundary. The bundled CAI license explicitly restricts Alias Robotics additions to non-commercial research/academic use and prohibits production use without a commercial license. Keep those components out of the production Jubi runtime and default prompt catalogue. A research-only lab option needs explicit licensing/eligibility review; this audit does not establish distribution rights for every bundled component.
2. **All four unique requested upstream repositories are accessible.** CyberGym appeared twice in the request and represents one provider. ExploitGym, CyberGym, CyberGym-E2E, and Experiential are not present as independent repositories in the audited local `sources/` inventory or `config/sources.json`; upstream presence is not local integration.
3. **The benchmarks' upstream network policies do not meet the requested universal no-network rule.** CyberGym's evaluator containers already use `network_mode="none"`, but its agent firewall permits external destinations. CyberGym-E2E and ExploitGym also provide proxy/LLM arrangements rather than a universal no-network envelope. Do not run their quick-start paths as Jubi integrations.
4. **Experiential is a real Apache-2.0 local gateway/router candidate, not evidence of free hosted inference.** It supports explicit OpenAI-compatible endpoints; its runtime requires a nonempty connection credential. Default telemetry is on. It stays disabled until a local-only adapter, Windows packaging, and offline compatibility have been verified. Paid provider routes and hosted telemetry are outside Jubi's zero-subscription defaults.

## Short source-to-pipeline map for approval

Canonical normal flow: request → Jubi Brain planner/model router → Hermes delegation → specialist worker → reviewer → verifier → final answer. The table adds providers to that flow; it does not add a second orchestrator, task state machine, or normal computer-control route.

| Source | Entry into canonical flow | Proposed output | Default and boundary |
|---|---|---|---|
| Local CAI | Explicit research-lab request → Brain → Hermes lab job → Cyber Lab Manager; no normal worker import | Research-only benchmark/evaluation evidence, subject to bundled license | Disabled; no production runtime/prompt reuse of restricted additions; no Windows/Playwright/UIA/PowerShell bridge |
| `sunblaze-ucb/exploitgym` | Explicit manual lab start → Brain plans approved benchmark manifest → Hermes resource-limited lab worker → Cyber Lab Manager | Offline benchmark score and evidence for reviewer/verifier | Disabled, optional offline container benchmark/training only; kernel/device-dependent cases unavailable under baseline confinement |
| `sunblaze-ucb/cybergym` | Same lab path; one provider despite duplicate URL | Offline vulnerability-reproduction evaluation results | Disabled, optional; no public submission service and no cloud-API firewall allowlist |
| `sunblaze-ucb/cybergym-e2e` | Same lab path | Offline patch and benchmark validation results; four-stage validation pattern | Disabled, optional; vendor runner is not granted host Docker or desktop credentials |
| `experientiallabs/experiential` | Optional adapter below **Jubi Brain model router**, using approved installed local model endpoints | OpenAI-compatible completion/embedding response and route metadata | Disabled pending offline/Windows smoke test; no second planner/swarm; no hosted fallback; telemetry off before first launch |
| Hugging Face | Explicit user-approved **manual model/dataset acquisition**, before a runtime job | Pinned, hashed local model/dataset artifact with license metadata | Optional source only; no inference dependency, background download, automatic model fetch, or required account |

Reviewer/verifier receives structured results and artifact references from the lab. A lab result is untrusted evidence; it never becomes a command for normal computer control. Jubi's ordinary defensive code review can remain a coding/reviewer capability using license-compatible tools and local models, without importing CAI runtime or cyber benchmark tooling.

## Local CAI recursive inventory and code evidence

Local root: `sources/cai-main(3)/cai-main/`. Initial recursive `rg --files --hidden` inventory returned **1,535 files**, including **1,079 Python files**, **207 Markdown files**, **40 TXT files**, and **16 shell scripts**. Later read-only Python enumeration found **1,533 files / 1,077 Python files**. The parent audit observed two source files becoming unavailable during inventory (`payload.py` under `CobaltGroupRansomware/c2` and `src/cai/tools/web/webshell_suit.py`); Git then reported deletions. No assistant deletion command caused these changes, and this audit did not restore or execute them. The cause is not established here.

The final accessible Python inventory was parsed with Python's `ast.parse` without importing source modules: **1,077/1,077 parsed**, **254,919 lines**, **zero syntax/encoding failures**. File counts by first directory: `src` 428; `tests` 555; `examples` 76; `tools` 15; `benchmarks` 2; `fluency` 1. There were 540 files named `test_*.py` across these directories. This is recursive enumeration/static parsing and focused manual code reading, not a claim of line-by-line manual review of all 254,919 lines or proof that the runtime works.

| Capability | Upstream/local source evidence | Jubi integration classification |
|---|---|---|
| Agents, handoffs, orchestration, sessions | Actual implementation in `src/cai/agents/`, `src/cai/sdk/agents/`, `src/cai/parallel_worker.py`; dynamic imports in `src/cai/agents/__init__.py:87,104,127,207` | Real CAI source; not native Jubi execution. Do not adopt its parallel-worker/session system as a second Hermes layer |
| Tool catalogue | `src/cai/tool_registry.py:77` defines `ToolRegistry`; agent/category filtering at lines 113,121,135; `src/cai/config.py:125` makes automatic supplementation opt-in | Overlaps Jubi registry; metadata adapter only, not another global registry |
| Shell / containers / SSH | `src/cai/tools/reconnaissance/generic_linux_command.py:305` is an executable tool; environment routing documented at lines 348–358; `src/cai/tools/executor.py:1289` dispatches Docker/CTF/SSH/local; host shell subprocesses at lines 151,694,833,1109,1166 | Active code present, not allowed into normal computer control. A missing container can fall back to local execution upstream |
| Network reconnaissance and control tools | Actual files under `src/cai/tools/reconnaissance/`, `tools/command_and_control/`, `tools/network/`, and `tools/web/`; includes HTTP clients, network scanning, SSH and traffic capture wrappers | Cyber lab only, and unavailable if they require a network under the required offline profile |
| Local/cloud model routing | `src/cai/config.py:177` has `ollama_url`; `src/cai/sdk/agents/models/openai_chatcompletions.py:368` selects Ollama mode; provider code and pricing tables are present | Local route capability exists upstream, not a reason to adopt CAI's router. Jubi Brain owns roles/installed-model routing |
| API and auth | FastAPI implementation in `src/cai/api/app.py`; `_require_api_key` at line 132; legacy configuration allows auth disabled when no root key is configured (lines 138–150) | Not a hardened Jubi service and not needed for prompt-only adapter |
| Voice | `tests/voice/test_openai_stt.py`, `test_openai_tts.py`, pipeline tests and `examples/voice/`; `pyproject.toml` voice extra includes numpy/websockets | Not a verified fully local STT/TTS provider; not selected for Jubi voice |
| Memory / tracing / telemetry | `src/cai/util/session.py`, `session_compact.py`, SDK tracing tree; optional CAI extensions checked in `src/cai/__init__.py` | Competing state/memory and optional extension presence checks, not Jubi's unified memory API |

Important defaults in `src/cai/config.py`: model `alias1` (line 68), price limit (78), tracing `True` (93), telemetry `True` (94), guardrails `False` (102), no active container by default (160). The dependency manifest identifies `cai-framework` version 1.1.5, Python >=3.10, LiteLLM/OpenAI/network/Docker dependencies, and a dual MIT/proprietary license. These defaults are incompatible with simply launching CAI inside the normal Jubi assistant.

### Actual Jubi CAI route

- `sarus/adapters/cai.py:3–8`: extends `PromptCatalogAdapter`; probe sets `native=False`; prepends a defensive/read-only instruction; returns `isolation='analysis-only'`.
- `sarus/adapters/base.py:15–20`: reads up to 18,000 source characters into the model system prompt, calls `app.providers.generate`, and returns `mode='model_reasoning', tools_executed=False`. Successful text generation is not native CAI execution or verified work.
- `sarus/core/orchestrator.py:27`: simple keyword routing sends security/audit/vulnerability/defensive requests to CAI. This also places restricted source content on a normal request path today.
- `sarus/core/policy.py:8`: non-`defensive_readonly` CAI actions return a string decision `isolated`; it does not construct or verify a sandbox.
- `sarus/core/execution.py:252,311`: every CAI step is evaluated as `defensive_readonly`. At lines 260–272, the adapter executes for a non-denied policy and an isolated decision only adds metadata. This is not an enforced Cyber Lab Manager.
- `sarus/core/native.py:8,19`: CAI native readiness checks whether `native/cai/Scripts/cai.exe` exists on Windows; file existence is not a runtime/isolation test.
- `tests/integration_test.py:23` and `sarus/acceptance.py:160`: “CAI isolation” verifies the decision string equals `isolated`; it does not test no network, quotas, containers, mounts, or host-control separation. Integration setup mocks model generation and SARA calls (`tests/integration_test.py:11–13`).

### License constraint

The bundled `LICENSE:3,7,15,21,23` distinguishes MIT-derived portions from Alias-authored additions. Line 23 states: “Commercial, professional, or production use of these components is strictly prohibited without a commercial license.” `LICENSE-MIT` alone is not the license for all bundled files. The source paths described by the license and the modern SDK layout need component-level attribution checking before any reuse; no blanket MIT classification is supported. This audit reports the checked license text, not a legal determination for a particular distribution.

### Test execution and limitations

The system `py -3.11` launcher reports a Microsoft Store Python path that fails to launch with access denied. A bundled Python executable was available and used only for recursive static AST parsing and dependency probes. `python -B -m pytest --version` returned `No module named pytest`; probes also found no `openai`, `litellm`, Python `docker`, `rich`, `griffe`, or `mako` in that runtime. No dependencies were installed.

CAI's test root imports its model clients and tracing modules in `tests/conftest.py`; real-model methods are generally monkeypatched but a marker can permit them. The GitHub workflow's active smoke jobs target prompt layering and selection-agent import loading, while full lint/typecheck sections are commented out (`.github/workflows/tests.yml`). Consequently no CAI runtime/benchmark/test suite was executed, and no passing runtime claim is made. Root Jubi test results are recorded separately by the principal audit.

## Upstream verification — read-only primary source review

GitHub recursive tree responses were not truncated. These are remote inventory snapshots, not local installations. Branch heads are mutable; the tree SHA identifies the inspected snapshot. The future acquisition manifest must pin the commit and artifact digests separately.

| Repository | Accessible | Inspected tree SHA | Blob count | Code license checked |
|---|---|---|---:|---|
| `experientiallabs/experiential` | Public | `c7ad7c93a4e80967086bd5086f0b20929a352247` | 950 | Apache-2.0 |
| `sunblaze-ucb/exploitgym` | Public | `e4123d043774623b2274e6bbe0155a423d631f0a` | 6,273 | Apache-2.0; separate task-data licenses |
| `sunblaze-ucb/cybergym` | Public | `c6fe2027d39471375920b92cf1025e23a99ffda5` | 31 | Apache-2.0; referenced targets/data require their own review |
| `sunblaze-ucb/cybergym-e2e` | Public | `b46456c46838b2b090d7e6ded5bfdf1ff583dba7` | 5,680 | Apache-2.0; referenced images/targets/data not exhaustively licensed here |

Tree inventory is not a full line-by-line upstream audit. Focused source reads verified the actual interfaces and isolation-relevant launch code below. No remote test suite, Git clone, package installer, benchmark image, payload, model, or dataset was run/downloaded.

### Experiential: real interface, optional local candidate

- The checked [LICENSE](https://github.com/experientiallabs/experiential/blob/main/LICENSE) is Apache-2.0.
- [Native server code](https://github.com/experientiallabs/experiential/blob/main/exp/runtime/gateway/native/src/server.rs#L158) registers `/v1/models`, `/v1/chat/completions`, `/v1/embeddings`, `/v1/responses`, `/v1/messages`, and health/metrics routes (lines 158–175). This is implemented API surface, not a roadmap.
- [Package exports](https://github.com/experientiallabs/experiential/blob/main/exp/__init__.py) lazily expose `load_router` from `exp.optimize.router.activation`. The [README](https://github.com/experientiallabs/experiential/blob/main/README.md) documents the local gateway and Python context-manager client; its hosted account, BYOK and optimization examples are separate workflows and do not establish free inference.
- [Runtime registry](https://github.com/experientiallabs/experiential/blob/main/exp/runtime/models/registry.py#L291) resolves a connection credential and constructs the explicitly configured provider. The `openai-compatible` entry maps to `OpenAICompatibleClient` with no default URL (line 627); [client code](https://github.com/experientiallabs/experiential/blob/main/exp/runtime/models/providers/openai_compatible.py#L440) takes `api_key` and `base_url`, and builds the HTTP route from that endpoint. It does not itself supply a model.
- [Credential resolution](https://github.com/experientiallabs/experiential/blob/main/exp/runtime/models/credentials.py#L171) requires a nonempty environment/stored credential and fails if absent. An app-managed local-only token/placeholder for an endpoint that accepts it is a proposed integration technique, not a tested upstream “no key” mode. No recurring paid key is inherently required by a local compatible backend, but exact installed Ollama interoperability remains unverified.
- [Telemetry settings](https://github.com/experientiallabs/experiential/blob/main/exp/common/config/settings.py#L24) default to enabled. [Telemetry implementation](https://github.com/experientiallabs/experiential/blob/main/exp/common/observability/telemetry.py#L381) honors `DO_NOT_TRACK` and `EXP_TELEMETRY`. An approved local adapter must disable telemetry **before first launch**, omit hosted login/account setup, restrict destinations to the selected local endpoint, and disable any remote fallback/optimization or external trace upload.
- [Packaging](https://github.com/experientiallabs/experiential/blob/main/pyproject.toml#L7) requires Python >=3.12 and `exp-gateway-native>=0.3.52,<0.4`; native Rust source is present. Windows wheel availability and offline installation were not tested. Tinker fine-tuning is an optional dependency/workflow and is not selected for Jubi's free local stack.

Decision: **optional, disabled, candidate adapter beneath Jubi's router**. Jubi continues to function with direct installed Ollama models. Do not replace the hardware-aware Brain scheduler or Hermes with Experiential's project/routing workflow; do not duplicate its state into the canonical task state.

### ExploitGym: benchmark-only; upstream launch is not the Jubi boundary

The [code license](https://github.com/sunblaze-ucb/exploitgym/blob/main/LICENSE) is Apache-2.0. [DATA_LICENSE.md](https://github.com/sunblaze-ucb/exploitgym/blob/main/DATA_LICENSE.md) explicitly says task artifacts retain external upstream licenses; code licensing does not relicense data. It identifies Linux GPL-2.0 and V8 BSD-3-Clause among the relevant source families, with other per-project/content terms.

The actual [server routes](https://github.com/sunblaze-ucb/exploitgym/blob/main/src/cybergym/server/__main__.py#L107) implement lifecycle endpoints including create/delete/restart/health; the private run-command endpoint is separate. [Controller code](https://github.com/sunblaze-ucb/exploitgym/blob/main/src/cybergym/server/controller.py#L89) delegates container creation to task handlers and accepts optional resource profiles; missing profiles use Docker defaults. [Kernel handler code](https://github.com/sunblaze-ucb/exploitgym/blob/main/src/cybergym/server/task_handler.py#L307) can pass `/dev/kvm`, runs QEMU, and accepts optional network/resources. Those cases cannot silently be admitted by a generic desktop container wrapper.

The [quick start](https://github.com/sunblaze-ucb/exploitgym/blob/main/README.md) pulls runtime artifacts/images and shows external model keys plus a firewall/LLM proxy. These are not executed or selected. Baseline Jubi lab profile must reject task types requiring host kernel changes, device passthrough, privilege escalation, or missing offline artifacts. Do not weaken isolation to make all benchmark tasks run.

### CyberGym: useful offline evaluator, separate agent network assumptions

The [LICENSE](https://github.com/sunblaze-ucb/cybergym/blob/main/LICENSE) is Apache-2.0. Actual [FastAPI server code](https://github.com/sunblaze-ucb/cybergym/blob/main/src/cybergym/server/__main__.py#L131) separates a public `/submit-vul` route from authenticated fix/query/verification routes; the default bind is loopback (line 237). A public route is not acceptable as a normal host-accessible execution API.

The [evaluator launch code](https://github.com/sunblaze-ucb/cybergym/blob/main/src/cybergym/server/server_utils.py#L70) uses Docker with `network_mode="none"` and a read-only input mount; its binary-mode launch also uses no network. Those paths include timeouts but do not specify CPU/RAM/PID quotas in the inspected creation call. The [README](https://github.com/sunblaze-ucb/cybergym/blob/main/README.md) separately documents agent-container firewall allowlists and warns against public exposure. It reports substantial optional data requirements (approximately 240 GB benchmark data, 130 GB binary mode, and roughly 10 TB full server data), so automatic installation is unsuitable. Its `examples/agents` tree entry is a submodule, not proof of a self-contained local agent implementation.

Decision: reuse the evaluator behind the Cyber Lab Manager after explicit artifact acquisition, license review, hardware admission and offline adaptation. Report subset/resource unavailability clearly.

### CyberGym-E2E: validation pattern available; default runner needs containment work

The [LICENSE](https://github.com/sunblaze-ucb/cybergym-e2e/blob/main/LICENSE) is Apache-2.0. Actual [runner code](https://github.com/sunblaze-ucb/cybergym-e2e/blob/main/scripts/run_agent.py#L332) validates in fresh containers per stage, and [validator code](https://github.com/sunblaze-ucb/cybergym-e2e/blob/main/scripts/validate.py#L116) implements the task validation. The four checks are: expected crash before patch, no crash after patch, patched project tests, and ground-truth input against the patch. This is useful verifier structure for lab evidence.

The [container helper](https://github.com/sunblaze-ucb/cybergym-e2e/blob/main/scripts/utils.py#L90) constructs a Docker run with an optional network argument and no CPU/RAM/PID limits. The [runner](https://github.com/sunblaze-ucb/cybergym-e2e/blob/main/scripts/run_agent.py#L440) installs agent CLIs and has network-dependent setup; its firewall can be opted out. The [README](https://github.com/sunblaze-ucb/cybergym-e2e/blob/main/README.md) shows a Hugging Face token/download, Docker image pulls, and a host sanitizer/ASLR setting. None is authorized automatically merely because the source is listed. Host kernel settings must not be changed by Jubi to satisfy a benchmark.

Decision: retain as optional offline benchmark/training provider, disabled. Stage dependencies ahead of time through explicit acquisition. Reject unsupported cases. A container runner with optional network is not the Cyber Lab Manager.

## Proposed Cyber Lab Manager contract

This is a design requirement, not implemented behavior.

1. **Manual start only.** Ordinary voice/text planning may explain or prepare a manifest; it cannot start a lab from a keyword, wake word, resumed session, background automation, or another agent. Each run needs an explicit local start action identifying benchmark subset, already-approved artifacts, duration and quotas. Cancelling a parent job cancels descendants and lab processes.
2. **Offline means no network.** Every workload container uses network none, no published ports, no host networking and no Squid/external-API exceptions. If a benchmark expects HTTP between components, adapt to a single offline job envelope with in-container loopback or bounded file/stdio IPC; do not introduce a host-accessible listener. A local model must be resident inside the offline envelope, or invoked through a narrow trusted inference-only broker using bounded file/stdio messages. It must never give the lab a host command channel, URL-fetch capability or arbitrary model-server endpoint.
3. **Fail-closed isolation admission.** No normal desktop/browser sessions, UIA handles, PowerShell tokens, Jubi service credentials, personal-memory database, host Docker socket, privileged containers, device passthrough, arbitrary host mounts, or host-kernel changes. Use a dedicated per-job filesystem, read-only staged inputs, a bounded output directory, non-admin identities, dropped capabilities, no-new-privileges, supported container security profile, and a read-only root filesystem where compatible. Unsupported benchmark cases stay unavailable. Containers share a kernel; a blanket “secure for every kernel benchmark” claim is not justified.
4. **Hardware and quota enforcement.** Jubi's scheduler grants an explicit finite lease for CPU, RAM, PIDs, storage/output bytes, duration and optional model resources. Enforce those at the container/runtime layer, inspect effective settings before starting, and terminate on timeout/OOM/quota violation. Concurrency comes from the Hermes worker lease, with a conservative default of one lab job; benchmark runners cannot spawn unbounded siblings or nested orchestrators.
5. **One registry and one task state.** Registry entries carry `execution_domain=cyber_lab`, `manual_start=true`, `network=none`, license/artifact pins and required resources. Brain discovers their metadata progressively. Hermes owns delegation; Jubi's canonical task store records lifecycle/events and artifact references. Vendor databases are job-local artifacts, not competing user task or memory systems. Normal computer-control workers cannot resolve lab tool handles, and lab workers cannot resolve normal worker handles.
6. **Evidence-only exit.** Export size-bounded structured metrics, logs and hashes through a quarantine directory. Validate filenames, types, paths and schemas. Reviewer assesses the result; independent verifier checks exit status, benchmark assertions, container settings, quotas and artifact hashes. Import only a non-executable outcome summary into unified memory. No automatic patch application, host command replay, external leaderboard submission, or executable artifact launch.
7. **Acquisition is separate.** Models, packages, images and datasets must be approved and staged before a job; license metadata and pinned digests are mandatory. No internet-enabled training/runtime container is used as a downloader. Hugging Face remains optional and manual; a missing token or offline artifact leaves only that optional provider unavailable.

Acceptance tests after mapping approval must prove denial of normal computer-control handles from a lab job, deny network egress/host reachability, detect absent quotas, prevent automatic startup, enforce nested-worker resource accounting, survive cancellation/restart, and reject untrusted output instructions. A policy string, container-presence check, or a model response alone is insufficient evidence.

## Integration order after approval

1. Remove CAI's restricted prompt/runtime material from the normal production path and make capability availability/license/domain explicit in the single registry.
2. Build and test the generic Cyber Lab Manager boundary using benign fixtures before admitting any cyber benchmark.
3. Add one manually acquired offline CyberGym evaluator subset first; add E2E validation only when its offline inputs and build tools are complete; treat ExploitGym task families as separate admission profiles and exclude incompatible ones.
4. Evaluate Experiential in a separate optional local-only adapter experiment with telemetry disabled and no installed paid-provider credentials. Verify Ollama completion/tool/embedding protocol compatibility and Windows native packaging; retain direct Ollama if these gates fail.
5. Keep all sources optional to the core install. Nothing here delays the normal voice/text → Brain → Hermes → coding/research/computer/memory → reviewer → verifier flow.

The requested source map is now concrete and ready for approval. No implementation has started in this audit slice.
