param()
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $Root 'sarus\server.py'))) {
    throw 'Run this installer from the extracted SARUS GitHub repository.'
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
    Log 'Requesting Administrator permission...'
    $p = Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Verb RunAs -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',"`"$PSCommandPath`"") -Wait -PassThru
    exit $p.ExitCode
}

try {
    Log 'SARUS GitHub installer started.'

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
            Log 'Reconstructing the verified public SARA source bundle...'
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
            # Owner fallback: the SARA source already exists in the user's GitHub account.
            # This does not publish the private repository; Git Credential Manager may ask
            # the owner to authenticate in the browser on first use.
            Log "Verified bundled SARA source is not complete ($($parts.Count)/24 parts). Trying authenticated GitHub SARA source fallback..."
            $git = Get-Command git.exe -ErrorAction SilentlyContinue
            if (-not $git) {
                throw 'The verified SARA bundle is incomplete and Git is not installed for the authenticated SARA fallback.'
            }
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $saraTarget) | Out-Null
            if (Test-Path $saraTarget) { Remove-Item $saraTarget -Recurse -Force }
            & $git.Source clone --depth 1 'https://github.com/kautukade/SARA-AI-OS.git' $saraTarget
            if ($LASTEXITCODE -ne 0) {
                throw 'Could not obtain SARA source. Complete the verified finalparts bundle or authenticate Git for kautukade/SARA-AI-OS.'
            }
        }

        if (-not (Test-Path $saraInstaller)) {
            $foundSaraInstaller = Get-ChildItem -Path (Join-Path $Root 'sources') -Filter 'INSTALL-AND-START-SARA.bat' -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($foundSaraInstaller) {
                $saraInstaller = $foundSaraInstaller.FullName
                $saraTarget = $foundSaraInstaller.DirectoryName
            }
            else {
                throw 'SARA Windows installer was not found after source restoration.'
            }
        }
    }

    # ------------------------------------------------------------------
    # 2) Restore the small Windows SARUS launcher and verify it.
    # ------------------------------------------------------------------
    $launcherB64 = Join-Path $Root 'vendor\launcher\SARUS.exe.b64'
    if (Test-Path $launcherB64) {
        $launcher = Join-Path $Root 'SARUS.exe'
        [IO.File]::WriteAllBytes($launcher, [Convert]::FromBase64String((Get-Content $launcherB64 -Raw).Trim()))
        $launcherHashFile = Join-Path $Root 'vendor\launcher\SHA256.txt'
        if (Test-Path $launcherHashFile) {
            $launcherExpected = ((Get-Content $launcherHashFile -Raw).Trim() -split '\s+')[0].ToLowerInvariant()
            $launcherActual = (Get-FileHash -Algorithm SHA256 $launcher).Hash.ToLowerInvariant()
            if ($launcherActual -ne $launcherExpected) {
                throw "SARUS.exe checksum mismatch. Expected $launcherExpected got $launcherActual"
            }
        }
        Log 'SARUS.exe reconstructed and verified.'
    }

    # ------------------------------------------------------------------
    # 3) Restore the 9 pinned public upstream projects.
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

        $work = Join-Path $env:TEMP ('sarus-source-' + [guid]::NewGuid().ToString('N'))
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
    # 4) Run the SARA Windows dependency setup.
    # ------------------------------------------------------------------
    $saraBat = Get-ChildItem -Path (Join-Path $Root 'sources') -Filter 'INSTALL-AND-START-SARA.bat' -File -Recurse | Select-Object -First 1
    if (-not $saraBat) { throw 'SARA Windows installer not found.' }
    Log 'Running SARA Windows setup. First install may take time...'
    $sp = Start-Process -FilePath 'cmd.exe' -ArgumentList @('/d','/c',"`"$($saraBat.FullName)`"") -WorkingDirectory $saraBat.DirectoryName -Wait -PassThru
    if ($sp.ExitCode -ne 0) { throw "SARA setup failed with exit code $($sp.ExitCode)" }

    # ------------------------------------------------------------------
    # 5) Create the SARUS Python runtime and run acceptance.
    # ------------------------------------------------------------------
    $py = $null
    try {
        $v = & py.exe -3.11 -c 'import sys; print(sys.executable)' 2>$null
        if ($LASTEXITCODE -eq 0) { $py = ($v | Select-Object -Last 1).Trim() }
    } catch {}
    if (-not $py) { throw 'Python 3.11 was not found after SARA setup.' }

    $venv = Join-Path $Root '.sarus-venv'
    & $py -m venv $venv
    if ($LASTEXITCODE -ne 0) { throw 'Could not create SARUS runtime.' }
    $runtimePy = Join-Path $venv 'Scripts\python.exe'
    Push-Location $Root
    & $runtimePy -m sarus.acceptance
    $accept = $LASTEXITCODE
    Pop-Location
    if ($accept -ne 0) { throw "SARUS acceptance failed with exit code $accept" }

    # ------------------------------------------------------------------
    # 6) Desktop shortcut and launch.
    # ------------------------------------------------------------------
    $shell = New-Object -ComObject WScript.Shell
    $desktop = [Environment]::GetFolderPath('Desktop')
    $lnk = $shell.CreateShortcut((Join-Path $desktop 'SARUS.lnk'))
    if (Test-Path (Join-Path $Root 'SARUS.exe')) { $lnk.TargetPath = Join-Path $Root 'SARUS.exe' }
    else { $lnk.TargetPath = Join-Path $Root 'START_SARUS.bat' }
    $lnk.WorkingDirectory = $Root
    $lnk.Description = 'SARUS Local Multi-Agent AI OS'
    $lnk.Save()

    Log 'SARUS GitHub installation completed.'
    if (Test-Path (Join-Path $Root 'SARUS.exe')) { Start-Process (Join-Path $Root 'SARUS.exe') -WorkingDirectory $Root }
    else { Start-Process (Join-Path $Root 'START_SARUS.bat') -WorkingDirectory $Root }

    Write-Host "`nSARUS INSTALL COMPLETE" -ForegroundColor Green
    Read-Host 'Press Enter to close'
    exit 0
}
catch {
    Log "INSTALL FAILED: $($_.Exception.Message)"
    Write-Host "`nSARUS INSTALL FAILED`n$($_.Exception.Message)`nLog: $Log" -ForegroundColor Red
    Read-Host 'Press Enter to close'
    exit 1
}
