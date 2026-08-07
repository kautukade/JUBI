param()
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
$Root=Split-Path -Parent $PSScriptRoot
if(-not (Test-Path (Join-Path $Root 'sarus\server.py'))){ throw 'Run this installer from the extracted SARUS GitHub repository.' }
$LogDir=Join-Path $Root 'logs'; New-Item -ItemType Directory -Force -Path $LogDir|Out-Null
$Log=Join-Path $LogDir 'github-install.log'
function Log([string]$m){ "[$(Get-Date -Format s)] $m" | Tee-Object -FilePath $Log -Append | Write-Host }
function IsAdmin { $id=[Security.Principal.WindowsIdentity]::GetCurrent(); $p=New-Object Security.Principal.WindowsPrincipal($id); $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) }
if(-not (IsAdmin)){
  Log 'Requesting Administrator permission...'
  $p=Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Verb RunAs -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',"`"$PSCommandPath`"") -Wait -PassThru
  exit $p.ExitCode
}
try {
  Log 'SARUS GitHub installer started.'
  $partsDir=Join-Path $Root 'vendor\sara\parts'
  $hashFile=Join-Path $Root 'vendor\sara\SHA256.txt'
  if(-not (Test-Path $partsDir)){ throw 'SARA source parts are missing from vendor\sara\parts.' }
  $parts=Get-ChildItem -LiteralPath $partsDir -Filter 'part-*.b64' -File | Sort-Object Name
  if($parts.Count -lt 1){ throw 'No SARA source parts found.' }
  $sb=New-Object Text.StringBuilder
  foreach($p in $parts){ [void]$sb.Append((Get-Content -LiteralPath $p.FullName -Raw).Trim()) }
  $saraZip=Join-Path $env:TEMP 'SARUS-SARA-PUBLIC-SOURCE.zip'
  [IO.File]::WriteAllBytes($saraZip,[Convert]::FromBase64String($sb.ToString()))
  $expected=((Get-Content $hashFile -Raw).Trim() -split '\s+')[0].ToLowerInvariant()
  $actual=(Get-FileHash -Algorithm SHA256 $saraZip).Hash.ToLowerInvariant()
  if($actual -ne $expected){ throw "SARA source checksum mismatch. Expected $expected got $actual" }
  Log "SARA source verified: $actual"
  Expand-Archive -LiteralPath $saraZip -DestinationPath $Root -Force
  Remove-Item $saraZip -Force -ErrorAction SilentlyContinue
  $launcherB64=Join-Path $Root 'vendor\launcher\SARUS.exe.b64'
  if(Test-Path $launcherB64){
    $launcher=Join-Path $Root 'SARUS.exe'
    [IO.File]::WriteAllBytes($launcher,[Convert]::FromBase64String((Get-Content $launcherB64 -Raw).Trim()))
    Log 'SARUS.exe reconstructed.'
  }
  $manifest=Join-Path $Root 'config\online_sources.json'
  $specs=Get-Content $manifest -Raw | ConvertFrom-Json
  $i=0
  foreach($s in $specs){
    $i++
    $dest=Join-Path $Root ("sources\"+$s.wrapper+"\"+$s.inner)
    if(Test-Path $dest){ Log "[$i/$($specs.Count)] $($s.repo) already present."; continue }
    $work=Join-Path $env:TEMP ('sarus-source-'+[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $work|Out-Null
    $zip=Join-Path $work 'src.zip'; $x=Join-Path $work 'x'
    $url="https://codeload.github.com/$($s.repo)/zip/$($s.sha)"
    Log "[$i/$($specs.Count)] Downloading $($s.repo) @ $($s.sha.Substring(0,12))"
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $zip -TimeoutSec 900
    Expand-Archive -LiteralPath $zip -DestinationPath $x -Force
    $src=Get-ChildItem $x -Directory | Select-Object -First 1
    if(-not $src){ throw "Archive root missing for $($s.repo)" }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest)|Out-Null
    Move-Item -LiteralPath $src.FullName -Destination $dest -Force
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
  }
  $saraBat=Get-ChildItem -Path (Join-Path $Root 'sources') -Filter 'INSTALL-AND-START-SARA.bat' -File -Recurse | Select-Object -First 1
  if(-not $saraBat){ throw 'SARA Windows installer not found after reconstruction.' }
  Log 'Running SARA Windows setup. First install may take time...'
  $sp=Start-Process -FilePath 'cmd.exe' -ArgumentList @('/d','/c',"`"$($saraBat.FullName)`"") -WorkingDirectory $saraBat.DirectoryName -Wait -PassThru
  if($sp.ExitCode -ne 0){ throw "SARA setup failed with exit code $($sp.ExitCode)" }
  $py=$null
  try { $v=& py.exe -3.11 -c 'import sys; print(sys.executable)' 2>$null; if($LASTEXITCODE -eq 0){$py=($v|Select-Object -Last 1).Trim()} } catch {}
  if(-not $py){ throw 'Python 3.11 was not found after SARA setup.' }
  $venv=Join-Path $Root '.sarus-venv'
  & $py -m venv $venv
  if($LASTEXITCODE -ne 0){ throw 'Could not create SARUS runtime.' }
  $runtimePy=Join-Path $venv 'Scripts\python.exe'
  Push-Location $Root
  & $runtimePy -m sarus.acceptance
  $accept=$LASTEXITCODE
  Pop-Location
  if($accept -ne 0){ throw "SARUS acceptance failed with exit code $accept" }
  $shell=New-Object -ComObject WScript.Shell
  $desktop=[Environment]::GetFolderPath('Desktop')
  $lnk=$shell.CreateShortcut((Join-Path $desktop 'SARUS.lnk'))
  if(Test-Path (Join-Path $Root 'SARUS.exe')){ $lnk.TargetPath=Join-Path $Root 'SARUS.exe' }
  else { $lnk.TargetPath=Join-Path $Root 'START_SARUS.bat' }
  $lnk.WorkingDirectory=$Root; $lnk.Description='SARUS Local Multi-Agent AI OS'; $lnk.Save()
  Log 'SARUS GitHub installation completed.'
  if(Test-Path (Join-Path $Root 'SARUS.exe')){ Start-Process (Join-Path $Root 'SARUS.exe') -WorkingDirectory $Root }
  else { Start-Process (Join-Path $Root 'START_SARUS.bat') -WorkingDirectory $Root }
  Write-Host "`nSARUS INSTALL COMPLETE" -ForegroundColor Green
  Read-Host 'Press Enter to close'
  exit 0
} catch {
  Log "INSTALL FAILED: $($_.Exception.Message)"
  Write-Host "`nSARUS INSTALL FAILED`n$($_.Exception.Message)`nLog: $Log" -ForegroundColor Red
  Read-Host 'Press Enter to close'
  exit 1
}
