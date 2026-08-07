param()
$ErrorActionPreference='Stop'
$Root = Split-Path -Parent $PSScriptRoot
$fail = New-Object System.Collections.Generic.List[string]
function Pass([string]$m){ Write-Host "[PASS] $m" -ForegroundColor Green }
function Fail([string]$m){ Write-Host "[FAIL] $m" -ForegroundColor Red; $fail.Add($m) }

$required = @(
  'sarus\server.py',
  'sarus\acceptance.py',
  'config\models.json',
  'config\policy.json',
  'config\online_sources.json',
  'installer\INSTALL-SARUS.ps1',
  'INSTALL-SARUS.bat',
  'START_SARUS.bat',
  'vendor\launcher\SARUS.exe.b64',
  'vendor\launcher\SHA256.txt',
  'vendor\sara\FINAL-SHA256.txt'
)
foreach($r in $required){
  if(Test-Path (Join-Path $Root $r)){ Pass $r } else { Fail "Missing $r" }
}

$partsDir = Join-Path $Root 'vendor\sara\finalparts'
$parts = @()
if(Test-Path $partsDir){ $parts = @(Get-ChildItem $partsDir -Filter 'part-*.b64' -File | Sort-Object Name) }
if($parts.Count -ne 24){
  Fail "SARA finalparts count is $($parts.Count), expected 24"
} else {
  Pass 'SARA finalparts count = 24'
  try {
    $sb = New-Object Text.StringBuilder
    foreach($p in $parts){ [void]$sb.Append((Get-Content $p.FullName -Raw).Trim()) }
    $tmp = Join-Path $env:TEMP 'SARUS-verify-SARA-public-final.tar.xz'
    [IO.File]::WriteAllBytes($tmp,[Convert]::FromBase64String($sb.ToString()))
    $expected=((Get-Content (Join-Path $Root 'vendor\sara\FINAL-SHA256.txt') -Raw).Trim() -split '\s+')[0].ToLowerInvariant()
    $actual=(Get-FileHash -Algorithm SHA256 $tmp).Hash.ToLowerInvariant()
    Remove-Item $tmp -Force -ErrorAction SilentlyContinue
    if($actual -eq $expected){ Pass "SARA bundle SHA256 $actual" } else { Fail "SARA SHA256 mismatch: $actual != $expected" }
  } catch { Fail "SARA bundle reconstruction failed: $($_.Exception.Message)" }
}

try {
  $launcherBytes=[Convert]::FromBase64String((Get-Content (Join-Path $Root 'vendor\launcher\SARUS.exe.b64') -Raw).Trim())
  $tmpLauncher=Join-Path $env:TEMP 'SARUS-verify-launcher.exe'
  [IO.File]::WriteAllBytes($tmpLauncher,$launcherBytes)
  $expectedLauncher=((Get-Content (Join-Path $Root 'vendor\launcher\SHA256.txt') -Raw).Trim() -split '\s+')[0].ToLowerInvariant()
  $actualLauncher=(Get-FileHash -Algorithm SHA256 $tmpLauncher).Hash.ToLowerInvariant()
  Remove-Item $tmpLauncher -Force -ErrorAction SilentlyContinue
  if($actualLauncher -eq $expectedLauncher){ Pass "SARUS launcher SHA256 $actualLauncher" } else { Fail 'SARUS launcher checksum mismatch' }
} catch { Fail "Launcher verification failed: $($_.Exception.Message)" }

try {
  $specs=@(Get-Content (Join-Path $Root 'config\online_sources.json') -Raw | ConvertFrom-Json)
  if($specs.Count -eq 9){ Pass '9 pinned upstream source specifications' } else { Fail "Expected 9 upstream sources, found $($specs.Count)" }
} catch { Fail "online_sources.json invalid: $($_.Exception.Message)" }

if($fail.Count -gt 0){
  Write-Host "`nSARUS repository verification FAILED: $($fail.Count) issue(s)." -ForegroundColor Red
  exit 1
}
Write-Host "`nSARUS repository verification PASSED." -ForegroundColor Green
exit 0
