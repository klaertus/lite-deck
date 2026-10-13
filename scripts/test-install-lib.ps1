# Self-test of install-lib.ps1. Runs without administrator rights; touches only HKCU:\Software\lite-deck-test.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "install-lib.ps1")
$failures = 0
function Check([string]$name, [bool]$ok) {
    if ($ok) { Write-Host "ok   $name" } else { Write-Host "FAIL $name" -ForegroundColor Red; $script:failures++ }
}

Check "quoted path keeps spaces inside quotes" ((Get-QuotedPath 'C:\Program Files\lite-deck\a.exe') -eq '"C:\Program Files\lite-deck\a.exe"')
Check "quoted path does not double quotes" ((Get-QuotedPath '"C:\x y\a.exe"') -eq '"C:\x y\a.exe"')
Check "Test-IsAdmin returns a boolean" ((Test-IsAdmin) -is [bool])
Check "install dir is under Program Files" ($LdInstallDir -like "*\lite-deck" -and $LdInstallDir.StartsWith($env:ProgramFiles))
Check "file list has the binaries and scripts" ((@('lite-deck.exe','lite-deck-sensors.exe','AMDFamily17.bin','install.ps1','uninstall.ps1') | ForEach-Object { $LdFiles -contains $_ }) -notcontains $false)
Check "legacy names are the former ones" ($LdLegacyServiceName -eq "LibreDeckSensors" -and $LdLegacyRunName -eq "libre-deck" -and $LdLegacyInstallDir -like "*\libre-deck" -and $LdLegacyInstallDir.StartsWith($env:ProgramFiles))
Check "legacy names never match the new ones" ($LdLegacyServiceName -ne $LdServiceName -and $LdLegacyRunName -ne $LdRunName -and $LdLegacyInstallDir -ne $LdInstallDir -and $LdLegacyStateKey -ne $LdStateKey -and $LdLegacyWindowClass -ne $LdWindowClass)
Check "legacy and new process lists are disjoint" (-not ($LdLegacyProcessNames | Where-Object { $LdProcessNames -contains $_ }))
Check "the cleanup and prompt helpers exist" ((@('Remove-LegacyInstall','Test-LegacyInstall','Stop-DeckDaemon','Wait-ProcessesExit','Remove-DeckService','Remove-FolderWithRetry','Test-CanPrompt') | ForEach-Object { [bool](Get-Command $_ -CommandType Function -ErrorAction SilentlyContinue) }) -notcontains $false)
Check "file list has the licence and the notices" ($LdFiles -contains 'LICENSE' -and $LdFiles -contains 'THIRD_PARTY_NOTICES.md')
Check "the staged legacy helpers exist" ((@('Stop-LegacyInstall','Restore-LegacyInstall','Test-ServiceMarkedForDeletion','Get-ScDeleteOutcome','Get-MarkedForDeletionMessage') | ForEach-Object { [bool](Get-Command $_ -CommandType Function -ErrorAction SilentlyContinue) }) -notcontains $false)
Check "sc delete 0 is deleted" ((Get-ScDeleteOutcome 0) -eq "deleted")
Check "sc delete 1072 (marked for deletion) is pending, not an error" ((Get-ScDeleteOutcome 1072) -eq "pending")
Check "sc delete 5 (access denied) is an error" ((Get-ScDeleteOutcome 5) -eq "error")
Check "sc delete 1060 (no such service) is an error" ((Get-ScDeleteOutcome 1060) -eq "error")
Check "marked-for-deletion message names the service and asks for a restart" ((Get-MarkedForDeletionMessage -Name "X1" -Script "y.ps1") -match 'X1' -and (Get-MarkedForDeletionMessage -Name "X1" -Script "y.ps1") -match 'restart' -and (Get-MarkedForDeletionMessage -Name "X1" -Script "y.ps1") -match 'y\.ps1')
Check "Restore-LegacyInstall does nothing when nothing ran" ($null -eq (Restore-LegacyInstall -State @{ DaemonWasRunning = $false; ServiceWasRunning = $false }))
Check "Restore-LegacyInstall accepts no state" ($null -eq (Restore-LegacyInstall -State $null))
Check "Test-CanPrompt returns a boolean" ((Test-CanPrompt) -is [bool])
Check "Wait-ProcessesExit returns at once for absent processes" ($null -eq (Wait-ProcessesExit -Names @("ld-no-such-process-$PID")))
Check "the stop and restore helpers exist" ((@('Find-DeckWindow','Get-SessionDeckProcesses','Lock-ProcessIdentity','Stop-DeckProcesses','Wait-DeckProcesses','Test-ProcessExited','Get-CurrentInstallState','Restore-CurrentInstall') | ForEach-Object { [bool](Get-Command $_ -CommandType Function -ErrorAction SilentlyContinue) }) -notcontains $false)

# The graceful close request must find a window whose title is not empty (the daemon's is
# "lite-deck"). Explorer's desktop window (class Progman, title "Program Manager") stands in for it.
if (Get-Process -Name explorer -ErrorAction SilentlyContinue) {
    Check "Find-DeckWindow finds a window by class whatever its title" ((Find-DeckWindow -WindowClass "Progman") -ne [IntPtr]::Zero)
    Check "a `$null title becomes an empty one (the bug Find-DeckWindow avoids)" ([LiteDeck.LdUser32]::FindWindowW("Progman", $null) -eq [IntPtr]::Zero)
} else {
    Write-Host "skip Find-DeckWindow checks (no desktop)"
}
Check "Find-DeckWindow returns zero for an absent class" ((Find-DeckWindow -WindowClass "LdNoSuchClass$PID") -eq [IntPtr]::Zero)

# Stand-in daemons: a copy of ping.exe under a test name in %TEMP%, stopped only through their own
# process objects. No window of their class exists, as with a daemon that cannot answer.
$fakeDir = Join-Path $env:TEMP "ld-test-deck-$PID"
$fakeName = "ld-test-deck-$PID"
New-Item -ItemType Directory -Force -Path $fakeDir | Out-Null
$fakeExe = Join-Path $fakeDir "$fakeName.exe"
Copy-Item -LiteralPath (Join-Path $env:WINDIR "System32\PING.EXE") -Destination $fakeExe -Force
$started = @()
function Start-Fake([int]$Pings) {
    $p = Start-Process -FilePath $fakeExe -ArgumentList "-n", $Pings, "127.0.0.1" -WindowStyle Hidden -PassThru
    $script:started += $p
    return $p
}
try {
    $a = Start-Fake 120
    $b = Start-Fake 120
    $t0 = Get-Date
    $stopped = Stop-DeckProcesses -Processes @($a) -WindowClass "LdNoSuchClass$PID" -Label "fake deck" -TimeoutSeconds 3 -KillWaitSeconds 10
    $took = ((Get-Date) - $t0).TotalSeconds
    Check "a daemon that does not close is stopped after the wait" ($stopped -and (Test-ProcessExited $a))
    Check "it is waited for the whole timeout first, not much longer ($([Math]::Round($took, 1)) s)" ($took -ge 2.8 -and $took -lt 8)
    Check "another process of the same name is left running" (-not (Test-ProcessExited $b))
    $c = Start-Fake 2  # ends by itself after about a second
    $t0 = Get-Date
    $stopped = Stop-DeckProcesses -Processes @($c) -WindowClass "LdNoSuchClass$PID" -Label "fake deck" -TimeoutSeconds 20
    $took = ((Get-Date) - $t0).TotalSeconds
    Check "a daemon that closes in time is not stopped" ($stopped -and $c.ExitCode -eq 0)
    Check "its exit is noticed at once ($([Math]::Round($took, 1)) s)" ($took -lt 5)
    Check "Stop-DeckProcesses accepts processes that already ended" (Stop-DeckProcesses -Processes @($a, $c) -WindowClass "LdNoSuchClass$PID" -Label "fake deck" -TimeoutSeconds 1)
    Check "Stop-DeckProcesses accepts an empty list" (Stop-DeckProcesses -Processes @() -WindowClass "LdNoSuchClass$PID" -Label "fake deck" -TimeoutSeconds 1)
    $mySession = (Get-Process -Id $PID).SessionId
    $inSession = @(Get-SessionDeckProcesses -ProcessName $fakeName)
    Check "Get-SessionDeckProcesses finds a daemon of this session" ([bool]($inSession | Where-Object { $_.Id -eq $b.Id }))
    Check "Get-SessionDeckProcesses ignores the daemons of another session" (@(Get-SessionDeckProcesses -ProcessName $fakeName -SessionId ($mySession + 1000)).Count -eq 0)
    Check "the processes it returns hold their handle (no PID reuse while waiting)" (@($inSession | Where-Object { $_.Handle -eq [IntPtr]::Zero }).Count -eq 0)
    $daemonCalls = 0
    $r = @(Restore-CurrentInstall -State @{ DaemonWasRunning = $true; ServiceWasRunning = $false } -ServiceName "LdNoSuchService$PID" -DaemonPath $fakeExe -StartDaemon { param($p) $script:daemonCalls++ })
    Check "Restore-CurrentInstall does not start a second daemon when one runs in this session" ($r -contains "daemon already running" -and $daemonCalls -eq 0)
    Check "Stop-DeckDaemon returns at once when nothing of that name runs" (Stop-DeckDaemon -WindowClass "LdNoSuchClass$PID" -ProcessName "ld-no-such-process-$PID" -TimeoutSeconds 1)
} finally {
    foreach ($p in $started) { if (-not (Test-ProcessExited $p)) { $p.Kill(); $p.WaitForExit(5000) | Out-Null } }
    Remove-Item -LiteralPath $fakeDir -Recurse -Force -ErrorAction SilentlyContinue
}

$state = Get-CurrentInstallState -ServiceName "LdNoSuchService$PID"
Check "Get-CurrentInstallState reports booleans" ($state.DaemonWasRunning -is [bool] -and $state.ServiceWasRunning -is [bool])
Check "an absent service was not running" (-not $state.ServiceWasRunning)
$noExe = Join-Path $env:TEMP "ld-no-such-dir-$PID\lite-deck.exe"
Check "Restore-CurrentInstall accepts no state" ($null -eq (Restore-CurrentInstall -State $null -ServiceName "LdNoSuchService$PID" -DaemonPath $noExe))
Check "Restore-CurrentInstall restarts nothing that did not run" ($null -eq (Restore-CurrentInstall -State @{ DaemonWasRunning = $false; ServiceWasRunning = $false } -ServiceName "LdNoSuchService$PID" -DaemonPath $noExe))
Check "Restore-CurrentInstall skips what is no longer installed" ($null -eq (Restore-CurrentInstall -State @{ DaemonWasRunning = $true; ServiceWasRunning = $true } -ServiceName "LdNoSuchService$PID" -DaemonPath $noExe))

# A failed update whose service no longer starts (the likeliest failure) must still restart the daemon.
$restoreDir = Join-Path $env:TEMP "ld-restore-test-$PID"
New-Item -ItemType Directory -Force -Path $restoreDir | Out-Null
$restoreExe = Join-Path $restoreDir "ld-restore-daemon-$PID.exe"  # a file only: never run
Set-Content -LiteralPath $restoreExe -Value "x"
try {
    $both = @{ DaemonWasRunning = $true; ServiceWasRunning = $true }
    $script:daemonStarts = @(); $script:serviceStarts = 0
    $r = @(Restore-CurrentInstall -State $both -ServiceName "LdFakeService" -DaemonPath $restoreExe `
        -StartDaemon { param($p) $script:daemonStarts += $p } -StartService { param($n) throw "service will not start" } `
        -ServiceStatus { param($n) "Stopped" } 6>$null)
    Check "a service that fails to start does not keep the daemon from restarting" ($script:daemonStarts -contains $restoreExe -and $r -contains "daemon")
    Check "the failed service is not reported as restarted" (-not ($r -contains "service"))
    $script:daemonStarts = @()
    $r = @(Restore-CurrentInstall -State $both -ServiceName "LdFakeService" -DaemonPath $restoreExe `
        -StartDaemon { param($p) throw "explorer refused" } -StartService { param($n) $script:serviceStarts++ } `
        -ServiceStatus { param($n) "Stopped" } 6>$null)
    Check "a daemon that fails to start does not keep the service from restarting" ($script:serviceStarts -eq 1 -and $r -contains "service" -and -not ($r -like "daemon*"))
    $script:serviceStarts = 0
    $r = @(Restore-CurrentInstall -State $both -ServiceName "LdFakeService" -DaemonPath $restoreExe `
        -StartDaemon { param($p) } -StartService { param($n) $script:serviceStarts++ } -ServiceStatus { param($n) "Running" })
    Check "a service already running is not started again" ($script:serviceStarts -eq 0 -and $r -contains "daemon")
} finally {
    Remove-Item -LiteralPath $restoreDir -Recurse -Force -ErrorAction SilentlyContinue
}

$tmpDir = Join-Path $env:TEMP "ld remove test $PID"
New-Item -ItemType Directory -Force -Path (Join-Path $tmpDir "sub") | Out-Null
Set-Content -LiteralPath (Join-Path $tmpDir "sub/a.txt") -Value "x"
Remove-FolderWithRetry -Path $tmpDir
Check "Remove-FolderWithRetry removes a folder tree" (-not (Test-Path -LiteralPath $tmpDir))
Remove-FolderWithRetry -Path $tmpDir
Check "Remove-FolderWithRetry accepts a missing folder" (-not (Test-Path -LiteralPath $tmpDir))

$base = "HKCU:\Software\lite-deck-test"
Remove-Item -LiteralPath $base -Recurse -Force -ErrorAction SilentlyContinue
$run = "$base\Run"; $approved = "$base\StartupApproved"
New-Item -Path $run -Force | Out-Null
Set-ItemProperty -LiteralPath $run -Name "StreamDock" -Value 'C:\Program Files\MiraBox\StreamDock.exe --RunInBackground'
try {
    Check "startup entry found" ((Get-StartupEntry -Name "StreamDock" -RunKey $run) -like "*StreamDock.exe*")
    Check "missing entry is null" ($null -eq (Get-StartupEntry -Name "Nope" -RunKey $run))
    Check "enabled when no approval value exists" (Test-StartupEntryEnabled -Name "StreamDock" -ApprovedKey $approved)
    Disable-StartupEntry -Name "StreamDock" -ApprovedKey $approved
    Check "disabled after Disable" (-not (Test-StartupEntryEnabled -Name "StreamDock" -ApprovedKey $approved))
    $bytes = (Get-ItemProperty -LiteralPath $approved -Name "StreamDock").StreamDock
    Check "approval value is 12 bytes starting with 03" ($bytes.Length -eq 12 -and $bytes[0] -eq 3)
    Disable-StartupEntry -Name "StreamDock" -ApprovedKey $approved
    Check "Disable is idempotent" (-not (Test-StartupEntryEnabled -Name "StreamDock" -ApprovedKey $approved))
    Enable-StartupEntry -Name "StreamDock" -ApprovedKey $approved
    Check "enabled after Enable" (Test-StartupEntryEnabled -Name "StreamDock" -ApprovedKey $approved)
    Check "Run value untouched" ((Get-StartupEntry -Name "StreamDock" -RunKey $run) -like "*StreamDock.exe*")
    $services = "$base\Services"
    New-Item -Path "$services\Svc1" -Force | Out-Null
    New-Item -Path "$services\Svc2" -Force | Out-Null
    Set-ItemProperty -LiteralPath "$services\Svc2" -Name "DeleteFlag" -Value 1 -Type DWord
    New-Item -Path "$services\Svc3" -Force | Out-Null
    Set-ItemProperty -LiteralPath "$services\Svc3" -Name "DeleteFlag" -Value 0 -Type DWord
    Check "service without DeleteFlag is not marked for deletion" (-not (Test-ServiceMarkedForDeletion -Name "Svc1" -ServicesKey $services))
    Check "service with DeleteFlag=1 is marked for deletion" (Test-ServiceMarkedForDeletion -Name "Svc2" -ServicesKey $services)
    Check "service with DeleteFlag=0 is not marked for deletion" (-not (Test-ServiceMarkedForDeletion -Name "Svc3" -ServicesKey $services))
    Check "missing service is not marked for deletion" (-not (Test-ServiceMarkedForDeletion -Name "Nope" -ServicesKey $services))
} finally {
    Remove-Item -LiteralPath $base -Recurse -Force -ErrorAction SilentlyContinue
}

$real = "E=admin@namazso.eu, CN=namazso.eu, O=namazso, L=Debrecen, C=HU"
Check "subject with the exact CN is accepted" (Test-SubjectHasCn $real "namazso.eu")
Check "CN namazso-tools is refused" (-not (Test-SubjectHasCn "CN=namazso-tools, O=x" "namazso.eu"))
Check "CN namazso.eu.evil is refused" (-not (Test-SubjectHasCn "CN=namazso.eu.evil, O=x" "namazso.eu"))
Check "namazso.eu only in E= is refused" (-not (Test-SubjectHasCn "E=a@namazso.eu, CN=evil, O=namazso.eu" "namazso.eu"))

$tmp = Join-Path $env:TEMP "ld test $PID.txt"
Set-Content -LiteralPath $tmp -Value "x"
try {
    $threw = $false
    try { Assert-SignedBy -Path $tmp -Cn "namazso.eu" } catch { $threw = $true }
    Check "unsigned file is refused" $threw
} finally { Remove-Item -LiteralPath $tmp -Force }

$notepad = Join-Path $env:WINDIR "System32\notepad.exe"
$threw = $false
try { Assert-SignedBy -Path $notepad -Cn "namazso.eu" } catch { $threw = $true }
Check "file signed by someone else is refused" $threw

if ($failures) { Write-Host "$failures failure(s)" -ForegroundColor Red; exit 1 }
Write-Host "all install-lib checks passed"
