param(
    [switch]$Fast
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Root = Split-Path -Parent $PSScriptRoot
$ConfigPath = Join-Path $Root 'config\bootstrap.json'
$ProductionPath = Join-Path $Root 'config\production.json'
$LogDir = Join-Path $env:LOCALAPPDATA 'Jubi\logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$Log = Join-Path $LogDir 'prerequisites.log'

function Log([string]$Message) {
    "[$(Get-Date -Format s)] $Message" | Tee-Object -FilePath $Log -Append | Write-Host
}

function Test-Command([string]$Name) {
    return [bool](Get-Command $Name -ErrorAction SilentlyContinue)
}

function Invoke-Download([string]$Uri, [string]$OutFile) {
    $last = $null
    foreach ($attempt in 1..3) {
        try {
            Log "Downloading $Uri (attempt $attempt/3)"
            Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $OutFile -TimeoutSec 300
            if ((Test-Path -LiteralPath $OutFile) -and ((Get-Item -LiteralPath $OutFile).Length -gt 500KB)) { return }
            throw 'Downloaded file is unexpectedly small.'
        }
        catch {
            $last = $_
            Remove-Item -LiteralPath $OutFile -Force -ErrorAction SilentlyContinue
            Start-Sleep -Seconds (3 * $attempt)
        }
    }
    throw "Download failed after retries: $($last.Exception.Message)"
}

function Get-Winget {
    $cmd = Get-Command winget.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $alias = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\winget.exe'
    if (Test-Path -LiteralPath $alias) { return $alias }
    return $null
}

function Install-WingetPackage([string]$Id, [string]$DisplayName) {
    $winget = Get-Winget
    if (-not $winget) { return $false }
    Log "Installing missing prerequisite with Windows Package Manager: $DisplayName ($Id)"
    & $winget install --id $Id --exact --silent --accept-package-agreements --accept-source-agreements --disable-interactivity
    if ($LASTEXITCODE -eq 0) { return $true }
    Log "winget returned exit code $LASTEXITCODE for $Id; trying fallback if available."
    return $false
}

function Find-Python311 {
    try {
        $result = & py.exe -3.11 -c "import sys; print(sys.executable)" 2>$null
        if ($LASTEXITCODE -eq 0 -and $result) {
            $candidate = ($result | Select-Object -Last 1).Trim()
            if (Test-Path -LiteralPath $candidate) { return $candidate }
        }
    } catch {}
    $candidates = @(
        'C:\Program Files\Python311\python.exe',
        (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python311\python.exe')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return $null
}

function Ensure-Python311($Bootstrap) {
    $python = Find-Python311
    if ($python) {
        Log "Python 3.11 found: $python"
        return $python
    }
    if ($Fast) { throw 'Python 3.11 is missing during fast repair.' }

    $pythonCfg = $Bootstrap.prerequisites.python
    [void](Install-WingetPackage ([string]$pythonCfg.winget_id) 'Python 3.11')
    $python = Find-Python311
    if ($python) { return $python }

    $url = [string]$pythonCfg.fallback_url
    if (-not $url.StartsWith('https://www.python.org/')) { throw 'Python fallback URL is not trusted.' }
    $installer = Join-Path $env:TEMP 'jubi-python-3.11-amd64.exe'
    Invoke-Download $url $installer
    $sig = Get-AuthenticodeSignature -LiteralPath $installer
    if ($sig.Status -ne 'Valid') { throw "Python installer signature is not valid: $($sig.Status)" }
    Log 'Installing Python 3.11 fallback silently.'
    $p = Start-Process -FilePath $installer -ArgumentList @('/quiet','InstallAllUsers=1','PrependPath=1','Include_launcher=1','Include_test=0') -Wait -PassThru
    Remove-Item -LiteralPath $installer -Force -ErrorAction SilentlyContinue
    if ($p.ExitCode -ne 0) { throw "Python installer failed with exit code $($p.ExitCode)" }
    $python = Find-Python311
    if (-not $python) { throw 'Python 3.11 installation completed but python.exe could not be found.' }
    return $python
}

function Find-Ollama {
    $cmd = Get-Command ollama.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA 'Programs\Ollama\ollama.exe'),
        'C:\Program Files\Ollama\ollama.exe'
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return $null
}

function Test-OllamaApi {
    try {
        $r = Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:11434/api/tags' -TimeoutSec 3
        return ($r.StatusCode -eq 200)
    }
    catch { return $false }
}

function Ensure-Ollama($Bootstrap) {
    $ollama = Find-Ollama
    if (-not $ollama -and -not $Fast) {
        $cfg = $Bootstrap.prerequisites.ollama
        [void](Install-WingetPackage ([string]$cfg.winget_id) 'Ollama')
        $ollama = Find-Ollama
        if (-not $ollama) {
            $url = [string]$cfg.fallback_url
            if (-not $url.StartsWith('https://ollama.com/')) { throw 'Ollama fallback URL is not trusted.' }
            $installer = Join-Path $env:TEMP 'Jubi-OllamaSetup.exe'
            Invoke-Download $url $installer
            $sig = Get-AuthenticodeSignature -LiteralPath $installer
            if ($sig.Status -ne 'Valid') { throw "Ollama installer signature is not valid: $($sig.Status)" }
            Log 'Installing Ollama fallback silently.'
            $p = Start-Process -FilePath $installer -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART') -Wait -PassThru
            Remove-Item -LiteralPath $installer -Force -ErrorAction SilentlyContinue
            if ($p.ExitCode -ne 0) { throw "Ollama installer failed with exit code $($p.ExitCode)" }
            $ollama = Find-Ollama
        }
    }
    if (-not $ollama) { throw 'Ollama is not installed and automatic provisioning was unavailable.' }
    Log "Ollama found: $ollama"

    if (-not (Test-OllamaApi)) {
        Log 'Starting Ollama background API.'
        Start-Process -FilePath $ollama -ArgumentList @('serve') -WindowStyle Hidden | Out-Null
        foreach ($i in 1..30) {
            Start-Sleep -Seconds 1
            if (Test-OllamaApi) { break }
        }
    }
    if (-not (Test-OllamaApi)) { throw 'Ollama API did not become healthy on 127.0.0.1:11434.' }
    return $ollama
}

function Ensure-OptionalTool([string]$Command, [string]$WingetId, [string]$DisplayName) {
    if (Test-Command $Command) {
        Log "$DisplayName found."
        return
    }
    if ($Fast) {
        Log "$DisplayName is not available; it is optional for Jubi core."
        return
    }
    if (-not (Install-WingetPackage $WingetId $DisplayName)) {
        Log "$DisplayName could not be provisioned automatically, but Jubi core can continue without it."
    }
}

function Ensure-Models([string]$Ollama, $Production) {
    if ($Fast) { return }
    $required = @($Production.required_models)
    if ($required.Count -eq 0) { return }
    $list = (& $Ollama list 2>$null | Out-String)
    foreach ($model in $required) {
        $name = [string]$model
        $base = ($name -split ':')[0]
        if ($list -match [regex]::Escape($name) -or $list -match [regex]::Escape($base)) {
            Log "Required Ollama model already present: $name"
            continue
        }
        Log "Pulling required Ollama model automatically: $name"
        & $Ollama pull $name
        if ($LASTEXITCODE -ne 0) { throw "Ollama could not pull required model $name (exit $LASTEXITCODE)." }
    }
}

try {
    if (-not [Environment]::Is64BitOperatingSystem) { throw 'Jubi requires 64-bit Windows.' }
    if (-not (Test-Path -LiteralPath $ConfigPath)) { throw 'config\bootstrap.json is missing.' }
    if (-not (Test-Path -LiteralPath $ProductionPath)) { throw 'config\production.json is missing.' }
    $bootstrap = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
    $production = Get-Content -LiteralPath $ProductionPath -Raw | ConvertFrom-Json

    $drive = Get-PSDrive -Name ([IO.Path]::GetPathRoot($Root).Substring(0,1)) -ErrorAction SilentlyContinue
    if ($drive -and $drive.Free -lt 20GB) {
        Log "WARNING: less than 20 GB free on the Jubi install drive. Required AI models may need additional storage."
    }

    Log "Jubi prerequisite check started. Fast=$Fast"
    $python = Ensure-Python311 $bootstrap
    $ollama = Ensure-Ollama $bootstrap
    Ensure-OptionalTool 'git.exe' ([string]$bootstrap.prerequisites.git.winget_id) 'Git'
    Ensure-OptionalTool 'node.exe' ([string]$bootstrap.prerequisites.node.winget_id) 'Node.js LTS'
    Ensure-Models $ollama $production

    Log "Prerequisite check passed. Python=$python Ollama=$ollama"
    exit 0
}
catch {
    Log "PREREQUISITE FAILURE: $($_.Exception.Message)"
    exit 1
}
