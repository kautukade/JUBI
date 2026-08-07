param([string]$LauncherScriptPath='')
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ReleaseDir = Split-Path -Parent $PSCommandPath
$Payload = Join-Path $ReleaseDir 'SARUS-Core-Lite.tar.xz'
$HashFile = Join-Path $ReleaseDir 'SARUS-Core-Lite.sha256'
$InstallDir = Join-Path $env:LOCALAPPDATA 'Programs\SARUS'
$LogDir = Join-Path $env:LOCALAPPDATA 'SARUS\logs'
$LogFile = Join-Path $LogDir 'online-installer.log'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
function Log([string]$m) { $line="[$(Get-Date -Format s)] $m"; $line | Tee-Object -FilePath $LogFile -Append | Write-Host }
function Fail([string]$m) { Log "ERROR: $m"; throw $m }
# GitHub-friendly payload reconstruction. The repo stores the compressed core as small text chunks.
if (-not (Test-Path $Payload)) {
  $chunkDir = Join-Path $ReleaseDir 'payload'
  $chunks = Get-ChildItem -LiteralPath $chunkDir -Filter 'core_*.b64' -File | Sort-Object Name
  if (-not $chunks -or $chunks.Count -lt 1) { Fail "SARUS core chunks are missing from this GitHub download." }
  Log "Reconstructing SARUS core from $($chunks.Count) GitHub chunks..."
  $builder = New-Object System.Text.StringBuilder
  foreach($c in $chunks){ [void]$builder.Append((Get-Content -LiteralPath $c.FullName -Raw).Trim()) }
  try { [System.IO.File]::WriteAllBytes($Payload,[Convert]::FromBase64String($builder.ToString())) }
  catch { Fail "Could not reconstruct SARUS core: $($_.Exception.Message)" }
}

function Is-Admin { $id=[Security.Principal.WindowsIdentity]::GetCurrent(); $p=New-Object Security.Principal.WindowsPrincipal($id); return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) }
function Download-Source($stage,$spec,$index,$total) {
  $repo=$spec.repo; $sha=$spec.sha; $wrapper=$spec.wrapper; $inner=$spec.inner
  $dest=Join-Path $stage ("sources\\"+$wrapper+"\\"+$inner)
  if(Test-Path $dest){ Log "[$index/$total] $repo already staged."; return }
  $work=Join-Path $env:TEMP ('sarus-src-'+[guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Force -Path $work | Out-Null
  $zip=Join-Path $work 'source.zip'; $extract=Join-Path $work 'x'
  $url="https://codeload.github.com/$repo/zip/$sha"
  Log "[$index/$total] Downloading $repo @ $($sha.Substring(0,12)) ..."
  $ok=$false
  for($attempt=1;$attempt -le 3 -and -not $ok;$attempt++){
    try { Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $zip -TimeoutSec 900; $ok=$true }
    catch { Log "Download attempt $attempt failed for $repo: $($_.Exception.Message)"; if($attempt -lt 3){Start-Sleep -Seconds (3*$attempt)} }
  }
  if(-not $ok){ Fail "Could not download $repo after 3 attempts. Internet/GitHub access is required." }
  if((Get-Item $zip).Length -lt 100){ Fail "Downloaded archive for $repo is unexpectedly small." }
  New-Item -ItemType Directory -Force -Path $extract | Out-Null
  Expand-Archive -LiteralPath $zip -DestinationPath $extract -Force
  $root=Get-ChildItem -LiteralPath $extract -Directory | Select-Object -First 1
  if(-not $root){ Fail "Archive root missing for $repo" }
  $parent=Split-Path -Parent $dest; New-Item -ItemType Directory -Force -Path $parent | Out-Null
  Move-Item -LiteralPath $root.FullName -Destination $dest -Force
  Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
  Log "[$index/$total] $repo ready."
}
try {
  Log 'SARUS Online Installer v1.0 started.'
  if (-not (Is-Admin)) {
    Log 'Requesting Administrator permission.'
    $args=@('-NoProfile','-ExecutionPolicy','Bypass','-File',"`"$PSCommandPath`"")
    $p=Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Verb RunAs -ArgumentList $args -Wait -PassThru
    exit $p.ExitCode
  }
  if (-not (Test-Path $Payload)) { Fail "Missing reconstructed core payload: $Payload" }
  if (-not (Test-Path $HashFile)) { Fail "Missing checksum: $HashFile" }
  $expected=((Get-Content $HashFile -Raw).Trim() -split '\s+')[0].ToLowerInvariant()
  $actual=(Get-FileHash -Algorithm SHA256 $Payload).Hash.ToLowerInvariant()
  if ($expected -ne $actual) { Fail 'SARUS Core SHA256 mismatch. Re-download the GitHub ZIP.' }
  Log "Core integrity verified: $actual"

  $stage=Join-Path $env:SystemDrive ('SARUS-TMP-'+[guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Force -Path $stage | Out-Null
  $tar=Join-Path $env:SystemRoot 'System32\tar.exe'
  if(-not (Test-Path $tar)){ Fail 'Windows tar.exe is required. Update Windows 10/11 and try again.' }
  & $tar -xJf $Payload -C $stage
  if($LASTEXITCODE -ne 0){ Fail "Could not extract SARUS core (tar exit $LASTEXITCODE)." }
  if (-not (Test-Path (Join-Path $stage 'sarus\server.py'))) { Fail 'Core validation failed: sarus/server.py missing.' }
  $manifestPath=Join-Path $stage 'config\online_sources.json'
  if (-not (Test-Path $manifestPath)) { Fail 'Online source manifest missing.' }
  $specs=Get-Content $manifestPath -Raw | ConvertFrom-Json
  $i=0; foreach($s in $specs){ $i++; Download-Source $stage $s $i $specs.Count }

  $quote=Join-Path $stage 'sources\second-brain-skills-main(3)\second-brain-skills-main\.claude\skills\pptx-generator\cookbook\carousels\quote-slide.py'
  if(Test-Path $quote){
    $txt=Get-Content -LiteralPath $quote -Raw
    if($txt -match 'p\.text\s*=\s*"""'){ $txt=$txt -replace 'p\.text\s*=\s*"""','p.text = ""'; Set-Content -LiteralPath $quote -Value $txt -Encoding UTF8; Log 'Applied verified Second Brain quote-slide syntax fix.' }
  }

  $dbBackup=$null
  if (Test-Path (Join-Path $InstallDir 'data\sarus.db')) { $dbBackup=Join-Path $env:TEMP ('sarus-db-'+[guid]::NewGuid().ToString('N')+'.db'); Copy-Item (Join-Path $InstallDir 'data\sarus.db') $dbBackup -Force }
  if (Test-Path $InstallDir) { Remove-Item $InstallDir -Recurse -Force }
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $InstallDir) | Out-Null
  Move-Item $stage $InstallDir
  New-Item -ItemType Directory -Force -Path (Join-Path $InstallDir 'data') | Out-Null
  if ($dbBackup -and (Test-Path $dbBackup)) { Move-Item $dbBackup (Join-Path $InstallDir 'data\sarus.db') -Force }
  Log "SARUS sources and core installed to $InstallDir"

  $saraBat=Get-ChildItem -Path (Join-Path $InstallDir 'sources') -Filter 'INSTALL-AND-START-SARA.bat' -File -Recurse | Select-Object -First 1
  if (-not $saraBat) { Fail 'Bundled SARA Windows installer not found.' }
  Log 'Running SARA Windows dependency installer/tests. This may take time on first install...'
  $sp=Start-Process -FilePath 'cmd.exe' -ArgumentList @('/d','/c',"`"$($saraBat.FullName)`"") -WorkingDirectory $saraBat.DirectoryName -Wait -PassThru -NoNewWindow
  if ($sp.ExitCode -ne 0) { Fail "SARA installer failed with exit code $($sp.ExitCode)." }

  $py=$null
  try { $x=& py.exe -3.11 -c 'import sys; print(sys.executable)' 2>$null; if($LASTEXITCODE -eq 0){$py=($x|Select-Object -Last 1).Trim()} } catch {}
  foreach($cand in @("$env:LOCALAPPDATA\Programs\Python\Python311\python.exe","$env:ProgramFiles\Python311\python.exe")) { if((-not $py) -and (Test-Path $cand)){$py=$cand} }
  if (-not $py -or -not (Test-Path $py)) { Fail 'Python 3.11 not found after SARA setup.' }
  $runtime=Join-Path $InstallDir 'runtime'; & $py -m venv $runtime
  if ($LASTEXITCODE -ne 0) { Fail 'Could not create SARUS private runtime.' }
  $runtimePy=Join-Path $runtime 'Scripts\python.exe'; $runtimePyw=Join-Path $runtime 'Scripts\pythonw.exe'

  try {
    Log 'Preparing native Hermes runtime...'
    $hv=Join-Path $InstallDir 'native\hermes'; & $runtimePy -m venv $hv; $hp=Join-Path $hv 'Scripts\python.exe'
    & $hp -m pip install --disable-pip-version-check --upgrade pip | Out-File -FilePath $LogFile -Append
    $hermes=(Get-ChildItem -Path (Join-Path $InstallDir 'sources') -Filter 'pyproject.toml' -File -Recurse | Where-Object { (Get-Content $_.FullName -Raw) -match 'name\s*=\s*"hermes-agent"' } | Select-Object -First 1).DirectoryName
    if($hermes){ & $hp -m pip install --disable-pip-version-check -e $hermes | Out-File -FilePath $LogFile -Append; if($LASTEXITCODE -ne 0){throw 'Hermes pip install failed'} }
  } catch { Log "WARNING: Hermes native install failed; SARUS Ollama adapter remains available. $($_.Exception.Message)" }

  try {
    $ecc=(Get-ChildItem -Path (Join-Path $InstallDir 'sources') -Filter 'package.json' -File -Recurse | Where-Object { (Get-Content $_.FullName -Raw) -match '"name"\s*:\s*"ecc-universal"' } | Select-Object -First 1).DirectoryName
    if($ecc){ Log 'Preparing ECC native Node runtime...'; Push-Location $ecc; & npm.cmd install --omit=dev --no-audit --no-fund | Out-File -FilePath $LogFile -Append; $ec=$LASTEXITCODE; Pop-Location; if($ec -ne 0){throw "npm exited $ec"} }
  } catch { try{Pop-Location}catch{}; Log "WARNING: ECC native dependency setup failed; skill adapter remains available. $($_.Exception.Message)" }

  $launcherScript=Join-Path $InstallDir 'SARUS-script.pyw'; $shebang='#!"'+$runtimePyw+'"'
  $launchCode=@'
import os,sys,socket,subprocess,time,webbrowser
ROOT=os.path.dirname(os.path.abspath(__file__)); HOST='127.0.0.1'; PORT=8877
def alive():
    try:
        with socket.create_connection((HOST,PORT),0.8): return True
    except OSError: return False
if not alive():
    os.makedirs(os.path.join(ROOT,'logs'),exist_ok=True); log=open(os.path.join(ROOT,'logs','sarus-server.log'),'ab',buffering=0)
    py=sys.executable.replace('pythonw.exe','python.exe'); flags=0x08000000 if os.name=='nt' else 0
    subprocess.Popen([py,'-m','sarus.server'],cwd=ROOT,stdout=log,stderr=log,creationflags=flags)
    for _ in range(50):
        if alive(): break
        time.sleep(.2)
webbrowser.open(f'http://{HOST}:{PORT}')
'@
  Set-Content -LiteralPath $launcherScript -Value ($shebang+"`r`n"+$launchCode) -Encoding UTF8

  Log 'Running SARUS full target acceptance...'
  Push-Location $InstallDir; & $runtimePy -m sarus.acceptance --full | Tee-Object -FilePath (Join-Path $LogDir 'acceptance.json'); $accept=$LASTEXITCODE; Pop-Location
  if ($accept -ne 0) { Fail "SARUS acceptance failed (exit $accept). See $LogDir\acceptance.json" }

  $shell=New-Object -ComObject WScript.Shell; $desktop=[Environment]::GetFolderPath('Desktop')
  $lnk=$shell.CreateShortcut((Join-Path $desktop 'SARUS.lnk')); $lnk.TargetPath=(Join-Path $InstallDir 'SARUS.exe'); $lnk.WorkingDirectory=$InstallDir; $lnk.Description='SARUS Local Multi-Agent AI OS'; $lnk.Save()
  & schtasks.exe /Create /TN 'SARUS-AI-OS' /SC ONLOGON /RL LIMITED /TR ('"'+(Join-Path $InstallDir 'SARUS.exe')+'"') /F | Out-File -FilePath $LogFile -Append
  Start-Process -FilePath (Join-Path $InstallDir 'SARUS.exe') -WorkingDirectory $InstallDir
  Log 'SARUS Online installation completed successfully.'
  Write-Host "`nSARUS INSTALLATION COMPLETE. Dashboard is opening." -ForegroundColor Green
  Read-Host 'Press Enter to close installer'
  exit 0
} catch {
  Log "INSTALLATION FAILED: $($_.Exception.Message)"
  Write-Host "`nSARUS INSTALLATION FAILED`n$($_.Exception.Message)`nLog: $LogFile" -ForegroundColor Red
  Read-Host 'Press Enter to close'
  exit 1
}
