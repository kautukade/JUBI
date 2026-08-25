# Jubi v0.1.0 — Windows Validation Guide

This checklist validates the Phase 0 migration on the real Windows 10/11 laptop. GitHub CI cannot certify local Ollama, SARA, hardware, signed drivers or installer behavior.

## 1. Work on the migration branch

```powershell
git fetch origin
git checkout agent/jubi-migration-v0.1
git pull origin agent/jubi-migration-v0.1
```

## 2. Confirm Python

```powershell
py -3.11 --version
```

Expected: Python 3.11.x or newer compatible runtime.

## 3. Confirm Ollama

```powershell
ollama list
```

Confirm the local API is available and at least one normal conversational model is installed.

## 4. Run the lightweight Phase 0 tests

From the repository root:

```powershell
py -3.11 tests\jubi_phase0_test.py
```

Expected: all tests pass.

## 5. Run production/static tests

```powershell
py -3.11 tests\production_readiness_test.py
```

## 6. Compile Python

```powershell
py -3.11 -m compileall -q jubi sarus tests scripts
```

No compile errors should be printed.

## 7. Start Jubi from source

```powershell
py -3.11 -m jubi.server
```

Expected terminal line:

```text
Jubi v0.1.0 dashboard: http://127.0.0.1:8877
```

Open:

```text
http://127.0.0.1:8877
```

## 8. Dashboard checks

Confirm:

- brand says JUBI
- Ollama status is ONLINE when Ollama is running
- Models page lists the actual current Ollama models
- Security page opens
- System Health shows Jubi Doctor

## 9. Real local chat

Open **AI & Models** and ask a short question.

Expected:

- response comes from local Ollama
- an unavailable model is not silently selected
- if Ollama is stopped, Jubi returns a useful error instead of crashing

## 10. Memory persistence

In **Knowledge**:

1. save a unique text such as `JUBI-PERSISTENCE-TEST-2026`
2. stop Jubi with Ctrl+C
3. restart with `py -3.11 -m jubi.server`
4. use the memory API or application tooling to verify the record remains in SQLite

Automated reopen coverage is also in `tests/jubi_phase0_test.py`.

## 11. Task persistence

Run a normal pipeline task, restart Jubi and confirm the task remains visible under Recent Tasks.

## 12. Approval/resume behavior

The Phase 0 test suite contains a deterministic high-risk fake step to prove restart/resume behavior without executing a real system action.

For any real approval surfaced in the UI:

- confirm the action/task/step shown is the intended one
- Approve should resume only that exact step
- Reject should not execute it
- a resolved approval should not be usable twice

## 13. Automation persistence

Create a harmless automation with an interval of at least 60 seconds.

Restart Jubi and verify the automation is still listed.

## 14. Windows read-only broker checks

Open **Computer** and run:

- LIST PROCESSES
- LIST SERVICES

Expected: real Windows results returned through typed actions.

Do not test destructive system actions during the initial Phase 0 validation.

## 15. Receipt chain

Open **Security**.

Expected:

```text
Chain: VERIFIED
```

If the chain fails, stop validation and inspect key migration/state before proceeding.

## 16. Full Jubi acceptance

```powershell
py -3.11 -m jubi.acceptance --full
```

This attempts a real local Ollama generation when Ollama is online.

Review every required check; do not treat optional SARA/ECC/Hermes/QEMU results as equivalent to core Jubi failure unless the production profile marks them required.

## 17. Installer test

After the GitHub installer workflow succeeds, use the built `Jubi-Setup.exe` on a test Windows machine or controlled testing laptop.

Expected installation path:

```text
C:\Program Files\Jubi
```

Expected launcher:

```text
Jubi.exe
```

Expected dashboard:

```text
http://127.0.0.1:8877
```

An existing `C:\Program Files\SARUS` installation should not be silently deleted by the Jubi installer.

## 18. Controlled Ring0 compatibility bridge

Ring0 is optional in normal Phase 0 acceptance. If a valid signed legacy `SarusRing0.sys` is not bundled, installation should continue with driver activation skipped.

Do not disable Secure Boot, Code Integrity, HVCI, Defender or Windows driver-signature enforcement to make this test pass.

Strict check only when a correctly signed controlled driver is intentionally being tested:

```powershell
py -3.11 -m jubi.acceptance --full --require-ring0
```

## 19. Report results

For each test record:

```text
PASS / FAIL
command or action
exact error (if any)
Windows version
Python version
Ollama version
models installed
```

Do not proceed to advanced Jubi provider/LAN phases until the core persistence, approvals, local chat and receipt chain are stable on the target laptop.
