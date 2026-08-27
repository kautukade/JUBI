param(
    [switch]$UpdateMode
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Root = Split-Path -Parent $PSScriptRoot
$Prerequisites = Join-Path $PSScriptRoot 'JUBI-PREREQUISITES.ps1'
$Bootstrap = Join-Path $PSScriptRoot 'SETUP-BROKER.ps1'
$Installer = Join-Path $PSScriptRoot 'INSTALL-SARUS.ps1'
$Certifier = Join-Path $PSScriptRoot 'CERTIFY-SARUS.ps1'
$RegisterBackground = Join-Path $PSScriptRoot 'REGISTER-JUBI-BACKGROUND.ps1'
$Ring0Installer = Join-Path $Root 'driver\SarusRing0\INSTALL-RING0.ps1'
$Ring0Driver = Join-Path $Root 'driver\SarusRing0\bin\Release\SarusRing0.sys'
$PowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

$LogDir = Join-Path $Root 'logs'
$ChildLogDir = Join-Path $env:LOCALAPPDATA 'Jubi\logs\installer-steps'
New-Item -ItemType Directory -Force -Path $LogDir,$ChildLogDir | Out-Null
$Log = Join-Path $LogDir 'exe-install.log'

function Log([string]$Message) {
    "[$(Get-Date -Format s)] $Message" | Tee-Object -FilePath $Log -Append | Write-Host
}

function Append-ChildLog([string]$Name, [string]$Path, [string]$StreamName) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    foreach ($line in (Get-Content -LiteralPath $Path -ErrorAction SilentlyContinue)) {
        if (-not [string]::IsNullOrWhiteSpace([string]$line)) { Log "[$Name/$StreamName] $line" }
    }
}

function Invoke-JubiPowerShell([string]$ScriptPath, [string[]]$Arguments = @(), [string]$AttemptTag = '') {
    if (-not (Test-Path -LiteralPath $ScriptPath)) { throw "Required installer script is missing: $ScriptPath" }
    $name = [IO.Path]::GetFileNameWithoutExtension($ScriptPath)
    $suffix = if ([string]::IsNullOrWhiteSpace($AttemptTag)) { '' } else { '-' + $AttemptTag }
    $stdout = Join-Path $ChildLogDir "$name$suffix.stdout.log"
    $stderr = Join-Path $ChildLogDir "$name$suffix.stderr.log"
    Remove-Item -LiteralPath $stdout,$stderr -Force -ErrorAction SilentlyContinue

    $argumentList = @('-NoLogo','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',"`"$ScriptPath`"") + $Arguments
    $process = Start-Process -FilePath $PowerShell -ArgumentList $argumentList -WorkingDirectory $Root -Wait -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    Append-ChildLog $name $stdout 'stdout'
    Append-ChildLog $name $stderr 'stderr'
    if ($process.ExitCode -ne 0) {
        $detail = ''
        if (Test-Path -LiteralPath $stderr) {
            $tail = Get-Content -LiteralPath $stderr -Tail 8 -ErrorAction SilentlyContinue
            if ($tail) { $detail = (($tail | ForEach-Object { $_.Trim() }) -join ' | ') }
        }
        if (-not $detail -and (Test-Path -LiteralPath $stdout)) {
            $tail = Get-Content -LiteralPath $stdout -Tail 8 -ErrorAction SilentlyContinue
            if ($tail) { $detail = (($tail | ForEach-Object { $_.Trim() }) -join ' | ') }
        }
        if ($detail) { throw "Installer step failed ($([IO.Path]::GetFileName($ScriptPath))) with exit code $($process.ExitCode): $detail" }
        throw "Installer step failed ($([IO.Path]::GetFileName($ScriptPath))) with exit code $($process.ExitCode). See $stdout and $stderr"
    }
}

function Invoke-WithRetry([string]$Label, [scriptblock]$Operation, [int]$Attempts = 2) {
    $last = $null
    foreach ($attempt in 1..$Attempts) {
        try {
            if ($attempt -gt 1) { Log "$Label automatic retry $attempt/$Attempts started." }
            & $Operation
            return
        }
        catch {
            $last = $_
            Log "$Label attempt $attempt/$Attempts failed: $($_.Exception.Message)"
            if ($attempt -lt $Attempts) { Start-Sleep -Seconds (5 * $attempt) }
        }
    }
    throw "$Label failed after automatic recovery attempts: $($last.Exception.Message)"
}

function Test-InstallRootWritable {
    $probe = Join-Path $Root ('.jubi-write-test-' + [guid]::NewGuid().ToString('N') + '.tmp')
    try {
        'ok' | Set-Content -LiteralPath $probe -Encoding ascii
        return (Test-Path -LiteralPath $probe)
    }
    finally { Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue }
}

try {
    if (-not (Test-Path -LiteralPath $PowerShell)) { throw 'Windows PowerShell 5.1 is required.' }
    if (-not [Environment]::Is64BitOperatingSystem) { throw 'Jubi requires 64-bit Windows.' }
    if (-not (Test-InstallRootWritable)) { throw 'Jubi install directory is not writable even with installer elevation.' }
    if (-not (Test-Path -LiteralPath (Join-Path $Root 'sarus\server.py'))) { throw 'Jubi foundation payload is incomplete.' }
    if (-not (Test-Path -LiteralPath (Join-Path $Root 'vendor\launcher\SARUS.exe.b64'))) { throw 'Bundled verified launcher payload is missing.' }
    if (-not (Test-Path -LiteralPath (Join-Path $Root 'vendor\launcher\SHA256.txt'))) { throw 'Bundled launcher checksum is missing.' }
    if (-not (Test-Path -LiteralPath (Join-Path $Root 'config\production.json'))) { throw 'Production profile is missing.' }
    if (-not (Test-Path -LiteralPath (Join-Path $Root 'config\bootstrap.json'))) { throw 'One-click bootstrap profile is missing.' }

    Log "Jubi one-click installation started. UpdateMode=$UpdateMode"
    $env:JUBI_INSTALL_MODE = 'exe'
    $env:SARUS_INSTALL_MODE = 'exe'

    Log 'Checking Windows requirements and automatically installing/repairing missing prerequisites.'
    try {
        Invoke-JubiPowerShell $Prerequisites @() 'initial'
    }
    catch {
        Log "Initial prerequisite pass failed; Jubi is entering automatic repair mode. $($_.Exception.Message)"
        Invoke-JubiPowerShell $Prerequisites @('-Repair') 'repair'
    }

    Log 'Preparing protected privileged-broker storage.'
    Invoke-WithRetry 'Broker storage setup' { Invoke-JubiPowerShell $Bootstrap @() 'broker' } 2

    Log 'Installing/repairing Jubi core, integrations and private Python runtime.'
    $coreArgs = @('-NonInteractive','-NoLaunch')
    if ($UpdateMode) { $coreArgs += '-UpdateMode' }
    try {
        Invoke-JubiPowerShell $Installer $coreArgs 'initial'
    }
    catch {
        Log "Initial core installation failed; running prerequisite repair and one clean core retry. $($_.Exception.Message)"
        Invoke-JubiPowerShell $Prerequisites @('-Repair') 'core-repair-prereq'
        $repairArgs = @('-NonInteractive','-NoLaunch','-RepairMode')
        if ($UpdateMode) { $repairArgs += '-UpdateMode' }
        Invoke-JubiPowerShell $Installer $repairArgs 'repair'
    }

    $LegacyLauncher = Join-Path $Root 'SARUS.exe'
    $JubiLauncher = Join-Path $Root 'Jubi.exe'
    if (-not (Test-Path -LiteralPath $LegacyLauncher)) { throw 'Verified launcher was not reconstructed by the installer.' }
    Copy-Item -LiteralPath $LegacyLauncher -Destination $JubiLauncher -Force
    $legacyHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $LegacyLauncher).Hash
    $jubiHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $JubiLauncher).Hash
    if ($legacyHash -ne $jubiHash) {
        Remove-Item -LiteralPath $JubiLauncher -Force -ErrorAction SilentlyContinue
        throw 'Jubi.exe launcher copy failed integrity verification.'
    }
    Log "Jubi.exe launcher prepared and verified: $jubiHash"

    $shell = New-Object -ComObject WScript.Shell
    $desktop = [Environment]::GetFolderPath('Desktop')
    $lnk = $shell.CreateShortcut((Join-Path $desktop 'Jubi.lnk'))
    $lnk.TargetPath = $JubiLauncher
    $lnk.WorkingDirectory = $Root
    $lnk.Description = 'Jubi Local AI Agent Platform'
    $lnk.IconLocation = "$JubiLauncher,0"
    $lnk.Save()
    Log 'Desktop Jubi shortcut prepared.'

    if ((Test-Path -LiteralPath $Ring0Driver) -and (Test-Path -LiteralPath $Ring0Installer)) {
        $signature = Get-AuthenticodeSignature -LiteralPath $Ring0Driver
        if ($signature.Status -eq 'Valid') {
            Log 'A validly signed legacy SarusRing0.sys is bundled; installing the controlled compatibility bridge.'
            Invoke-WithRetry 'Ring0 compatibility setup' { Invoke-JubiPowerShell $Ring0Installer @('-DriverPath',"`"$Ring0Driver`"") 'ring0' } 2
        }
        else { Log "Ring0 driver payload is not validly signed for this machine (status: $($signature.Status)); activation skipped." }
    }
    else { Log 'No prebuilt signed Ring0 driver is bundled; controlled source remains available but activation is skipped.' }

    $requiredFinal = @(
        $JubiLauncher,(Join-Path $Root '.sarus-venv\Scripts\python.exe'),(Join-Path $Root 'README.md'),
        (Join-Path $Root 'BUILD_MANIFEST.json'),(Join-Path $Root 'config\production.json'),(Join-Path $Root 'config\bootstrap.json'),
        (Join-Path $Root 'config\models.json'),(Join-Path $Root 'config\broker_allowlist.json'),(Join-Path $Root 'jubi\background.py'),
        (Join-Path $Root 'jubi\updater.py'),(Join-Path $Root 'installer\JUBI-BACKGROUND.ps1'),
        (Join-Path $Root 'installer\REGISTER-JUBI-BACKGROUND.ps1'),(Join-Path $Root 'sarus\server.py')
    )
    foreach ($path in $requiredFinal) {
        if (-not (Test-Path -LiteralPath $path)) { throw "Post-install verification failed; required file is missing: $path" }
    }

    Log 'Running target-machine production certification (core profile).'
    try {
        Invoke-JubiPowerShell $Certifier @() 'certify-initial'
    }
    catch {
        Log "Certification found a repairable problem; running one automatic repair cycle. $($_.Exception.Message)"
        Invoke-JubiPowerShell $Prerequisites @('-Repair') 'certify-prereq-repair'
        $certRepairArgs = @('-NonInteractive','-NoLaunch','-RepairMode')
        if ($UpdateMode) { $certRepairArgs += '-UpdateMode' }
        Invoke-JubiPowerShell $Installer $certRepairArgs 'certify-core-repair'
        Invoke-JubiPowerShell $Certifier @() 'certify-final'
    }

    Log 'Registering Jubi to start with Windows, self-restart on failure and check verified updates automatically.'
    Invoke-WithRetry 'Background task registration' { Invoke-JubiPowerShell $RegisterBackground @() 'background' } 2

    Log 'Post-install verification, background registration and core certification passed.'
    if (-not $UpdateMode) {
        Log 'Launching Jubi dashboard.'
        Start-Process -FilePath $JubiLauncher -WorkingDirectory $Root
    }
    else { Log 'Silent update completed; background task will run the refreshed Jubi build.' }
    Log 'Jubi one-click installation completed successfully.'
    exit 0
}
catch {
    Log "INSTALL FAILED: $($_.Exception.Message)"
    Write-Host "Jubi installation failed. See: $Log" -ForegroundColor Red
    exit 1
}
