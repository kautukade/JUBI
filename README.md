# Jubi

> **Local-first AI Agent, Research & Windows Automation Platform**  
> Primary target: **Windows 10/11 x64**  
> Foundation: **SARUS v1.3.1 compatibility layer**

Jubi is the evolving product built on the stabilized SARUS foundation. User-facing runtime, dashboard and installer are Jubi; selected internal `sarus/` paths, installer helper names, the physical `data/sarus.db` filename and the narrow `SarusRing0` ABI are intentionally retained until a target-tested migration can change them safely.

## Download & one-click install

The recommended Windows installation method is the packaged installer:

**Jubi-Setup.exe**  
https://github.com/kautukade/JUBI/releases/download/continuous/Jubi-Setup.exe

Continuous release page:  
https://github.com/kautukade/JUBI/releases/tag/continuous

### What the installer does

A normal user should not need to manually prepare the development environment. `Jubi-Setup.exe` is designed to:

1. request the required Windows administrator permission through UAC;
2. inspect required prerequisites;
3. install or repair supported missing dependencies where possible;
4. ensure the required Python runtime and Ollama baseline are available;
5. provision the configured local Ollama models;
6. create Jubi's private Python environment;
7. install the Jubi application payload;
8. create `Jubi.exe`, Start Menu and optional Desktop shortcuts;
9. register the **Jubi Background Agent** for automatic startup when the Windows user logs in;
10. run post-install verification before launching Jubi.

The installer currently targets Windows 10/11 x64. Internet access is required when prerequisites or Ollama models must be downloaded.

### Background operation

Jubi registers a persistent per-user background task. After Windows login it starts automatically, keeps the local Jubi service available, and is configured to restart after unexpected termination.

This is intentionally a **user-session background agent**, not a pre-login SYSTEM service. That design keeps Jubi aligned with per-user encrypted credentials, the desktop session, browser integration and user-approved computer actions.

### Automatic updates

Jubi includes a continuous self-update channel tied to this repository.

When a successful `main` build produces a new installer, GitHub Actions refreshes the `continuous` release with:

- `Jubi-Setup.exe`
- `Jubi-Update-Manifest.json`
- `SHA256.txt`

The installed background agent periodically checks the canonical `kautukade/JUBI` release channel. Before an update is applied, Jubi verifies that the manifest belongs to the expected repository and validates the downloaded installer using SHA-256. The update is then installed through the supported silent update path.

This means normal future releases are intended to update the existing Jubi installation instead of requiring the user to manually uninstall and reinstall the application each time.

> **Windows signing note:** the current continuous build may still show Windows SmartScreen / Unknown Publisher warnings until a production Authenticode signing certificate is configured for public distribution.

## Current capabilities

### Jubi Brain
- Automatic intent classification: general, coding, vision, research, planning, document and system work.
- Complexity and privacy classification.
- Installed-model-only Ollama routing with bounded fallback.
- Per-model success/failure and latency history used by future routing.
- Explicit user model override remains available.

### Local models
The production baseline is configured in `config/production.json`. The typical local roles are:

```text
qwen2.5:7b                       general
qwen2.5-coder:7b                 coding
qwen2.5vl:3b                     vision
nomic-embed-text-v2-moe:latest   embeddings / semantic memory
```

Jubi queries local Ollama at `http://127.0.0.1:11434`. A configured model is never treated as available unless Ollama actually reports it as installed.

### Provider Manager
Optional cloud inference is available through:
- OpenRouter
- NVIDIA NIM
- Hugging Face Inference Providers

Routing modes:

```text
Local Only   -> cloud generation disabled
Hybrid Auto  -> local first; bounded cloud use for eligible work
Cloud Boost  -> cloud may be preferred for eligible work
```

High-privacy prompts remain blocked from automatic cloud transmission. On Windows, dashboard-entered provider credentials are encrypted per-user with DPAPI under `%LOCALAPPDATA%\Jubi`; secrets are not committed to GitHub or stored in the Jubi SQLite database.

### Semantic Knowledge / RAG
Jubi includes local semantic knowledge using Ollama embeddings:
- document/content chunking
- local embedding generation
- semantic similarity search
- namespace/project separation
- RAG answering with visible retrieved-source markers
- persistent local vector data in SQLite

### Experience / bounded self-learning
Jubi learns from outcomes without modifying base model weights after every conversation. It records bounded experience such as task type, route, provider/model, success/failure, latency and lessons, and can retrieve similar prior experience for later work.

### AI Council & Multi-Agent Supervisor
- AI Council asks multiple eligible models independently and uses a Judge to synthesize the final result.
- Multi-Agent Supervisor performs planner -> specialist reasoning -> reviewer workflows.
- These are reasoning layers; they do not bypass tool policy, approval or privileged-broker boundaries.

### Public Web Research
Jubi can search/read public HTTP/HTTPS pages and synthesize source-marked research. Internet content is treated as **untrusted evidence**.

The public research reader blocks:
- localhost/loopback
- private/link-local/reserved targets
- credential-bearing URLs
- unsupported network targets

Fetched page text cannot directly trigger Windows/LAN privileged execution.

### Computer Operator
The Windows broker exposes typed, allowlisted operations instead of arbitrary shell strings. Current dashboard-accessible examples include:
- process/service inventory
- workspace file read/write/stat
- directory list/create
- scoped file copy/move
- approval-protected scoped file delete
- read-only Git status/log
- fixed allowlisted app launch (VS Code, Notepad, Explorer)
- HTTP/HTTPS URL opening
- narrow Ring0 status/ping compatibility checks

Workspace actions are confined to configured workspace roots. Jubi intentionally does not expose a model-facing unrestricted PowerShell/CMD/shell primitive.

### Authorized LAN Manager
Jubi includes a bounded LAN foundation for devices the user is authorized to manage:
- passive host neighbor-cache discovery (`arp -a` / `ip neigh`)
- explicit authorized-device registry
- user-declared TCP service registry
- health checks only against registered service ports
- persistent local network observations

It does **not** perform subnet-wide active scanning, credential brute force, exploitation, stealth lateral movement or arbitrary remote command execution.

### Vision & browser voice controls
Local vision uses an installed Ollama vision model and supports PNG/JPEG/WebP image analysis. This feature does not send images to OpenRouter/NVIDIA/Hugging Face.

The dashboard also offers optional browser speech controls when supported:
- click-to-talk dictation
- text-to-speech read-aloud

Browser speech recognition is not guaranteed to be local/offline. A native always-on `Hey Jubi` wake-word service is not claimed by the current release.

### Persistent execution and approvals
Jubi persists task plans and execution cursors in SQLite. An approval-required task can survive restart and resume the exact pending step after approval. Rejected steps do not execute.

### Fable intelligence and source families
The existing Fable intelligence layer and ten SARUS-era source families remain available as compatibility/runtime assets. A connected source repository is not automatically equivalent to a fully running upstream native runtime.

## Dashboard

Jubi uses one professional local dashboard at:

```text
http://127.0.0.1:8877
```

Feature surfaces include:
- Overview
- AI Chat
- Tasks & Planner
- Brain / AI Council / Supervisor
- Providers
- Models
- Agents & Capabilities
- Development
- Knowledge / Experience
- Fable Lab
- Automation
- Computer Operator
- Web Research
- Authorized LAN
- Vision & Voice
- Security & Receipts
- System Health
- Activity

The HTTP dashboard remains localhost-only. Setting `JUBI_HOST=0.0.0.0` is rejected.

## Security model

The central rule is:

```text
AI reasoning != unrestricted privileged execution
```

Important boundaries:
- default-deny privileged policy
- typed action IDs and parameter schemas
- allowlisted resources
- workspace path scoping
- request-bound approval proofs for configured high-risk actions
- signed execution receipts
- no arbitrary kernel memory API
- no raw driver IOCTL interface exposed to models
- no security-control disabling scripts
- public web treated as untrusted data
- passively discovered LAN devices remain untrusted until explicitly registered
- updater restricted to the canonical JUBI release channel with SHA-256 verification

The legacy Ring0 bridge remains intentionally narrow:

```text
ring0.ping
ring0.status
```

## Local persistence

For compatibility the physical database remains:

```text
data/sarus.db
```

Jubi-owned state includes memory, semantic knowledge, experiences, tasks, approvals, automations, events, receipts, Brain/provider performance, Council/Supervisor history, research history, Fable state and authorized-network observations.

SQLite uses WAL, busy timeout, foreign keys and explicit commit/rollback transactions.

## Run from source

Source installation is intended for development. Normal Windows users should prefer `Jubi-Setup.exe`.

Development prerequisites:
- Windows 10/11 x64 for full broker behavior
- Python 3.11+
- Ollama for local inference
- required local models from `config/production.json`

Clone:

```powershell
git clone --recurse-submodules https://github.com/kautukade/JUBI.git
cd JUBI
```

Start Jubi:

```powershell
python -m jubi.server
```

Open:

```text
http://127.0.0.1:8877
```

Run acceptance:

```powershell
python -m jubi.acceptance --full
```

## CI validation

GitHub Actions validates on Linux and Windows:
- Python compilation
- installer lifecycle contracts
- PowerShell installer syntax
- dashboard JavaScript syntax
- unified dashboard wiring
- Advanced Brain
- Provider Manager
- semantic knowledge / experience
- AI Council / Supervisor
- public web research safety
- typed computer operator
- authorized LAN manager
- local vision contracts
- Phase 0 persistence and approval regression
- production readiness
- Fable and foundation integration
- privileged broker / Ring0 policy tests
- Windows installer compilation
- installer SHA-256 generation
- continuous update-manifest generation

CI does **not** prove target-machine hardware/runtime facts such as real Ollama inference speed, live provider quota, actual LAN device reachability, browser speech availability, Windows SmartScreen behavior, or the final installed EXE behavior on every laptop. Those require physical target validation.

## Windows installer details

Canonical installer:

```text
Jubi-Setup.exe
```

Direct download:

```text
https://github.com/kautukade/JUBI/releases/download/continuous/Jubi-Setup.exe
```

Release assets are published under:

```text
https://github.com/kautukade/JUBI/releases/tag/continuous
```

The installed launcher is exposed as `Jubi.exe`. Legacy SARUS helper filenames may remain internally where changing them would risk installer/driver compatibility.

## Current production boundary

Code and CI cover the main Jubi architecture, packaged installer, background startup and self-update pipeline. Before calling a specific laptop deployment fully certified, perform a clean Windows install and test:
- `Jubi-Setup.exe` first-install flow
- automatic prerequisite provisioning on that machine
- automatic required-model provisioning
- `Jubi.exe` launch and shortcuts
- background start after Windows login
- local Ollama chat/coding/vision/embedding inference
- provider credentials and live OpenRouter/NVIDIA/Hugging Face calls if desired
- restart persistence and approval resume
- Computer Operator actions inside real workspace roots
- authorized LAN devices on the user's network
- dashboard/browser speech support
- automatic update from one `main` build to a newer build
- installer/upgrade/uninstall lifecycle

Do not treat unverified target-machine behavior as proven merely because CI is green.
