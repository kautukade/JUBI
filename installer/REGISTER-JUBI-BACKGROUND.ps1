param()
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$TaskName = 'Jubi Background Agent'
$Bootstrap = Join-Path $PSScriptRoot 'JUBI-BACKGROUND.ps1'
$PowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$LogDir = Join-Path $env:LOCALAPPDATA 'Jubi\logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$Log = Join-Path $LogDir 'task-registration.log'
function Log([string]$Message) { "[$(Get-Date -Format s)] $Message" | Tee-Object -FilePath $Log -Append | Write-Host }

try {
    if (-not (Test-Path -LiteralPath $Bootstrap)) { throw 'JUBI-BACKGROUND.ps1 is missing.' }
    if (-not (Test-Path -LiteralPath $PowerShell)) { throw 'Windows PowerShell is missing.' }

    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $user = $identity.Name
    Log "Registering $TaskName for $user."

    $arguments = '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $Bootstrap + '"'
    $action = New-ScheduledTaskAction -Execute $PowerShell -Argument $arguments -WorkingDirectory $Root
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
    $trigger.Delay = 'PT10S'
    $settings = New-ScheduledTaskSettingsSet `
        -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries `
        -StartWhenAvailable `
        -ExecutionTimeLimit ([TimeSpan]::Zero) `
        -MultipleInstances IgnoreNew `
        -RestartCount 999 `
        -RestartInterval (New-TimeSpan -Minutes 1)
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
    $task = New-ScheduledTask -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Description 'Keeps Jubi running in the signed-in Windows session and applies verified JUBI updates.'

    Register-ScheduledTask -TaskName $TaskName -InputObject $task -Force | Out-Null
    Start-ScheduledTask -TaskName $TaskName
    Start-Sleep -Seconds 1
    $registered = Get-ScheduledTask -TaskName $TaskName -ErrorAction Stop
    Log "Jubi background task registered. State=$($registered.State)"
    exit 0
}
catch {
    Log "TASK REGISTRATION FAILED: $($_.Exception.Message)"
    exit 1
}
