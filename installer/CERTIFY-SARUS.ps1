param(
    [switch]$RequireSignedApp,
    [switch]$RequireRing0
)
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
# Legacy physical venv path retained during Jubi Phase 0 compatibility.
$Python = Join-Path $Root '.sarus-venv\Scripts\python.exe'
$LogDir = Join-Path $Root 'logs'
$ReportPath = Join-Path $LogDir 'production-certification.json'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

if (-not (Test-Path -LiteralPath $Python)) {
    throw "Jubi private Python runtime is missing: $Python"
}

Push-Location $Root
try {
    $args = @('-m', 'jubi.acceptance', '--full')
    if ($RequireRing0) { $args += '--require-ring0' }
    $acceptanceText = (& $Python @args 2>&1) -join "`n"
    $acceptanceExit = $LASTEXITCODE
    try { $acceptance = $acceptanceText | ConvertFrom-Json } catch { $acceptance = @{ ok = $false; parse_error = $_.Exception.Message; raw = $acceptanceText } }

    $appPath = Join-Path $Root 'Jubi.exe'
    $appSignature = if (Test-Path -LiteralPath $appPath) { Get-AuthenticodeSignature -LiteralPath $appPath } else { $null }
    $appSigned = $appSignature -and $appSignature.Status -eq 'Valid'

    # The controlled driver keeps its SARUS-era ABI/name during Phase 0 so
    # device paths and signing behavior are not changed as part of a branding migration.
    $driverPath = Join-Path $Root 'driver\SarusRing0\bin\Release\SarusRing0.sys'
    $driverSignature = if (Test-Path -LiteralPath $driverPath) { Get-AuthenticodeSignature -LiteralPath $driverPath } else { $null }
    $driverBundled = Test-Path -LiteralPath $driverPath
    $driverSigned = $driverSignature -and $driverSignature.Status -eq 'Valid'

    $ring0Text = (& $Python -c "from sarus.core.ring0 import Ring0Bridge; import json; print(json.dumps(Ring0Bridge().status()))" 2>&1) -join "`n"
    try { $ring0 = $ring0Text | ConvertFrom-Json } catch { $ring0 = @{ ok = $false; raw = $ring0Text } }

    $requiredFiles = @(
        'Jubi.exe',
        'README.md',
        'BUILD_MANIFEST.json',
        'config\production.json',
        'config\models.json',
        'config\broker_allowlist.json',
        'jubi\server.py',
        'jubi\acceptance.py',
        'sarus\server.py',
        'sarus\core\fable.py',
        'sarus\web\fable.html'
    )
    $missingFiles = @($requiredFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $Root $_)) })

    $coreReady = ($acceptanceExit -eq 0) -and ($acceptance.ok -eq $true) -and ($missingFiles.Count -eq 0)
    $strictReady = $coreReady
    if ($RequireSignedApp) { $strictReady = $strictReady -and [bool]$appSigned }
    if ($RequireRing0) { $strictReady = $strictReady -and [bool]$ring0.ok -and [bool]$driverSigned }

    $report = [ordered]@{
        name = 'Jubi Production Certification'
        version = '0.1.0'
        foundation = 'SARUS 1.3.1'
        generated_at = (Get-Date).ToUniversalTime().ToString('o')
        root = $Root
        core_ready = [bool]$coreReady
        strict_ready = [bool]$strictReady
        public_release_ready = [bool]($coreReady -and $appSigned -and ((-not $driverBundled) -or $driverSigned))
        require_signed_app = [bool]$RequireSignedApp
        require_ring0 = [bool]$RequireRing0
        acceptance = $acceptance
        application_signature = @{
            path = $appPath
            status = if ($appSignature) { [string]$appSignature.Status } else { 'Missing' }
            signer = if ($appSignature -and $appSignature.SignerCertificate) { $appSignature.SignerCertificate.Subject } else { $null }
        }
        ring0 = $ring0
        ring0_driver = @{
            bundled = [bool]$driverBundled
            path = $driverPath
            signature_status = if ($driverSignature) { [string]$driverSignature.Status } else { 'NotBundled' }
            signer = if ($driverSignature -and $driverSignature.SignerCertificate) { $driverSignature.SignerCertificate.Subject } else { $null }
            compatibility_name = 'SarusRing0.sys'
        }
        missing_required_files = $missingFiles
        notes = @(
            'core_ready certifies the installed Jubi application checks on this machine.',
            'public_release_ready also requires a valid Authenticode signature on Jubi.exe and, when a driver binary is bundled, a valid driver signature.',
            'The SarusRing0 driver name is a legacy compatibility ABI retained during Jubi Phase 0.',
            'Original Fable QEMU readiness is reported by Jubi Doctor/Fable status and is optional for the normal Windows host runtime.'
        )
    }

    $report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
    Write-Host "Jubi production certification report: $ReportPath"
    Write-Host "Core ready: $coreReady"
    Write-Host "Public release ready: $($report.public_release_ready)"

    if (-not $strictReady) { exit 2 }
    exit 0
}
finally {
    Pop-Location
}
