param()
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Root = Split-Path -Parent $PSScriptRoot
$Bootstrap = Join-Path $PSScriptRoot 'SETUP-BROKER.ps1'
$Installer = Join-Path $PSScriptRoot 'INSTALL-SARUS.ps1'
$Ring0Installer = Join-Path $Root 'driver\SarusRing0\INSTALL-RING0.ps1'
$Ring0Driver = Join-Path $Root 'driver\SarusRing0\bin\Release\SarusRing0.sys'
$PowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

$LogDir = Join-Path $Root 'logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$Log = Join-Path $LogDir 'exe-install.log'

function Log([string]$Message) {
    "[$(Get-Date -Format s)] $Message" | Tee-Object -FilePath $Log -Append | Write-Host
}

function Invoke-SarusPowerShell([string]$ScriptPath, [string[]]$Arguments = @()) {
    if (-not (Test-Path -LiteralPath $ScriptPath)) {
        throw "Required installer script is missing: $ScriptPath"
    }

    $argumentList = @(
        '-NoLogo',
        '-NoProfile',
        '-NonInteractive',
        '-ExecutionPolicy', 'Bypass',
        '-File', "`"$ScriptPath`""
    ) + $Arguments

    $startParams = @{
        FilePath = $PowerShell
        ArgumentList = $argumentList
        WorkingDirectory = $Root
        Wait = $true
        PassThru = $true
    }
    $process = Start-Process @startParams

    if ($process.ExitCode -ne 0) {
        throw "Installer step failed ($([IO.Path]::GetFileName($ScriptPath))) with exit code $($process.ExitCode)."
    }
}

try {
    if (-not (Test-Path -LiteralPath $PowerShell)) { throw 'Windows PowerShell 5.1 is required.' }
    if (-not (Test-Path -LiteralPath (Join-Path $Root 'sarus\server.py'))) { throw 'SARUS payload is incomplete.' }
    if (-not (Test-Path -LiteralPath (Join-Path $Root 'vendor\launcher\SARUS.exe.b64'))) { throw 'Bundled SARUS launcher payload is missing.' }
    if (-not (Test-Path -LiteralPath (Join-Path $Root 'vendor\launcher\SHA256.txt'))) { throw 'Bundled SARUS launcher checksum is missing.' }

    Log 'SARUS single-EXE installation started.'
    $env:SARUS_INSTALL_MODE = 'exe'

    Log 'Preparing protected privileged-broker storage.'
    Invoke-SarusPowerShell $Bootstrap

    Log 'Installing SARUS core, SARA, source integrations, dependencies and private Python runtime.'
    Invoke-SarusPowerShell $Installer @('-NonInteractive', '-NoLaunch')

    # If a trusted prebuilt Ring0 driver is bundled in a future signed build,
    # activate it automatically. Source-only builds remain fully installable and
    # simply leave the optional kernel bridge inactive until a validly signed
    # SarusRing0.sys is supplied. Windows security enforcement is never disabled.
    if ((Test-Path -LiteralPath $Ring0Driver) -and (Test-Path -LiteralPath $Ring0Installer)) {
        $signature = Get-AuthenticodeSignature -LiteralPath $Ring0Driver
        if ($signature.Status -eq 'Valid') {
            Log 'A validly signed SarusRing0.sys is bundled; installing the controlled Ring0 bridge automatically.'
            Invoke-SarusPowerShell $Ring0Installer @('-DriverPath', "`"$Ring0Driver`"")
        }
        else {
            Log "Ring0 driver payload exists but is not validly signed for this machine (status: $($signature.Status)); kernel driver activation was skipped."
        }
    }
    else {
        Log 'No prebuilt signed Ring0 driver is bundled; controlled Ring0 source is installed but driver activation is skipped.'
    }

    $requiredFinal = @(
        (Join-Path $Root 'SARUS.exe'),
        (Join-Path $Root '.sarus-venv\Scripts\python.exe'),
        (Join-Path $Root 'README.md'),
        (Join-Path $Root 'config\models.json'),
        (Join-Path $Root 'config\broker_allowlist.json'),
        (Join-Path $Root 'sarus\server.py')
    )
    foreach ($path in $requiredFinal) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Post-install verification failed; required file is missing: $path"
        }
    }

    Log 'Post-install verification passed.'
    Log 'Launching SARUS.exe.'
    Start-Process -FilePath (Join-Path $Root 'SARUS.exe') -WorkingDirectory $Root
    Log 'SARUS single-EXE installation completed successfully.'
    exit 0
}
catch {
    Log "INSTALL FAILED: $($_.Exception.Message)"
    Write-Host "SARUS installation failed. See: $Log" -ForegroundColor Red
    exit 1
}
