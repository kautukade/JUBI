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

High-risk actions require a short-lived, request-bound approval proof supplied in:

```text
X-SARUS-Approval: v1:<expiry>:<HMAC-SHA256>
```

The proof is cryptographically bound to all of these values:

- request ID
- action ID
- SHA-256 hash of the exact validated parameters
- expiration timestamp

The maximum proof lifetime is 300 seconds. A proof for `process.stop` cannot authorize `service.stop`, another request ID, or modified parameters.

### Protected approval-key storage

`INSTALL-SARUS.bat` and `INSTALL-SARUS-FALLBACK.bat` run `installer/SETUP-BROKER.ps1`. It generates a random approval key outside the repository at:

```text
%LOCALAPPDATA%\SARUS\broker\approval.secret
```

The broker directory has inherited ACLs removed and explicitly grants access to the installing Windows identity and `SYSTEM`. The SARUS runtime loads this file automatically. The approval key therefore does not need to be placed in source code, `.env`, prompts, browser storage, or automation payloads.

For CI or controlled overrides, `SARUS_BROKER_APPROVAL_SECRET` and `SARUS_BROKER_SECRET_FILE` remain supported. Environment values take precedence.

If no secure approval key can be loaded, high-risk broker actions fail closed with `approval_required`.

A JSON field such as `"approved": true` is not authorization and is rejected for privileged legacy requests.

A local helper is included for the trusted operator side:

```text
python scripts/create_broker_approval.py --request-id <REQUEST_ID> --action-id process.stop --parameters-json "{\"resource_id\":\"ollama\"}" --ttl 120
```

The helper automatically uses the protected LocalAppData key and is not exposed through the SARUS HTTP API. In the future native-service phase, approval issuance should move to a separate elevated UI/service identity.

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

Raw file contents, stdout/stderr, passwords, tokens, API keys and authorization values are not copied into broker receipts. Sensitive values are represented by byte length and SHA-256 hash instead.

### Receipt signing-key storage

On Windows the receipt signing key is kept outside the SARUS workspace at:

```text
%LOCALAPPDATA%\SARUS\broker\receipt-signing.key
```

If an earlier SARUS 1.1 development build created `data/receipt-signing.key`, `ReceiptStore` migrates that same key to protected LocalAppData storage and removes the workspace copy. This preserves verification of previously signed receipts while preventing workspace file-read capabilities from exposing the signing key.

For controlled testing, `SARUS_RECEIPT_SIGNING_KEY_FILE` can override the location.

`GET /api/receipts` verifies both the hash chain and signatures for new rows. Older pre-signature rows remain readable and are reported as legacy unsigned receipts.

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

## CI validation

The `Privileged Broker Security` workflow runs both Linux security tests and a Windows broker smoke job. The Windows job provisions the protected broker storage and exercises real read-only `tasklist` and `sc.exe` calls through the typed broker.

## Current certification boundary

This repository upgrade hardens the SARUS Python/API execution path and its Windows installation path. It does not claim that Python itself is a kernel isolation boundary or that a dedicated native SCM broker service has already been certified on the user's physical machine.

For higher assurance on the target Windows machine, the next native hardening layer should run the privileged executor as a dedicated Windows Service with a restricted service SID, use a named-pipe or local RPC ACL, move receipt signing from local HMAC to Windows CNG/TPM, and expose only fixed typed broker handlers. Any future kernel driver must use a small signed KMDF interface with fixed IOCTLs and a broker-only device ACL.

No future native layer should reintroduce arbitrary shell, arbitrary IOCTL, or arbitrary kernel-memory primitives.
