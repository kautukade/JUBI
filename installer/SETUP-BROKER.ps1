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

# Keep broker secrets outside the SARUS workspace and remove inherited ACLs.
# The current Windows identity and SYSTEM are the only principals explicitly
# granted access by this setup step.
$identity = [Security.Principal.WindowsIdentity]::GetCurrent().Name
& icacls.exe $BrokerDir /inheritance:r /grant:r "${identity}:(OI)(CI)F" 'SYSTEM:(OI)(CI)F' /T /C | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Could not secure SARUS broker directory (icacls exit $LASTEXITCODE)." }

$check = (Get-Content -LiteralPath $SecretFile -Raw).Trim()
if ($check.Length -lt 24) { throw 'SARUS broker approval key is missing or truncated after provisioning.' }

# Persist only file locations, never secret material. Future SARUS launches can
# reliably locate protected keys regardless of launcher environment details.
[Environment]::SetEnvironmentVariable('SARUS_BROKER_SECRET_FILE', $SecretFile, 'User')
[Environment]::SetEnvironmentVariable('SARUS_RECEIPT_SIGNING_KEY_FILE', $ReceiptKeyFile, 'User')
$env:SARUS_BROKER_SECRET_FILE = $SecretFile
$env:SARUS_RECEIPT_SIGNING_KEY_FILE = $ReceiptKeyFile

Write-Host "SARUS broker key storage ready: $BrokerDir"
