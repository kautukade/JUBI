param(
    [switch]$PurgeUserData
)
$ErrorActionPreference = 'SilentlyContinue'

# Stop the SARUS launcher if it is running. Ignore failures because the user
# may already have closed it or may be using the batch launcher instead.
& taskkill.exe /IM SARUS.exe /F *> $null

# Remove only SARUS' own Ring0 service/driver. Do not touch unrelated drivers.
& sc.exe query SarusRing0 *> $null
if ($LASTEXITCODE -eq 0) {
    & sc.exe stop SarusRing0 *> $null
    Start-Sleep -Milliseconds 500
    & sc.exe delete SarusRing0 *> $null
    Start-Sleep -Milliseconds 500
}
$Ring0Driver = Join-Path $env:SystemRoot 'System32\drivers\SarusRing0.sys'
Remove-Item -LiteralPath $Ring0Driver -Force -ErrorAction SilentlyContinue

# Protected broker keys and local SARUS state are deliberately preserved by
# default so reinstall/update does not invalidate receipts or erase user data.
if ($PurgeUserData) {
    $BrokerDir = Join-Path $env:LOCALAPPDATA 'SARUS\broker'
    $SaraData = Join-Path $env:LOCALAPPDATA 'SARA'
    Remove-Item -LiteralPath $BrokerDir -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $SaraData -Recurse -Force -ErrorAction SilentlyContinue
    [Environment]::SetEnvironmentVariable('SARUS_BROKER_SECRET_FILE', $null, 'User')
    [Environment]::SetEnvironmentVariable('SARUS_RECEIPT_SIGNING_KEY_FILE', $null, 'User')
}

exit 0
