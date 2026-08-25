# Jubi v0.1.0 Production Readiness

## Purpose

Jubi v0.1.0 is the Phase 0 stabilization/migration release evolved from the SARUS v1.3.1 foundation. This document separates what can be proven in CI from what still must be validated on the real Windows laptop or by a code-signing authority.

## Release gates

A revision is release-candidate ready only when all relevant checks pass:

1. Python compile checks for Jubi and the compatibility foundation.
2. Jubi Phase 0 persistence/approval regression tests.
3. Production-readiness static tests.
4. Existing ten-source integration/regression tests.
5. Fable integration tests on Windows and Linux.
6. Privileged Broker and controlled Ring0 policy tests.
7. Windows PowerShell syntax validation for installer/certification/driver scripts.
8. Inno Setup compilation of `Jubi-Setup.exe`.
9. SHA-256 generation for the final installer artifact.

Repository contents remain read-only to normal CI gates; only the dedicated artifact-cleanup job requires Actions write permission.

## Phase 0 reliability repairs

### Explicit SQLite persistence

Jubi uses explicit transactions for memory, events, task state, approvals and automations. SQLite connections use WAL mode and a busy timeout to reduce contention from the HTTP server and background workers.

### Resumable approval state

The execution plan, step cursor, results and context are persisted before an approval pause. After restart, an approved task resumes the same pending step rather than re-planning a different task. Rejected/resolved approvals cannot silently execute or be reused.

### Safer Ollama routing

Jubi discovers the actual Ollama model list and never returns a configured-but-missing model as a valid fallback. Required production models are defined in `config/production.json`.

### Manifest-driven acceptance

`jubi.acceptance` delegates to the maintained foundation acceptance implementation, which reads `BUILD_MANIFEST.json` and `config/production.json`. The expected indexed-source count is defined by the manifest rather than a stale hard-coded number.

## Ollama provisioning

During the official EXE installation, `JUBI_INSTALL_MODE=exe` (plus the temporary legacy compatibility variable) causes acceptance to:

- verify the local Ollama service;
- attempt to start `ollama serve` when possible;
- download missing required local models through Ollama's local API;
- verify required models after provisioning;
- fail clearly instead of pretending an incomplete install succeeded.

## Target-machine certification

The compatibility-named script:

```text
installer\CERTIFY-SARUS.ps1
```

now certifies **Jubi** and runs:

```powershell
python -m jubi.acceptance --full
```

It writes:

```text
C:\Program Files\Jubi\logs\production-certification.json
```

It reports:

- Jubi acceptance result;
- required-file completeness;
- `Jubi.exe` Authenticode status;
- controlled legacy Ring0 compatibility status;
- bundled driver signature status;
- Doctor/model/Fable status.

Use `-RequireSignedApp` and/or `-RequireRing0` only for stricter target certification.

## Production status levels

### CI release-candidate ready

Source, security, integration and installer-build gates are green in GitHub Actions.

### Windows target certified

`Jubi-Setup.exe` has been clean-installed on the actual target Windows laptop and `production-certification.json` reports `core_ready: true`.

Cloud CI cannot prove local camera/audio hardware, SARA runtime, real Ollama inference performance, GPU/driver behavior or optional WSL/QEMU readiness.

### Public release signed

`Jubi.exe` and `Jubi-Setup.exe` have valid organization Authenticode signatures, and any bundled kernel driver satisfies the applicable Microsoft driver-signing requirements.

`installer/SIGN-RELEASE.ps1` signs the Jubi launcher/installer with SHA-256 and RFC3161 timestamping when an organization signing certificate is available. No private signing key is stored in the repository.

## Controlled Ring0 compatibility boundary

The legacy driver/device ABI remains named `SarusRing0` during Phase 0. This is intentional because renaming a Windows kernel driver/device is a compatibility and signing change, not normal product branding.

The normal Jubi host remains usable without an active kernel driver. When a prebuilt driver is bundled, activation occurs only if Windows reports a valid signature.

No release script disables Secure Boot, Code Integrity, HVCI, Defender or driver-signature enforcement.

Strict compatibility-driver certification, only when a correctly signed driver is intentionally being tested:

```powershell
& "C:\Program Files\Jubi\installer\CERTIFY-SARUS.ps1" -RequireRing0
```

Signed application certification:

```powershell
& "C:\Program Files\Jubi\installer\CERTIFY-SARUS.ps1" -RequireSignedApp
```

## Fable production boundary

The Jubi-integrated Fable Intelligence Layer remains part of normal application acceptance. The original Fable x86_64 kernel is an optional isolated WSL/QEMU research lab. Missing WSL/QEMU therefore does not make the normal Jubi host installation fail.

Fable Lab:

```text
http://127.0.0.1:8877/fable.html
```

## Release procedure

1. Merge only after Jubi Production Readiness, Fable Integration, Privileged Broker Security and Windows Installer checks are green.
2. Build the canonical `Jubi-Windows-Installer` artifact from `main`.
3. Verify the provided SHA-256.
4. Clean-install on the actual test laptop.
5. Run `python -m jubi.acceptance --full` and preserve `logs/production-certification.json`.
6. Test dashboard launch, local Ollama chat, persistence, approval/resume, reboot/relaunch and uninstall behavior.
7. For public distribution, sign Jubi application artifacts with the organization certificate and verify the signatures.
8. If a kernel driver is distributed publicly, complete the required Microsoft driver-signing process separately.

## What CI cannot certify

CI cannot truthfully certify:

- the user's physical microphone/camera/audio devices;
- real target GPU/driver performance;
- real local Ollama inference on the user's exact laptop;
- SARA credentials/runtime on that laptop;
- a Microsoft-signed driver binary that has not been supplied;
- a company Authenticode certificate that has not been supplied;
- physical reboot persistence;
- optional Fable QEMU/WSL readiness on the target laptop;
- the full installed `Jubi.exe` launcher behavior until clean-installed on Windows.

Those remain explicit target/certificate-backed gates rather than being marked complete without evidence.
