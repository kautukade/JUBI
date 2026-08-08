param()
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Root = Split-Path -Parent $PSScriptRoot
$Bootstrap = Join-Path $PSScriptRoot 'SETUP-BROKER.ps1'
$Installer = Join-Path $PSScriptRoot 'INSTALL-SARUS.ps1'

if (-not (Test-Path -LiteralPath $Bootstrap)) { throw 'SETUP-BROKER.ps1 is missing.' }
if (-not (Test-Path -LiteralPath $Installer)) { throw 'INSTALL-SARUS.ps1 is missing.' }
if (-not (Test-Path -LiteralPath (Join-Path $Root 'sarus\server.py'))) { throw 'SARUS payload is incomplete.' }

# The Inno Setup EXE is already elevated. Keep the existing installer engine
# unchanged, but replace its final Read-Host pauses so GUI installation can
# complete without waiting for an invisible console prompt.
function global:Read-Host {
    param([string]$Prompt)
    Write-Host $Prompt
    return ''
}

Write-Host 'Preparing SARUS protected broker storage...'
& $Bootstrap
if ($LASTEXITCODE -ne 0) { throw "Broker setup failed with exit code $LASTEXITCODE" }

$env:SARUS_INSTALL_MODE = 'exe'
Write-Host 'Starting SARUS installation engine...'
. $Installer
