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
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$Log = Join-Path $LogDir 'exe-install.log'

function Log([string]$Message) {
    "[$(Get-Date -Format s)] $Message" | Tee-Object -FilePath $Log -Append | Write-Host
}

function Invoke-JubiPowerShell([string]$ScriptPath, [string[]]$Arguments = @()) {
    if (-not (Test-Path -LiteralPath $ScriptPath)) {
        throw "Required installer script is missing: $ScriptPath"
    }
    $argumentList = @(
        '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
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
    if (-not (Test-Path -LiteralPath (Join-Path $Root 'sarus\server.py'))) { throw 'Jubi foundation payload is incomplete.' }
    if (-not (Test-Path -LiteralPath (Join-Path $Root 'vendor\launcher\SARUS.exe.b64'))) { throw 'Bundled verified launcher payload is missing.' }
    if (-not (Test-Path -LiteralPath (Join-Path $Root 'vendor\launcher\SHA256.txt'))) { throw 'Bundled launcher checksum is missing.' }
    if (-not (Test-Path -LiteralPath (Join-Path $Root 'config\production.json'))) { throw 'Production profile is missing.' }
    if (-not (Test-Path -LiteralPath (Join-Path $Root 'config\bootstrap.json'))) { throw 'One-click bootstrap profile is missing.' }

    Log "Jubi one-click installation started. UpdateMode=$UpdateMode"
    $env:JUBI_INSTALL_MODE = 'exe'
    $env:SARUS_INSTALL_MODE = 'exe'

    Log 'Checking Windows requirements and automatically installing missing prerequisites.'
    Invoke-JubiPowerShell $Prerequisites

    Log 'Preparing protected privileged-broker storage.'
    Invoke-JubiPowerShell $Bootstrap

    Log 'Installing/repairing Jubi core, integrations and private Python runtime.'
    Invoke-JubiPowerShell $Installer @('-NonInteractive', '-NoLaunch')

    # The verified legacy native launcher remains the current PE foundation.
    # Expose the branded Jubi.exe name and verify byte-for-byte integrity.
    $LegacyLauncher = Join-Path $Root 'SARUS.exe'
    $JubiLauncher = Join-Path $Root 'Jubi.exe'
    if (-not (Test-Path -LiteralPath $LegacyLauncher)) {
        throw 'Verified launcher was not reconstructed by the installer.'
    }
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

    # A trusted prebuilt controlled Ring0 driver is activated only when Windows
    # validates its signature. Security enforcement is never disabled.
    if ((Test-Path -LiteralPath $Ring0Driver) -and (Test-Path -LiteralPath $Ring0Installer)) {
        $signature = Get-AuthenticodeSignature -LiteralPath $Ring0Driver
        if ($signature.Status -eq 'Valid') {
            Log 'A validly signed legacy SarusRing0.sys is bundled; installing the controlled compatibility bridge.'
            Invoke-JubiPowerShell $Ring0Installer @('-DriverPath', "`"$Ring0Driver`"")
        }
        else {
            Log "Ring0 driver payload is not validly signed for this machine (status: $($signature.Status)); activation skipped."
        }
    }
    else {
        Log 'No prebuilt signed Ring0 driver is bundled; controlled source remains available but activation is skipped.'
    }

    $requiredFinal = @(
        $JubiLauncher,
        (Join-Path $Root '.sarus-venv\Scripts\python.exe'),
        (Join-Path $Root 'README.md'),
        (Join-Path $Root 'BUILD_MANIFEST.json'),
        (Join-Path $Root 'config\production.json'),
        (Join-Path $Root 'config\bootstrap.json'),
        (Join-Path $Root 'config\models.json'),
        (Join-Path $Root 'config\broker_allowlist.json'),
        (Join-Path $Root 'jubi\background.py'),
        (Join-Path $Root 'jubi\updater.py'),
        (Join-Path $Root 'installer\JUBI-BACKGROUND.ps1'),
        (Join-Path $Root 'installer\REGISTER-JUBI-BACKGROUND.ps1'),
        (Join-Path $Root 'sarus\server.py')
    )
    foreach ($path in $requiredFinal) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Post-install verification failed; required file is missing: $path"
        }
    }

    Log 'Running target-machine production certification (core profile).'
    Invoke-JubiPowerShell $Certifier

    Log 'Registering Jubi to start with Windows, self-restart on failure and check verified updates automatically.'
    Invoke-JubiPowerShell $RegisterBackground

    Log 'Post-install verification, background registration and core certification passed.'
    if (-not $UpdateMode) {
        Log 'Launching Jubi dashboard.'
        Start-Process -FilePath $JubiLauncher -WorkingDirectory $Root
    }
    else {
        Log 'Silent update completed; background task will run the refreshed Jubi build.'
    }
    Log 'Jubi one-click installation completed successfully.'
    exit 0
}
catch {
    Log "INSTALL FAILED: $($_.Exception.Message)"
    Write-Host "Jubi installation failed. See: $Log" -ForegroundColor Red
    exit 1
}
