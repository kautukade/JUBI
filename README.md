# SARUS

**SARUS** is a Windows-first local multi-agent AI workspace that combines the SARUS orchestration core, the custom SARA Windows assistant, local Ollama model routing, multiple pinned open-source agent/research projects, a browser dashboard, a privileged Windows broker, and a controlled Ring-0 kernel bridge.

> Primary target: Windows 10/11 x64.
>
> Default SARUS dashboard: `http://127.0.0.1:8877`
>
> Default SARA dashboard: `http://127.0.0.1:3000/dashboard/command`

---

## Table of contents

1. [What SARUS contains](#what-sarus-contains)
2. [Main features](#main-features)
3. [Architecture](#architecture)
4. [System requirements](#system-requirements)
5. [Recommended installation: SARUS-Setup.exe](#recommended-installation-sarus-setupexe)
6. [Manual Git installation](#manual-git-installation)
7. [First launch](#first-launch)
8. [Ollama and model setup](#ollama-and-model-setup)
9. [Controlled Ring-0 bridge](#controlled-ring-0-bridge)
10. [How to verify the installation](#how-to-verify-the-installation)
11. [Configuration](#configuration)
12. [Important folders](#important-folders)
13. [Privileged broker and approvals](#privileged-broker-and-approvals)
14. [API overview](#api-overview)
15. [Updating SARUS](#updating-sarus)
16. [Uninstalling SARUS](#uninstalling-sarus)
17. [Troubleshooting](#troubleshooting)
18. [Installer logs](#installer-logs)
19. [Building the Windows installer](#building-the-windows-installer)
20. [Security notes](#security-notes)

---

## What SARUS contains

SARUS is not one single upstream project. It is a unified workspace around the SARUS core and SARA, with pinned integrations for the following source projects:

1. **SARA** — custom Windows local AI assistant and automation runtime.
2. **NousResearch / hermes-agent** — agent capabilities and tool-oriented workflows.
3. **ECC** — external capability/source integration.
4. **agency-agents** — multi-agent role and workflow patterns.
5. **awesome-llm-apps** — LLM application examples and integrations.
6. **second-brain-skills** — reusable assistant skills and knowledge workflows.
7. **superpowers** — additional agent/skill patterns.
8. **fable-os** — experimental agent/OS-style capabilities.
9. **CAI** — security-oriented agent source integration.
10. **autoresearch** — research automation source integration.

Pinned public source versions are described by `config/online_sources.json`. SARUS uses adapters and policy boundaries around source integrations rather than blindly exposing every upstream capability to the local model.

---

## Main features

### Local AI runtime

- Ollama-based local model routing.
- General, coding, vision, embedding and fast-model categories.
- Cloud-tagged models can be excluded from the local-only route.
- Browser dashboard served locally on `127.0.0.1`.

### Multi-agent workspace

- Unified task planning and execution surface.
- Capability discovery and routing.
- Task, event, memory, automation and approval state.
- Integration adapters for bundled/pinned source projects.

### Windows control

- Process and service inspection.
- Allowlisted service start/stop operations.
- Allowlisted process stop operations.
- Approved workspace file read/write.
- URL opening.
- Privileged actions routed through the SARUS broker rather than directly from the LLM.

### Controlled Ring-0 support

SARUS includes a Windows kernel-mode bridge project under `driver/SarusRing0/`.

Current broker-visible Ring-0 actions are:

- `ring0.ping`
- `ring0.status`

These are real kernel-bridge actions when `SarusRing0.sys` has been built, validly signed, installed and started on the target machine.

The bridge intentionally uses fixed typed IOCTLs. It does **not** expose arbitrary kernel-memory read/write, arbitrary physical-memory mapping, caller-selected IOCTL numbers, or arbitrary driver loading.

### Audit and approval layer

- Default-deny broker policy.
- Typed action IDs.
- Strict parameter validation.
- Logical allowlisted resources.
- Replay protection.
- Request-bound approval proofs for high-risk operations.
- Hash-chained authenticated action receipts.
- Sensitive payload redaction in audit receipts.
- Protected broker key storage under the Windows user profile.

---

## Architecture

```text
User / SARUS Dashboard
        |
        v
SARUS Local HTTP/API Layer
        |
        +---------------------> Ollama local models
        |
        +---------------------> Agent/source adapters
        |
        v
Privileged Broker
        |
        +--> safe user-level Windows actions
        |
        +--> approval gate for high-risk actions
        |
        +--> Ring0 typed bridge
                 |
                 v
          \\.\SarusRing0
                 |
                 v
          SarusRing0.sys
                 |
                 v
          Windows Kernel
```

The LLM does not receive a raw kernel handle or generic `DeviceIoControl` primitive. The model requests a typed SARUS capability; the broker validates it and only then calls the fixed implementation.

---

## System requirements

### End-user requirements

- **Windows 10 or Windows 11, x64**.
- Administrator access for installation.
- Internet connection for first-time dependency/source/model downloads.
- Enough free disk space for source trees, Python/Node dependencies and Ollama models. Model storage can become large; keeping **25 GB or more free** is recommended for a comfortable setup.
- A modern browser for the local dashboard.

### Runtime components

The SARA/SARUS installation process may install or use:

- **Python 3.11 x64** — SARUS currently explicitly checks `py -3.11` during setup.
- Python Launcher for Windows (`py.exe`).
- Git for Windows.
- Node.js + npm for SARA's web/runtime components.
- Ollama for local models.
- Windows `tar.exe`.

If the one-click installer successfully installs/configures these, you do not need to install them separately.

### Ring-0 development requirements

Only required if you want to build the kernel driver yourself:

- Visual Studio 2022 or Visual Studio Build Tools.
- Desktop C++ build tools.
- Windows Driver Kit (WDK) integration.
- A driver-signing method/certificate trusted by the target Windows machine.

The normal SARUS application can run without compiling the Ring-0 driver, but `ring0.ping` and `ring0.status` will report that the driver is unavailable until it is installed.

---

# Recommended installation: SARUS-Setup.exe

The easiest installation method for a normal Windows laptop is the generated **`SARUS-Setup.exe`**.

The EXE installer is built from `installer/SARUS-Setup.iss` by the GitHub Actions workflow:

```text
Build SARUS Windows Installer
```

The workflow publishes an artifact named:

```text
SARUS-Windows-Installer
```

It contains:

```text
SARUS-Setup.exe
SHA256.txt
```

## Step 1 — Download the installer

Open this repository on GitHub, then:

1. Open **Actions**.
2. Open **Build SARUS Windows Installer**.
3. Open the latest successful run for `main`.
4. Scroll to **Artifacts**.
5. Download **SARUS-Windows-Installer**.
6. Extract the downloaded ZIP.
7. You should see `SARUS-Setup.exe` and `SHA256.txt`.

## Step 2 — Optional SHA-256 verification

Open PowerShell in the folder containing the installer:

```powershell
Get-FileHash .\SARUS-Setup.exe -Algorithm SHA256
Get-Content .\SHA256.txt
```

The two SHA-256 values should match.

## Step 3 — Run the installer

Right-click:

```text
SARUS-Setup.exe
```

and choose:

```text
Run as administrator
```

The default installation directory is:

```text
C:\Program Files\SARUS
```

You may choose another directory if required.

## Step 4 — Let the bootstrap complete

The installer copies the SARUS payload, then runs the existing SARUS installation engine. During first installation it can:

- create protected broker key storage;
- restore/verify bundled SARA source;
- restore pinned source integrations when missing;
- run SARA's Windows dependency setup;
- create the SARUS Python virtual environment;
- run SARUS acceptance checks;
- reconstruct/verify the SARUS launcher when bundled;
- create shortcuts;
- start SARUS.

Do not terminate the installer while dependency installation is running.

## Step 5 — Open SARUS

The local dashboard is normally:

```text
http://127.0.0.1:8877
```

A desktop/start-menu shortcut named **SARUS** is also created.

### Windows SmartScreen note

The GitHub-built `SARUS-Setup.exe` is a real executable but may be **unsigned as an application installer** unless a Windows code-signing certificate has been configured in the build pipeline. Windows SmartScreen can therefore display **Unknown publisher**.

This is separate from the Ring-0 driver signing requirement. Production distribution should code-sign both the installer and the kernel driver with appropriate certificates.

---

# Manual Git installation

Use this method for development, debugging, or when you prefer to run directly from the repository.

## Step 1 — Open PowerShell

Go to a folder where you want the source, for example Desktop:

```powershell
cd "$HOME\Desktop"
```

## Step 2 — Clone SARUS

```powershell
git clone --recurse-submodules https://github.com/kautukade/SARUS.git
cd SARUS
```

If the repository was already cloned without submodules:

```powershell
git submodule update --init --recursive
```

## Step 3 — Verify important commands

```powershell
git --version
py -3.11 --version
node --version
npm --version
ollama --version
```

It is okay if some runtime components are not yet installed and the SARA setup is intended to provision them, but `Python 3.11` must ultimately be available because the SARUS installer explicitly checks it.

## Step 4 — Start installation

From the repository root:

```powershell
Start-Process -FilePath ".\INSTALL-SARUS.bat" -Verb RunAs
```

Or right-click `INSTALL-SARUS.bat` and select **Run as administrator**.

## Step 5 — Launch SARUS

```powershell
.\START_SARUS.bat
```

If the reconstructed launcher is available:

```powershell
.\SARUS.exe
```

---

## First launch

When SARUS starts successfully, it binds locally by default.

Primary URL:

```text
http://127.0.0.1:8877
```

SARUS does not need to be exposed to the public internet for normal local use.

SARA's internal dashboard, when its web runtime is running, is normally:

```text
http://127.0.0.1:3000/dashboard/command
```

If a browser opens before the server is completely ready, refresh the page after the console reports that the server has started.

---

## Ollama and model setup

SARUS model routing is configured in:

```text
config/models.json
```

Current configured model groups are:

### General

```text
qwen2.5:7b
glm4:latest
mistral:latest
llama3:latest
```

### Coding

```text
qwen2.5-coder:7b
deepseek-coder:latest
```

### Vision

```text
qwen2.5vl:3b
```

### Embeddings

```text
nomic-embed-text-v2-moe:latest
```

### Fast/lightweight

```text
qwen2:1.5b
gemma:2b
```

### Cloud-tagged models disabled from the strict local route

```text
kimi-k2.6:cloud
deepseek-v3.1:671b-cloud
gpt-oss:120b-cloud
```

Check currently installed Ollama models:

```powershell
ollama list
```

Pull a missing model, for example:

```powershell
ollama pull qwen2.5:7b
```

Check whether Ollama is responding:

```powershell
Invoke-WebRequest http://127.0.0.1:11434/api/tags
```

### `ollama serve` says the port is already in use

If you see a bind error for port `11434`, it commonly means Ollama is **already running**. Do not start a second server. Confirm with:

```powershell
Get-NetTCPConnection -LocalPort 11434 -ErrorAction SilentlyContinue
```

---

# Controlled Ring-0 bridge

SARUS contains an actual Windows kernel driver project:

```text
driver\SarusRing0\
```

Important files:

```text
driver\SarusRing0\driver.c
driver\SarusRing0\sarus_ring0_shared.h
driver\SarusRing0\SarusRing0.vcxproj
driver\SarusRing0\BUILD-RING0.ps1
driver\SarusRing0\INSTALL-RING0.ps1
INSTALL-RING0.bat
```

## What Ring-0 currently provides

The broker exposes:

```text
ring0.ping
ring0.status
```

When the driver is loaded, these calls cross from SARUS user mode through:

```text
DeviceIoControl -> \\.\SarusRing0 -> SarusRing0.sys -> Windows kernel mode
```

The status response includes kernel-side protocol/capability information and telemetry such as current IRQL and interrupt timing.

## Step 1 — Install WDK build tools

Install Visual Studio/Build Tools with:

- MSBuild;
- Desktop development with C++;
- Windows SDK;
- Windows Driver Kit integration.

## Step 2 — Build the driver

Open PowerShell from the SARUS repository:

```powershell
cd .\driver\SarusRing0
.\BUILD-RING0.ps1 -Configuration Release
```

Expected output:

```text
driver\SarusRing0\bin\Release\SarusRing0.sys
```

## Step 3 — Sign the driver

Windows x64 normally requires an appropriately trusted kernel driver signature. The SARUS installer intentionally does **not** disable Windows signature enforcement, Secure Boot or Defender.

Sign `SarusRing0.sys` using an appropriate development/production driver-signing workflow for your target environment.

Confirm signature status:

```powershell
Get-AuthenticodeSignature .\bin\Release\SarusRing0.sys
```

The install script expects:

```text
Status : Valid
```

## Step 4 — Install the Ring-0 driver

From the SARUS root, run as Administrator:

```powershell
.\INSTALL-RING0.bat
```

The script copies the driver to:

```text
C:\Windows\System32\drivers\SarusRing0.sys
```

and creates the kernel service:

```text
SarusRing0
```

Check it:

```powershell
sc.exe query SarusRing0
```

## Step 5 — Test the Ring-0 bridge directly

From the SARUS root:

```powershell
.\.sarus-venv\Scripts\python.exe -c "from sarus.core.ring0 import Ring0Bridge; import json; print(json.dumps(Ring0Bridge().status(), indent=2))"
```

A working installation should report values including:

```text
"ok": true
"driver_present": true
"protocol_version": 1
```

If `driver_present` is true but `ok` is false with Access Denied, run SARUS/equivalent test in the appropriate elevated context because the device ACL is intentionally restricted.

---

## How to verify the installation

### Verify Python runtime

```powershell
Test-Path .\.sarus-venv\Scripts\python.exe
```

Expected:

```text
True
```

### Run SARUS acceptance

```powershell
.\.sarus-venv\Scripts\python.exe -m sarus.acceptance
```

### Start SARUS manually with the private environment

If the normal batch launcher has a Python-selection problem, use:

```powershell
.\.sarus-venv\Scripts\python.exe -m sarus.server
```

Then open:

```text
http://127.0.0.1:8877
```

### Verify port 8877

```powershell
Get-NetTCPConnection -LocalPort 8877 -ErrorAction SilentlyContinue
```

### Verify basic HTTP status

```powershell
Invoke-WebRequest http://127.0.0.1:8877/api/status
```

### Verify broker status

```powershell
Invoke-WebRequest http://127.0.0.1:8877/api/broker
```

---

## Configuration

Important SARUS configuration files:

```text
config\models.json
config\online_sources.json
config\policy.json
config\broker_allowlist.json
config\sources.json
```

### `config/models.json`

Controls Ollama model categories and local/cloud routing choices.

### `config/online_sources.json`

Contains pinned public upstream repositories and exact source revisions used by the restoration installer.

### `config/policy.json`

Defines global SARUS risk/approval policy.

### `config/broker_allowlist.json`

Defines the privileged broker's allowed typed actions, parameter schemas, resource mappings and workspace scopes.

Do not replace these security configuration files with untrusted generated content.

---

## Important folders

```text
sarus/                 SARUS Python runtime and API server
sarus/core/            broker, policy, storage and Windows/Ring0 bridge logic
sarus/web/             local dashboard assets
sources/               SARA and pinned source integrations
config/                models, sources and security policy
installer/             Windows BAT/PowerShell/Inno installer engine
driver/SarusRing0/     controlled Windows kernel driver project
scripts/               launch/build/approval helpers
tests/                 integration and security tests
docs/                  architecture, delivery and security documentation
vendor/                 bundled/reconstructable installation assets
workspace/             approved SARUS user workspace
outputs/               generated output workspace
projects/              approved project workspace
logs/                   local installation/runtime logs
```

The privileged broker intentionally limits normal LLM file-control operations to approved workspace roots rather than the SARUS security/source directories.

---

## Privileged broker and approvals

High-risk broker actions require a short-lived approval proof.

The protected approval key is stored outside the repository at:

```text
%LOCALAPPDATA%\SARUS\broker\approval.secret
```

The receipt authentication key is stored at:

```text
%LOCALAPPDATA%\SARUS\broker\receipt-signing.key
```

Do **not** paste these keys into prompts, source code, browser storage or GitHub.

A trusted operator-side approval helper is available:

```powershell
python .\scripts\create_broker_approval.py --request-id <REQUEST_ID> --action-id process.stop --parameters-json "{\"resource_id\":\"ollama\"}" --ttl 120
```

The proof is bound to the request ID, action ID, exact parameter hash and expiration time. A proof for one request cannot authorize another different request.

---

## API overview

SARUS exposes local endpoints from `sarus/server.py`.

Useful read-only endpoints include:

```text
GET /api/session
GET /api/status
GET /api/doctor
GET /api/events
GET /api/models
GET /api/capabilities
GET /api/tasks
GET /api/approvals
GET /api/receipts
GET /api/memory
GET /api/automations
GET /api/broker
```

Action endpoints include:

```text
POST /api/plan
POST /api/task
POST /api/chat
POST /api/capability/run
POST /api/memory
POST /api/approval
POST /api/system/action
POST /api/automation
POST /api/automation/toggle
```

Mutating requests are protected by SARUS session/origin controls. Do not expose the local API directly to an untrusted network without an additional authenticated reverse-proxy/security design.

---

## Updating SARUS

### EXE installation

For a future installer release:

1. Download the new `SARUS-Setup.exe`.
2. Close SARUS.
3. Run the new installer as Administrator.
4. Install to the same SARUS directory.
5. The setup is designed to preserve protected user/broker state outside the application folder.

### Git installation

```powershell
cd "C:\path\to\SARUS"
git checkout main
git pull origin main
git submodule update --init --recursive
```

Then rerun:

```powershell
Start-Process -FilePath ".\INSTALL-SARUS.bat" -Verb RunAs
```

---

## Uninstalling SARUS

If installed with `SARUS-Setup.exe`, use:

```text
Windows Settings -> Apps -> Installed apps -> SARUS -> Uninstall
```

The EXE uninstaller runs SARUS-specific cleanup before removing application files.

By default it preserves protected local keys/user data so a reinstall does not destroy receipt verification state.

The cleanup script is:

```text
installer\UNINSTALL-SARUS.ps1
```

It removes SARUS' own `SarusRing0` service/driver if present, but does not touch unrelated Windows drivers.

For a deliberate full user-data purge, review and manually run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\installer\UNINSTALL-SARUS.ps1 -PurgeUserData
```

Only use `-PurgeUserData` if you intentionally want to remove protected SARUS/SARA local state.

---

# Troubleshooting

## 1. `Python 3.11 was not found after SARA setup`

SARUS currently asks the Windows Python Launcher specifically for Python 3.11.

Check installed Python registrations:

```powershell
py -0p
```

Check 3.11 directly:

```powershell
py -3.11 --version
```

If it fails, install Python 3.11 x64 with the Python Launcher, then rerun the installer.

You do not need to uninstall Python 3.12/3.13/3.14; Python 3.11 can exist alongside them.

---

## 2. `Test-Path .\.sarus-venv\Scripts\python.exe` returns False

The SARUS virtual environment was not created successfully.

First confirm:

```powershell
py -3.11 --version
```

Then rerun:

```powershell
Start-Process -FilePath ".\INSTALL-SARUS.bat" -Verb RunAs
```

Check:

```text
logs\github-install.log
```

---

## 3. `npm.cmd is not recognized` or npm quoting error

First verify npm manually:

```powershell
node --version
npm --version
where.exe node
where.exe npm
```

If `npm --version` works manually but the nested SARA installer fails with a path such as:

```text
C:\Program Files\nodejs\npm.cmd
```

then inspect the SARA installer log for command quoting around `npm.cmd`. Reinstalling Node usually will not fix a script quoting bug if npm already works from PowerShell.

---

## 4. Ollama port 11434 bind error

Example:

```text
bind ... 11434 ... address already in use
```

Usually Ollama is already running.

Check:

```powershell
Get-NetTCPConnection -LocalPort 11434 -ErrorAction SilentlyContinue
ollama list
```

Do not start a second `ollama serve` if the first instance is healthy.

---

## 5. SARUS port 8877 is already in use

Check the owning process:

```powershell
Get-NetTCPConnection -LocalPort 8877 -ErrorAction SilentlyContinue
```

If another SARUS instance is already running, use that instance or stop it before starting a second one.

For manual testing you can start the Python server directly after setting a different environment port, but note that the normal batch launcher currently sets its expected default port to `8877`.

---

## 6. `START_SARUS.bat` says Python is missing even though installation succeeded

The safest manual runtime is the dedicated SARUS environment:

```powershell
.\.sarus-venv\Scripts\python.exe -m sarus.server
```

If this works while the batch launcher fails, the problem is Python selection in the launcher environment rather than the SARUS runtime itself.

---

## 7. SARA source reconstruction fails

The installer can reconstruct the verified bundled SARA source when the expected bundle parts are present.

Check:

```powershell
(Get-ChildItem .\vendor\sara\finalparts\part-*.b64).Count
```

The current reconstruction path expects the complete verified set used by the installer.

If the bundled source is incomplete, the owner fallback may require authenticated Git access to the configured private SARA repository.

---

## 8. GitHub/codeload download fails

Possible causes:

- VPN/proxy interference;
- DNS failure;
- firewall rules;
- GitHub outage;
- TLS interception;
- no internet connection.

Test:

```powershell
git ls-remote https://github.com/kautukade/SARUS.git
```

Try again after restoring normal network access.

---

## 9. `SarusRing0.sys is not validly signed`

Check:

```powershell
Get-AuthenticodeSignature .\driver\SarusRing0\bin\Release\SarusRing0.sys
```

The Ring-0 installation script intentionally refuses to disable Windows driver-signature enforcement.

Use a legitimate signing/trust process for the target machine.

---

## 10. Ring-0 status says driver is missing

Check the service:

```powershell
sc.exe query SarusRing0
```

Check the driver file:

```powershell
Test-Path "$env:SystemRoot\System32\drivers\SarusRing0.sys"
```

If missing, build/sign/install the driver first.

---

## 11. Ring-0 status says Access Denied

The driver device is restricted to privileged Windows identities.

Run the relevant SARUS/test process from an elevated Administrator context and retry.

Do not weaken the device ACL merely to avoid elevation.

---

## 12. Windows SmartScreen blocks `SARUS-Setup.exe`

A CI-generated installer may not have an application code-signing signature configured yet.

For development, verify the file hash against `SHA256.txt` and verify that the artifact came from this repository's successful GitHub Actions run.

For production distribution, sign `SARUS-Setup.exe` with a trusted Windows code-signing certificate.

---

## 13. Antivirus/security product flags the project

SARUS includes automation, local service control, a privileged broker and an optional kernel driver project. Security products may therefore apply additional scrutiny.

Do not broadly disable Defender or antivirus. Instead:

- verify the repository/installer hash;
- inspect the flagged file;
- use trusted code signing;
- submit a false-positive review to the security vendor when appropriate.

---

## Installer logs

Main SARUS GitHub installation log:

```text
<install-folder>\logs\github-install.log
```

For the default EXE location:

```text
C:\Program Files\SARUS\logs\github-install.log
```

The nested SARA installation has its own installer/runtime logs inside the SARA source/runtime tree.

When reporting a problem, provide:

```powershell
py -0p
py -3.11 --version
node --version
npm --version
ollama --version
ollama list
Get-NetTCPConnection -LocalPort 8877 -ErrorAction SilentlyContinue
Get-NetTCPConnection -LocalPort 11434 -ErrorAction SilentlyContinue
```

and the relevant installer log error lines. Do not publicly post passwords, tokens or protected broker key contents.

---

# Building the Windows installer

End users do **not** need Inno Setup. This section is for maintainers/developers.

## Build locally

Install Inno Setup 6, then from the repository root run:

```powershell
& "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" .\installer\SARUS-Setup.iss
```

Output:

```text
dist-installer\SARUS-Setup.exe
```

Generate a checksum:

```powershell
(Get-FileHash .\dist-installer\SARUS-Setup.exe -Algorithm SHA256).Hash
```

## Build using GitHub Actions

Workflow file:

```text
.github\workflows\build-windows-installer.yml
```

The workflow:

1. checks out SARUS on a Windows runner;
2. verifies required payload files;
3. compiles Python sources as a syntax/smoke check;
4. locates or installs Inno Setup;
5. builds `SARUS-Setup.exe`;
6. rejects an unexpectedly tiny/missing EXE;
7. generates `SHA256.txt`;
8. uploads `SARUS-Windows-Installer` as a GitHub Actions artifact.

---

## Security notes

SARUS is designed to keep powerful local capabilities behind explicit boundaries.

### The local model can request capabilities, but should not receive:

- arbitrary shell execution as a privileged primitive;
- arbitrary executable paths;
- unrestricted service names;
- arbitrary raw IOCTL forwarding;
- arbitrary kernel-memory read/write;
- physical-memory mapping;
- arbitrary driver loading;
- direct access to protected broker secrets.

### Ring-0 is available, but typed

Ring-0 itself is **not globally blocked**. SARUS has a real kernel bridge, but kernel capabilities are added as fixed reviewed action handlers rather than an unrestricted memory/IOCTL interface.

### Protected key material

Never commit or share:

```text
%LOCALAPPDATA%\SARUS\broker\approval.secret
%LOCALAPPDATA%\SARUS\broker\receipt-signing.key
```

### Network boundary

By default the dashboard/API is intended for localhost. Do not port-forward `8877` to the internet without a separate authenticated network security layer.

### Driver signing

The repository contains driver source and build/install helpers, not a universal bypass for Windows Code Integrity. A target machine must accept the driver's legitimate signature/trust chain.

---

## Quick beginner checklist

For the simplest path:

```text
1. Download SARUS-Windows-Installer artifact.
2. Extract it.
3. Verify SHA256.txt if desired.
4. Right-click SARUS-Setup.exe.
5. Run as administrator.
6. Let installation finish.
7. Open SARUS shortcut.
8. Open http://127.0.0.1:8877
9. Run ollama list if a model is missing.
10. Build/sign/install SarusRing0.sys only if you want the Ring-0 bridge active.
```

---

## Repository

```text
https://github.com/kautukade/SARUS
```

SARUS is under active development. Treat kernel-mode and privileged features as security-sensitive code: test changes on a dedicated Windows test machine before deploying them broadly.
