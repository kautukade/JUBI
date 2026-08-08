# SARUS Ring0 Bridge v1

This directory contains the Windows kernel-mode component for SARUS.

## What is enabled

SARUS Ring0 is **not globally blocked**. The broker exposes these real kernel-mode calls:

- `ring0.ping` — opens `\\.\SarusRing0` and performs the fixed ping IOCTL.
- `ring0.status` — asks the kernel driver for protocol/capability state, current IRQL and interrupt-time telemetry.

The response is produced by `SarusRing0.sys` in kernel mode and returned to the SARUS broker through `DeviceIoControl`.

## Fixed ABI

The v1 driver accepts only two compile-time IOCTLs:

- `IOCTL_SARUS_RING0_PING`
- `IOCTL_SARUS_RING0_STATUS`

There is deliberately no API that accepts a caller-selected IOCTL number, kernel address, arbitrary pointer, arbitrary physical-memory range, executable path or shell command. Additional Ring0 functions should be added as individually named, validated IOCTLs and broker actions.

## Build prerequisites

Install on the development PC:

1. Visual Studio 2022 or Visual Studio Build Tools with Desktop C++ workload.
2. Windows Driver Kit (WDK) integrated with Visual Studio.
3. A driver-signing certificate trusted by the target Windows machine.

Build from an x64 Developer PowerShell:

```powershell
cd driver\SarusRing0
.\BUILD-RING0.ps1 -Configuration Release
```

Expected output:

```text
driver\SarusRing0\bin\Release\SarusRing0.sys
```

## Driver signing

Modern Windows requires kernel drivers to satisfy Windows code-integrity/signing policy. SARUS does not automatically disable Secure Boot, signature enforcement, Defender or other Windows protections.

Sign `SarusRing0.sys` with a certificate that the target machine accepts, then verify:

```powershell
Get-AuthenticodeSignature .\bin\Release\SarusRing0.sys
```

The status must be `Valid` before `INSTALL-RING0.ps1` will proceed.

## Install and start

Run PowerShell as Administrator:

```powershell
cd driver\SarusRing0
.\INSTALL-RING0.ps1
```

The script copies the signed driver to Windows' drivers directory, creates the `SarusRing0` kernel service and starts it.

The driver device ACL is restricted to `SYSTEM` and Administrators. Run SARUS elevated when you want it to call the Ring0 bridge directly.

## Test through SARUS

With SARUS running, send a typed broker request:

```json
{
  "action_id": "ring0.status",
  "parameters": {}
}
```

or:

```json
{
  "action_id": "ring0.ping",
  "parameters": {}
}
```

A successful response includes `driver_present: true`, protocol version `1`, capability flags, current IRQL, interrupt time and build tag `SARUS-RING0-1`.

## Extending Ring0

Add capabilities one at a time:

1. Define a new fixed IOCTL in `sarus_ring0_shared.h`.
2. Validate all buffer lengths and values in `driver.c`.
3. Add one named method in `sarus/core/ring0.py` (never a generic `raw_ioctl`).
4. Add one typed broker action in `config/broker_allowlist.json` with an explicit risk/approval level.
5. Add Linux policy tests and Windows smoke tests.

This keeps Ring0 available to SARUS while preventing the LLM or HTTP caller from turning the driver into an unrestricted kernel primitive.
