# Source-to-pipeline map — approval required

Proposed canonical flow:

**User voice/text → Jubi Brain planner + installed-Ollama model router → Jubi hardware admission around Hermes swarm → specialist workers → reviewer → verifier → final answer + unified memory.**

The hardware wrapper grants resources and checks authority; **Hermes implements child agents, parallel delegation, bounded nested orchestration, sessions and Kanban**. It is not replaced with another swarm.

| Source | Where it feeds the flow | Exact proposed contribution and boundary |
|---|---|---|
| Existing Jubi/SARUS | Input, Brain, tools, evidence, memory, installer | Retain working components; consolidate competing entrypoints, registries and state. Replace SARA dependence with real guarded Windows workers. |
| Hermes | Brain → swarm → worker sessions | Native `AIAgent`, `delegate_task`, `SessionDB`, tools registry and Kanban. Sole durable task backend; Jubi adds global hardware budgets and completion verification. |
| Superpowers | Coding worker → reviewer → verifier | **Canonical dev loop:** spec/plan → debug or red/green/refactor → review → fresh verification. Hermes executes its stages. |
| ECC | Coding/review/verification specialists | Selected language/domain guidance and checklists; optional pure collision advisory. Exclude its separate ReAct runtime, tmux swarm, state DB and memory service. |
| Agency Agents | Registry discovery → selected specialist | Reuse lazy search/inspect/load catalogue. Translate model labels to installed local roles. Use Hermes directly for delegation until catalogue helper errors are handled correctly. |
| Second Brain | Knowledge/memory worker → Memory API | Knowledge organization, SOP and progressive-loading patterns. It supplies no memory backend. Direct content reuse awaits missing top-level license resolution. |
| SARA | Computer-worker design reference | Owner-check/interface ideas only. No dependency on missing binaries, incomplete Next.js bundle or opaque `/v7/command` service. |
| Awesome LLM Apps | Registry → selected coding/research/memory tool | Start with bounded offline Git/dependency helpers and selected retrieval patterns; no wholesale demo installs, extra swarm, memory stack or automatic downloads. |
| Fable | Registry versioning, reviewer/verifier, evidence | Action/prose distinction, trace and budget patterns; consolidate existing Jubi Fable stores. Original kernel only in a separate optional offline OS lab; resolve source license before copying code. |
| AutoResearch | Optional evaluation worker → reviewer/verifier | Frozen baseline, bounded trial and measurable promotion patterns. CUDA training remains disabled/manual, with preapproved assets; no production self-modification. Resolve license before code redistribution. |
| CAI and other cyber content | **Cyber Lab Manager only** | Disabled and excluded from normal production prompt/tool paths. CAI's restricted additions cannot be assumed eligible for production; research-only use requires license eligibility. No desktop/browser/PowerShell bridge. |
| `sunblaze-ucb/exploitgym` | Manual Cyber Lab → offline benchmark/training | Optional, disabled. Pinned container/subset, no network, quotas. Reject privileged/device/kernel-dependent cases unsupported by confinement. |
| `sunblaze-ucb/cybergym` | Manual Cyber Lab → offline evaluator | Optional, disabled; one provider despite duplicate URL. Reuse evaluator with enforced quotas and offline inputs; no public submission service or cloud firewall exceptions. |
| `sunblaze-ucb/cybergym-e2e` | Manual Cyber Lab → benchmark verifier | Optional, disabled. Reuse pre/post-patch and regression validation in offline containers. No host kernel changes, Docker socket or automatic agent installation. |
| `experientiallabs/experiential` | Optional adapter **beneath Brain's model router** | Apache-2.0 local gateway candidate. Disabled until Ollama/Windows/offline tests pass, telemetry is off, and local credential handling is app-managed. No hosted/paid fallback or second router authority. |
| Hugging Face | **Manual asset acquisition before execution** | Optional approved model/dataset downloads only, pinned with license and hash. No inference provider, mandatory account, background fetch or hard dependency. |

All normal capabilities are discoverable through **one Jubi registry**. Hermes Kanban is the sole durable task-state authority behind one Jubi Task API; sessions are transcripts, and event/receipt tables are evidence. All memory providers use **one Jubi Memory API**. Source directories are providers, not independent installed operating agents.

The three requested cyber repositories and Experiential were inspected upstream but are **not installed locally**. They have real code and Apache-2.0 code licenses; dataset/target licenses remain separate. Experiential is not a free hosted inference service. See [upstream evidence](cyber-optional-sources.md).

**Approval decision:** approve this map, the Hermes task-state ownership, Superpowers/ECC split, and optional-source boundaries to authorize the first integration milestone in [IMPLEMENTATION-PLAN.md](IMPLEMENTATION-PLAN.md). This approval does not itself start a lab, download Hugging Face assets, enable external services, or publish a release. Those remain governed by the explicit user consent specified above.

No product implementation has started. This gate comes directly from the user's instruction: “Do not start implementation until this mapping is reviewed and approved.”
