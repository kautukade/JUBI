param(
    [switch]$Fast,
    [switch]$Repair
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
    $line = "[$(Get-Date -Format s)] $Message"
    $line | Tee-Object -FilePath $Log -Append | Write-Host
}

function Test-Python311([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path)) { return $false }
    try {
        $v = & $Path -c "import sys; print('%d.%d' % (sys.version_info.major, sys.version_info.minor))" 2>$null
        return ($LASTEXITCODE -eq 0 -and (($v | Select-Object -Last 1).Trim() -eq '3.11'))
    } catch { return $false }
}

function Find-Python311 {
    try {
        $v = & py.exe -3.11 -c "import sys; print(sys.executable)" 2>$null
        if ($LASTEXITCODE -eq 0 -and $v) {
            $p = ($v | Select-Object -Last 1).Trim()
            if (Test-Python311 $p) { return $p }
        }
    } catch {}
    foreach ($p in @(
        'C:\Program Files\Python311\python.exe',
        (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python311\python.exe'),
        (Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\python.exe')
    )) {
        if (Test-Python311 $p) { return $p }
    }
    return $null
}

function Get-Winget {
    $c = Get-Command winget.exe -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $p = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\winget.exe'
    if (Test-Path -LiteralPath $p) { return $p }
    return $null
}

function Invoke-Download([string]$Uri, [string]$OutFile) {
    $last = $null
    foreach ($attempt in 1..3) {
        $part = "$OutFile.$([guid]::NewGuid().ToString('N')).download"
        try {
            Log "Downloading $Uri (attempt $attempt/3)"
            Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $part -TimeoutSec 600
            if (-not (Test-Path -LiteralPath $part)) { throw 'Download did not create a file.' }
            if ((Get-Item -LiteralPath $part).Length -lt 500KB) { throw 'Downloaded file is unexpectedly small.' }
            Move-Item -LiteralPath $part -Destination $OutFile -Force
            return
        } catch {
            $last = $_
            Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
            if ($attempt -lt 3) { Start-Sleep -Seconds (3 * $attempt) }
        }
    }
    throw "Download failed after retries: $($last.Exception.Message)"
}

function Install-WithWinget([string]$Id, [string]$Name) {
    $winget = Get-Winget
    if (-not $winget) { return $false }
    try {
        Log "Provisioning $Name with Windows Package Manager ($Id)."
        & $winget install --id $Id --exact --silent --accept-package-agreements --accept-source-agreements --disable-interactivity --force 2>&1 | ForEach-Object { Log ([string]$_) }
        return ($LASTEXITCODE -eq 0)
    } catch {
        Log "winget failed for ${Name}: $($_.Exception.Message)"
        return $false
    }
}

function Ensure-Python311($Bootstrap) {
    $p = Find-Python311
    if ($p) { Log "Python 3.11 found and executable: $p"; return $p }
    if ($Fast) { throw 'Python 3.11 is missing during fast health check.' }
    $cfg = $Bootstrap.prerequisites.python
    [void](Install-WithWinget ([string]$cfg.winget_id) 'Python 3.11')
    $p = Find-Python311
    if ($p) { Log "Python 3.11 provisioned: $p"; return $p }
    $url = [string]$cfg.fallback_url
    if ($url -notlike 'https://www.python.org/*') { throw 'Python fallback URL is not trusted.' }
    $installer = Join-Path $env:TEMP "Jubi-Python311-$([guid]::NewGuid().ToString('N')).exe"
    try {
        Invoke-Download $url $installer
        $sig = Get-AuthenticodeSignature -LiteralPath $installer
        if ($sig.Status -ne 'Valid') { throw "Python installer signature is not valid: $($sig.Status)" }
        $pInfo = Start-Process -FilePath $installer -ArgumentList @('/quiet','InstallAllUsers=1','PrependPath=1','Include_launcher=1','Include_test=0') -Wait -PassThru
        if ($pInfo.ExitCode -notin @(0,3010)) { throw "Python installer failed with exit code $($pInfo.ExitCode)" }
    } finally { Remove-Item -LiteralPath $installer -Force -ErrorAction SilentlyContinue }
    $p = Find-Python311
    if (-not $p) { throw 'Python 3.11 installation finished but python.exe could not be located.' }
    return $p
}

function Find-Ollama {
    $c = Get-Command ollama.exe -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    foreach ($p in @(
        (Join-Path $env:LOCALAPPDATA 'Programs\Ollama\ollama.exe'),
        'C:\Program Files\Ollama\ollama.exe'
    )) { if (Test-Path -LiteralPath $p) { return $p } }
    return $null
}

function Normalize-OllamaUrl([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { return $null }
    $raw = $Value.Trim()
    if ($raw -notmatch '^https?://') { $raw = "http://$raw" }
    try { $u = [Uri]$raw } catch { return $null }
    if ($u.Scheme -ne 'http') { return $null }
    $h = $u.Host.ToLowerInvariant()
    if ($h -in @('localhost','0.0.0.0','::1','::','[::]')) { $h = '127.0.0.1' }
    if ($h -ne '127.0.0.1') { return $null }
    $port = if ($u.IsDefaultPort) { 11434 } else { $u.Port }
    if ($port -lt 1 -or $port -gt 65535) { return $null }
    return "http://127.0.0.1:$port"
}

function Get-OllamaCandidates($Bootstrap) {
    $values = @()
    foreach ($v in @(
        $env:JUBI_OLLAMA_URL,
        $env:OLLAMA_HOST,
        [Environment]::GetEnvironmentVariable('JUBI_OLLAMA_URL','User'),
        [Environment]::GetEnvironmentVariable('OLLAMA_HOST','User'),
        [string]$Bootstrap.prerequisites.ollama.api
    )) { if ($v) { $values += [string]$v } }
    foreach ($p in @($Bootstrap.prerequisites.ollama.candidate_ports)) { try { $values += "http://127.0.0.1:$([int]$p)" } catch {} }
    $values += @('http://127.0.0.1:11434','http://127.0.0.1:11500','http://127.0.0.1:11435','http://127.0.0.1:11436','http://127.0.0.1:11437','http://127.0.0.1:11438','http://127.0.0.1:11439','http://127.0.0.1:11440','http://127.0.0.1:11501','http://127.0.0.1:11502','http://127.0.0.1:11503','http://127.0.0.1:11504','http://127.0.0.1:11505')
    $out = @()
    foreach ($v in $values) {
        $n = Normalize-OllamaUrl ([string]$v)
        if ($n -and ($out -notcontains $n)) { $out += $n }
    }
    return [string[]]$out
}

function Test-OllamaApi([string]$BaseUrl) {
    try {
        $req = [Net.HttpWebRequest]::Create("$BaseUrl/api/tags")
        $req.Method = 'GET'; $req.Proxy = $null; $req.Timeout = 2500; $req.ReadWriteTimeout = 2500
        $res = $req.GetResponse()
        try { return ([int]$res.StatusCode -eq 200) } finally { $res.Close() }
    } catch { return $false }
}

function Get-ListeningPid([int]$Port) {
    try {
        $rows = Get-NetTCPConnection -LocalAddress '127.0.0.1' -LocalPort $Port -State Listen -ErrorAction Stop
        if ($rows) { return [int]($rows | Select-Object -First 1).OwningProcess }
    } catch {}
    try {
        $netstat = netstat.exe -ano -p tcp 2>$null
        foreach ($line in $netstat) {
            if ($line -match "\s127\.0\.0\.1:$Port\s+\S+\s+LISTENING\s+(\d+)") { return [int]$Matches[1] }
            if ($line -match "\s0\.0\.0\.0:$Port\s+\S+\s+LISTENING\s+(\d+)") { return [int]$Matches[1] }
        }
    } catch {}
    return $null
}

function Stop-TrustedOllamaProcesses([string]$OllamaExe) {
    if (-not $OllamaExe) { return }
    $trusted = [IO.Path]::GetFullPath((Split-Path -Parent $OllamaExe)).TrimEnd('\') + '\'
    foreach ($p in @(Get-Process -Name 'ollama','ollama_app' -ErrorAction SilentlyContinue)) {
        try {
            $path = $p.Path
            if ($path -and ([IO.Path]::GetFullPath($path)).StartsWith($trusted,[StringComparison]::OrdinalIgnoreCase)) {
                Log "Stopping stale trusted Ollama process PID=$($p.Id)."
                Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            }
        } catch {}
    }
    Start-Sleep -Seconds 2
}

function Test-PortFree([int]$Port) {
    $listener = $null
    try {
        $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,$Port)
        $listener.Start(); return $true
    } catch { return $false }
    finally { if ($listener) { try { $listener.Stop() } catch {} } }
}

function Start-Ollama([string]$Exe,[string]$BaseUrl) {
    $port = ([Uri]$BaseUrl).Port
    if (-not (Test-PortFree $port)) { throw "Port $port is occupied." }
    $out = Join-Path $LogDir "ollama-$port.stdout.log"
    $err = Join-Path $LogDir "ollama-$port.stderr.log"
    Remove-Item -LiteralPath $out,$err -Force -ErrorAction SilentlyContinue
    $oldHost = $env:OLLAMA_HOST
    try {
        $env:OLLAMA_HOST = "127.0.0.1:$port"
        Log "Starting Ollama local API at $BaseUrl."
        $proc = Start-Process -FilePath $Exe -ArgumentList @('serve') -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    } finally {
        if ($null -eq $oldHost) { Remove-Item Env:OLLAMA_HOST -ErrorAction SilentlyContinue } else { $env:OLLAMA_HOST = $oldHost }
    }
    foreach ($i in 1..45) {
        Start-Sleep -Seconds 1
        if (Test-OllamaApi $BaseUrl) { return $true }
        try { if ($proc.HasExited) { break } } catch {}
    }
    $detail = ''
    if (Test-Path $err) { $x = Get-Content $err -Tail 12 -ErrorAction SilentlyContinue; if ($x) { $detail = ($x -join ' | ') } }
    try { if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } } catch {}
    throw "Ollama API did not become healthy at $BaseUrl. $detail"
}

function Ensure-Ollama($Bootstrap) {
    $exe = Find-Ollama
    if (-not $exe -and $Fast) { throw 'Ollama is missing during fast health check.' }
    if (-not $exe) {
        $cfg = $Bootstrap.prerequisites.ollama
        [void](Install-WithWinget ([string]$cfg.winget_id) 'Ollama')
        $exe = Find-Ollama
    }
    if (-not $exe) {
        $url = [string]$Bootstrap.prerequisites.ollama.fallback_url
        if ($url -notlike 'https://ollama.com/*') { throw 'Ollama fallback URL is not trusted.' }
        $installer = Join-Path $env:TEMP "Jubi-Ollama-$([guid]::NewGuid().ToString('N')).exe"
        try {
            Invoke-Download $url $installer
            $sig = Get-AuthenticodeSignature -LiteralPath $installer
            if ($sig.Status -ne 'Valid') { throw "Ollama installer signature is not valid: $($sig.Status)" }
            Log 'Installing Ollama silently from the trusted official installer.'
            $p = Start-Process -FilePath $installer -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART') -Wait -PassThru
            if ($p.ExitCode -notin @(0,3010)) { throw "Ollama installer failed with exit code $($p.ExitCode)" }
        } finally { Remove-Item -LiteralPath $installer -Force -ErrorAction SilentlyContinue }
        $exe = Find-Ollama
    }
    if (-not $exe) { throw 'Ollama installation completed but ollama.exe was not found.' }
    Log "Ollama executable found: $exe"

    $candidates = Get-OllamaCandidates $Bootstrap
    foreach ($u in $candidates) {
        if (Test-OllamaApi $u) { Log "Existing healthy Ollama API detected at $u"; return [pscustomobject]@{Exe=$exe;BaseUrl=$u} }
    }

    Stop-TrustedOllamaProcesses $exe
    foreach ($u in $candidates) {
        $port = ([Uri]$u).Port
        if (Test-PortFree $port) {
            try {
                if (Start-Ollama $exe $u) { return [pscustomobject]@{Exe=$exe;BaseUrl=$u} }
            } catch { Log "Ollama start attempt at $u failed: $($_.Exception.Message)" }
        } else {
            $pid = Get-ListeningPid $port
            if ($pid) {
                try { $pn = (Get-Process -Id $pid -ErrorAction Stop).ProcessName; Log "Port $port is occupied by PID $pid ($pn)." } catch { Log "Port $port is occupied by PID $pid." }
            } else { Log "Port $port is occupied." }
        }
    }

    # Never launch the vendor repair installer merely because an existing Ollama
    # server is unhealthy. That repair path was the source of locked temporary
    # installer files and parent-process termination on real machines.
    throw 'Could not start a healthy local Ollama API after safe process recovery and port selection.'
}

function Ensure-Models([string]$Exe,[string]$BaseUrl,$Production) {
    if ($Fast) { return @() }
    $pending = @()
    $oldHost = $env:OLLAMA_HOST
    try {
        $env:OLLAMA_HOST = $BaseUrl.Substring('http://'.Length)
        foreach ($model in @($Production.required_models)) {
            $name = [string]$model
            $list = (& $Exe list 2>$null | Out-String)
            if ($list -match [regex]::Escape($name)) { Log "Required Ollama model already present: $name"; continue }
            $ok = $false
            foreach ($attempt in 1..3) {
                try {
                    Log "Pulling required Ollama model: $name (attempt $attempt/3)"
                    & $Exe pull $name
                    if ($LASTEXITCODE -eq 0) { $ok = $true; break }
                } catch { Log "Model pull error for ${name}: $($_.Exception.Message)" }
                if ($attempt -lt 3) { Start-Sleep -Seconds (5*$attempt) }
            }
            if (-not $ok) { $pending += $name; Log "WARNING: model $name remains pending; background repair will retry it." }
        }
    } finally {
        if ($null -eq $oldHost) { Remove-Item Env:OLLAMA_HOST -ErrorAction SilentlyContinue } else { $env:OLLAMA_HOST = $oldHost }
    }
    return [string[]]$pending
}

function Save-Runtime([string]$PythonExe,[string]$OllamaUrl,[string[]]$Pending) {
    New-Item -ItemType Directory -Force -Path $JubiDataDir | Out-Null
    $data = @{}
    if (Test-Path $RuntimeConfig) {
        try { $old = Get-Content $RuntimeConfig -Raw | ConvertFrom-Json; foreach ($p in $old.PSObject.Properties) { $data[$p.Name]=$p.Value } } catch {}
    }
    $data['python_exe']=$PythonExe
    $data['ollama_base_url']=$OllamaUrl
    $data['pending_models']=@($Pending)
    $data['updated_at']=(Get-Date).ToUniversalTime().ToString('o')
    $data | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $RuntimeConfig -Encoding UTF8
    try {
        [Environment]::SetEnvironmentVariable('JUBI_OLLAMA_URL',$OllamaUrl,'User')
        $env:JUBI_OLLAMA_URL=$OllamaUrl
    } catch { Log "WARNING: could not persist JUBI_OLLAMA_URL: $($_.Exception.Message)" }
    Log "Jubi runtime state saved. Ollama=$OllamaUrl PendingModels=$(@($Pending).Count)"
}

try {
    if (-not [Environment]::Is64BitOperatingSystem) { throw 'Jubi requires 64-bit Windows.' }
    if (-not (Test-Path $ConfigPath)) { throw 'config\bootstrap.json is missing.' }
    if (-not (Test-Path $ProductionPath)) { throw 'config\production.json is missing.' }
    $bootstrap = Get-Content $ConfigPath -Raw | ConvertFrom-Json
    $production = Get-Content $ProductionPath -Raw | ConvertFrom-Json
    Log "Jubi prerequisite check started. Fast=$Fast Repair=$Repair"
    $python = Ensure-Python311 $bootstrap
    $ollama = Ensure-Ollama $bootstrap
    $pending = Ensure-Models $ollama.Exe $ollama.BaseUrl $production
    Save-Runtime $python $ollama.BaseUrl $pending
    Log 'Jubi prerequisite check completed successfully.'
    exit 0
} catch {
    Log "PREREQUISITE FAILURE: $($_.Exception.Message)"
    exit 1
}
