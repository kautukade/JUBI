param(
    [switch]$NonInteractive,
    [switch]$NoLaunch
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $Root 'sarus\server.py'))) {
    throw 'Jubi foundation payload is incomplete. Run the official Jubi-Setup.exe or execute this compatibility script from the repository.'
}

$LogDir = Join-Path $Root 'logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$Log = Join-Path $LogDir 'github-install.log'
function Log([string]$m) { "[$(Get-Date -Format s)] $m" | Tee-Object -FilePath $Log -Append | Write-Host }
function IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (IsAdmin)) {
    if ($NonInteractive) {
        throw 'Administrator permission is required. Start Jubi-Setup.exe with its normal UAC prompt.'
    }
    Log 'Requesting Administrator permission...'
    $args = @('-NoProfile','-ExecutionPolicy','Bypass','-File',"`"$PSCommandPath`"")
    if ($NoLaunch) { $args += '-NoLaunch' }
    $p = Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Verb RunAs -ArgumentList $args -Wait -PassThru
    exit $p.ExitCode
}

try {
    Log "Jubi installation engine started. Mode=$($env:JUBI_INSTALL_MODE) LegacyMode=$($env:SARUS_INSTALL_MODE) NonInteractive=$NonInteractive NoLaunch=$NoLaunch"

    # ------------------------------------------------------------------
    # 1) Restore the custom SARA source.
    # ------------------------------------------------------------------
    $saraWrapper = 'SARA-AI-Assistant-Local-AI-OS-v7.1.1-ROBUST-ONE-CLICK(4)'
    $saraInner = 'SARA-AI-Assistant-Local-AI-OS-v7.1.1-ROBUST-ONE-CLICK'
    $saraTarget = Join-Path $Root ("sources\$saraWrapper\$saraInner")
    $saraInstaller = Join-Path $saraTarget 'INSTALL-AND-START-SARA.bat'

    if (Test-Path $saraInstaller) {
        Log 'SARA source is already present.'
    }
    else {
        $partsDir = Join-Path $Root 'vendor\sara\finalparts'
        $hashFile = Join-Path $Root 'vendor\sara\FINAL-SHA256.txt'
        $parts = @()
        if (Test-Path $partsDir) {
            $parts = @(Get-ChildItem -LiteralPath $partsDir -Filter 'part-*.b64' -File | Sort-Object Name)
        }

        if ($parts.Count -eq 24) {
            if (-not (Test-Path $hashFile)) { throw 'FINAL-SHA256.txt is missing.' }
            Log 'Reconstructing the verified bundled SARA source...'
            $sb = New-Object Text.StringBuilder
            foreach ($part in $parts) {
                [void]$sb.Append((Get-Content -LiteralPath $part.FullName -Raw).Trim())
            }
            $saraArchive = Join-Path $env:TEMP 'SARA-public-final.tar.xz'
            try {
                [IO.File]::WriteAllBytes($saraArchive, [Convert]::FromBase64String($sb.ToString()))
            }
            catch {
                throw "SARA bundle base64 reconstruction failed: $($_.Exception.Message)"
            }
            $expected = ((Get-Content -LiteralPath $hashFile -Raw).Trim() -split '\s+')[0].ToLowerInvariant()
            $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $saraArchive).Hash.ToLowerInvariant()
            if ($actual -ne $expected) {
                throw "SARA source checksum mismatch. Expected $expected got $actual. Installation stopped."
            }
            Log "SARA source verified: $actual"
            $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
            if (-not (Test-Path $tar)) { throw 'Windows tar.exe is required to extract SARA.' }
            & $tar -xf $saraArchive -C $Root
            if ($LASTEXITCODE -ne 0) { throw "SARA source extraction failed with exit code $LASTEXITCODE" }
            Remove-Item $saraArchive -Force -ErrorAction SilentlyContinue
        }
        else {
            # Owner fallback only. The normal release installer carries the verified
            # bundled source, so end users should not need to perform this step.
            Log "Verified bundled SARA source is incomplete ($($parts.Count)/24 parts). Trying authenticated GitHub owner fallback..."
            $git = Get-Command git.exe -ErrorAction SilentlyContinue
            if (-not $git) {
                throw 'The verified SARA bundle is incomplete and Git is unavailable for the authenticated owner fallback.'
            }
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $saraTarget) | Out-Null
            if (Test-Path $saraTarget) { Remove-Item $saraTarget -Recurse -Force }
            & $git.Source clone --depth 1 'https://github.com/kautukade/SARA-AI-OS.git' $saraTarget
            if ($LASTEXITCODE -ne 0) {
                throw 'Could not obtain SARA source. Use an official installer containing the verified finalparts bundle.'
            }
        }

        if (-not (Test-Path $saraInstaller)) {
            $foundSaraInstaller = Get-ChildItem -Path (Join-Path $Root 'sources') -Filter 'INSTALL-AND-START-SARA.bat' -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($foundSaraInstaller) {
                $saraInstaller = $foundSaraInstaller.FullName
                $saraTarget = $foundSaraInstaller.DirectoryName
            }
            else {
                throw 'SARA Windows dependency installer was not found after source restoration.'
            }
        }
    }

    # ------------------------------------------------------------------
    # 2) Restore and verify the legacy native launcher payload.
    # ------------------------------------------------------------------
    # Jubi Phase 0 reuses the byte-identical verified launcher binary. The outer
    # EXE bootstrap copies this to Jubi.exe after SHA-256 equality verification.
    $launcherB64 = Join-Path $Root 'vendor\launcher\SARUS.exe.b64'
    $launcherHashFile = Join-Path $Root 'vendor\launcher\SHA256.txt'
    if (-not (Test-Path -LiteralPath $launcherB64)) { throw 'vendor\launcher\SARUS.exe.b64 is missing.' }
    if (-not (Test-Path -LiteralPath $launcherHashFile)) { throw 'vendor\launcher\SHA256.txt is missing.' }

    $launcher = Join-Path $Root 'SARUS.exe'
    [IO.File]::WriteAllBytes($launcher, [Convert]::FromBase64String((Get-Content $launcherB64 -Raw).Trim()))
    $launcherExpected = ((Get-Content $launcherHashFile -Raw).Trim() -split '\s+')[0].ToLowerInvariant()
    $launcherActual = (Get-FileHash -Algorithm SHA256 $launcher).Hash.ToLowerInvariant()
    if ($launcherActual -ne $launcherExpected) {
        Remove-Item -LiteralPath $launcher -Force -ErrorAction SilentlyContinue
        throw "Launcher checksum mismatch. Expected $launcherExpected got $launcherActual"
    }
    Log "Verified compatibility launcher reconstructed: $launcherActual"

    # ------------------------------------------------------------------
    # 3) Restore pinned public upstream projects when they are not bundled.
    # ------------------------------------------------------------------
    $manifest = Join-Path $Root 'config\online_sources.json'
    if (-not (Test-Path $manifest)) { throw 'config\online_sources.json is missing.' }
    $specs = @(Get-Content $manifest -Raw | ConvertFrom-Json)
    $i = 0
    foreach ($s in $specs) {
        $i++
        $dest = Join-Path $Root ("sources\" + $s.wrapper + "\" + $s.inner)
        $alreadyPresent = $false
        if (Test-Path $dest) {
            $alreadyPresent = ((Get-ChildItem -LiteralPath $dest -Force -ErrorAction SilentlyContinue | Select-Object -First 1) -ne $null)
        }
        if ($alreadyPresent) {
            Log "[$i/$($specs.Count)] $($s.repo) already present."
            continue
        }

        $work = Join-Path $env:TEMP ('jubi-source-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Force -Path $work | Out-Null
        $zip = Join-Path $work 'src.zip'
        $x = Join-Path $work 'x'
        $url = "https://codeload.github.com/$($s.repo)/zip/$($s.sha)"
        Log "[$i/$($specs.Count)] Downloading $($s.repo) @ $($s.sha.Substring(0,12))"
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $zip -TimeoutSec 900
        Expand-Archive -LiteralPath $zip -DestinationPath $x -Force
        $src = Get-ChildItem $x -Directory | Select-Object -First 1
        if (-not $src) { throw "Archive root missing for $($s.repo)" }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
        if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
        Move-Item -LiteralPath $src.FullName -Destination $dest -Force
        Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    }

    # ------------------------------------------------------------------
    # 4) Provision SARA/Windows dependencies automatically.
    # ------------------------------------------------------------------
    $saraBat = Get-ChildItem -Path (Join-Path $Root 'sources') -Filter 'INSTALL-AND-START-SARA.bat' -File -Recurse | Select-Object -First 1
    if (-not $saraBat) { throw 'SARA Windows dependency installer not found.' }
    Log 'Running bundled SARA dependency provisioning automatically in non-interactive mode.'

    $oldCI = $env:CI
    $oldNpmYes = $env:NPM_CONFIG_YES
    $oldPipCheck = $env:PIP_DISABLE_PIP_VERSION_CHECK
    $env:CI = '1'
    $env:NPM_CONFIG_YES = 'true'
    $env:PIP_DISABLE_PIP_VERSION_CHECK = '1'
    try {
        $escapedSaraBat = $saraBat.FullName.Replace('"', '""')
        $cmdLine = "call `"$escapedSaraBat`" < NUL"
        $sp = Start-Process -FilePath 'cmd.exe' -ArgumentList @('/d','/s','/c', $cmdLine) -WorkingDirectory $saraBat.DirectoryName -Wait -PassThru
        if ($sp.ExitCode -ne 0) { throw "SARA dependency setup failed with exit code $($sp.ExitCode)" }
    }
    finally {
        $env:CI = $oldCI
        $env:NPM_CONFIG_YES = $oldNpmYes
        $env:PIP_DISABLE_PIP_VERSION_CHECK = $oldPipCheck
    }

    # ------------------------------------------------------------------
    # 5) Create private Python runtime and run Jubi acceptance tests.
    # ------------------------------------------------------------------
    $py = $null
    try {
        $v = & py.exe -3.11 -c 'import sys; print(sys.executable)' 2>$null
        if ($LASTEXITCODE -eq 0) { $py = ($v | Select-Object -Last 1).Trim() }
    } catch {}
    if (-not $py) { throw 'Python 3.11 was not found after dependency provisioning.' }

    # Legacy physical venv path retained for Phase 0 compatibility.
    $venv = Join-Path $Root '.sarus-venv'
    if (Test-Path -LiteralPath $venv) {
        Log 'Refreshing existing private Python environment.'
        Remove-Item -LiteralPath $venv -Recurse -Force
    }
    & $py -m venv $venv
    if ($LASTEXITCODE -ne 0) { throw 'Could not create Jubi private Python runtime.' }
    $runtimePy = Join-Path $venv 'Scripts\python.exe'
    if (-not (Test-Path -LiteralPath $runtimePy)) { throw 'Jubi private Python runtime is incomplete.' }

    Push-Location $Root
    try {
        & $runtimePy -m jubi.acceptance
        $accept = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    if ($accept -ne 0) { throw "Jubi acceptance failed with exit code $accept" }
    Log 'Jubi acceptance checks passed.'

    # ------------------------------------------------------------------
    # 6) Create a Jubi-branded direct shortcut for compatibility installs.
    # ------------------------------------------------------------------
    # The official Inno Setup package creates a Jubi.exe shortcut itself. This
    # shortcut is mainly useful when this compatibility script is run directly.
    $shell = New-Object -ComObject WScript.Shell
    $desktop = [Environment]::GetFolderPath('Desktop')
    $lnk = $shell.CreateShortcut((Join-Path $desktop 'Jubi.lnk'))
    $lnk.TargetPath = $launcher
    $lnk.WorkingDirectory = $Root
    $lnk.Description = 'Jubi Local AI Agent Platform'
    $lnk.Save()

    $finalRequired = @(
        $launcher,
        $runtimePy,
        (Join-Path $Root 'README.md'),
        (Join-Path $Root 'jubi\server.py'),
        (Join-Path $Root 'sarus\server.py'),
        (Join-Path $Root 'config\models.json'),
        (Join-Path $Root 'config\broker_allowlist.json')
    )
    foreach ($path in $finalRequired) {
        if (-not (Test-Path -LiteralPath $path)) { throw "Final installation verification failed: $path is missing." }
    }

    Log 'Jubi compatibility installation engine completed and verified.'
    if (-not $NoLaunch) {
        Start-Process -FilePath $launcher -WorkingDirectory $Root
    }

    Write-Host "`nJUBI INSTALL COMPLETE" -ForegroundColor Green
    if (-not $NonInteractive) { Read-Host 'Press Enter to close' | Out-Null }
    exit 0
}
catch {
    Log "INSTALL FAILED: $($_.Exception.Message)"
    Write-Host "`nJUBI INSTALL FAILED`n$($_.Exception.Message)`nLog: $Log" -ForegroundColor Red
    if (-not $NonInteractive) { Read-Host 'Press Enter to close' | Out-Null }
    exit 1
}
