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

function Test-PrivateRuntime {
    if (-not (Test-Path -LiteralPath $RuntimeConsole)) { return $false }
    try {
        Push-Location $Root
        try {
            & $RuntimeConsole -c "import sys, jubi, sarus; raise SystemExit(0 if sys.version_info[:2] == (3, 11) else 3)" *> $null
            return ($LASTEXITCODE -eq 0)
        }
        finally { Pop-Location }
    }
    catch { return $false }
}

function Repair-CoreRuntime {
    Log 'Running automatic prerequisite repair.'
    $repairCode = Invoke-Script $Prerequisites @('-Repair')
    if ($repairCode -ne 0) { throw "Automatic prerequisite repair failed with exit code $repairCode." }

    if (-not (Test-PrivateRuntime)) {
        Log 'Private Jubi Python runtime is missing or unhealthy; rebuilding it automatically.'
        $installCode = Invoke-Script $InstallEngine @('-NonInteractive','-NoLaunch','-RepairMode')
        if ($installCode -ne 0) { throw "Private runtime repair failed with exit code $installCode." }
    }
    if (-not (Test-PrivateRuntime)) { throw 'Jubi private Python runtime is still unhealthy after repair.' }
}

try {
    Log 'Jubi background bootstrap started.'
    if (-not (Test-Path -LiteralPath $Prerequisites)) { throw 'JUBI-PREREQUISITES.ps1 is missing.' }

    $fastCode = Invoke-Script $Prerequisites @('-Fast')
    if ($fastCode -ne 0) {
        Log 'Fast prerequisite health check failed; entering automatic repair mode.'
        Repair-CoreRuntime
    }
    elseif (-not (Test-PrivateRuntime)) {
        Log 'Prerequisites are healthy but the private runtime is not; repairing runtime automatically.'
        Repair-CoreRuntime
    }

    if (-not (Test-Path -LiteralPath $Runtime)) {
        if (Test-Path -LiteralPath $RuntimeConsole) { $Runtime = $RuntimeConsole }
        else { throw 'Jubi private Python runtime is missing after repair.' }
    }

    $consecutiveSupervisorFailures = 0
    while ($true) {
        Log 'Starting Jubi background supervisor.'
        & $Runtime -m jubi.background
        $code = $LASTEXITCODE
        if ($code -eq 0) {
            Log 'Jubi background supervisor exited normally.'
            exit 0
        }
        if ($code -eq 75) {
            Log 'Jubi was updated successfully; validating and reloading refreshed runtime.'
            Start-Sleep -Seconds 8
            if (-not (Test-PrivateRuntime)) { Repair-CoreRuntime }
            $Runtime = Join-Path $Root '.sarus-venv\Scripts\pythonw.exe'
            if (-not (Test-Path -LiteralPath $Runtime)) { $Runtime = $RuntimeConsole }
            $consecutiveSupervisorFailures = 0
            continue
        }

        $consecutiveSupervisorFailures++
        Log "Jubi background supervisor exited with code $code; failure count=$consecutiveSupervisorFailures."
        if ($consecutiveSupervisorFailures -ge 3) {
            try {
                Log 'Repeated supervisor failures detected; running automatic repair before retry.'
                Repair-CoreRuntime
                $consecutiveSupervisorFailures = 0
            }
            catch {
                Log "Automatic background repair failed: $($_.Exception.Message)"
            }
        }
        Start-Sleep -Seconds 30
    }
}
catch {
    Log "BACKGROUND BOOTSTRAP FAILED: $($_.Exception.Message)"
    exit 1
}
