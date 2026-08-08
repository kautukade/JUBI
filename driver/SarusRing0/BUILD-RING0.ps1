param(
    [ValidateSet('Debug','Release')]
    [string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'

$Project = Join-Path $PSScriptRoot 'SarusRing0.vcxproj'
$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $VsWhere)) {
    throw 'Visual Studio Build Tools/Visual Studio with Desktop C++ and Windows Driver Kit integration is required.'
}

$MsBuild = & $VsWhere -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1
if (-not $MsBuild) { throw 'MSBuild was not found.' }

Write-Host "Building SARUS Ring0 ($Configuration x64)..."
& $MsBuild $Project /m /t:Build "/p:Configuration=$Configuration" /p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw "Ring0 build failed with exit code $LASTEXITCODE" }

$Driver = Join-Path $PSScriptRoot "bin\$Configuration\SarusRing0.sys"
if (-not (Test-Path -LiteralPath $Driver)) { throw 'Build completed but SarusRing0.sys was not found.' }
Write-Host "Ring0 driver built: $Driver" -ForegroundColor Green
Write-Host 'The .sys must be signed with a certificate trusted by the target Windows machine before installation.'
