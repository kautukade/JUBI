param(
    [switch]$Fast,
    [switch]$Repair
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

if ($Fast -and $Repair) {
    Write-Error 'Choose either -Fast or -Repair, not both.'
    exit 2
}

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

function Invoke-Download([string]$Uri, [string]$OutFile, [int]$Attempts = 3) {
    $last = $null
    foreach ($attempt in 1..$Attempts) {
        try {
            Log "Downloading $Uri (attempt $attempt/$Attempts)"
            Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $OutFile -TimeoutSec 300
            if ((Test-Path -LiteralPath $OutFile) -and ((Get-Item -LiteralPath $OutFile).Length -gt 500KB)) { return }
            throw 'Downloaded file is unexpectedly small.'
        }
        catch {
            $last = $_
            Remove-Item -LiteralPath $OutFile -Force -ErrorAction SilentlyContinue
            if ($attempt -lt $Attempts) { Start-Sleep -Seconds (3 * $attempt) }
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
    if (-not $winget) {
        Log "Windows Package Manager is unavailable for $DisplayName; trusted fallback will be used when supported."
        return $false
    }
    try {
        Log "Installing/repairing prerequisite with Windows Package Manager: $DisplayName ($Id)"
        & $winget install --id $Id --exact --silent --accept-package-agreements --accept-source-agreements --disable-interactivity --force
        if ($LASTEXITCODE -eq 0) { return $true }
        Log "winget returned exit code $LASTEXITCODE for $Id; trying trusted fallback if available."
    }
    catch {
        Log "winget invocation failed for ${Id}: $($_.Exception.Message)"
    }
    return $false
}

function Test-Python311Exe([string]$Candidate) {
    if ([string]::IsNullOrWhiteSpace($Candidate) -or -not (Test-Path -LiteralPath $Candidate)) { return $false }
    try {
        $value = & $Candidate -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')" 2>$null
        return ($LASTEXITCODE -eq 0 -and (($value | Select-Object -Last 1).Trim() -eq '3.11'))
    }
    catch { return $false }
}

function Find-Python311 {
    try {
        $result = & py.exe -3.11 -c "import sys; print(sys.executable)" 2>$null
        if ($LASTEXITCODE -eq 0 -and $result) {
            $candidate = ($result | Select-Object -Last 1).Trim()
            if (Test-Python311Exe $candidate) { return $candidate }
        }
    } catch {}
    $candidates = @(
        'C:\Program Files\Python311\python.exe',
        (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python311\python.exe')
    )
    foreach ($candidate in $candidates) {
        if (Test-Python311Exe $candidate) { return $candidate }
    }
    return $null
}

function Ensure-Python311($Bootstrap) {
    $pythonExe = Find-Python311
    if ($pythonExe) {
        Log "Python 3.11 found and executable: $pythonExe"
        return $pythonExe
    }
    if ($Fast) { throw 'Python 3.11 is missing or not executable during fast health check.' }

    $pythonCfg = $Bootstrap.prerequisites.python
    [void](Install-WingetPackage ([string]$pythonCfg.winget_id) 'Python 3.11')
    $pythonExe = Find-Python311
    if ($pythonExe) { return $pythonExe }

    $url = [string]$pythonCfg.fallback_url
    if (-not $url.StartsWith('https://www.python.org/')) { throw 'Python fallback URL is not trusted.' }
    $installer = Join-Path $env:TEMP 'jubi-python-3.11-amd64.exe'
    Invoke-Download $url $installer
    $sig = Get-AuthenticodeSignature -LiteralPath $installer
    if ($sig.Status -ne 'Valid') { throw "Python installer signature is not valid: $($sig.Status)" }
    Log 'Installing/repairing Python 3.11 fallback silently.'
    $p = Start-Process -FilePath $installer -ArgumentList @('/quiet','InstallAllUsers=1','PrependPath=1','Include_launcher=1','Include_test=0') -Wait -PassThru
    Remove-Item -LiteralPath $installer -Force -ErrorAction SilentlyContinue
    if ($p.ExitCode -notin @(0,3010)) { throw "Python installer failed with exit code $($p.ExitCode)" }
    $pythonExe = Find-Python311
    if (-not $pythonExe) { throw 'Python 3.11 installation/repair completed but a working python.exe could not be found.' }
    return $pythonExe
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
    $hostName = $uri.Host.ToLowerInvariant()
    if ($hostName -in @('localhost','0.0.0.0','::','[::]')) { $hostName = '127.0.0.1' }
    if ($hostName -eq '::1') { $hostName = '127.0.0.1' }
    if ($hostName -ne '127.0.0.1') { return $null }
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
        [string]$Bootstrap.prerequisites.ollama.api
    )
    foreach ($candidatePort in @($Bootstrap.prerequisites.ollama.candidate_ports)) {
        try {
            $portNumber = [int]$candidatePort
            if ($portNumber -ge 1 -and $portNumber -le 65535) { $values += "http://127.0.0.1:$portNumber" }
        } catch {}
    }
    $values += 'http://127.0.0.1:11434'

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

function Test-LocalPortAvailable([string]$BaseUrl) {
    try {
        $uri = [Uri]$BaseUrl
        $listener = New-Object System.Net.Sockets.TcpListener([Net.IPAddress]::Loopback, $uri.Port)
        try {
            $listener.Start()
            return $true
        }
        finally {
            $listener.Stop()
        }
    }
    catch { return $false }
}

function Install-OrRepair-Ollama($Bootstrap, [switch]$ForceRepair) {
    $cfg = $Bootstrap.prerequisites.ollama
    $ollamaExe = Find-Ollama
    if (-not $ollamaExe) {
        [void](Install-WingetPackage ([string]$cfg.winget_id) 'Ollama')
        $ollamaExe = Find-Ollama
    }
    if ($ollamaExe -and -not $ForceRepair) { return $ollamaExe }
    if ($Fast) { return $ollamaExe }

    $url = [string]$cfg.fallback_url
    if (-not $url.StartsWith('https://ollama.com/')) { throw 'Ollama fallback URL is not trusted.' }
    $installer = Join-Path $env:TEMP 'Jubi-OllamaSetup.exe'
    Invoke-Download $url $installer
    $sig = Get-AuthenticodeSignature -LiteralPath $installer
    if ($sig.Status -ne 'Valid') { throw "Ollama installer signature is not valid: $($sig.Status)" }
    if ($ForceRepair) { Log 'Repairing the existing Ollama installation with the trusted official installer.' }
    else { Log 'Installing Ollama fallback silently.' }
    $p = Start-Process -FilePath $installer -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART') -Wait -PassThru
    Remove-Item -LiteralPath $installer -Force -ErrorAction SilentlyContinue
    if ($p.ExitCode -notin @(0,3010)) { throw "Ollama installer failed with exit code $($p.ExitCode)" }
    $ollamaExe = Find-Ollama
    return $ollamaExe
}

function Start-OllamaLocal([string]$OllamaExe, [string]$BaseUrl) {
    if (-not (Test-LocalPortAvailable $BaseUrl)) { throw "Local port is already occupied by a non-responsive service: $BaseUrl" }

    $bindAddress = $BaseUrl.Substring('http://'.Length)
    $safePort = ([Uri]$BaseUrl).Port
    $stdout = Join-Path $LogDir "ollama-serve-$safePort.stdout.log"
    $stderr = Join-Path $LogDir "ollama-serve-$safePort.stderr.log"
    Remove-Item -LiteralPath $stdout,$stderr -Force -ErrorAction SilentlyContinue

    $previousOllamaHost = $env:OLLAMA_HOST
    try {
        $env:OLLAMA_HOST = $bindAddress
        Log "Starting Ollama local API at $BaseUrl"
        $process = Start-Process -FilePath $OllamaExe -ArgumentList @('serve') -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    }
    finally {
        if ($null -eq $previousOllamaHost) { Remove-Item Env:OLLAMA_HOST -ErrorAction SilentlyContinue }
        else { $env:OLLAMA_HOST = $previousOllamaHost }
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
    try { if (-not $process.HasExited) { $process.Kill() } } catch {}
    if ($details) { throw "Ollama API did not become healthy at $BaseUrl ($exitText). $details" }
    throw "Ollama API did not become healthy at $BaseUrl ($exitText). See $stderr"
}

function Start-OllamaOnCandidates([string]$OllamaExe, [string[]]$Candidates) {
    $failures = @()
    foreach ($candidateUrl in $Candidates) {
        if (Test-OllamaApi $candidateUrl) {
            return [pscustomobject]@{ Exe = $OllamaExe; BaseUrl = $candidateUrl }
        }
        if (-not (Test-LocalPortAvailable $candidateUrl)) {
            $failures += "$candidateUrl occupied"
            continue
        }
        try {
            [void](Start-OllamaLocal $OllamaExe $candidateUrl)
            if (Test-OllamaApi $candidateUrl) {
                return [pscustomobject]@{ Exe = $OllamaExe; BaseUrl = $candidateUrl }
            }
        }
        catch {
            $failures += "$candidateUrl -> $($_.Exception.Message)"
            Log "Ollama start attempt failed at $candidateUrl; trying another local port. $($_.Exception.Message)"
        }
    }
    $detail = if ($failures.Count -gt 0) { $failures -join ' || ' } else { 'no candidate endpoints were available' }
    throw "Could not start a healthy local Ollama API. $detail"
}

function Ensure-Ollama($Bootstrap) {
    $ollamaExe = Install-OrRepair-Ollama $Bootstrap
    if (-not $ollamaExe) {
        if ($Fast) { throw 'Ollama is missing during fast health check.' }
        throw 'Ollama is not installed and automatic provisioning was unavailable.'
    }
    Log "Ollama executable found: $ollamaExe"

    $candidates = @(Get-OllamaCandidates $Bootstrap)
    foreach ($candidateUrl in $candidates) {
        if (Test-OllamaApi $candidateUrl) {
            Log "Existing healthy Ollama API detected at $candidateUrl"
            return [pscustomobject]@{ Exe = $ollamaExe; BaseUrl = $candidateUrl }
        }
    }

    try {
        return Start-OllamaOnCandidates $ollamaExe $candidates
    }
    catch {
        $firstFailure = $_.Exception.Message
        if ($Fast) { throw $firstFailure }
        Log "Initial Ollama recovery failed. Attempting one trusted Ollama repair. $firstFailure"
        $ollamaExe = Install-OrRepair-Ollama $Bootstrap -ForceRepair
        if (-not $ollamaExe) { throw 'Ollama repair completed but ollama.exe could not be found.' }
        Start-Sleep -Seconds 2
        return Start-OllamaOnCandidates $ollamaExe $candidates
    }
}

function Ensure-OptionalTool([string]$Command, [string]$WingetId, [string]$DisplayName) {
    if (Test-Command $Command) {
        Log "$DisplayName found."
        return
    }
    if ($Fast -or $Repair) {
        Log "$DisplayName is optional for Jubi core; automatic repair mode will not block on it."
        return
    }
    if (-not (Install-WingetPackage $WingetId $DisplayName)) {
        Log "$DisplayName could not be provisioned automatically, but Jubi core can continue without it."
    }
}

function Ensure-Models([string]$OllamaExe, [string]$BaseUrl, $Production) {
    if ($Fast) { return @() }
    $required = @($Production.required_models)
    if ($required.Count -eq 0) { return @() }
    $pending = @()
    $previousOllamaHost = $env:OLLAMA_HOST
    try {
        $env:OLLAMA_HOST = $BaseUrl.Substring('http://'.Length)
        $list = (& $OllamaExe list 2>$null | Out-String)
        foreach ($model in $required) {
            $name = [string]$model
            $baseName = ($name -split ':')[0]
            if ($list -match [regex]::Escape($name) -or $list -match [regex]::Escape($baseName)) {
                Log "Required Ollama model already present: $name"
                continue
            }

            $pulled = $false
            foreach ($attempt in 1..3) {
                Log "Pulling required Ollama model automatically: $name (attempt $attempt/3)"
                try {
                    & $OllamaExe pull $name
                    if ($LASTEXITCODE -eq 0) {
                        $pulled = $true
                        break
                    }
                    Log "Ollama model pull returned exit code $LASTEXITCODE for $name."
                }
                catch {
                    Log "Ollama model pull failed for ${name}: $($_.Exception.Message)"
                }
                if ($attempt -lt 3) { Start-Sleep -Seconds (5 * $attempt) }
            }
            if (-not $pulled) {
                $pending += $name
                Log "WARNING: model $name is still pending. Jubi installation will continue and background self-repair will retry automatically."
            }
        }
    }
    finally {
        if ($null -eq $previousOllamaHost) { Remove-Item Env:OLLAMA_HOST -ErrorAction SilentlyContinue }
        else { $env:OLLAMA_HOST = $previousOllamaHost }
    }
    return ,$pending
}

function Save-JubiRuntime([string]$PythonExe, [string]$OllamaBaseUrl, [string[]]$PendingModels) {
    New-Item -ItemType Directory -Force -Path $JubiDataDir | Out-Null
    $data = @{}
    if (Test-Path -LiteralPath $RuntimeConfig) {
        try {
            $existing = Get-Content -LiteralPath $RuntimeConfig -Raw | ConvertFrom-Json
            foreach ($property in $existing.PSObject.Properties) { $data[$property.Name] = $property.Value }
        }
        catch {}
    }
    $data['python_exe'] = $PythonExe
    $data['ollama_base_url'] = $OllamaBaseUrl
    $data['pending_models'] = @($PendingModels)
    $data['updated_at'] = (Get-Date).ToUniversalTime().ToString('o')
    $data | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $RuntimeConfig -Encoding utf8
    try {
        [Environment]::SetEnvironmentVariable('JUBI_OLLAMA_URL', $OllamaBaseUrl, 'User')
        $env:JUBI_OLLAMA_URL = $OllamaBaseUrl
    }
    catch {
        Log "WARNING: could not persist JUBI_OLLAMA_URL user environment value: $($_.Exception.Message)"
    }
    Log "Jubi runtime state saved. Ollama=$OllamaBaseUrl PendingModels=$(@($PendingModels).Count)"
}

try {
    if (-not [Environment]::Is64BitOperatingSystem) { throw 'Jubi requires 64-bit Windows.' }
    if (-not (Test-Path -LiteralPath $ConfigPath)) { throw 'config\bootstrap.json is missing.' }
    if (-not (Test-Path -LiteralPath $ProductionPath)) { throw 'config\production.json is missing.' }
    $bootstrap = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
    $production = Get-Content -LiteralPath $ProductionPath -Raw | ConvertFrom-Json

    $drive = Get-PSDrive -Name ([IO.Path]::GetPathRoot($Root).Substring(0,1)) -ErrorAction SilentlyContinue
    if ($drive -and $drive.Free -lt 20GB) {
        Log "WARNING: less than 20 GB free on the Jubi install drive. Core installation can continue, but local AI models may need more storage."
    }

    Log "Jubi prerequisite check started. Fast=$Fast Repair=$Repair"
    $pythonExe = Ensure-Python311 $bootstrap
    $ollamaState = Ensure-Ollama $bootstrap
    Ensure-OptionalTool 'git.exe' ([string]$bootstrap.prerequisites.git.winget_id) 'Git'
    Ensure-OptionalTool 'node.exe' ([string]$bootstrap.prerequisites.node.winget_id) 'Node.js LTS'
    $pendingModels = @(Ensure-Models ([string]$ollamaState.Exe) ([string]$ollamaState.BaseUrl) $production)
    Save-JubiRuntime $pythonExe ([string]$ollamaState.BaseUrl) $pendingModels

    Log "Prerequisite check passed. Python=$pythonExe Ollama=$($ollamaState.Exe) OllamaApi=$($ollamaState.BaseUrl) PendingModels=$($pendingModels.Count)"
    exit 0
}
catch {
    Log "PREREQUISITE FAILURE: $($_.Exception.Message)"
    exit 1
}
