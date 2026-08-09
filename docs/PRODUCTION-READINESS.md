# SARUS v1.3.1 Production Readiness

## Purpose

SARUS v1.3.1 is the stabilization release after the Fable v1.3 integration. This document separates what can be proven in CI from what must be certified on the real Windows research laptop or through an external code-signing authority.

## Release gates

A source revision is release-candidate ready only when all of these pass:

1. Python compile checks.
2. Production-readiness static tests.
3. All 10 source-adapter integration/regression tests.
4. Fable integration tests on Windows and Linux.
5. Privileged Broker and controlled Ring0 policy tests.
6. Windows PowerShell syntax validation for installer/certification/driver scripts.
7. Inno Setup compilation of `SARUS-Setup.exe`.
8. SHA-256 generation for the final EXE artifact.

The build workflow uses read-only repository permissions.

## Fixed in v1.3.1

### Manifest-driven acceptance

`sarus.acceptance` reads `BUILD_MANIFEST.json` and `config/production.json`. It no longer contains the legacy `17,356` file-count assertion. The reproducible current clean-checkout count is defined only in the build manifest.

### Explicit Ollama model provisioning

During the official EXE install, `SARUS_INSTALL_MODE=exe` causes acceptance to:

- verify the local Ollama service;
- attempt to start `ollama serve` when the executable is present but the service is offline;
- download any missing required local models using Ollama's local `/api/pull` endpoint;
- verify all required model names after provisioning;
- fail installation rather than silently completing with a missing required model.

Required models are defined in `config/production.json`, not duplicated in the installer.

### Target-machine certification report

`installer/CERTIFY-SARUS.ps1` runs the full acceptance suite on the installed machine and writes:

```text
C:\Program Files\SARUS\logs\production-certification.json
```

It reports:

- core acceptance result;
- required file completeness;
- application Authenticode status;
- controlled Ring0 runtime status;
- bundled driver signature status;
- Doctor/model/Fable status.

The default internal certification does not pretend an unsigned internal build is a publicly signed release. Use `-RequireSignedApp` and/or `-RequireRing0` for stricter certification.

### Production workflow cleanup

Old one-time source-transfer/materialization workflows, trigger marker files and stale source-count status files were removed from the release tree. These workflows were not runtime features and are not needed to install or operate SARUS.

Active release workflows should not have repository write permission and should not post source status to transient external endpoints.

## Production status levels

### CI release-candidate ready

Means source, security, integration and installer build gates pass in GitHub Actions.

### Windows target certified

Means the generated installer was run on the actual target laptop and `production-certification.json` reports `core_ready: true`.

This cannot be inferred from cloud CI because camera/audio hardware, SARA native integration, local Ollama state, device drivers and optional WSL/QEMU availability depend on the target laptop.

### Public release signed

Means the application installer/launcher have valid organization Authenticode signatures and any bundled kernel driver satisfies current Microsoft kernel-driver signing requirements.

`installer/SIGN-RELEASE.ps1` supports normal SHA-256 Authenticode signing and RFC3161 timestamping for the application launcher/installer when an organization certificate is available. No private key is stored in the repository.

The controlled `SarusRing0.sys` public distribution path is separate. A locally built `.sys` is not treated as publicly production-signed merely because it was compiled successfully.

## Ring0 release behavior

The normal SARUS application remains installable without an active kernel driver. When a prebuilt driver is bundled, the EXE bootstrap checks Windows Authenticode status and activates it only when Windows reports the signature as valid.

No release script disables Secure Boot, Code Integrity, Defender or driver-signature enforcement.

For a research laptop where controlled Ring0 is required, run:

```powershell
& "C:\Program Files\SARUS\installer\CERTIFY-SARUS.ps1" -RequireRing0
```

A public signed application certification can be requested with:

```powershell
& "C:\Program Files\SARUS\installer\CERTIFY-SARUS.ps1" -RequireSignedApp
```

## Fable production boundary

The SARUS-native Fable Intelligence Layer is part of the normal application acceptance. The original Fable x86_64 kernel remains an optional isolated QEMU research lab. Missing WSL/QEMU therefore does not make the normal SARUS host installation fail.

When Fable QEMU research is required, install the upstream-compatible WSL/Linux toolchain and verify readiness from:

```text
http://127.0.0.1:8877/fable.html
```

## Release procedure

1. Merge only after Production Readiness, Fable Integration, Privileged Broker Security and Windows Installer checks are green.
2. Obtain the `SARUS-Windows-Installer` artifact from the `main` workflow.
3. Verify its SHA-256 file.
4. For public distribution, sign the application artifacts with the company signing certificate and verify them.
5. If a kernel driver is bundled publicly, complete the Microsoft driver-signing process and verify the returned driver/package on the target Windows version.
6. Clean-install on the actual test laptop.
7. Preserve `logs/production-certification.json` as the hardware acceptance record.
8. Test launch, reboot, update/reinstall and uninstall behavior.

## What CI cannot certify

CI cannot truthfully certify:

- the user's physical microphone/camera/audio devices;
- target GPU/driver behavior;
- a Microsoft-signed Ring0 binary that has not been supplied;
- a company Authenticode certificate that has not been supplied;
- physical Windows reboot persistence;
- optional Fable QEMU/WSL hardware/runtime readiness on the target laptop.

Those are represented explicitly as target/certificate-backed gates rather than being marked complete without evidence.
