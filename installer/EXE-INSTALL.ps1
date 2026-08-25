param()
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Root = Split-Path -Parent $PSScriptRoot
$Bootstrap = Join-Path $PSScriptRoot 'SETUP-BROKER.ps1'
# Legacy filenames are intentionally retained during Jubi Phase 0 so the
# existing tested installer chain remains compatible.
$Installer = Join-Path $PSScriptRoot 'INSTALL-SARUS.ps1'
$Certifier = Join-Path $PSScriptRoot 'CERTIFY-SARUS.ps1'
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
    if (-not (Test-Path -LiteralPath (Join-Path $Root 'vendor\launcher\SARUS.exe.b64'))) { throw 'Bundled legacy launcher payload is missing.' }
    if (-not (Test-Path -LiteralPath (Join-Path $Root 'vendor\launcher\SHA256.txt'))) { throw 'Bundled launcher checksum is missing.' }
    if (-not (Test-Path -LiteralPath (Join-Path $Root 'config\production.json'))) { throw 'Production profile is missing.' }

    Log 'Jubi v0.1.0 single-EXE installation started (SARUS 1.3.1 foundation).'
    $env:JUBI_INSTALL_MODE = 'exe'
    # Compatibility for the SARUS-era acceptance/install scripts during Phase 0.
    $env:SARUS_INSTALL_MODE = 'exe'

    Log 'Preparing protected privileged-broker storage.'
    Invoke-JubiPowerShell $Bootstrap

    Log 'Installing Jubi core foundation, SARA, source integrations, dependencies, required Ollama models and private Python runtime.'
    Invoke-JubiPowerShell $Installer @('-NonInteractive', '-NoLaunch')

    # The legacy launcher binary is reused byte-for-byte during Phase 0. Jubi.exe
    # is a branded filename copy; a separately rebuilt native launcher can be
    # introduced later after target-machine validation.
    $LegacyLauncher = Join-Path $Root 'SARUS.exe'
    $JubiLauncher = Join-Path $Root 'Jubi.exe'
    if (-not (Test-Path -LiteralPath $LegacyLauncher)) {
        throw 'Legacy verified launcher was not reconstructed by the installer.'
    }
    Copy-Item -LiteralPath $LegacyLauncher -Destination $JubiLauncher -Force
    $legacyHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $LegacyLauncher).Hash
    $jubiHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $JubiLauncher).Hash
    if ($legacyHash -ne $jubiHash) {
        Remove-Item -LiteralPath $JubiLauncher -Force -ErrorAction SilentlyContinue
        throw 'Jubi.exe launcher copy failed integrity verification.'
    }
    Log "Jubi.exe launcher prepared and verified: $jubiHash"

    # INSTALL-SARUS.ps1 retains a compatibility shortcut path while it rebuilds
    # the verified legacy launcher. Recreate the final branded shortcut here,
    # after Jubi.exe exists, so the official installer always points to Jubi.exe.
    $shell = New-Object -ComObject WScript.Shell
    $desktop = [Environment]::GetFolderPath('Desktop')
    $lnk = $shell.CreateShortcut((Join-Path $desktop 'Jubi.lnk'))
    $lnk.TargetPath = $JubiLauncher
    $lnk.WorkingDirectory = $Root
    $lnk.Description = 'Jubi Local AI Agent Platform'
    $lnk.Save()
    Log 'Final Jubi desktop shortcut now targets Jubi.exe.'

    # A trusted prebuilt controlled Ring0 driver is activated only when Windows
    # validates its signature. Security enforcement is never disabled.
    if ((Test-Path -LiteralPath $Ring0Driver) -and (Test-Path -LiteralPath $Ring0Installer)) {
        $signature = Get-AuthenticodeSignature -LiteralPath $Ring0Driver
        if ($signature.Status -eq 'Valid') {
            Log 'A validly signed legacy SarusRing0.sys is bundled; installing the controlled compatibility bridge.'
            Invoke-JubiPowerShell $Ring0Installer @('-DriverPath', "`"$Ring0Driver`"")
        }
        else {
            Log "Ring0 driver payload exists but is not validly signed for this machine (status: $($signature.Status)); kernel driver activation was skipped."
        }
    }
    else {
        Log 'No prebuilt signed Ring0 driver is bundled; controlled Ring0 source is installed but driver activation is skipped.'
    }

    $requiredFinal = @(
        $JubiLauncher,
        (Join-Path $Root '.sarus-venv\Scripts\python.exe'),
        (Join-Path $Root 'README.md'),
        (Join-Path $Root 'BUILD_MANIFEST.json'),
        (Join-Path $Root 'config\production.json'),
        (Join-Path $Root 'config\models.json'),
        (Join-Path $Root 'config\broker_allowlist.json'),
        (Join-Path $Root 'sarus\server.py'),
        (Join-Path $Root 'sarus\core\fable.py'),
        (Join-Path $Root 'sarus\web\fable.html')
    )
    foreach ($path in $requiredFinal) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Post-install verification failed; required file is missing: $path"
        }
    }

    Log 'Running target-machine production certification (core profile).'
    Invoke-JubiPowerShell $Certifier

    Log 'Post-install verification and core certification passed.'
    Log 'Launching Jubi.exe.'
    Start-Process -FilePath $JubiLauncher -WorkingDirectory $Root
    Log 'Jubi v0.1.0 single-EXE installation completed successfully.'
    exit 0
}
catch {
    Log "INSTALL FAILED: $($_.Exception.Message)"
    Write-Host "Jubi installation failed. See: $Log" -ForegroundColor Red
    exit 1
}
