# lite-deck uninstaller. Run as administrator. PawnIO is kept unless -RemovePawnIO is given.
# Also removes what an installation under the former name (libre-deck) may have left.
param([switch]$RemovePawnIO)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "install-lib.ps1")

if (-not (Test-IsAdmin)) {
    Write-Host "uninstall.ps1 must be run as administrator." -ForegroundColor Red
    exit 1
}
if (-not [Environment]::Is64BitProcess) {
    Write-Host "uninstall.ps1 must run in 64-bit PowerShell." -ForegroundColor Red
    exit 1
}
# The script may run from %ProgramFiles%\lite-deck itself: that folder is removed last.
Stop-LiteDeck -Script "uninstall.ps1"
Remove-DeckService -Name $LdServiceName
Remove-ItemProperty -LiteralPath $LdRunKey -Name $LdRunName -ErrorAction SilentlyContinue
Remove-ItemProperty -LiteralPath $LdApprovedKey -Name $LdRunName -ErrorAction SilentlyContinue  # set by Task Manager
Remove-LegacyInstall -Script "uninstall.ps1"  # moves its StreamDock flag to $LdStateKey, read below

$state = Get-ItemProperty -LiteralPath $LdStateKey -ErrorAction SilentlyContinue
if ($state -and $state.StreamDockDisabledByInstaller -eq 1) {
    Enable-StartupEntry -Name "StreamDock"
    Write-Host "StreamDock autostart enabled again."
}
Remove-Item -LiteralPath $LdStateKey -Recurse -Force -ErrorAction SilentlyContinue

if ($RemovePawnIO) {
    $cmd = Get-PawnIOUninstallCommand
    if ($cmd) {
        $p = Start-Process -FilePath $cmd.File -ArgumentList $cmd.Arguments -Wait -PassThru
        Write-Host "PawnIO uninstaller finished (code $($p.ExitCode))."
    } else {
        Write-Host "PawnIO is not installed."
    }
} elseif (Get-PawnIOVersion) {
    Write-Host "PawnIO is kept (other software may use it). Run again with -RemovePawnIO to remove it."
}

Wait-ProcessesExit -Names $LdProcessNames -Script "uninstall.ps1"
Remove-FolderWithRetry -Path $LdInstallDir
$dataDirs = @("lite-deck", "libre-deck") | ForEach-Object { Join-Path $env:APPDATA $_ } | Where-Object { Test-Path -LiteralPath $_ }
$kept = if ($dataDirs) { " The config stays in $($dataDirs -join ' and ') (delete it by hand if needed)." } else { "" }
Write-Host "lite-deck uninstalled.$kept" -ForegroundColor Green
