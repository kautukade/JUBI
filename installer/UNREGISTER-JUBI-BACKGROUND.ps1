param()
$ErrorActionPreference = 'SilentlyContinue'
$TaskName = 'Jubi Background Agent'

Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue

$root = Split-Path -Parent $PSScriptRoot
Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
    ($_.CommandLine -match '(?i)-m\s+jubi\.(background|server)') -and
    ($_.ExecutablePath -like "$root*")
} | ForEach-Object {
    Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
}

exit 0
