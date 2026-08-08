# SARUS Privileged Broker v1

SARUS 1.1 introduces a zero-trust privileged action gateway between the local LLM/runtime and Windows system actions.

## Security boundary

The LLM does not receive shell access, arbitrary executable paths, raw driver handles, arbitrary IOCTL forwarding, kernel-memory access, or a direct privileged Windows API surface.

Requests use typed logical action IDs, for example:

```json
{
  "schema": "sarus.controlbridge.action.v1",
  "request_id": "optional-client-id",
  "timestamp": "2026-08-08T17:00:00Z",
  "nonce": "unique-value",
  "action_id": "service.query",
  "parameters": {
    "resource_id": "ollama"
  },
  "reason": "Check local model service health"
}
```

The broker resolves `resource_id` through `config/broker_allowlist.json`. The caller never supplies an SCM service name, executable path, command line, driver path, or kernel address.

## Default deny

`config/broker_allowlist.json` is the source of truth for broker-visible actions and resources. Unknown actions, unknown request fields, unknown parameters, and unknown resources are denied.

Permanently forbidden examples include:

- arbitrary PowerShell / cmd / shell execution
- arbitrary executable launch
- raw driver loading
- raw IOCTL forwarding
- kernel memory read/write
- physical memory mapping
- disabling audit/security controls

The legacy Windows executor names `powershell`, `service_control`, `stop_process`, and `open_app` are blocked even if a caller supplies `approved=true`.

## Approval model

High-risk broker actions require an out-of-band approval proof in the HTTP header:

```text
X-SARUS-Approval: <secret>
```

The secret is read only from the SARUS process environment variable:

```text
SARUS_BROKER_APPROVAL_SECRET
```

Use a random value of at least 24 characters. Do not place it in prompts, model context, repository files, browser localStorage, logs, receipts, or automation payloads.

If the environment variable is not configured, high-risk broker actions fail closed with `approval_required`.

A JSON field such as `"approved": true` is not authorization and is rejected for privileged legacy requests.

## Action receipts

Every broker attempt creates a receipt, including denied and invalid attempts.

New receipts contain:

- request ID
- action ID
- status
- risk level where applicable
- allowlisted resource ID
- sanitized parameters/result
- previous receipt hash
- current SHA-256 receipt hash
- HMAC-SHA256 authentication value
- signing key ID

The receipt chain is stored in the SARUS SQLite database. The local receipt key is generated at first run in:

```text
data/receipt-signing.key
```

The key is not stored inside the SQLite receipt row.

`GET /api/receipts` verifies both the hash chain and signatures for new rows. Older pre-1.1 rows remain readable and are reported as legacy unsigned receipts.

## Replay protection

Broker requests support timestamp, request ID and nonce validation. The default replay window is 300 seconds. A successfully executed request ID or nonce cannot be reused within the replay window.

## API

Broker status:

```text
GET /api/broker
```

Typed system action:

```text
POST /api/system/action
```

Example low-risk action:

```json
{
  "action_id": "system.processes.list",
  "parameters": {}
}
```

Example allowlisted workspace read:

```json
{
  "action_id": "workspace.file.read",
  "parameters": {
    "path": "C:\\path\\to\\SARUS\\README.md"
  }
}
```

## Windows execution layer

`sarus/core/windows.py` is now a typed executor. It invokes fixed Windows tools with `shell=False` and receives sensitive resource names only after broker allowlist resolution.

Implemented typed operations:

- `system.processes.list`
- `system.services.list`
- `workspace.file.read`
- `workspace.file.write`
- `url.open`
- `service.query`
- `service.start`
- `service.stop`
- `process.stop`

The default allowlist currently contains only the Ollama logical service/process mapping for privileged operations. Add additional resources explicitly rather than accepting user-supplied names.

## Current certification boundary

This repository upgrade hardens the SARUS Python/API execution path. It does not claim that Python itself is a kernel isolation boundary.

For higher assurance on the target Windows machine, the next native hardening layer should run the executor as a dedicated Windows Service with a restricted service SID, use a named-pipe or local RPC ACL, move receipt signing to Windows CNG/TPM, and expose only fixed typed broker handlers. Any future kernel driver must use a small signed KMDF interface with fixed IOCTLs and a broker-only device ACL.

No future native layer should reintroduce arbitrary shell, arbitrary IOCTL, or arbitrary kernel-memory primitives.
