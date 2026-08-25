# Jubi / SARUS Foundation Test Report

This file replaces the stale SARUS-era hard-coded `17,356` source-count statement.

## Canonical source count

The current expected indexed source count is read from:

```text
BUILD_MANIFEST.json
```

At Jubi v0.1.0 migration time the manifest records:

```text
17,129 indexed original files
10 configured source repositories
```

Tests and acceptance should read the manifest instead of embedding this number in code.

## Automated regression coverage

The active Jubi Phase 0 CI is configured to run combinations of:

- Python compilation for `jubi`, `sarus`, tests and scripts
- `tests/jubi_phase0_test.py`
- `tests/production_readiness_test.py`
- `tests/fable_integration_test.py`
- `tests/integration_test.py`
- `tests/broker_security_test.py`
- `tests/ring0_bridge_test.py`
- Windows PowerShell syntax validation
- Windows installer compilation

The Jubi Phase 0 regression suite specifically adds reopen/restart coverage for memory, events, automations and approval-resume state.

## What CI does not physically certify

A successful GitHub CI run is not proof of the following target-machine behaviors:

- real Ollama inference on the user's Windows laptop
- local GPU performance
- SARA native API runtime
- camera or microphone access
- Windows Hello
- browser/account sessions
- external cloud APIs
- signed kernel-driver activation on the target
- public Authenticode release signing
- physical installer clean-install/uninstall behavior
- optional Fable QEMU/WSL runtime

These require the target Windows validation procedure in:

```text
docs/JUBI_WINDOWS_VALIDATION.md
```

Do not represent target-only capabilities as verified merely because their source code or integration adapter is present.
