param(
  [string]$Zip = ".\SARUS-FINAL-v1.0.0-ALL-IN-ONE-WINDOWS.zip",
  [string]$Tag = "v1.0.0-rc1"
)

$ErrorActionPreference = "Stop"

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
  Write-Host "GitHub CLI (gh) not found. Install it first: winget install --id GitHub.cli -e" -ForegroundColor Yellow
  exit 1
}

if (-not (Test-Path $Zip)) {
  Write-Host "ZIP not found: $Zip" -ForegroundColor Red
  exit 1
}

$expected = "6d370ef1a85ef72cbf8f96391a4a10c960849b35eeb8db1bdd7be75ab90db16b"
$actual = (Get-FileHash -Algorithm SHA256 $Zip).Hash.ToLowerInvariant()
if ($actual -ne $expected) {
  Write-Host "SHA256 mismatch. Refusing upload." -ForegroundColor Red
  Write-Host "Expected: $expected"
  Write-Host "Actual:   $actual"
  exit 1
}

Write-Host "Authenticating GitHub CLI if needed..." -ForegroundColor Cyan
gh auth status 2>$null
if ($LASTEXITCODE -ne 0) {
  gh auth login
}

$notes = @"
SARUS v1.0.0 RC1 Windows Release

- Single all-in-one Windows ZIP
- Includes SARUS-Setup.exe
- Local Ollama routing
- Unified multi-agent runtime foundation
- SHA256 verified before upload

SHA256: $expected
"@

Write-Host "Publishing release to kautukade/SARUS..." -ForegroundColor Cyan
$existing = gh release view $Tag --repo kautukade/SARUS 2>$null
if ($LASTEXITCODE -eq 0) {
  gh release upload $Tag $Zip --repo kautukade/SARUS --clobber
} else {
  gh release create $Tag $Zip --repo kautukade/SARUS --title "SARUS v1.0.0 RC1" --notes $notes
}

Write-Host "Done." -ForegroundColor Green
gh release view $Tag --repo kautukade/SARUS --web
