# Swarm and coding workflow source audit — 2026-09-12

Status: **audit/design only; integration requires the user's approval of the source-to-pipeline map.** No product implementation was changed for this report.

## Scope and method

This report covers the following recursively enumerated source trees. Root-relative path abbreviations used in evidence below are:

| Abbreviation | Root relative to Jubi | All files, including hidden/ignored | Dominant extensions |
|---|---|---:|---|
| H | `sources/hermes-agent-main(3)/hermes-agent-main` | 8,515 | 3,852 `.py`, 1,475 `.md`, 1,371 `.ts`, 637 `.tsx` |
| E | `sources/ECC-main(20260807-042951)/ECC-main` | 3,437 | 2,507 `.md`, 480 `.js`, 112 `.json`, 63 `.py` |
| S | `sources/superpowers-main(3)/superpowers-main` | 180 | 89 `.md`, 38 `.sh`, 13 `.json`, 10 `.js` |
| A | `sources/agency-agents-main(3)/agency-agents-main` | 343 | 316 `.md`, 9 `.sh`, 7 `.yml`, 2 `.py` |

Total: **12,475 files**. Counts use `rg --files --hidden --no-ignore`, not README claims. Extension counts are file inventory, not executable capability counts. In particular, Hermes has contributor address marker files whose suffixes resemble domains; those are not executables. The full repository inventory is maintained by the main audit rather than duplicated here.

Read the Hermes and ECC root `AGENTS.md` files and Superpowers `AGENTS.md` → `CLAUDE.md`. Manually inspected actual delegation, provider, session, Kanban, plugin, adapter, LLM executor, state-store, bootstrap, catalog and test implementations, plus representative coding and persona definitions. This is recursive structural coverage and focused semantic review of integration-critical paths; it is **not** a claim that every line of all 12,475 files was manually reviewed. Desktop-specific Hermes implementation was not audited deeply; no desktop readiness claim is made. Translated ECC documentation is inventoried, not a separate runtime. No dependencies were installed and no model, paid API, external agent, or model download was invoked.

## Capability matrix

| Capability | Upstream implementation | Jubi state in this checkout | Disposition |
|---|---|---|---|
| Hermes child agents | Real `AIAgent` child construction and execution | Prompt retrieval only | Adopt Hermes runtime through a bounded adapter |
| Hermes parallel delegation | Daemon executor fan-out and result aggregation | No Hermes invocation in adapter | Reuse; wrap hardware scheduling |
| Hermes nested orchestration | Role and depth gates implemented; flat by default | Not connected | Explicit bounded opt-in profile configuration |
| Hermes sessions | SQLite session/message history, parent session linkage | Separate Jubi state exists | Preserve transcript store; one authoritative task-state API |
| Hermes durable work queue | SQLite Kanban claims, dependencies, worker processes, recovery | Not connected | Canonical durable task backend; no replacement swarm |
| Local Ollama inference in Hermes | OpenAI-compatible custom endpoint, placeholder SDK key | Jubi has its own provider invocation; not Hermes delegation | Jubi selects installed model; pass pinned local endpoint/model |
| ECC coding workflows | Agent/skill definitions, executable CLI, hooks, state and utilities | One selected text fragment sent to Jubi model | Reuse specialist/check definitions selectively |
| ECC independent runtime | Python `ReActAgent`, provider resolver, Node control pane, tmux orchestration, state store | Not invoked by ECC adapter | Exclude from canonical execution path |
| Superpowers coding loop | Skills plus real harness bootstrap and helper scripts | Coding prompt fragment only | Canonical workflow spine, using Hermes execution |
| Agency personas | Markdown catalog with generated Hermes lazy-router plugin | Selected text fragment only | Reuse search/inspect/load behind capability registry |
| Agency delegation helper | Calls Hermes registry; has response validation/context gaps | Not integrated | Repair/normalize only after approval; do not trust its success flag |
| Review and verification | Useful workflow definitions | Model reasoning is not independent execution evidence | Review changes; separately verify real artifacts/test outcomes |

## Current Jubi integration: what actually runs

All four adapters subclass `PromptCatalogAdapter`, which is an empty subclass of `SourceAdapter`:

- `sarus/adapters/base.py:11` begins capability selection; line 15 constructs a prompt with at most 18,000 characters of source text; line 16 adds at most 8,000 characters of prior context; line 17 calls `app.providers.generate`.
- `sarus/adapters/base.py:20` returns `mode='model_reasoning'` and `tools_executed=False`. This is useful evidence that the implementation itself knows these are reasoning steps.
- `sarus/adapters/hermes.py:3`, `ecc.py:2`, `superpowers.py:2`, and `agency_agents.py:2` provide metadata and probes. None overrides `execute` to invoke Hermes, ECC workflows, Superpowers bootstrap, or Agency's router.
- Hermes' probe checks a PATH CLI, whereas `sarus/core/native.py:8` checks a different expected executable under `native/hermes`. ECC's adapter probe labels an existing source directory `native=True`; native status additionally requires Node and source `node_modules` at `sarus/core/native.py:13`. Agency/Superpowers readiness is unconditionally true at lines 14 and 17. These are different readiness meanings, not interchangeable proof of working execution.
- `sarus/core/orchestrator.py:20` always adds a Hermes-labelled planner, but the plan is assembled by keyword checks at lines 21–28. It is not a Hermes swarm. Coding instructions, two ECC reviews, SARA execution and final ECC verification are listed in sequence; labels alone do not prove any action or verification occurred.

**Finding:** source presence and generic local generation are implemented. Runtime integration, workflow auto-triggering, tool execution, and verified swarm outcomes for these four sources are absent in the reviewed adapters. The display/API should ultimately distinguish `indexed`, `loadable`, `runtime_available`, `configured`, `tested`, and `enabled`, rather than treating a directory or a prose result as “native ready.”

## Hermes: precise reusable APIs and limitations

### Execution and scheduling

- Public Python entry point: `tools.delegate_tool.delegate_task(goal=None, context=None, tasks=None, max_iterations=None, role=None, background=None, parent_agent=None) -> str`, at **H/tools/delegate_tool.py:2977**. Requires a real parent agent. Single and batch forms return JSON; it validates goals and rejects a batch larger than the configured child count.
- `_build_child_agent` at **H/tools/delegate_tool.py:1304** constructs a real `run_agent.AIAgent`; construction at line 1616 passes local/model/provider overrides, child tool restrictions, parent `session_db`, and `parent_session_id`.
- Parallel fan-out uses `DaemonThreadPoolExecutor(max_workers=max_children)` at **H/tools/delegate_tool.py:3217**, copies context into each worker, polls futures, and propagates interruption. Reuse these capabilities rather than writing another swarm.
- Actual public delegation no longer accepts a `toolsets` or model argument. Child construction at **H/tools/delegate_tool.py:3157** uses `toolsets=None` and credentials from trusted delegation config. Comments say tools are inherited and are not model-selectable. `_build_child_agent` has more powerful private arguments, but they are not a stable public contract for the Jubi wrapper.
- `role='leaf'` is the default. Orchestrator role is degraded to leaf unless enabled and `child_depth < max_spawn_depth`, **H/tools/delegate_tool.py:1346**. Exact disabled toolsets are passed to the child, including Kanban, at line 1415; a role-authorized orchestrator regains delegation at line 1426. Do not let ordinary child tools mutate the durable board directly.
- The model-facing registry path always backgrounds top-level delegation but keeps nested orchestration synchronous, **H/tools/delegate_tool.py:4078**. Direct Python callers retain a synchronous default. Background work is process-local, not a durable queue; use Kanban for restart-surviving execution.
- Runtime control includes `set_spawn_paused`, `interrupt_subagent`, `steer_subagent`, and `list_active_subagents`, **H/tools/delegate_tool.py:154,211,235,301**. These are useful hooks for an operator stop button and scheduler integration, but need contract tests under the pinned source version.

### Actual limits differ from documentation

The checked-in **H/hermes_cli/config_defaults.py:1682–1727** and runtime code are authoritative:

| Setting | Actual default/behavior | Jubi implication |
|---|---|---|
| `max_iterations` | 50 per child | Each child gets a fresh budget, not a shared global total |
| `child_timeout_seconds` | 0, no timeout; positive timeout has a 30-second floor | Set an explicit bounded timeout for local hardware |
| `max_concurrent_children` | 3; no upper ceiling | Apply a hardware-derived ceiling before delegating |
| `max_spawn_depth` | **1**, flat | Explicitly set 2 only when nested work is useful and feasible |
| `orchestrator_enabled` | true, still depth-gated | A true flag alone does not enable grandchildren |
| `subagent_auto_approve` | false | Preserve; route required approval through Jubi UI |
| `inherit_mcp_toolsets` | true | Pin allowed tools; optional external capabilities must not leak |

The Hermes guide claims depth 2 and shared iteration budgets, but **H/tools/delegate_tool.py:1654** sets `iteration_budget=None` and **H/hermes_cli/config_defaults.py:1717** sets depth 1. Nested branches and background units can multiply real resource consumption: a per-batch child count is not a machine-wide CPU/RAM/VRAM budget. This is exactly the scheduler gap Jubi should fill. Do not tune process-global config independently from concurrent conversations; prefer admitted profile/session-bound jobs and a single scheduling authority.

### Sessions, registry and Kanban

- **H/hermes_state.py:2092** defines `SessionDB`; public operations include `create_session` at 3304, `end_session` at 3797, `get_session` at 5456 and `append_message` at 6513. Parent IDs relate child/transcript sessions. This is a conversation store, not a reason to duplicate task lifecycle state.
- **H/tools/registry.py:521,676,760** implement registration, schema selection and dispatch; built-in discovery inspects AST registration calls before importing provider files at lines 30–67. Jubi's single capability registry should expose a manifest/index over these native capabilities and route execution to this existing dispatch surface, not copy every Hermes tool into a second executor.
- **H/hermes_cli/plugins.py:413** exposes plugin `register_tool`; `dispatch_tool` at 607 forwards through registry and injects a parent only from CLI context at 628–632. A gateway integration must supply proper session authority rather than assume the CLI reference exists.
- **H/hermes_cli/kanban_db.py:102** defines durable task states: `triage`, `todo`, `scheduled`, `ready`, `running`, `blocked`, `review`, `done`, `archived`. Reuse these through one Jubi task API. “Reviewing/verifying” can be a stage/subtask within `review`, not another independent status database.
- Database operations: `connect` at 2149, `create_task` at 2881, `claim_task` at 4226, `heartbeat_claim` at 4423, `complete_task` at 4836, `block_task` at 5618. Write transactions use `BEGIN IMMEDIATE` plus status/claim-lock comparisons. Boards partition DBs and worker workspaces.
- `dispatch_once` at **H/hermes_cli/kanban_db.py:8204** accepts `spawn_fn`, `dry_run`, `max_spawn`, `max_in_progress`, `max_in_progress_per_profile`, timeout and failure limits. These are concrete scheduler extension points. Its worker path at 9105 adds per-task skills and model/provider overrides; at 9155 it starts actual Hermes workers with an argv list and Windows `CREATE_NO_WINDOW`.
- A board, profile or worker directory is organizational isolation, **not a security sandbox**. These facilities must not host or authorize Cyber Lab execution as normal computer control.

### Required completion gate: worker output is not task completion

The canonical task API must prevent workers from bypassing the reviewer/verifier:

- **H/hermes_cli/kanban_db.py:4836** `complete_task` accepts result/summary/metadata, optional created-card validation, and an optional `expected_run_id`. Its SQL at line 4917 changes **running/ready/blocked directly to done**. The run ID is a stale-run guard, not verifier authorization. Metadata describing tests is not checked execution evidence.
- **H/tools/kanban_tools.py:727** has a goal-mode judge gate, but only when the judge is available. Exceptions explicitly fail open, preserving `verdict='done'`. **H/hermes_cli/kanban.py:2176** provides a similar CLI gate. This cannot be the hard Jubi verification boundary.
- **H/hermes_cli/kanban_db.py:4348** `claim_review_task` atomically moves `review → running` and creates a separate run. The dispatcher uses it at 8623, then **forces `claimed.skills=['sdlc-review']` at 8650**, a PR-oriented review/merge workflow. Placing a generic Jubi job in `review` does not automatically run Jubi's artifact verifier. Active review can be projected as a Jubi stage while Hermes holds its legitimate `running` claim.
- `complete_task` does not directly accept `review → done`; a verifier run must claim the review job before trusted completion. No public submit-review helper was found in this module. Normalize the worker-to-review handoff through a supported transition path or a narrow provider extension with transaction/run checks; do not invent a second task database.

Proposed boundary: the trusted Jubi Task API owns Kanban mutation and board access. Workers submit artifacts/evidence to a handoff capability, then the trusted adapter moves the task to review and admits the reviewer/verifier. Only a trusted verifier acceptance record authorizes `complete_task(expected_run_id=...)`. The evidence/consent ledger is append-only supporting data, not a competing task-state authority. Generic verification requires adapting the dispatcher/spawn policy so the forced PR-review skill is not silently used for every task.

Hiding `kanban_complete` from tool schemas is insufficient if a worker has arbitrary same-user shell access to the board database or CLI. Enforce the boundary at process/filesystem permissions and the structured mutation broker as well as tool exposure; use restricted workspaces/identities or similarly effective isolation for untrusted workers. Test direct tool, CLI, stale-run and database-write bypass attempts, cancellation and missing verifier evidence. This is a completion/policy wrapper around Hermes, not a rebuilt swarm.

### Local-first and Windows feasibility

**H/hermes_cli/runtime_provider.py:1198** resolves local aliases such as `ollama` to `custom`; configured endpoint resolution follows at 1214. Lines 1292–1311 allow an SDK placeholder `api_key='no-key-required'` for custom unauthenticated servers. A local endpoint such as `http://127.0.0.1:11434/v1` therefore needs no paid account or rotating secret. This is API compatibility evidence, not a live inference pass in this audit.

Jubi should discover installed Ollama models, capability-test tool/structured-output behavior, then pin planner/coder/research/review/verifier role assignments. Pin auxiliary model calls too, disable automatic external fallbacks, and keep online provider plugins disabled unless explicitly opted into a free service. **H/plugins/model-providers/ollama-cloud/__init__.py:81–86** is a separate remote `https://ollama.com/v1` provider; it is not local Ollama and should not be confused with the local alias.

**H/pyproject.toml:15** requires Python `>=3.11,<3.14`; lines 112–117 declare Windows `pywinpty` and `pywin32`; line 359 exposes `hermes = hermes_cli.main:main`. Kanban includes native Windows process flags. However **H/hermes_cli/pty_bridge.py:130–138** still explicitly rejects unavailable PTY spawning on Windows with a WSL-only message. Do not claim every Hermes frontend is native-Windows-ready from the presence of Windows dependencies. The Python worker/runtime integration is a smaller initial target than bundling Hermes' TUI, web app and Electron app.

## ECC: reusable workflow content, competing runtimes excluded

ECC is more than a README collection, but it is not the canonical swarm here:

- **E/scripts/ecc.js:8** is a real command dispatcher with install/catalog/consult/control-pane/memory/state/session commands. **E/scripts/catalog.js** selects install components and profiles; its executable catalog tests pass (below).
- **E/package.json:466–482** declares `@iarna/toml`, `ajv`, `sql.js`, development tooling and Node `>=18`. Jubi's native check requiring `node_modules` does not prove a meaningful Hermes-connected coding runtime.
- **E/src/llm/tools/executor.py:15,37,65** contain `ToolRegistry`, `ToolExecutor` and an independent `ReActAgent`. **E/src/llm/providers/ollama.py:17** is an actual urllib Ollama provider, but its catalog is hardcoded (`llama3.2`, `mistral`, `codellama`); `generate` at line 54 does not include provided tool definitions in the chat payload. This is not suitable as Jubi's dynamic installed-model router.
- **E/pyproject.toml** identifies this Python package as an alpha provider abstraction and requires both Anthropic and OpenAI SDKs. No reason exists to install a second provider/executor stack merely to reuse ECC review knowledge.
- **E/scripts/lib/state-store/index.js:7,13–24** imports `sql.js` and defaults to a separate `.claude/ecc/state.db`. **E/scripts/lib/memory-vault.js** implements another durable-memory surface; **E/scripts/lib/tmux-worktree-orchestrator.js** is another shell/tmux worktree orchestrator. Keep all three outside the canonical Jubi runtime to avoid competing states, memory and swarm logic.
- **E/scripts/lib/agent-proximity/** computes path/dependency collision advisories with real pure functions. It is a useful optional scheduler advisory (19 behavior tests pass), not a lock manager or proof edits cannot collide.
- **E/agents/code-reviewer.md:3–5** describes a review persona and specifies `model: sonnet` plus harness-specific tool names. Jubi must translate these metadata choices to local model roles and real local tools, not require an Anthropic subscription. The review checklist supplies useful exact-line/failure-mode standards; it does not execute review or tests by itself.

## Superpowers: choose one development loop

Use Superpowers as the canonical process spine: **clarify/spec → concrete plan → isolated work area → red/green/refactor per change → review → actual verification → final evidence**. Load only the applicable workflow and ECC domain reviewer/check details. For a bug, systematic debugging precedes the failing regression test.

Evidence:

- **S/skills/test-driven-development/SKILL.md:47** defines the red/green/refactor sequence with observed RED and GREEN gates.
- **S/skills/subagent-driven-development/SKILL.md** defines bounded implementation/review rounds, task briefs, review packages and progress recovery. Its helpers under `scripts/sdd-workspace`, `task-brief` and `review-package` are real shell programs. The file-ledger design overlaps task state; treat generated briefs/ledger as derived artifacts of canonical task records, not a second scheduler authority.
- **S/skills/verification-before-completion/SKILL.md:16–37** requires fresh command evidence. **E/skills/verification-loop/SKILL.md** contributes build/type/lint/test/security/diff check categories; its sample commands need translation to the target project/Windows environment. Do not run a second independent ECC implementation loop.
- **S/hooks/session-start:10** reads the bootstrap skill and returns harness context JSON. **S/.opencode/plugins/superpowers.js:49** caches bootstrap content and later injects it while registering the skill directory. The Pi extension likewise implements lifecycle injection. These are harness bootstrap adapters, not model providers or swarm engines.
- Merely copying a `SKILL.md` into Jubi or calling a model with one arbitrary text fragment does not bootstrap the workflow. A Jubi/Hermes skill loader must intentionally resolve relevant workflow stages and translate harness tools to actual Hermes/Jubi capabilities.

**Canonical split:** Superpowers owns workflow ordering; ECC supplies focused language/domain reviewers and applicable verification checklists; Hermes owns delegation/session/work queue execution; Jubi owns model/hardware routing, user authority and evidence acceptance. Agency supplies specialist descriptions where ECC has no appropriate role. Do not simultaneously enable ECC's ReActAgent/tmux orchestrator, Superpowers-as-another-controller, and Jubi's present keyword pipeline.

## Agency Agents: progressive loading implementation and integration defect

The catalog is primarily Markdown personas; for example **A/engineering/engineering-software-architect.md:10–18** describes identity, experience and “memory” in prose. That text does not implement persistent memory or software actions.

There is already useful integration code:

- **A/scripts/build-hermes-plugin.py:1–6** explicitly avoids advertising all personas in initial Hermes context. `collect_agents` at line 70 derives the roster from `divisions.json`, reads individual definitions and rejects duplicate slugs.
- Its generated router keeps catalog data on disk and returns concise search results. It exposes `agency_agents_search`, `agency_agents_inspect`, `agency_agents_load`, `agency_agents_delegate`, with a bounded search limit. This can feed Jubi's single capability registry using source-qualified IDs while loading full content for only the selected worker. The whole JSON is cached in process memory on first search, but the whole roster is not injected into model context.
- **A/scripts/check-hermes-plugin.py:32** builds in a temporary directory and checks schemas plus search/inspect behavior. This audit ran it successfully. It does **not** test real delegation, local model capability, execution, or verification.
- **A/scripts/build-hermes-plugin.py:366–379** forwards `toolsets` to `ctx.dispatch_tool('delegate_task', ...)`, then returns `success=True, delegated=True` for any returned value; only thrown exceptions trigger fallback. Current Hermes' public handler ignores this toolset field. Hermes often returns JSON tool errors instead of throwing, and the plugin's gateway context may lack the required parent. Consequently `delegated=True` is not proof a child exists. A compatibility adapter must preserve errors, require a real child/delegation identifier and bind session/tool authority. Until then reuse search/inspect/load, and invoke the canonical delegation API from Jubi's trusted adapter.

## Licenses

All four checked-in source roots carry the **MIT License**:

- **H/LICENSE:1–10** — Nous Research, 2025.
- **E/LICENSE:1–10** and **E/package.json:33** — Affaan Mustafa, 2026; MIT (also E's Python package metadata).
- **S/LICENSE:1–10** — Jesse Vincent, 2025.
- **A/LICENSE:1–10** — AgentLand Contributors, 2025.

Preserve notices when distributing reused code/content. This establishes source-root licenses; optional transitive packages, bundled assets and model weights need their own distribution inventory. MIT source licensing does not make remote model APIs free, nor prove a downloaded model's license is suitable. No source needs a subscription merely to reuse its local code/workflow definitions.

## Offline validation performed

Tests were selected after reading test/implementation paths to avoid installers, external agents and network execution.

| Source | Exact command from source root | Outcome | What it proves |
|---|---|---|---|
| ECC | `node tests/lib/agent-proximity.test.js` | **19 passed, 0 failed; exit 0** | Pure distance, dependency, overlap and advisory behavior |
| ECC | `node tests/scripts/catalog.test.js` | **7 passed, 0 failed; exit 0** | Actual catalog CLI selections and error handling |
| Superpowers | `node tests/brainstorm-server/ws-protocol.test.js` | **32 passed, 0 failed; exit 0** | Local WebSocket encoding/decoding functions; server was not started |
| Superpowers | `node tests/opencode/test-bootstrap-caching.mjs <absolute-path-to-.opencode/plugins/superpowers.js> present` | **exit 0** | Bootstrap injected once into each fresh transcript; cached file read remains 1; current tool mappings present |
| Agency | `<bundled-python> -B scripts/check-hermes-plugin.py` | **PASSED; exit 0** | Generated plugin schema and search/inspect contract |
| Hermes | Git Bash `scripts/run_tests.sh tests/tools/test_delegate.py -q` | **blocked before collection; exit 1** | No virtualenv with pytest; no Hermes tests executed |

Runtime paths: Node `C:/Program Files/nodejs/node.exe`; Python `C:/Users/hp/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe`; Git Bash `C:/Program Files/Git/bin/bash.exe`. First Hermes attempt also lacked Git `dirname` on PATH; rerunning with Git `/usr/bin:/bin` restored found the actual blocker: no supported pytest environment. The required Hermes wrapper was honored; no direct pytest bypass or installation occurred.

The Hermes tree includes substantive tests such as `tests/tools/test_delegate.py`, `test_delegate_toolset_scope.py`, `test_delegate_kanban_isolation.py`, `tests/hermes_cli/test_kanban_db.py`, and `tests/test_hermes_state.py`. They remain **unrun** here. ECC's full suite requires missing dependencies and includes wider integrations; Superpowers' skill-evaluation suites can invoke external harnesses/models and were intentionally not run. These passing focused tests do not establish production readiness or local Windows end-to-end success.

## Short source-to-pipeline map for approval

| Source | Exact position in canonical flow | Reuse | Exclude / boundary |
|---|---|---|---|
| Hermes | **Brain → swarm → specialist sessions → durable queue** | `AIAgent`, `delegate_task`, native registry/plugins, SessionDB, Kanban | No second swarm; pinned local inference; Jubi admission/verification wrapper |
| Superpowers | **Coding worker's workflow → reviewer → verifier** | Selected planning/debugging/TDD/review/verification skills and useful helpers | No separate orchestrator or competing task ledger authority |
| ECC | **Coding/review specialist knowledge and verification criteria** | Selected agents, domain skills/checklists, optional pure collision advisory | Exclude ReAct runtime, tmux swarm, control-pane state DB and separate memory service |
| Agency Agents | **Registry discovery → selected specialist worker** | Generated lazy search/inspect/load catalog; local-model role translation | No bulk prompt loading; do not trust unvalidated delegate-success wrapper |

Proposed flow for these providers: user request → **Jubi Brain/local model router** → **Jubi hardware admission around Hermes** → **selected Hermes worker with Superpowers/ECC/Agency content** → **independent reviewer** → **verifier of real tool/artifact evidence** → final answer. Memory writes go through the one shared memory API designed by the main audit.

## Integration sequence after mapping approval

1. Pin source snapshots and expose truthful registry statuses. Add provider manifests with entrypoint, license, runtime, platform, local/external policy, role, tool requirements and test evidence.
2. Start a minimal isolated Hermes local profile/runtime; pin the Ollama loopback endpoint, installed model and auxiliary model roles; explicitly disable paid/external fallbacks. Validate an actual local tool round trip.
3. Bind Jubi task IDs to canonical Kanban records and Hermes session/delegation IDs. Project current UI/bus states from this authority; preserve conversation data without dual task writers. Prove restart/reclaim/idempotency behavior.
4. Use Kanban admission parameters and a bounded profile/session configuration to enforce hardware concurrency, time/output limits and pause/cancel. Verify nested/batch multiplication cannot bypass machine-wide limits.
5. Register selected progressive workflow/persona content, translate tool/model metadata, repair Agency error normalization if its delegate helper is retained, and prove only selected content is loaded.
6. Run one coding job with an observed failing test, actual edit, passing regression, separate review and artifact verification. Add restart/cancel/resource-limit/denied-action coverage before declaring the integration ready.

No implementation step above has been started. The user explicitly required review and approval of the source-to-pipeline mapping first.
