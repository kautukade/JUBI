param()
$ErrorActionPreference = 'Stop'

$BrokerDir = Join-Path $env:LOCALAPPDATA 'SARUS\broker'
$SecretFile = Join-Path $BrokerDir 'approval.secret'
$ReceiptKeyFile = Join-Path $BrokerDir 'receipt-signing.key'
New-Item -ItemType Directory -Force -Path $BrokerDir | Out-Null

if (-not (Test-Path -LiteralPath $SecretFile)) {
    $bytes = New-Object byte[] 48
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
    $secret = [Convert]::ToBase64String($bytes)
    [IO.File]::WriteAllText($SecretFile, $secret, (New-Object Text.UTF8Encoding($false)))
}

# Validate before tightening ACLs so a truncated file never becomes the active key.
$check = (Get-Content -LiteralPath $SecretFile -Raw).Trim()
if ($check.Length -lt 24) { throw 'SARUS broker approval key is missing or truncated after provisioning.' }

# Use the current identity SID rather than a localized/domain-qualified account
# name. This is reliable on consumer Windows, domain PCs and hosted runners.
$currentSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$systemSid = 'S-1-5-18'

# Directory: current user + SYSTEM only, with inheritable full-control ACEs.
& icacls.exe $BrokerDir /inheritance:r /grant:r "*$currentSid`:(OI)(CI)F" "*$systemSid`:(OI)(CI)F" /C | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Could not secure SARUS broker directory (icacls exit $LASTEXITCODE)." }

# Existing approval key: explicit non-inheriting file ACEs. Future receipt key
# files inherit the protected directory ACL; if one already exists, lock it too.
& icacls.exe $SecretFile /inheritance:r /grant:r "*$currentSid`:F" "*$systemSid`:F" /C | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Could not secure SARUS approval key (icacls exit $LASTEXITCODE)." }
if (Test-Path -LiteralPath $ReceiptKeyFile) {
    & icacls.exe $ReceiptKeyFile /inheritance:r /grant:r "*$currentSid`:F" "*$systemSid`:F" /C | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Could not secure SARUS receipt key (icacls exit $LASTEXITCODE)." }
}

# Confirm the installing identity still has access after ACL hardening.
$checkAfterAcl = (Get-Content -LiteralPath $SecretFile -Raw).Trim()
if ($checkAfterAcl.Length -lt 24) { throw 'SARUS broker approval key became unreadable after ACL hardening.' }

# Persist only file locations, never secret material. Future SARUS launches can
# reliably locate protected keys regardless of launcher environment details.
[Environment]::SetEnvironmentVariable('SARUS_BROKER_SECRET_FILE', $SecretFile, 'User')
[Environment]::SetEnvironmentVariable('SARUS_RECEIPT_SIGNING_KEY_FILE', $ReceiptKeyFile, 'User')
$env:SARUS_BROKER_SECRET_FILE = $SecretFile
$env:SARUS_RECEIPT_SIGNING_KEY_FILE = $ReceiptKeyFile

Write-Host "SARUS broker key storage ready: $BrokerDir"
