param()
$ErrorActionPreference = 'Stop'

$BrokerDir = Join-Path $env:LOCALAPPDATA 'SARUS\broker'
$SecretFile = Join-Path $BrokerDir 'approval.secret'
New-Item -ItemType Directory -Force -Path $BrokerDir | Out-Null

if (-not (Test-Path -LiteralPath $SecretFile)) {
    $bytes = New-Object byte[] 48
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
    $secret = [Convert]::ToBase64String($bytes)
    [IO.File]::WriteAllText($SecretFile, $secret, (New-Object Text.UTF8Encoding($false)))
}

# Keep the approval key outside the SARUS workspace and remove inherited ACLs.
# The current Windows identity and SYSTEM are the only principals explicitly
# granted access by this setup step.
$identity = [Security.Principal.WindowsIdentity]::GetCurrent().Name
& icacls.exe $BrokerDir /inheritance:r /grant:r "${identity}:(OI)(CI)F" 'SYSTEM:(OI)(CI)F' /T /C | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Could not secure SARUS broker directory (icacls exit $LASTEXITCODE)." }

Write-Host "SARUS broker approval key ready in protected LocalAppData storage."
