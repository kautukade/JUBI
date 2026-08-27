param(
    [switch]$Fast
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Root = Split-Path -Parent $PSScriptRoot
$ConfigPath = Join-Path $Root 'config\bootstrap.json'
$ProductionPath = Join-Path $Root 'config\production.json'
$JubiDataDir = Join-Path $env:LOCALAPPDATA 'Jubi'
$LogDir = Join-Path $JubiDataDir 'logs'
$RuntimeConfig = Join-Path $JubiDataDir 'runtime.json'
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

function Normalize-LocalOllamaUrl([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { return $null }
    $raw = $Value.Trim()
    if ($raw -notmatch '^https?://') { $raw = "http://$raw" }
    try { $uri = [Uri]$raw } catch { return $null }
    if ($uri.Scheme -ne 'http') { return $null }
    $host = $uri.Host.ToLowerInvariant()
    if ($host -in @('localhost','0.0.0.0','::','[::]')) { $host = '127.0.0.1' }
    if ($host -eq '::1') { $host = '127.0.0.1' }
    if ($host -ne '127.0.0.1') { return $null }
    $port = if ($uri.IsDefaultPort) { 11434 } else { $uri.Port }
    if ($port -lt 1 -or $port -gt 65535) { return $null }
    return "http://127.0.0.1:$port"
}

function Get-OllamaCandidates($Bootstrap) {
    $values = @(
        $env:JUBI_OLLAMA_URL,
        $env:OLLAMA_HOST,
        [Environment]::GetEnvironmentVariable('JUBI_OLLAMA_URL', 'User'),
        [Environment]::GetEnvironmentVariable('OLLAMA_HOST', 'User'),
        [string]$Bootstrap.prerequisites.ollama.api,
        'http://127.0.0.1:11434'
    )
    $result = @()
    foreach ($value in $values) {
        $normalized = Normalize-LocalOllamaUrl ([string]$value)
        if ($normalized -and ($result -notcontains $normalized)) { $result += $normalized }
    }
    return ,$result
}

function Test-OllamaApi([string]$BaseUrl) {
    try {
        $request = [System.Net.HttpWebRequest]::Create("$BaseUrl/api/tags")
        $request.Method = 'GET'
        $request.Proxy = $null
        $request.Timeout = 2500
        $request.ReadWriteTimeout = 2500
        $response = $request.GetResponse()
        try { return ([int]$response.StatusCode -eq 200) }
        finally { $response.Close() }
    }
    catch { return $false }
}

function Save-JubiRuntime([string]$BaseUrl) {
    New-Item -ItemType Directory -Force -Path $JubiDataDir | Out-Null
    $data = [ordered]@{
        ollama_base_url = $BaseUrl
        updated_at = (Get-Date).ToUniversalTime().ToString('o')
    }
    $data | ConvertTo-Json | Set-Content -LiteralPath $RuntimeConfig -Encoding utf8
    [Environment]::SetEnvironmentVariable('JUBI_OLLAMA_URL', $BaseUrl, 'User')
    $env:JUBI_OLLAMA_URL = $BaseUrl
    Log "Jubi local Ollama endpoint saved: $BaseUrl"
}

function Start-OllamaLocal([string]$Ollama, [string]$BaseUrl) {
    $bind = $BaseUrl.Substring('http://'.Length)
    $stdout = Join-Path $LogDir 'ollama-serve.stdout.log'
    $stderr = Join-Path $LogDir 'ollama-serve.stderr.log'
    Remove-Item -LiteralPath $stdout,$stderr -Force -ErrorAction SilentlyContinue

    $previousHost = $env:OLLAMA_HOST
    try {
        $env:OLLAMA_HOST = $bind
        Log "Starting Ollama local API at $BaseUrl"
        $process = Start-Process -FilePath $Ollama -ArgumentList @('serve') -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    }
    finally {
        if ($null -eq $previousHost) { Remove-Item Env:OLLAMA_HOST -ErrorAction SilentlyContinue }
        else { $env:OLLAMA_HOST = $previousHost }
    }

    foreach ($i in 1..45) {
        Start-Sleep -Seconds 1
        if (Test-OllamaApi $BaseUrl) { return $process }
        try { if ($process.HasExited) { break } } catch {}
    }

    $details = ''
    if (Test-Path -LiteralPath $stderr) {
        $tail = Get-Content -LiteralPath $stderr -Tail 12 -ErrorAction SilentlyContinue
        if ($tail) { $details = (($tail | ForEach-Object { $_.Trim() }) -join ' | ') }
    }
    $exitText = 'still running'
    try { if ($process.HasExited) { $exitText = "exit code $($process.ExitCode)" } } catch {}
    if ($details) { throw "Ollama API did not become healthy at $BaseUrl ($exitText). $details" }
    throw "Ollama API did not become healthy at $BaseUrl ($exitText). See $stderr"
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

    $candidates = @(Get-OllamaCandidates $Bootstrap)
    foreach ($candidate in $candidates) {
        if (Test-OllamaApi $candidate) {
            Log "Existing healthy Ollama API detected at $candidate"
            Save-JubiRuntime $candidate
            return [pscustomobject]@{ Exe = $ollama; BaseUrl = $candidate }
        }
    }

    $startUrl = if ($candidates.Count -gt 0) { $candidates[0] } else { 'http://127.0.0.1:11434' }
    [void](Start-OllamaLocal $ollama $startUrl)
    if (-not (Test-OllamaApi $startUrl)) { throw "Ollama API recovery failed at $startUrl." }
    Save-JubiRuntime $startUrl
    return [pscustomobject]@{ Exe = $ollama; BaseUrl = $startUrl }
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

function Ensure-Models([string]$Ollama, [string]$BaseUrl, $Production) {
    if ($Fast) { return }
    $required = @($Production.required_models)
    if ($required.Count -eq 0) { return }
    $previousHost = $env:OLLAMA_HOST
    try {
        $env:OLLAMA_HOST = $BaseUrl.Substring('http://'.Length)
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
    finally {
        if ($null -eq $previousHost) { Remove-Item Env:OLLAMA_HOST -ErrorAction SilentlyContinue }
        else { $env:OLLAMA_HOST = $previousHost }
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
    $ollamaState = Ensure-Ollama $bootstrap
    Ensure-OptionalTool 'git.exe' ([string]$bootstrap.prerequisites.git.winget_id) 'Git'
    Ensure-OptionalTool 'node.exe' ([string]$bootstrap.prerequisites.node.winget_id) 'Node.js LTS'
    Ensure-Models ([string]$ollamaState.Exe) ([string]$ollamaState.BaseUrl) $production

    Log "Prerequisite check passed. Python=$python Ollama=$($ollamaState.Exe) OllamaApi=$($ollamaState.BaseUrl)"
    exit 0
}
catch {
    Log "PREREQUISITE FAILURE: $($_.Exception.Message)"
    exit 1
}
