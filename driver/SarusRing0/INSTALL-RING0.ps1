param(
    [string]$DriverPath = (Join-Path $PSScriptRoot 'bin\Release\SarusRing0.sys')
)
$ErrorActionPreference = 'Stop'

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-IsAdmin)) {
    throw 'Run this script from an elevated PowerShell window (Run as administrator).'
}
if (-not (Test-Path -LiteralPath $DriverPath)) {
    throw "Driver not found: $DriverPath"
}

$signature = Get-AuthenticodeSignature -LiteralPath $DriverPath
if ($signature.Status -ne 'Valid') {
    throw "SarusRing0.sys is not validly signed for this machine (status: $($signature.Status)). Sign/trust the driver first; this installer will not disable Windows signature enforcement."
}

$Target = Join-Path $env:SystemRoot 'System32\drivers\SarusRing0.sys'
Copy-Item -LiteralPath $DriverPath -Destination $Target -Force

& sc.exe query SarusRing0 *> $null
if ($LASTEXITCODE -eq 0) {
    & sc.exe stop SarusRing0 *> $null
    Start-Sleep -Milliseconds 500
    & sc.exe delete SarusRing0 *> $null
    Start-Sleep -Milliseconds 500
}

& sc.exe create SarusRing0 type= kernel start= demand error= normal binPath= 'System32\drivers\SarusRing0.sys' DisplayName= 'SARUS Ring0 Bridge'
if ($LASTEXITCODE -ne 0) { throw "Could not create SarusRing0 kernel service (sc.exe exit $LASTEXITCODE)." }

& sc.exe start SarusRing0
if ($LASTEXITCODE -ne 0) {
    & sc.exe delete SarusRing0 *> $null
    throw "SarusRing0 driver did not start (sc.exe exit $LASTEXITCODE). Check Windows Code Integrity/Event Viewer and driver signing."
}

Write-Host 'SARUS Ring0 driver is installed and running.' -ForegroundColor Green
Write-Host 'Test from SARUS with action_id ring0.status or ring0.ping.'
