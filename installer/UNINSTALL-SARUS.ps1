param(
    [switch]$PurgeUserData,
    [switch]$RemoveLegacyRing0,
    [switch]$PurgeSharedLegacyBrokerData
)
$ErrorActionPreference = 'SilentlyContinue'

$Root = Split-Path -Parent $PSScriptRoot
$UnregisterBackground = Join-Path $PSScriptRoot 'UNREGISTER-JUBI-BACKGROUND.ps1'
$PowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

if ((Test-Path -LiteralPath $UnregisterBackground) -and (Test-Path -LiteralPath $PowerShell)) {
    & $PowerShell -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $UnregisterBackground *> $null
}

# Stop the user-facing launcher and any Jubi runtime process that belongs to this
# installation tree. Do not terminate unrelated system Python processes.
& taskkill.exe /IM Jubi.exe /F *> $null
Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
    ($_.CommandLine -match '(?i)-m\s+jubi\.(background|server)') -and
    ($_.ExecutablePath -like "$Root*")
} | ForEach-Object {
    Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
}

# SarusRing0 is a shared legacy compatibility ABI. A normal Jubi uninstall must
# not silently remove it because a legacy installation may share the same ABI.
if ($RemoveLegacyRing0) {
    & sc.exe query SarusRing0 *> $null
    if ($LASTEXITCODE -eq 0) {
        & sc.exe stop SarusRing0 *> $null
        Start-Sleep -Milliseconds 500
        & sc.exe delete SarusRing0 *> $null
        Start-Sleep -Milliseconds 500
    }
    $Ring0Driver = Join-Path $env:SystemRoot 'System32\drivers\SarusRing0.sys'
    Remove-Item -LiteralPath $Ring0Driver -Force -ErrorAction SilentlyContinue
}

if ($PurgeUserData) {
    $JubiData = Join-Path $env:LOCALAPPDATA 'Jubi'
    Remove-Item -LiteralPath $JubiData -Recurse -Force -ErrorAction SilentlyContinue
    [Environment]::SetEnvironmentVariable('JUBI_HOST', $null, 'User')
    [Environment]::SetEnvironmentVariable('JUBI_PORT', $null, 'User')
    [Environment]::SetEnvironmentVariable('JUBI_DEBUG', $null, 'User')
    [Environment]::SetEnvironmentVariable('JUBI_AUTO_UPDATE', $null, 'User')
}

if ($PurgeSharedLegacyBrokerData) {
    $BrokerDir = Join-Path $env:LOCALAPPDATA 'SARUS\broker'
    Remove-Item -LiteralPath $BrokerDir -Recurse -Force -ErrorAction SilentlyContinue
    [Environment]::SetEnvironmentVariable('SARUS_BROKER_SECRET_FILE', $null, 'User')
    [Environment]::SetEnvironmentVariable('SARUS_RECEIPT_SIGNING_KEY_FILE', $null, 'User')
}

exit 0
