param([switch]$RequireSaraRuntime)
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
  'installer\JubiLauncher.cs',
  'installer\BUILD-LAUNCHER.ps1',
  'vendor\sara\FINAL-SHA256.txt'
)
foreach($r in $required){
  if(Test-Path (Join-Path $Root $r)){ Pass $r } else { Fail "Missing $r" }
}

$partsDir = Join-Path $Root 'vendor\sara\finalparts'
$parts = @()
if(Test-Path $partsDir){ $parts = @(Get-ChildItem $partsDir -Filter 'part-*.b64' -File | Sort-Object Name) }
if($parts.Count -ne 24){
  $message = "Optional SARA native bundle has $($parts.Count)/24 parts; full native installation is unavailable."
  if ($RequireSaraRuntime) { Fail $message } else { Write-Warning $message }
} else {
  Pass 'SARA finalparts count = 24'
  try {
    $sb = New-Object Text.StringBuilder
    foreach($p in $parts){ [void]$sb.Append((Get-Content $p.FullName -Raw).Trim()) }
    $tmp = Join-Path $env:TEMP ('Jubi-verify-SARA-' + [guid]::NewGuid().ToString('N') + '.tar.xz')
    [IO.File]::WriteAllBytes($tmp,[Convert]::FromBase64String($sb.ToString()))
    $expected=((Get-Content (Join-Path $Root 'vendor\sara\FINAL-SHA256.txt') -Raw).Trim() -split '\s+')[0].ToLowerInvariant()
    $actual=(Get-FileHash -Algorithm SHA256 $tmp).Hash.ToLowerInvariant()
    Remove-Item $tmp -Force -ErrorAction SilentlyContinue
    if($actual -eq $expected){ Pass "SARA bundle SHA256 $actual" } else { Fail "SARA SHA256 mismatch: $actual != $expected" }
  } catch { Fail "SARA bundle reconstruction failed: $($_.Exception.Message)" }
}

$tmpLauncher=Join-Path $env:TEMP ('Jubi-verify-launcher-' + [guid]::NewGuid().ToString('N') + '.exe')
try {
  & (Join-Path $Root 'installer\BUILD-LAUNCHER.ps1') -OutputPath $tmpLauncher
  $bytes=[IO.File]::ReadAllBytes($tmpLauncher)
  if($bytes.Length -gt 2 -and $bytes[0] -eq 77 -and $bytes[1] -eq 90){ Pass 'Tracked launcher source compiles to a Windows executable' }
  else { Fail 'Compiled launcher has no Windows executable header' }
} catch { Fail "Launcher compilation failed: $($_.Exception.Message)" }
finally { Remove-Item $tmpLauncher -Force -ErrorAction SilentlyContinue }

try {
  $decoded = Get-Content (Join-Path $Root 'config\online_sources.json') -Raw | ConvertFrom-Json
  $baseObject = $decoded.PSObject.BaseObject
  if ($baseObject -is [System.Array]) {
    $specCount = $baseObject.Length
  } elseif ($null -eq $baseObject) {
    $specCount = 0
  } else {
    $specCount = 1
  }
  if($specCount -eq 9){ Pass '9 pinned upstream source specifications' } else { Fail "Expected 9 upstream sources, found $specCount" }
} catch { Fail "online_sources.json invalid: $($_.Exception.Message)" }

if($fail.Count -gt 0){
  Write-Host "`nSARUS repository verification FAILED: $($fail.Count) issue(s)." -ForegroundColor Red
  exit 1
}
Write-Host "`nJubi core repository verification PASSED. See optional runtime warnings above." -ForegroundColor Green
exit 0
