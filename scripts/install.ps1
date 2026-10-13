# lite-deck installer. Run once as administrator, from the user's own account (UAC prompt).
#   -DisableStreamDock / -KeepStreamDock : answer the StreamDock question without prompting.
#   -InstallPawnIO / -NoPawnIO           : answer the PawnIO question without prompting. PawnIO is an
#                                          optional kernel driver, only needed for the CPU temperature.
# An installation under the former name (libre-deck) is only stopped at first. It is removed once
# lite-deck is installed and started, and restarted if the install fails before that. The daemon moves
# its data folder (%APPDATA%\libre-deck) at its first start. Safe to run again after any failure.
param([switch]$DisableStreamDock, [switch]$KeepStreamDock, [switch]$InstallPawnIO, [switch]$NoPawnIO)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "install-lib.ps1")

if (-not (Test-IsAdmin)) {
    Write-Host "install.ps1 must be run as administrator: right-click PowerShell > Run as administrator." -ForegroundColor Red
    exit 1
}
if (-not [Environment]::Is64BitProcess) {
    Write-Host "install.ps1 must run in 64-bit PowerShell (the install folder and the service depend on it)." -ForegroundColor Red
    exit 1
}
if ($InstallPawnIO -and $NoPawnIO) {
    Write-Host "-InstallPawnIO and -NoPawnIO cannot be given together." -ForegroundColor Red
    exit 1
}
if (Test-ServiceMarkedForDeletion -Name $LdServiceName) {
    Write-Host (Get-MarkedForDeletionMessage -Name $LdServiceName) -ForegroundColor Red
    exit 1
}
$src = $PSScriptRoot
# Every PawnIO module of the folder (the sensors service picks the one for this CPU), with its licence.
$moduleFiles = @(Get-DistModuleFiles @(Get-ChildItem -LiteralPath $src -File | ForEach-Object { $_.Name }))

# 1. PawnIO: never installed without consent (a switch, or a yes to the question)
$installPawnIO = $false
if (Get-PawnIOVersion) {
    Write-Host "PawnIO $(Get-PawnIOVersion) is already installed: the CPU temperature will be available."
} elseif ($InstallPawnIO) {
    $installPawnIO = $true
} elseif ($NoPawnIO) {
    Write-Host "PawnIO is not installed (-NoPawnIO): the CPU temperature will not be shown."
} elseif (Test-CanPrompt) {
    Write-Host ""
    Write-Host "PawnIO is optional. lite-deck only needs it to show the CPU temperature."
    Write-Host "It is a signed kernel driver from pawnio.eu, installed with its official setup."
    Write-Host "Everything else works without it, and you can install it later by running install.ps1 again."
    $installPawnIO = (Read-Host "Install PawnIO? (y/N)") -match '^[yY]'
    if (-not $installPawnIO) { Write-Host "PawnIO is not installed: the CPU temperature will not be shown." }
} else {
    Write-Host "PawnIO is not installed (no console to ask; use -InstallPawnIO to install it)."
}

$required = if ($installPawnIO) { $LdFiles + $moduleFiles + @("PawnIO_setup.exe") } else { $LdFiles + $moduleFiles }
foreach ($f in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $src $f))) { throw "Missing file in ${src}: $f (use the folder made by package.ps1)" }
}

# What existed before this run, so that a failure undoes only what this run added.
$hadService = [bool](Get-Service -Name $LdServiceName -ErrorAction SilentlyContinue)
$hadRunValue = [bool](Get-StartupEntry -Name $LdRunName)
$legacy = $null           # what ran of the libre-deck installation, once stopped
$current = $null          # what ran of the current lite-deck installation, once this run stops it
$daemonStarted = $false   # from then on, lite-deck is in place and the old installation is not restarted
try {
    # 2. An installation under the former name (libre-deck): stopped only (files and device released)
    if (Test-LegacyInstall) {
        Write-Host "Stopping the previous installation (libre-deck)..."
        $legacy = Stop-LegacyInstall -Script "install.ps1"
    }

    # 3. PawnIO (silent, official installer), only with consent
    if ($installPawnIO) {
        # Copy to an admin-only folder, verify THAT copy, run it, delete it (no gap between check and run).
        $tmpDir = Join-Path $LdInstallDir ("setup-" + [guid]::NewGuid().ToString("N"))
        New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
        try {
            $setup = Join-Path $tmpDir "PawnIO_setup.exe"
            Copy-Item -LiteralPath (Join-Path $src "PawnIO_setup.exe") -Destination $setup -Force
            Assert-SignedBy -Path $setup -Cn $PawnIOSignerCn
            Write-Host "Installing PawnIO..."
            $p = Start-Process -FilePath $setup -ArgumentList "-install", "-silent" -Wait -PassThru
        } finally {
            Remove-Item -LiteralPath $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
        }
        if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) { throw "PawnIO setup failed (code $($p.ExitCode))" }
        if (-not (Get-PawnIOVersion)) { throw "PawnIO is not detected after its setup ran" }
        if ($p.ExitCode -eq 3010) { Write-Host "PawnIO asks for a restart: the CPU temperature will show after it." -ForegroundColor Yellow }
    }

    # 4. Copy to %ProgramFiles%\lite-deck (stop what runs from there first: asked to close, given up
    # to 60 s, then that process alone is stopped). From here, a failure restarts what was running.
    $service = Get-Service -Name $LdServiceName -ErrorAction SilentlyContinue
    $current = Get-CurrentInstallState
    if ($service -and $service.Status -ne "Stopped") { Stop-Service -Name $LdServiceName -Force }
    Stop-LiteDeck -Script "install.ps1"
    Wait-ProcessesExit -Names $LdProcessNames -Script "install.ps1"
    New-Item -ItemType Directory -Force -Path $LdInstallDir | Out-Null
    foreach ($f in $LdFiles + $moduleFiles) {
        $from = [IO.Path]::GetFullPath((Join-Path $src $f))
        $to = [IO.Path]::GetFullPath((Join-Path $LdInstallDir $f))
        # Re-running from the installed copy: source and destination are the same file (-ne is case-insensitive).
        if ($from -ne $to) {
            for ($try = 1; ; $try++) {  # an exe can stay locked a moment after its process ended (antivirus scan)
                try { Copy-Item -LiteralPath $from -Destination $LdInstallDir -Force; break }
                catch { if ($try -ge 10) { throw }; Start-Sleep -Milliseconds 500 }
            }
        }
    }

    # 5. Sensors service, only with PawnIO (it cannot read anything without it): create once (quoted
    # path), then always automatic + started
    $serviceExe = Join-Path $LdInstallDir "lite-deck-sensors.exe"
    $hasPawnIO = [bool](Get-PawnIOVersion)
    if (-not $hasPawnIO) {
        if ($service) { Remove-DeckService -Name $LdServiceName }
        Write-Host "The sensors service is not installed (it needs PawnIO)."
    } elseif (-not $service) {
        try {
            New-Service -Name $LdServiceName -BinaryPathName (Get-QuotedPath $serviceExe) -DisplayName "lite-deck sensors" `
                -Description "Publishes the CPU temperature for lite-deck (PawnIO, read only)." -StartupType Automatic | Out-Null
        } catch {
            if (Test-ServiceMarkedForDeletion -Name $LdServiceName) { throw (Get-MarkedForDeletionMessage -Name $LdServiceName) }
            throw
        }
        if (-not (Get-Service -Name $LdServiceName -ErrorAction SilentlyContinue)) { throw "Creating service $LdServiceName failed" }
        & sc.exe failure $LdServiceName reset= 86400 actions= restart/60000 | Out-Null
        if ($LASTEXITCODE) { Write-Host "Warning: setting the service recovery actions failed (sc.exe code $LASTEXITCODE)." -ForegroundColor Yellow }
    } else {
        Set-Service -Name $LdServiceName -StartupType Automatic
    }
    if ($hasPawnIO) { Start-Service -Name $LdServiceName }

    # 6. Autostart of the daemon for the current user
    $daemon = Join-Path $LdInstallDir "lite-deck.exe"
    Set-ItemProperty -LiteralPath $LdRunKey -Name $LdRunName -Value (Get-QuotedPath $daemon)

    # 7. Offer to disable StreamDock's autostart
    if ((Get-StartupEntry -Name "StreamDock") -and (Test-StartupEntryEnabled -Name "StreamDock")) {
        $answer = if ($DisableStreamDock) { "y" } elseif ($KeepStreamDock) { "n" } else {
            Read-Host "StreamDock starts with Windows. Disable its autostart? (y/n)"
        }
        if ($answer -match '^[yY]') {
            Disable-StartupEntry -Name "StreamDock"
            New-Item -Path $LdStateKey -Force | Out-Null
            Set-ItemProperty -LiteralPath $LdStateKey -Name "StreamDockDisabledByInstaller" -Value 1 -Type DWord
            Write-Host "StreamDock autostart disabled (re-enable it in Task Manager > Startup apps)."
        }
    }

    # 8. Start the daemon unelevated: explorer.exe launches it with the shell's (non-admin) token.
    Start-Process -FilePath "explorer.exe" -ArgumentList (Get-QuotedPath $daemon)
    $daemonStarted = $true

    # 9. Only now remove the installation under the former name: service, folder, autostart, registry key
    Remove-LegacyInstall -Script "install.ps1"
} catch {
    $failure = $_
    Write-Host ""
    if ($daemonStarted) {
        Write-Host "lite-deck is installed and started, but removing the previous installation (libre-deck) failed:" -ForegroundColor Red
        Write-Host "  $failure" -ForegroundColor Red
        Write-Host "Run install.ps1 again to finish removing it (it is safe to re-run)." -ForegroundColor Red
        exit 1
    }
    Write-Host "Installation failed: $failure" -ForegroundColor Red
    $restoredDaemon = $false
    if ($current -and ($current.DaemonWasRunning -or $current.ServiceWasRunning)) {
        # The deck is never left without its daemon: what this run stopped is started again, as installed now.
        Write-Host "Restarting lite-deck as it ran before this attempt..."
        try {
            $restarted = @(Restore-CurrentInstall -State $current)  # each step reports its own failure
            $restoredDaemon = [bool]($restarted | Where-Object { $_ -like "daemon*" })
            if ($restarted) { Write-Host "lite-deck: $($restarted -join ', ')." }
        } catch {
            Write-Host "Restarting it failed too: $_" -ForegroundColor Red
        }
    }
    if ($legacy -and -not $restoredDaemon) {
        Write-Host "Restarting the previous installation (libre-deck), which was stopped but not removed..."
        try {
            # Undo only what this run added, so the two installations do not both run.
            if (-not $hadService) { Stop-Service -Name $LdServiceName -Force -ErrorAction SilentlyContinue }
            if (-not $hadRunValue) { Remove-ItemProperty -LiteralPath $LdRunKey -Name $LdRunName -ErrorAction SilentlyContinue }
            Restore-LegacyInstall -State $legacy
        } catch {
            Write-Host "Restarting it failed too: $_" -ForegroundColor Red
        }
    }
    Write-Host "The system may be partially installed. Fix the cause above, then run install.ps1 again (it is safe to re-run)." -ForegroundColor Red
    exit 1
}

$serviceState = if ($hasPawnIO) { (Get-Service $LdServiceName).Status } else { "not installed (no PawnIO)" }
Write-Host "lite-deck installed in $LdInstallDir. Service ${LdServiceName}: $serviceState." -ForegroundColor Green
