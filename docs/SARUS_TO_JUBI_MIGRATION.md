# SARUS v1.3.1 → Jubi v0.1.0 Migration

## Purpose

Jubi is not a clean-room rewrite. Jubi v0.1.0 deliberately evolves the existing SARUS v1.3.1 foundation so the mature local Ollama, source-adapter, Fable, receipt, broker, installer and controlled-driver work is not discarded.

## Migration strategy

Phase 0 follows a compatibility-first strategy:

```text
SARUS v1.3.1
  -> repair confirmed persistence/state bugs
  -> add resumable task execution
  -> harden model discovery
  -> move product identity to Jubi
  -> keep high-risk legacy ABI/path names temporarily
  -> add Jubi entry points and installer artifact
  -> run old + new regression gates
```

## Product identity

New product metadata:

```text
Name: Jubi
Version: 0.1.0
Foundation: SARUS 1.3.1
Dashboard: http://127.0.0.1:8877
Installer artifact: Jubi-Setup.exe
Launcher: Jubi.exe
```

## Python package strategy

The runtime implementation still lives under:

```text
sarus/
```

for Phase 0 compatibility.

New user-facing entry package:

```text
jubi/
```

provides:

```powershell
python -m jubi.server
python -m jubi.acceptance
```

`Jubi` is the canonical runtime class. `Sarus = Jubi` remains as a temporary import alias for existing tests/install scripts.

A later refactor may move internal modules physically under `jubi/` after the Windows target and installer are stable. Phase 0 intentionally avoids a mass source move that would obscure functional regressions.

## Database

The current physical database path remains:

```text
data/sarus.db
```

This is intentionally retained for the first migration release to preserve existing user state and all Fable/receipt relationships. The runtime property `app.db_path` is now the canonical reference for new core code.

Data repaired/preserved includes:

- memories
- events
- tasks
- approvals
- task_state
- automations
- receipts
- Fable traces
- learned capabilities
- Fable agenda

A future physical rename to `jubi.db` should be done with a target-tested backup/migration step, not a cosmetic deletion/rename in this Phase 0 PR.

## Environment variables

New server configuration prefers:

```text
JUBI_HOST
JUBI_PORT
JUBI_DEBUG
JUBI_HTTP_LOG
```

Legacy fallbacks remain accepted:

```text
SARUS_HOST
SARUS_PORT
SARUS_DEBUG
SARUS_HTTP_LOG
```

The installer sets `JUBI_INSTALL_MODE=exe` and also the legacy `SARUS_INSTALL_MODE=exe` until remaining helper scripts are migrated.

Protected broker/receipt environment names and locations remain SARUS-era compatibility identifiers during Phase 0 because they affect existing key identity and receipt-chain verification.

## Installer

The existing source definition filename remains:

```text
installer/SARUS-Setup.iss
```

but its product metadata now builds:

```text
Jubi-Setup.exe
```

with installation directory:

```text
C:\Program Files\Jubi
```

and executable:

```text
Jubi.exe
```

The installer uses a new AppId so a legacy SARUS installation is not automatically replaced as the same Windows product.

The Phase 0 bootstrap still calls legacy helper filenames such as `INSTALL-SARUS.ps1` and `CERTIFY-SARUS.ps1`. Those are implementation compatibility names and can be migrated later after clean-install validation.

## Launcher compatibility

The repository currently contains a verified base64 launcher payload named for SARUS. The Phase 0 installer reconstructs and verifies that payload, then creates `Jubi.exe` as a byte-identical copy and confirms both SHA-256 hashes match.

This avoids inventing an untested launcher binary during a stabilization migration. A rebuilt Jubi-native launcher can replace this in a later release.

## Controlled Ring0 driver

Driver path and ABI remain:

```text
driver/SarusRing0/
\\.\SarusRing0
```

This is intentional. Renaming a Windows kernel device/driver is a compatibility, packaging and signing change rather than ordinary branding.

The Jubi user-facing broker continues to expose only the existing fixed compatibility capabilities:

```text
ring0.ping
ring0.status
```

No arbitrary kernel-memory or raw IOCTL capability is added by this migration.

## Source repositories

The ten configured source families and pinned upstream SHAs remain unchanged. The build manifest source count remains manifest-driven rather than being changed because the product name changed.

## API compatibility

Existing endpoint paths are preserved:

```text
/api/status
/api/models
/api/chat
/api/tasks
/api/approvals
/api/approval
/api/system/action
...
```

State-changing HTTP calls now use `X-JUBI-Token` in the dashboard. The server temporarily accepts the legacy `X-SARUS-Token` header for compatibility.

For privileged proof, `X-JUBI-Approval` is accepted and the legacy `X-SARUS-Approval` remains a fallback.

## Tests and CI

New Phase 0 test:

```text
tests/jubi_phase0_test.py
```

covers:

- memory reopen persistence
- event reopen persistence
- automation reopen persistence
- missing-model fallback
- embedding role safety
- approval persistence across engine restart
- exact-step approval resume
- rejection safety
- approval replay rejection

Existing production, Fable, integration, broker and Ring0 tests remain part of the CI gates.

## Intentionally deferred migrations

The following are not required to be renamed in Phase 0:

- full `sarus/` package tree
- `.sarus-venv`
- protected SARUS broker key directory
- driver `SarusRing0`
- historical SARUS reports/docs
- pinned source folder wrapper names
- legacy installer helper script filenames

They should only be migrated when the change has a functional reason and a dedicated compatibility test.

## Next migration boundary

Before moving more internal names, validate on the actual Windows target:

1. Jubi dashboard startup
2. real Ollama chat
3. memory persistence
4. task persistence
5. approval/resume
6. automation persistence
7. receipt-chain verification
8. process/service read-only broker
9. Jubi-Setup clean install
10. uninstall behavior

After these pass, the project can safely proceed to advanced Jubi feature phases.
