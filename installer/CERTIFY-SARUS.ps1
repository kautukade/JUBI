param(
    [switch]$RequireSignedApp,
    [switch]$RequireRing0
)
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$Python = Join-Path $Root '.sarus-venv\Scripts\python.exe'
$LogDir = Join-Path $Root 'logs'
$ReportPath = Join-Path $LogDir 'production-certification.json'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

if (-not (Test-Path -LiteralPath $Python)) {
    throw "SARUS private Python runtime is missing: $Python"
}

Push-Location $Root
try {
    $args = @('-m', 'sarus.acceptance', '--full')
    if ($RequireRing0) { $args += '--require-ring0' }
    $acceptanceText = (& $Python @args 2>&1) -join "`n"
    $acceptanceExit = $LASTEXITCODE
    try { $acceptance = $acceptanceText | ConvertFrom-Json } catch { $acceptance = @{ ok = $false; parse_error = $_.Exception.Message; raw = $acceptanceText } }

    $appPath = Join-Path $Root 'SARUS.exe'
    $appSignature = if (Test-Path -LiteralPath $appPath) { Get-AuthenticodeSignature -LiteralPath $appPath } else { $null }
    $appSigned = $appSignature -and $appSignature.Status -eq 'Valid'

    $driverPath = Join-Path $Root 'driver\SarusRing0\bin\Release\SarusRing0.sys'
    $driverSignature = if (Test-Path -LiteralPath $driverPath) { Get-AuthenticodeSignature -LiteralPath $driverPath } else { $null }
    $driverBundled = Test-Path -LiteralPath $driverPath
    $driverSigned = $driverSignature -and $driverSignature.Status -eq 'Valid'

    $ring0Text = (& $Python -c "from sarus.core.ring0 import Ring0Bridge; import json; print(json.dumps(Ring0Bridge().status()))" 2>&1) -join "`n"
    try { $ring0 = $ring0Text | ConvertFrom-Json } catch { $ring0 = @{ ok = $false; raw = $ring0Text } }

    $requiredFiles = @(
        'SARUS.exe',
        'README.md',
        'BUILD_MANIFEST.json',
        'config\production.json',
        'config\models.json',
        'config\broker_allowlist.json',
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
        name = 'SARUS Production Certification'
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
        }
        missing_required_files = $missingFiles
        notes = @(
            'core_ready certifies the installed SARUS application checks on this machine.',
            'public_release_ready also requires a valid Authenticode application signature and, when a driver binary is bundled, a valid driver signature.',
            'Original Fable QEMU readiness is reported by SARUS Doctor/Fable status and is optional for the normal Windows host runtime.'
        )
    }

    $report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
    Write-Host "SARUS production certification report: $ReportPath"
    Write-Host "Core ready: $coreReady"
    Write-Host "Public release ready: $($report.public_release_ready)"

    if (-not $strictReady) { exit 2 }
    exit 0
}
finally {
    Pop-Location
}
