param()
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Root = Split-Path -Parent $PSScriptRoot
$Runtime = Join-Path $Root '.sarus-venv\Scripts\pythonw.exe'
$RuntimeConsole = Join-Path $Root '.sarus-venv\Scripts\python.exe'
$Prerequisites = Join-Path $PSScriptRoot 'JUBI-PREREQUISITES.ps1'
$InstallEngine = Join-Path $PSScriptRoot 'INSTALL-SARUS.ps1'
$LogDir = Join-Path $env:LOCALAPPDATA 'Jubi\logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$Log = Join-Path $LogDir 'background-bootstrap.log'

function Log([string]$Message) {
    "[$(Get-Date -Format s)] $Message" | Tee-Object -FilePath $Log -Append | Write-Output
}

function Invoke-Script([string]$Path, [string[]]$Arguments = @()) {
    $PowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $all = @('-NoLogo','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',"`"$Path`"") + $Arguments
    $p = Start-Process -FilePath $PowerShell -ArgumentList $all -WorkingDirectory $Root -Wait -PassThru -WindowStyle Hidden
    return $p.ExitCode
}

try {
    Log 'Jubi background bootstrap started.'
    if (-not (Test-Path -LiteralPath $Prerequisites)) { throw 'JUBI-PREREQUISITES.ps1 is missing.' }

    $fastCode = Invoke-Script $Prerequisites @('-Fast')
    if ($fastCode -ne 0) {
        Log 'Fast prerequisite repair failed; attempting full automatic repair.'
        $fullCode = Invoke-Script $Prerequisites
        if ($fullCode -ne 0) { throw "Automatic prerequisite repair failed with exit code $fullCode." }
    }

    if (-not (Test-Path -LiteralPath $Runtime)) {
        Log 'Private Jubi Python runtime is missing; rebuilding installation runtime automatically.'
        $installCode = Invoke-Script $InstallEngine @('-NonInteractive','-NoLaunch')
        if ($installCode -ne 0) { throw "Private runtime repair failed with exit code $installCode." }
    }
    if (-not (Test-Path -LiteralPath $Runtime)) {
        if (Test-Path -LiteralPath $RuntimeConsole) { $Runtime = $RuntimeConsole }
        else { throw 'Jubi private Python runtime is still missing after repair.' }
    }

    while ($true) {
        Log 'Starting Jubi background supervisor.'
        & $Runtime -m jubi.background
        $code = $LASTEXITCODE
        if ($code -eq 0) {
            Log 'Jubi background supervisor exited normally.'
            exit 0
        }
        if ($code -eq 75) {
            Log 'Jubi was updated successfully; reloading refreshed runtime.'
            Start-Sleep -Seconds 8
            $Runtime = Join-Path $Root '.sarus-venv\Scripts\pythonw.exe'
            if (-not (Test-Path -LiteralPath $Runtime)) { $Runtime = Join-Path $Root '.sarus-venv\Scripts\python.exe' }
            continue
        }
        Log "Jubi background supervisor exited with code $code; retrying in 30 seconds."
        Start-Sleep -Seconds 30
    }
}
catch {
    Log "BACKGROUND BOOTSTRAP FAILED: $($_.Exception.Message)"
    exit 1
}
