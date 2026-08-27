param(
    [switch]$NonInteractive,
    [switch]$NoLaunch,
    [switch]$UpdateMode,
    [switch]$RepairMode
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $Root 'sarus\server.py'))) {
    throw 'Jubi foundation payload is incomplete. Run the official Jubi-Setup.exe or execute this compatibility script from the repository.'
}

$LogDir = Join-Path $Root 'logs'
$UserLogDir = Join-Path $env:LOCALAPPDATA 'Jubi\logs'
$RuntimeState = Join-Path $env:LOCALAPPDATA 'Jubi\runtime.json'
New-Item -ItemType Directory -Force -Path $LogDir,$UserLogDir | Out-Null
$Log = Join-Path $LogDir 'github-install.log'
function Log([string]$m) { "[$(Get-Date -Format s)] $m" | Tee-Object -FilePath $Log -Append | Write-Host }
function IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-WebDownloadRetry([string]$Uri, [string]$OutFile, [int]$Attempts = 3, [int]$TimeoutSec = 900) {
    $last = $null
    foreach ($attempt in 1..$Attempts) {
        try {
            Log "Downloading $Uri (attempt $attempt/$Attempts)"
            Remove-Item -LiteralPath $OutFile -Force -ErrorAction SilentlyContinue
            Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $OutFile -TimeoutSec $TimeoutSec
            if ((Test-Path -LiteralPath $OutFile) -and ((Get-Item -LiteralPath $OutFile).Length -gt 1KB)) { return }
            throw 'Downloaded file is empty or unexpectedly small.'
        }
        catch {
            $last = $_
            Remove-Item -LiteralPath $OutFile -Force -ErrorAction SilentlyContinue
            if ($attempt -lt $Attempts) { Start-Sleep -Seconds (5 * $attempt) }
        }
    }
    throw "Download failed after retries: $($last.Exception.Message)"
}

function Test-PythonRuntime([string]$PythonExe) {
    if ([string]::IsNullOrWhiteSpace($PythonExe) -or -not (Test-Path -LiteralPath $PythonExe)) { return $false }
    try {
        $value = & $PythonExe -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')" 2>$null
        return ($LASTEXITCODE -eq 0 -and (($value | Select-Object -Last 1).Trim() -eq '3.11'))
    }
    catch { return $false }
}

function Resolve-Python311 {
    if (Test-Path -LiteralPath $RuntimeState) {
        try {
            $runtimeInfo = Get-Content -LiteralPath $RuntimeState -Raw | ConvertFrom-Json
            $savedPython = [string]$runtimeInfo.python_exe
            if (Test-PythonRuntime $savedPython) {
                Log "Using prerequisite-verified Python runtime: $savedPython"
                return $savedPython
            }
        }
        catch {}
    }
    try {
        $value = & py.exe -3.11 -c 'import sys; print(sys.executable)' 2>$null
        if ($LASTEXITCODE -eq 0 -and $value) {
            $candidate = ($value | Select-Object -Last 1).Trim()
            if (Test-PythonRuntime $candidate) { return $candidate }
        }
    }
    catch {}
    foreach ($candidate in @('C:\Program Files\Python311\python.exe',(Join-Path $env:LOCALAPPDATA 'Programs\Python\Python311\python.exe'))) {
        if (Test-PythonRuntime $candidate) { return $candidate }
    }
    return $null
}

function Invoke-SaraDependencySetup([IO.FileInfo]$SaraBat) {
    $stdout = Join-Path $UserLogDir 'sara-dependencies.stdout.log'
    $stderr = Join-Path $UserLogDir 'sara-dependencies.stderr.log'
    $lastCode = -1
    foreach ($attempt in 1..2) {
        Remove-Item -LiteralPath $stdout,$stderr -Force -ErrorAction SilentlyContinue
        Log "Running bundled SARA dependency provisioning automatically (attempt $attempt/2)."
        $oldCI = $env:CI
        $oldNpmYes = $env:NPM_CONFIG_YES
        $oldPipCheck = $env:PIP_DISABLE_PIP_VERSION_CHECK
        $env:CI = '1'
        $env:NPM_CONFIG_YES = 'true'
        $env:PIP_DISABLE_PIP_VERSION_CHECK = '1'
        try {
            $escapedSaraBat = $SaraBat.FullName.Replace('"', '""')
            $cmdLine = "call `"$escapedSaraBat`" < NUL"
            $sp = Start-Process -FilePath 'cmd.exe' -ArgumentList @('/d','/s','/c', $cmdLine) -WorkingDirectory $SaraBat.DirectoryName -Wait -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
            $lastCode = $sp.ExitCode
            if ($lastCode -eq 0) {
                Log 'SARA dependency provisioning completed successfully.'
                return
            }
        }
        finally {
            $env:CI = $oldCI
            $env:NPM_CONFIG_YES = $oldNpmYes
            $env:PIP_DISABLE_PIP_VERSION_CHECK = $oldPipCheck
        }
        $tail = ''
        if (Test-Path -LiteralPath $stderr) {
            $lines = Get-Content -LiteralPath $stderr -Tail 10 -ErrorAction SilentlyContinue
            if ($lines) { $tail = ($lines -join ' | ') }
        }
        Log "SARA dependency setup attempt $attempt failed with exit code $lastCode. $tail"
        if ($attempt -lt 2) { Start-Sleep -Seconds 8 }
    }
    throw "SARA dependency setup failed after automatic retry. Exit code $lastCode. See $stdout and $stderr"
}

if (-not (IsAdmin)) {
    if ($NonInteractive) {
        throw 'Administrator permission is required. Start Jubi-Setup.exe with its normal UAC prompt.'
    }
    Log 'Requesting Administrator permission...'
    $args = @('-NoProfile','-ExecutionPolicy','Bypass','-File',"`"$PSCommandPath`"")
    if ($NoLaunch) { $args += '-NoLaunch' }
    if ($UpdateMode) { $args += '-UpdateMode' }
    if ($RepairMode) { $args += '-RepairMode' }
    $p = Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Verb RunAs -ArgumentList $args -Wait -PassThru
    exit $p.ExitCode
}

try {
    Log "Jubi installation engine started. UpdateMode=$UpdateMode RepairMode=$RepairMode NonInteractive=$NonInteractive NoLaunch=$NoLaunch"

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
            foreach ($part in $parts) { [void]$sb.Append((Get-Content -LiteralPath $part.FullName -Raw).Trim()) }
            $saraArchive = Join-Path $env:TEMP 'SARA-public-final.tar.xz'
            try { [IO.File]::WriteAllBytes($saraArchive, [Convert]::FromBase64String($sb.ToString())) }
            catch { throw "SARA bundle base64 reconstruction failed: $($_.Exception.Message)" }
            $expected = ((Get-Content -LiteralPath $hashFile -Raw).Trim() -split '\s+')[0].ToLowerInvariant()
            $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $saraArchive).Hash.ToLowerInvariant()
            if ($actual -ne $expected) { throw "SARA source checksum mismatch. Expected $expected got $actual. Installation stopped." }
            Log "SARA source verified: $actual"
            $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
            if (-not (Test-Path $tar)) { throw 'Windows tar.exe is required to extract SARA.' }
            & $tar -xf $saraArchive -C $Root
            if ($LASTEXITCODE -ne 0) { throw "SARA source extraction failed with exit code $LASTEXITCODE" }
            Remove-Item $saraArchive -Force -ErrorAction SilentlyContinue
        }
        else {
            Log "Verified bundled SARA source is incomplete ($($parts.Count)/24 parts). Trying authenticated GitHub owner fallback..."
            $git = Get-Command git.exe -ErrorAction SilentlyContinue
            if (-not $git) { throw 'The verified SARA bundle is incomplete and Git is unavailable for the authenticated owner fallback.' }
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $saraTarget) | Out-Null
            if (Test-Path $saraTarget) { Remove-Item $saraTarget -Recurse -Force }
            & $git.Source clone --depth 1 'https://github.com/kautukade/SARA-AI-OS.git' $saraTarget
            if ($LASTEXITCODE -ne 0) { throw 'Could not obtain SARA source. Use an official installer containing the verified finalparts bundle.' }
        }

        if (-not (Test-Path $saraInstaller)) {
            $foundSaraInstaller = Get-ChildItem -Path (Join-Path $Root 'sources') -Filter 'INSTALL-AND-START-SARA.bat' -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($foundSaraInstaller) { $saraInstaller = $foundSaraInstaller.FullName; $saraTarget = $foundSaraInstaller.DirectoryName }
            else { throw 'SARA Windows dependency installer was not found after source restoration.' }
        }
    }

    # ------------------------------------------------------------------
    # 2) Restore and verify the legacy native launcher payload.
    # ------------------------------------------------------------------
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
        if (Test-Path $dest) { $alreadyPresent = ((Get-ChildItem -LiteralPath $dest -Force -ErrorAction SilentlyContinue | Select-Object -First 1) -ne $null) }
        if ($alreadyPresent) { Log "[$i/$($specs.Count)] $($s.repo) already present."; continue }

        $work = Join-Path $env:TEMP ('jubi-source-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Force -Path $work | Out-Null
        try {
            $zip = Join-Path $work 'src.zip'
            $x = Join-Path $work 'x'
            $url = "https://codeload.github.com/$($s.repo)/zip/$($s.sha)"
            Log "[$i/$($specs.Count)] Restoring $($s.repo) @ $($s.sha.Substring(0,12))"
            Invoke-WebDownloadRetry $url $zip 3 900
            Expand-Archive -LiteralPath $zip -DestinationPath $x -Force
            $src = Get-ChildItem $x -Directory | Select-Object -First 1
            if (-not $src) { throw "Archive root missing for $($s.repo)" }
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
            if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
            Move-Item -LiteralPath $src.FullName -Destination $dest -Force
        }
        finally { Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue }
    }

    # ------------------------------------------------------------------
    # 4) Provision SARA/Windows dependencies. Normal installs and explicit
    # repairs run this; silent updates keep the already-proven machine setup.
    # ------------------------------------------------------------------
    $saraBat = Get-ChildItem -Path (Join-Path $Root 'sources') -Filter 'INSTALL-AND-START-SARA.bat' -File -Recurse | Select-Object -First 1
    if (-not $saraBat) { throw 'SARA Windows dependency installer not found.' }
    if ($UpdateMode -and -not $RepairMode) {
        Log 'Update mode: keeping previously provisioned SARA/Windows dependencies to avoid unnecessary global reinstall work.'
    }
    else {
        Invoke-SaraDependencySetup $saraBat
    }

    # ------------------------------------------------------------------
    # 5) Reuse a healthy private runtime. This is critical for safe in-place
    # updates because the updater itself may currently be running from it.
    # ------------------------------------------------------------------
    $py = Resolve-Python311
    if (-not $py) { throw 'Python 3.11 was not found after prerequisite provisioning.' }

    $venv = Join-Path $Root '.sarus-venv'
    $runtimePy = Join-Path $venv 'Scripts\python.exe'
    if (Test-PythonRuntime $runtimePy) {
        Log 'Existing private Jubi Python runtime is healthy; reusing it safely.'
    }
    else {
        Log 'Private Jubi Python runtime is missing or unhealthy; rebuilding it automatically.'
        if (Test-Path -LiteralPath $venv) {
            try { Remove-Item -LiteralPath $venv -Recurse -Force }
            catch { throw "Could not remove unhealthy private runtime: $($_.Exception.Message)" }
        }
        $created = $false
        foreach ($attempt in 1..2) {
            & $py -m venv $venv
            if ($LASTEXITCODE -eq 0 -and (Test-PythonRuntime $runtimePy)) { $created = $true; break }
            Log "Private runtime creation attempt $attempt failed; retrying automatically."
            Remove-Item -LiteralPath $venv -Recurse -Force -ErrorAction SilentlyContinue
            if ($attempt -lt 2) { Start-Sleep -Seconds 5 }
        }
        if (-not $created) { throw 'Could not create a healthy Jubi private Python runtime after automatic retry.' }
    }

    Push-Location $Root
    try { & $runtimePy -m jubi.acceptance; $accept = $LASTEXITCODE }
    finally { Pop-Location }
    if ($accept -ne 0) { throw "Jubi acceptance failed with exit code $accept" }
    Log 'Jubi acceptance checks passed.'

    # ------------------------------------------------------------------
    # 6) Create a Jubi-branded direct shortcut for compatibility installs.
    # ------------------------------------------------------------------
    $shell = New-Object -ComObject WScript.Shell
    $desktop = [Environment]::GetFolderPath('Desktop')
    $lnk = $shell.CreateShortcut((Join-Path $desktop 'Jubi.lnk'))
    $lnk.TargetPath = $launcher
    $lnk.WorkingDirectory = $Root
    $lnk.Description = 'Jubi Local AI Agent Platform'
    $lnk.Save()

    $finalRequired = @(
        $launcher,$runtimePy,(Join-Path $Root 'README.md'),(Join-Path $Root 'jubi\server.py'),
        (Join-Path $Root 'sarus\server.py'),(Join-Path $Root 'config\models.json'),(Join-Path $Root 'config\broker_allowlist.json')
    )
    foreach ($path in $finalRequired) {
        if (-not (Test-Path -LiteralPath $path)) { throw "Final installation verification failed: $path is missing." }
    }

    Log 'Jubi compatibility installation engine completed and verified.'
    if (-not $NoLaunch) { Start-Process -FilePath $launcher -WorkingDirectory $Root }

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
