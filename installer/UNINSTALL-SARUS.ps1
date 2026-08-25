param(
    [switch]$PurgeUserData,
    [switch]$RemoveLegacyRing0,
    [switch]$PurgeSharedLegacyBrokerData
)
$ErrorActionPreference = 'SilentlyContinue'

# Phase 0 keeps this legacy helper filename, but it is invoked by the Jubi
# installer. Stop only Jubi.exe so a separate SARUS installation is not killed.
& taskkill.exe /IM Jubi.exe /F *> $null

# SarusRing0 is a shared legacy compatibility ABI. A Jubi uninstall must not
# silently remove it because an existing SARUS installation may still use the
# same service/device. Removal is therefore explicit only.
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

# Inno Setup removes the Jubi installation directory itself. Protected broker
# keys are deliberately shared with the SARUS-era receipt identity in Phase 0,
# so the normal uninstall preserves them. This avoids invalidating receipt
# chains or breaking a separate legacy installation.
if ($PurgeUserData) {
    $JubiData = Join-Path $env:LOCALAPPDATA 'Jubi'
    Remove-Item -LiteralPath $JubiData -Recurse -Force -ErrorAction SilentlyContinue
    [Environment]::SetEnvironmentVariable('JUBI_HOST', $null, 'User')
    [Environment]::SetEnvironmentVariable('JUBI_PORT', $null, 'User')
    [Environment]::SetEnvironmentVariable('JUBI_DEBUG', $null, 'User')
}

if ($PurgeSharedLegacyBrokerData) {
    $BrokerDir = Join-Path $env:LOCALAPPDATA 'SARUS\broker'
    Remove-Item -LiteralPath $BrokerDir -Recurse -Force -ErrorAction SilentlyContinue
    [Environment]::SetEnvironmentVariable('SARUS_BROKER_SECRET_FILE', $null, 'User')
    [Environment]::SetEnvironmentVariable('SARUS_RECEIPT_SIGNING_KEY_FILE', $null, 'User')
}

exit 0
