param(
    [Parameter(Mandatory=$true)][string]$CertificateThumbprint,
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [string]$InstallerPath = '',
    [switch]$SkipLauncher
)
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
if (-not $InstallerPath) { $InstallerPath = Join-Path $Root 'dist-installer\Jubi-Setup.exe' }
$LauncherPath = Join-Path $Root 'Jubi.exe'

$signTool = Get-Command signtool.exe -ErrorAction SilentlyContinue
if (-not $signTool) {
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (Test-Path -LiteralPath $kits) {
        $candidate = Get-ChildItem -LiteralPath $kits -Filter signtool.exe -File -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($candidate) { $signTool = $candidate }
    }
}
if (-not $signTool) { throw 'signtool.exe was not found. Install the Windows SDK signing tools.' }
$signToolPath = if ($signTool.Source) { $signTool.Source } else { $signTool.FullName }

function Sign-And-Verify([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { throw "Release file not found: $Path" }
    & $signToolPath sign /sha1 $CertificateThumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 $Path
    if ($LASTEXITCODE -ne 0) { throw "SignTool signing failed for $Path" }
    & $signToolPath verify /pa /v $Path
    if ($LASTEXITCODE -ne 0) { throw "SignTool verification failed for $Path" }
    $sig = Get-AuthenticodeSignature -LiteralPath $Path
    if ($sig.Status -ne 'Valid') { throw "Windows does not report a valid Authenticode signature for $Path (status: $($sig.Status))." }
    Write-Host "Signed and verified: $Path" -ForegroundColor Green
}

if (-not $SkipLauncher) { Sign-And-Verify $LauncherPath }
Sign-And-Verify $InstallerPath

Write-Host 'Jubi application release signing complete.' -ForegroundColor Green
Write-Host 'The legacy compatibility driver SarusRing0.sys is intentionally not signed by this script. Public Windows kernel drivers must follow Microsoft Hardware Dev Center / Windows driver signing requirements.'
