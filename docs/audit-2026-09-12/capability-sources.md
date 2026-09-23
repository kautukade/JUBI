# Capability-provider source audit — 2026-09-12

This is an audit artifact, not an integration change. The user's source-to-pipeline approval gate remains closed. No source package, model, browser, container, native SARA service, QEMU guest, training job, or external MCP session was installed or launched during this audit.

## Scope and evidence limits

All files under the five assigned source trees were recursively enumerated, including hidden `.claude` content. Every Python file was read and parsed with `ast.parse`, without importing or executing it. Actual implementation paths were inspected for SARA, the five matching adapters, Second Brain's MCP and skill tooling, selected Awesome local RAG/memory/scraping and offline testing modules, AutoResearch's preparation/training scripts, Fable's dispatcher/capability/agenda/agent/network/build paths, and Jubi's Fable integration. This is a complete recursive inventory plus risk-focused code audit; it is **not** a claim that every line of 3,119 files or all bundled vendor C/JavaScript was manually reviewed. Syntax checks do not prove dependencies, model quality, or end-to-end operation.

Paths below use these exact repository-relative aliases; append the referenced filename and line to the alias:

| Alias | Source root |
|---|---|
| SARA | `sources/SARA-AI-Assistant-Local-AI-OS-v7.1.1-ROBUST-ONE-CLICK(4)/SARA-AI-Assistant-Local-AI-OS-v7.1.1-ROBUST-ONE-CLICK` |
| SB | `sources/second-brain-skills-main(3)/second-brain-skills-main` |
| AWESOME | `sources/awesome-llm-apps-main(2)/awesome-llm-apps-main` |
| FABLE | `sources/fable-os-main(3)/fable-os-main` |
| AUTO | `sources/autoresearch-master(3)/autoresearch-master` |

The applicable `FABLE/AGENTS.md` was read. It accurately calls out ring-0 execution, cooperative scheduling, intentionally writable/executable pages, no IOMMU, and unauthenticated TLS by default. These are facts about an experimental kernel, not suitable defaults for a Windows personal agent. Nested Awesome dashboard instructions apply only to that dashboard; no work was performed there.

## Recursive inventory

Counts include source assets, documentation, hidden files, vendor code, fixtures and logs present in the snapshot. There are no assumptions that a directory means a working service.

| Source | Files | Bytes | Principal file types | Entrypoints / test surface |
|---|---:|---:|---|---|
| SARA | 16 | 14,780 | 7 TS routes; 2 MD; 2 TXT; YAML/env/ignore/log | Seven Next.js route modules. No package manifest, test suite or runnable native agent in the snapshot. |
| Second Brain | 97 | 759,144 | 57 MD; 26 Python; 5 JSON; 3 TSX; 2 PPTX | Hidden `.claude/skills`: MCP client, skill creation/validation/packaging, slide cookbook, Remotion examples, SOP and branding patterns. No dedicated test suite. |
| Awesome LLM Apps | 1,802 | 86,348,917 | 527 Python; 270 Markdown case-insensitively; 204 TSX; 162 TXT; 93 TS; 91 JSON; 55 JS | Many independent examples, 14 TOML manifests and many app-specific requirements. Nineteen test/eval-named files found, including four offline agent-skill evals, Release Radar/HN unit tests, typed RAG, and cloud/browser/media examples. No unified production runtime. |
| Fable OS | 1,194 | 20,151,160 | 427 C; 456 H; 100 C#; 7 Python; assembly/build/vendor assets | `kernel/main.c`, `Makefile`; 16 tool-family C files; 39 `tests/host/test_*.c` suites; six QEMU `.case` fixtures. Vendor lwIP and mbedTLS are present; much of the file count is bundled third-party material. |
| AutoResearch | 10 | 765,126 | 2 Python; 2 MD; TOML/lock/notebook/image/ignore | `prepare.py`, `train.py`, experiment protocol `program.md`; no dedicated test suite. |

## Capability matrix

| Capability | Actual source implementation | Present Jubi integration | Classification | Decision |
|---|---|---|---|---|
| SARA computer/voice/vision runtime | Missing. TS routes proxy to absent server modules/local service. | HTTP wrapper expects `/health` and `/v7/command`; blocks computer/developer requests without configured runtime. | Broken/incomplete source bundle; optional adapter only. | Do not depend on SARA binaries or claim host-control availability. Extract interface/safety ideas only. |
| Second Brain knowledge/SOP skills | Structured prompt/reference assets; MCP and artifact scripts are real. | Generic source excerpt → model generation. | Catalogue/pattern provider, not memory engine. | Progressive skill loading; unify outcomes through Jubi Memory API. |
| Awesome local RAG | Actual Ollama/Qdrant/Agno and LangChain examples. | Generic source excerpt → model generation. | Usable examples, unintegrated runtimes. | Select individual algorithms only; disable cloud/download side effects and replace hardcoded model choices. |
| Awesome personal memory | Mem0 + Qdrant + Ollama example with Streamlit user/session state. | No invocation of that memory implementation. | Working-shaped example; not verified in this environment. | Borrow scoped retrieval patterns; do not add a second memory daemon/API. |
| Awesome deterministic coding helpers | Read-only Git archaeology and offline dependency checks; source evals exist. | No native helper binding in generic adapter. | Strong candidates for individual registered coding tools. | Put under canonical ECC/Superpowers coding loop, not a new orchestrator. |
| Fable kernel tools | Real C registry/dispatch, device VM, filesystem, capabilities, agenda and trace code. | Source reasoning adapter plus separate Fable lab facade. | Real experimental OS; not Windows computer control. | Reuse trace/budget/versioning ideas; isolate original kernel experimentation. |
| Fable learned capability store | Jubi SQLite prompt versions, hashes, enabled state, result counters. | Native Jubi code in `sarus/core/fable.py`. | Implemented but competing registry/state and incomplete permission binding. | Migrate into one registry and task/event store. |
| Fable agenda | Jubi SQLite bounded periodic/boot task runner. | Automatically starts on integration construction. | Implemented competing scheduler. | Merge semantics into Jubi scheduler; Hermes owns delegation/session work. |
| AutoResearch experiment harness | CUDA transformer pretraining, data preparation, timed training, BPB evaluation. | Prompt-only bounded-experiment proposal. | Real source; runtime unintegrated; GPU-required path. | Optional offline evaluation provider after explicit setup, never background production self-modification. |

## Findings and code evidence

### SARA: proxy fragments, not a supplied local AI OS

1. Every available route imports modules absent from this bundle: `SARA/app/api/agent/execute/route.ts:2` imports auth, `:3` imports `@/lib/server/localAgent`, and `:4` imports database helpers. The snapshot contains no `lib` tree, `package.json`, lockfile, `Dockerfile`, installer BAT, Python/native agent implementation, or executable runtime. `SARA/docker-compose.yml:3` says `build: .`; this cannot build as supplied. `SARA/0-RUN-THIS-FIRST.txt:5` tells the user to launch `RUN-SARA-ONE-CLICK.bat`, which is absent.
2. The plan endpoint checks admin/owner identity and proxies `/plan` (`SARA/app/api/agent/plan/route.ts:10`, `:13`, `:22`). Execute proxies `/execute` and writes an audit row (`SARA/app/api/agent/execute/route.ts:9`, `:15`). These are interface fragments, not Playwright, Windows UI Automation, PowerShell execution, STT, TTS or wake-word implementations.
3. `sarus/adapters/sara.py:18` only probes health when a token exists. `:32` sends an entire natural-language request to `/v7/command` with `auto_execute=True`. The endpoint contract cannot be verified from the supplied source. The local route fragments do not implement this endpoint. This bypasses any ability to review individual SARA tool actions inside Jubi unless the remote runtime supplies enforceable policy and evidence.
4. `sarus/adapters/sara.py:25` handles `live-research` by invoking Jubi's own research object; that is not evidence of SARA browser operation. The computer/developer runtime-required response is correctly blocked at `:37`. Keep that honest failure behavior while replacing the dependency with native Jubi computer workers.
5. `SARA/app/api/admin/stats/route.ts:22` exposes an OpenAI-configured flag. This shows a cloud configuration concept, not an implemented or mandatory paid provider in the incomplete snapshot. No top-level license file was found; do not assume redistribution permission for absent code or binaries.

### Second Brain: progressive knowledge skills, no memory backend

1. The six skill groups are SOP creator, skill creator, Remotion, MCP client, PPTX generator, and brand voice generator. None supplies a database, embedding index, episodic memory service, or task-state engine. The README's “second brain” label refers to knowledge-work skills. Use content/schema/progressive-loading patterns; do not report this as installed long-term memory.
2. Real MCP transport detection is implemented in `SB/.claude/skills/mcp-client/scripts/mcp_client.py:104`; connection logic at `:139` supports stdio, SSE, streamable HTTP and FastMCP. Tool schemas load on demand at `:250`, and explicit tool execution is at `:267`. This is a useful lazy-discovery pattern.
3. The MCP client inherits the entire host environment for subprocess servers at `:166`; there is no Jubi permission gate, consent policy, endpoint allowlist or worker environment filter here. HTTP `timeout` is read at `:201` but not passed in the following streamable HTTP call. Do not expose this as an unrestricted production connector. Example remote configuration includes credential-bearing services; select only approved free/local providers and route them through the common broker.
4. All 26 Python files were syntax-parsed. `SB/.claude/skills/pptx-generator/cookbook/carousels/quote-slide.py:88` has an unterminated triple-quoted string detected at line 140. This is a concrete broken source example. It was documented, not fixed before mapping approval.
5. `sarus/adapters/second_brain.py:5` marks `native` true solely when its directory exists, but inherited `sarus/adapters/base.py:11` only selects source text and calls `app.providers.generate`; the return at `:20` says `mode=model_reasoning`, `tools_executed=False`. Discovery status must distinguish indexed content, dependency-ready tools and verified executors.
6. A bundled `SB/.claude/skills/skill-creator/LICENSE.txt` contains Apache-2.0 text for that component. No top-level license was found for the entire snapshot. Component licenses cannot silently be applied to unrelated branding/assets/skills.

### Awesome LLM Apps: selective examples, not another agent framework

1. Real local candidates exist. `AWESOME/rag_tutorials/local_rag_agent/local_rag_agent.py:13` sets up localhost Qdrant and Ollama embeddings; `:25` immediately ingests a remote S3 PDF at module import; `:32` hardcodes `llama3.2`. It is therefore not offline merely because inference uses Ollama. Refactor any selected behavior behind installed-model routing and explicit user-approved ingestion before integration.
2. `AWESOME/advanced_llm_apps/llm_apps_with_memory_tutorials/local_chatgpt_with_memory/local_chatgpt_memory.py:17` configures Ollama and `:26` local embeddings; `:59` constructs Mem0; `:88` stores user text; `:91` loads all user memories; `:134` stores assistant output without independent verification. It adds Qdrant, Mem0, LiteLLM and Streamlit state and uses hardcoded model IDs. Borrow scoped memory access while keeping one Jubi memory API, verified outcome provenance, retrieval limits, and installed-model selection.
3. `AWESOME/rag_tutorials/qwen_local_rag/qwen_local_rag_agent.py` implements PDF/web ingestion, chunking, relevance thresholds and local Qdrant. It also imports ExaTools (`:15`), offers a fixed model menu and optional API-key web fallback. This is not hardware detection or dynamic installed-model routing. Select ingestion/retrieval routines; do not import its service/UI/orchestrator stack wholesale.
4. `AWESOME/starter_ai_agents/web_scraping_ai_agent/local_ai_scrapper.py:10` uses local Ollama generation/embedding via ScrapeGraphAI; `:33` runs the scraper on button click. It still accesses the requested website and does not replace an approved, evidence-producing Windows computer worker.
5. `AWESOME/agent_skills/commit-archaeologist/scripts/archaeologist.py:33` uses bounded read-only local Git calls with 30-second timeouts and validates repository containment at `:58`. `AWESOME/agent_skills/dependency-doctor/scripts/dep_doctor.py:278` computes offline findings; `:376` places PyPI checks behind explicit `--online`. These narrowly scoped tools fit an existing coding worker well.
6. `AWESOME/always_on_agents/release_radar_agent/ranker.py:75` classifies release signals, while `radar.py` separates deterministic fixtures from live GitHub access. Its six original ranker tests passed offline. Keep its algorithms as potential tools; do not start its independent scheduler/delivery pipeline or treat fixture results as live research.
7. The collection also contains cloud-agent, voice, browser and multi-agent examples with API-key/external dependencies. Those are not approved integrations. `sarus/adapters/awesome_llm_apps.py:5` repeats the directory-exists-as-native bug; actual execution inherits prompt-only `sarus/adapters/base.py:11`. Swarms/teams in examples must not compete with Hermes, and Streamlit/Agno/session state must not compete with Jubi's canonical state.
8. `AWESOME/LICENSE:1` contains Apache License 2.0. Nested application licenses and dependency/model/data terms remain independent. All 527 Python files parsed; two invalid-escape SyntaxWarnings were emitted by `advanced_ai_agents/multi_agent_apps/ai_financial_coach_agent/ai_financial_coach_agent.py:533` and `:574`. No dependency installations or external app demos were run.

### Fable: useful evidence design with an incompatible runtime boundary

1. This is real kernel code. `FABLE/core/tool.c:47` enumerates linker-registered tools; `:90` validates tool descriptors; `:272` records emitted-trace count around `:275` dispatch; `:314` ensures an action produces a trace. The trace/prose distinction and schema validation are useful verifier patterns.
2. `FABLE/core/capability.c:53` and `:54` separate capability persistence from capability data paths. It implements bounded capability records, validation and program execution. `:170` explicitly describes FNV-1a as noncryptographic. `FABLE/core/agenda.c` implements boot/every/once scheduled work; `FABLE/tools/agent_tools.c:29` explicitly says its model-authored plan is a checklist, not an execution script. These are not a replacement for Hermes child agents or Jubi's task state.
3. Live networking defaults to Anthropic: `FABLE/net/net.c:158` sets `api.anthropic.com`, `:177` hardcodes a Claude model, and `FABLE/net/model.c:13` has the same model default. `FABLE/Makefile:229` adds a QEMU user-mode NIC. `FABLE/port/lwipopts.h:111` selects certificate-verification-none without the build flag. The default live kernel conflicts with no-paid-API/offline requirements. Use host-only scripted tests or an explicitly isolated offline guest; never supply a paid key.
4. `sarus/core/fable.py:79` records trusted execution receipts, whereas `:83` imports serial lines only as candidates/prose. This is a useful existing integration. However, a successful process exit or task-status field is not semantic verification of the user's outcome.
5. `sarus/core/fable.py:118` creates a separate learned-capability store. Saved prompt versions start enabled/untested (`:174`), retain permissions as metadata (`:162`), and `run_capability` at `:580` never evaluates those permissions before `:586` invokes the general execution engine. Success is inferred from `task.status == completed` at `:587`. The common execution policy may still apply, but capability-specific permission claims are not enforced here. Migrate declarations/results into the canonical registry and require review plus verifier evidence before promotion.
6. `sarus/core/fable.py:437` implements another SQLite agenda with max-items, period, total-run and consecutive-failure limits. At `:577` construction binds it to capability execution, and `:578` immediately starts it, including boot work. This is competing autonomous task ownership. Preserve useful budgets as Jubi scheduler policies; migrate agenda rows to canonical scheduled-task events. `tick` and `run_boot` have distinct counter checks; toggles/adds do not establish transactional single-claim semantics across processes.
7. `sarus/core/fable.py:227` is a real fixed-target QEMU/Make lab wrapper, but not a network sandbox. `:316` reports runtime readiness from a source directory plus WSL executable/native make, without proving a configured WSL distribution, QEMU, compiler or complete toolchain. `:374` launches `make run-nox`, inheriting the source Makefile's network behavior. No network-deny, CPU/RAM/PID/disk quota or kernel process containment is enforced by this wrapper. Do not reuse it unchanged for the required Cyber Lab Manager.
8. No top-level Fable license file was found in this snapshot. Vendored lwIP includes `FABLE/lwip/COPYING`; mbedTLS headers carry their own license notices. Those notices do not establish a license for first-party kernel code. Verification patterns can inform Jubi design; direct redistribution/code reuse needs explicit source-license resolution.

### AutoResearch: measurable CUDA experiments, not general autonomous improvement

1. `AUTO/prepare.py:31` sets a 300-second training budget, and `:41` hardcodes the Hugging Face ClimbMix dataset URL. The download path is at `:57` and the CLI calls it at `:383`. Default preparation requests ten training shards plus pinned validation data. Running preparation would violate the user's manual-download gate without a separate explicit approval.
2. `AUTO/train.py:21` queries CUDA immediately; `:23` chooses a FlashAttention kernel repository and `:24` calls `get_kernel` at import time. `:461` selects CUDA; `:508` compiles the model. This is not a CPU-compatible installed-Ollama tool. The adapter's `gpu=optional` at `sarus/adapters/autoresearch.py:5` describes aspiration/proposal mode, not the real upstream training path.
3. `AUTO/prepare.py:344` contains the BPB evaluation routine, and `AUTO/train.py:613` runs it after timed training. Measurable metrics, immutable baselines, trial budgets and explicit promotion are valuable patterns. A five-minute training loop is not a cap on preparation/download, JIT compile, validation time, disk or VRAM.
4. `AUTO/pyproject.toml:8` requires PyTorch, kernels, NumPy, PyArrow, tokenizer libraries and others; `:26` selects the CUDA 12.8 wheel index. These dependencies were not installed. `sarus/adapters/autoresearch.py:7` generates only an isolated-experiment proposal, correctly marking `proposal-only-until-accepted`; no training execution exists in that adapter.
5. Both Python files parsed successfully. Training and preparation were not imported or launched because of their top-level CUDA/kernel/download requirements. No dedicated tests or top-level license file were found. Do not infer a snapshot redistribution license from repository popularity or its name.

## Test evidence and limits

Runtime used: `C:/Users/hp/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe`. This runtime lacks `pytest`, `yaml`, `streamlit`, `agno`, `torch`, `mcp`, and `fastmcp`; no packages were installed. `make`, `gcc`, and `clang` were not found on the tested PATH. No WSL host compiler, QEMU guest, browser, model service or GPU training job was invoked.

| Check | Result | What it establishes |
|---|---|---|
| AST parse of every Python source in five assigned roots | 561/562 passed; one Second Brain quote-slide syntax failure | Source parses in the bundled runtime only; no import/dependency/runtime claim. |
| Fable original `tests/qemu/lint_printf.py --all --json` | Exit 0; 136 first-party C/H files; zero findings | Current source format strings fit the kernel formatter lint. Despite directory name this invocation does not start QEMU. |
| Awesome original Release Radar `tests/unit/test_ranker.py` | Six test functions directly invoked, six passed | Deterministic classification/ranking only. Invoked without pytest; socket connect calls were denied in the audit process and no network occurred. |
| Second Brain actual MCP `detect_transport` | Four audit smoke assertions passed | Correct stdio, SSE, HTTP and FastMCP classification. No config credentials were read and no server session opened. |
| Fable 39 host suites / six QEMU cases | Not run | Native compiler/make absent on PATH; QEMU and host operations outside the approved audit test scope. |
| Other Awesome cloud/browser/media/pytest examples | Not run | Missing dependencies and possible API/browser/service effects; syntax-only coverage is stated above. |
| SARA runtime/build | Not run | Required implementation/build inputs absent. |
| AutoResearch | Not run beyond AST parsing | CUDA, third-party kernels and dataset requirements; manual-download gate remains closed. |

Original passing ranker functions: `test_routine_patch_release_is_filtered`, `test_breaking_deprecation_and_security_signals_are_classified`, `test_major_version_is_relevant_without_keywords`, `test_yanked_release_signal_is_not_lost`, `test_ranker_prioritizes_security_and_breaking_releases`, and `test_equal_priority_releases_put_the_newest_first`.

## Short source → canonical pipeline map for approval

| Source | Canonical insertion point | Exact allowed contribution | Excluded contribution |
|---|---|---|---|
| SARA | Specialist computer worker design | Proxy-contract/owner-check ideas, subject to revalidation | Missing binary/service dependency; opaque auto-execution; a second planner/runtime |
| Second Brain | Lazy catalogue → specialist memory/knowledge worker → Memory API | SOP/knowledge organization and on-demand schema/reference loading; individually vetted artifact helpers | A claimed memory database; uncontrolled MCP subprocesses/cloud connectors |
| Awesome LLM Apps | Lazy capability registry → selected coding/research/memory tools | Specific offline Git/dependency helpers, local retrieval/chunking patterns, scoped memory patterns | Bundled app/framework installation, extra swarm/state, automatic cloud/data access |
| Fable OS | Reviewer/verifier + registry/versioning + scheduler policy | Action/prose separation, receipts, bounded work, learned-capability versioning; separate optional offline OS lab | Kernel as Windows agent, paid live model transport, duplicate agenda/registry, network-enabled lab defaults |
| AutoResearch | Optional evaluation worker → reviewer → verifier → explicit promotion | Frozen benchmark, trial budget, metric comparison and reproducibility patterns | Automatic GPU training, Hugging Face downloads, importing top-level training code, production self-modification |

These providers sit under `User request → Jubi Brain planner/model router → Hermes delegation → specialist workers → reviewer → verifier → final answer`. They add capabilities, never alternative swarm ownership. Jubi must own a single registry, a canonical task/event API and a unified Memory API; Hermes remains the delegation/session implementation. Hardware admission, installed-Ollama role assignment, permissions, resource budgets, evidence and cancellation belong to the Jubi wrapper.

## Integration work after approval

1. Make provider status honest: distinguish absent, indexed, blocked, dependency-ready, executable and verified; stop inferring native execution from a directory.
2. Register a small first tranche of offline tools, with pinned source identity, license evidence, schemas, permissions, dependency probes, bounded resources and verifier contracts. Keep all source text untrusted and load it only when selected.
3. Replace SARA runtime reliance with Jubi's real typed Playwright/UI Automation/PowerShell worker boundary; return blocked when the target operation lacks an implementation or verifier.
4. Consolidate Fable prompt versions/agenda/results and selected memory patterns behind the canonical registry/task/event/memory APIs. Do not silently start migrated scheduled work.
5. Keep source experiments isolated. AutoResearch requires manual approved assets and measured hardware admission; Fable guests require genuine network isolation; cyber benchmark repos require the separate stricter manual-start Cyber Lab Manager specified by the user.
6. Run integration tests against actual local tools and installed models only after approved implementation exists. Require tool receipts and postconditions before claiming execution, memory success, model readiness, host control, or a completed user outcome.
