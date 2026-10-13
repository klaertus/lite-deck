# Shared by install.ps1, uninstall.ps1, package.ps1 and test-install-lib.ps1. Defines values and functions only.
$LdServiceName = "LiteDeckSensors"
$LdInstallDir = Join-Path $env:ProgramFiles "lite-deck"
$LdRunKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
$LdApprovedKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run"
$LdStateKey = "HKCU:\Software\lite-deck"
# The fixed files of a distribution folder. The PawnIO modules (*.bin, each with its licence) come on top:
# Get-PawnIOModulePlan (package.ps1) and Get-DistModuleFiles (install.ps1).
$LdFiles = @("lite-deck.exe", "lite-deck-sensors.exe",
             "install.ps1", "uninstall.ps1", "install-lib.ps1", "LICENSE", "THIRD_PARTY_NOTICES.md")
$LdRunName = "lite-deck"
$LdWindowClass = "LiteDeckHiddenWindow"
$LdProcessNames = @("lite-deck-sensors", "lite-deck")
# The project's former name (libre-deck): what an older installation left, removed by install and uninstall.
$LdLegacyServiceName = "LibreDeckSensors"
$LdLegacyInstallDir = Join-Path $env:ProgramFiles "libre-deck"
$LdLegacyStateKey = "HKCU:\Software\libre-deck"
$LdLegacyRunName = "libre-deck"
$LdLegacyWindowClass = "LibreDeckHiddenWindow"
$LdLegacyProcessNames = @("libre-deck-sensors", "libre-deck")
$PawnIOSetupUrl = "https://github.com/namazso/PawnIO.Setup/releases/latest/download/PawnIO_setup.exe"
# Authenticode signer of PawnIO_setup.exe: CN=namazso.eu.
$PawnIOSignerCn = "namazso.eu"
$PawnIOUninstallKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO"
$LdServicesKey = "HKLM:\SYSTEM\CurrentControlSet\Services"

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return [bool](New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-QuotedPath([string]$Path) {
    if ($Path.StartsWith('"')) { return $Path }
    return '"' + $Path + '"'
}

# PawnIO's DisplayVersion, or $null when it is not installed.
function Get-PawnIOVersion {
    $v = Get-ItemProperty -LiteralPath $PawnIOUninstallKey -Name DisplayVersion -ErrorAction SilentlyContinue
    if ($v) { return $v.DisplayVersion } else { return $null }
}

# @{ File = ...; Arguments = ... } for a silent uninstall, or $null.
# PawnIO 2.2.0.0 registers QuietUninstallString = "<dir>\uninstall.exe" -uninstall -silent.
function Get-PawnIOUninstallCommand {
    $k = Get-ItemProperty -LiteralPath $PawnIOUninstallKey -ErrorAction SilentlyContinue
    if (-not $k) { return $null }
    $cmd = if ($k.QuietUninstallString) { $k.QuietUninstallString } else { "$($k.UninstallString) -uninstall -silent" }
    if ($cmd -match '^\s*"([^"]+)"\s*(.*)$') { return @{ File = $Matches[1]; Arguments = $Matches[2] } }
    if ($cmd -match '^\s*(\S+)\s*(.*)$') { return @{ File = $Matches[1]; Arguments = $Matches[2] } }
    return $null
}

# True when one component of the certificate subject ("E=..., CN=namazso.eu, O=...") is exactly CN=<Cn>.
function Test-SubjectHasCn([string]$Subject, [string]$Cn) {
    return [bool](($Subject -split ', ') | Where-Object { $_ -ieq "CN=$Cn" })
}

function Assert-SignedBy([string]$Path, [string]$Cn) {
    $sig = Get-AuthenticodeSignature -LiteralPath $Path
    if ($sig.Status -ne "Valid" -or -not $sig.SignerCertificate -or -not (Test-SubjectHasCn $sig.SignerCertificate.Subject $Cn)) {
        throw "Signature rejected for $Path (status $($sig.Status), signer '$($sig.SignerCertificate.Subject)')"
    }
}

# The licence shipped next to a PawnIO module: "AMDFamily17.bin" -> "AMDFamily17.COPYING.txt".
function Get-ModuleLicenceName([string]$Module) {
    return [IO.Path]::GetFileNameWithoutExtension($Module) + ".COPYING.txt"
}

# The lines of third_party\pawnio-modules\SOURCE.txt -> @{ "<Module>.bin" = "<SHA-256, upper case>" }.
# Each entry starts with a line "<Module>.bin - ..." and has a line "SHA-256: <64 hex digits>".
function Get-PawnIOModuleHashes([string[]]$Lines) {
    $hashes = @{}
    $current = $null
    foreach ($line in $Lines) {
        if ($line -match '^\s*([A-Za-z0-9_.-]+\.bin)\s+-\s') { $current = $Matches[1]; continue }
        if ($current -and $line -match '^\s*SHA-256:\s*([0-9A-Fa-f]{64})\s*$') {
            if ($hashes.ContainsKey($current)) { throw "SOURCE.txt: two SHA-256 lines for $current" }
            $hashes[$current] = $Matches[1].ToUpperInvariant()
        }
    }
    return $hashes
}

# What package.ps1 copies from third_party\pawnio-modules, from the names of the files there and the lines
# of its SOURCE.txt: one item per *.bin with its expected SHA-256, its licence (<Module>.COPYING, else the
# shared COPYING) and the licence's name in the distribution folder. Throws when there is no module, or
# when a module has an unexpected name, no SHA-256 in SOURCE.txt or no licence.
function Get-PawnIOModulePlan([string[]]$FileNames, [string[]]$SourceLines) {
    $hashes = Get-PawnIOModuleHashes $SourceLines
    $modules = @($FileNames | Where-Object { $_ -like "*.bin" } | Sort-Object)
    if (-not $modules) { throw "No PawnIO module (*.bin) in third_party\pawnio-modules" }
    foreach ($m in $modules) {
        if ($m -notmatch '^[A-Za-z0-9_-][A-Za-z0-9_.-]*\.bin$') { throw "PawnIO module with an unexpected name: $m" }
        if (-not $hashes.ContainsKey($m)) { throw "$m has no SHA-256 entry in third_party\pawnio-modules\SOURCE.txt" }
        $own = [IO.Path]::GetFileNameWithoutExtension($m) + ".COPYING"
        if ($FileNames -contains $own) { $licence = $own }
        elseif ($FileNames -contains "COPYING") { $licence = "COPYING" }
        else { throw "$m has no licence ($own or COPYING) in third_party\pawnio-modules" }
        [pscustomobject]@{ Module = $m; Sha256 = $hashes[$m]; Licence = $licence; LicenceOut = (Get-ModuleLicenceName $m) }
    }
}

# The PawnIO modules of a distribution folder and their licences, from the names of its files.
# Throws when there is no module or a module comes without its <Module>.COPYING.txt.
function Get-DistModuleFiles([string[]]$FileNames) {
    $modules = @($FileNames | Where-Object { $_ -like "*.bin" } | Sort-Object)
    if (-not $modules) { throw "No PawnIO module (*.bin) in the folder (use the folder made by package.ps1)" }
    $files = @()
    foreach ($m in $modules) {
        $licence = Get-ModuleLicenceName $m
        if ($FileNames -notcontains $licence) { throw "$m comes without $licence (use the folder made by package.ps1)" }
        $files += @($m, $licence)
    }
    return $files
}

function Initialize-LdUser32 {
    if (-not ("LiteDeck.LdUser32" -as [type])) {
        Add-Type -Name LdUser32 -Namespace LiteDeck -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern System.IntPtr FindWindowW(string cls, string title);
[DllImport("user32.dll")] public static extern bool PostMessageW(System.IntPtr h, uint msg, System.IntPtr w, System.IntPtr l);
'@
    }
}

# The top-level window of class $WindowClass, whatever its title, or [IntPtr]::Zero. The title must
# be passed as [NullString]::Value: PowerShell turns $null into "" for a string parameter, and
# FindWindowW(class, "") only finds a window whose title is empty (the daemon's is "lite-deck").
function Find-DeckWindow([string]$WindowClass) {
    Initialize-LdUser32
    return [LiteDeck.LdUser32]::FindWindowW($WindowClass, [NullString]::Value)
}

# Makes each process object hold an open handle to its process (.NET opens one on the first read of
# .Handle and keeps it). While it is held, Windows cannot give the PID to another process, so
# HasExited and Kill() below always mean that process. A process we may not open is left as it is.
function Lock-ProcessIdentity([System.Diagnostics.Process[]]$Processes) {
    foreach ($p in $Processes) { try { $null = $p.Handle } catch { } }
}

# The processes named $ProcessName in session $SessionId (default: this script's session, the user
# who runs the installer), each holding its handle (Lock-ProcessIdentity). A daemon of another
# user's session is never asked to close, waited for, stopped or counted as running.
function Get-SessionDeckProcesses([string]$ProcessName, [int]$SessionId = (Get-Process -Id $PID).SessionId) {
    $procs = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $SessionId })
    Lock-ProcessIdentity -Processes $procs
    return $procs
}

# True when the process (a System.Diagnostics.Process) has ended. Asked through the handle it holds
# (Lock-ProcessIdentity), so a later process that reuses its PID is never taken for it.
function Test-ProcessExited([System.Diagnostics.Process]$Process) {
    try { return $Process.HasExited } catch { return -not (Get-Process -Id $Process.Id -ErrorAction SilentlyContinue) }
}

# Waits up to $Seconds for every one of $Processes to end. True when they all have.
function Wait-DeckProcesses([System.Diagnostics.Process[]]$Processes, [double]$Seconds) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ($true) {
        $left = @($Processes | Where-Object { -not (Test-ProcessExited $_) })
        if (-not $left) { return $true }
        if ((Get-Date) -ge $deadline) { return $false }
        Start-Sleep -Milliseconds 200
    }
}

# Clean shutdown of the given daemon processes: WM_CLOSE to the hidden window of class $WindowClass
# (the same path as "Quit"), then up to $TimeoutSeconds for them to end. Any of them still running
# after that is stopped through its own process object (never by name: nothing else is touched),
# and given $KillWaitSeconds to disappear. True when none of them runs any more.
function Stop-DeckProcesses([System.Diagnostics.Process[]]$Processes, [string]$WindowClass, [string]$Label,
                            [int]$TimeoutSeconds = 60, [int]$KillWaitSeconds = 15) {
    $Processes = @($Processes | Where-Object { $_ })
    Lock-ProcessIdentity -Processes $Processes
    $Processes = @($Processes | Where-Object { -not (Test-ProcessExited $_) })
    if (-not $Processes) { return $true }
    $h = Find-DeckWindow -WindowClass $WindowClass
    if ($h -ne [IntPtr]::Zero) { [LiteDeck.LdUser32]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }
    if (Wait-DeckProcesses -Processes $Processes -Seconds 2) { return $true }
    Write-Host "Waiting for $Label to close (up to $TimeoutSeconds s)..."
    if (Wait-DeckProcesses -Processes $Processes -Seconds ([Math]::Max(0, $TimeoutSeconds - 2))) { return $true }
    foreach ($p in $Processes) {
        if (Test-ProcessExited $p) { continue }
        Write-Host "$Label (PID $($p.Id)) did not close within $TimeoutSeconds s: stopping that process." -ForegroundColor Yellow
        try { $p.Kill() } catch { Write-Host "Stopping PID $($p.Id) failed: $($_.Exception.Message)" -ForegroundColor Yellow }
    }
    return (Wait-DeckProcesses -Processes $Processes -Seconds $KillWaitSeconds)
}

# Stops the daemon processes named $ProcessName that run now in this session (see Stop-DeckProcesses).
function Stop-DeckDaemon([string]$WindowClass, [string]$ProcessName, [int]$TimeoutSeconds = 60) {
    $procs = Get-SessionDeckProcesses -ProcessName $ProcessName
    return (Stop-DeckProcesses -Processes $procs -WindowClass $WindowClass -Label "$ProcessName" -TimeoutSeconds $TimeoutSeconds)
}

# Stops the running lite-deck daemon; throws when it is still there afterwards.
function Stop-LiteDeck([string]$Script = "install.ps1") {
    if (-not (Stop-DeckDaemon -WindowClass $LdWindowClass -ProcessName "lite-deck")) {
        throw "lite-deck.exe did not close and could not be stopped: restart Windows, then run $Script again."
    }
}

# What runs of the current installation (lite-deck), before an update stops it: the daemon of this
# session, and the service.
function Get-CurrentInstallState([string]$ServiceName = $LdServiceName) {
    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    return @{
        DaemonWasRunning = [bool](Get-SessionDeckProcesses -ProcessName "lite-deck")
        ServiceWasRunning = [bool]($svc -and $svc.Status -eq "Running")
    }
}

# After a failed update: restarts what Get-CurrentInstallState saw running and what is still
# installed, so the deck keeps working. The daemon comes first, and each step handles its own
# failure: a service that cannot start never keeps the daemon from starting. The daemon is started
# unelevated (explorer.exe), only when none runs in this session. Returns what the deck now has:
# "daemon" (started), "daemon already running", "service" (started). Failures are written, not thrown.
# The tests replace $StartDaemon, $StartService and $ServiceStatus.
function Restore-CurrentInstall([hashtable]$State, [string]$ServiceName = $LdServiceName,
                                [string]$DaemonPath = (Join-Path $LdInstallDir "lite-deck.exe"),
                                [scriptblock]$StartDaemon = { param($Path) Start-Process -FilePath "explorer.exe" -ArgumentList (Get-QuotedPath $Path) },
                                [scriptblock]$StartService = { param($Name) Start-Service -Name $Name },
                                [scriptblock]$ServiceStatus = { param($Name) $s = Get-Service -Name $Name -ErrorAction SilentlyContinue; if ($s) { [string]$s.Status } else { $null } }) {
    $result = @()
    if (-not $State) { return $result }
    if ($State.DaemonWasRunning -and (Test-Path -LiteralPath $DaemonPath)) {
        try {
            if (Get-SessionDeckProcesses -ProcessName ([IO.Path]::GetFileNameWithoutExtension($DaemonPath))) {
                $result += "daemon already running"
            } else {
                & $StartDaemon $DaemonPath | Out-Null
                $result += "daemon"
            }
        } catch {
            Write-Host "Restarting lite-deck.exe failed: $_" -ForegroundColor Red
        }
    }
    if ($State.ServiceWasRunning) {
        try {
            $status = & $ServiceStatus $ServiceName
            if ($status -and $status -ne "Running") {
                & $StartService $ServiceName | Out-Null
                $result += "service"
            }
        } catch {
            Write-Host "Restarting the service $ServiceName failed: $_" -ForegroundColor Red
        }
    }
    return $result
}

# Stop-Service returns before the process has exited: waits for the exes to let go of their files.
function Wait-ProcessesExit([string[]]$Names, [string]$Script = "install.ps1") {
    foreach ($name in $Names) {
        Get-Process -Name $name -ErrorAction SilentlyContinue | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
        if (Get-Process -Name $name -ErrorAction SilentlyContinue) { throw "$name.exe is still running: close it and run $Script again." }
    }
}

# Meaning of an sc.exe delete exit code: "deleted", "pending" (1072, ERROR_SERVICE_MARKED_FOR_DELETE:
# already deleted, removed once the last handle closes or at the next restart) or "error".
function Get-ScDeleteOutcome([int]$Code) {
    if ($Code -eq 0) { return "deleted" }
    if ($Code -eq 1072) { return "pending" }
    return "error"
}

# True when a service is marked for deletion (deleted while services.msc or another tool held it open):
# the SCM sets DeleteFlag=1 on its key. Until that handle closes or Windows restarts, it can be neither
# configured nor started, and no service of the same name can be created.
function Test-ServiceMarkedForDeletion([string]$Name, [string]$ServicesKey = $LdServicesKey) {
    $v = Get-ItemProperty -LiteralPath (Join-Path $ServicesKey $Name) -Name DeleteFlag -ErrorAction SilentlyContinue
    return [bool]($v -and $v.DeleteFlag -eq 1)
}

function Get-MarkedForDeletionMessage([string]$Name, [string]$Script = "install.ps1") {
    return "Service $Name is marked for deletion (it was deleted while services.msc or another tool held it open). " +
           "Close services.msc and restart Windows, then run $Script again."
}

# Stops and deletes a service if it exists. A deletion already pending is only a warning.
function Remove-DeckService([string]$Name) {
    if (-not (Get-Service -Name $Name -ErrorAction SilentlyContinue)) { return }
    Stop-Service -Name $Name -Force -ErrorAction SilentlyContinue
    & sc.exe delete $Name | Out-Null
    $code = $LASTEXITCODE
    switch (Get-ScDeleteOutcome $code) {
        "pending" { Write-Host "Service $Name is already marked for deletion (removed at the next restart, or once services.msc is closed)." -ForegroundColor Yellow }
        "error" { throw "sc.exe delete $Name failed (code $code)" }
    }
}

# Removes a folder, retrying for 10 s (an exe can stay locked a moment after its process ended).
function Remove-FolderWithRetry([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $deadline = (Get-Date).AddSeconds(10)
    while ($true) {
        try { Remove-Item -LiteralPath $Path -Recurse -Force; break }
        catch { if ((Get-Date) -gt $deadline) { throw } ; Start-Sleep -Milliseconds 500 }
    }
}

# True when something of an installation under the former name (libre-deck) is still there.
function Test-LegacyInstall {
    return [bool]((Get-Service -Name $LdLegacyServiceName -ErrorAction SilentlyContinue) -or
                  (Test-Path -LiteralPath $LdLegacyInstallDir) -or
                  (Get-StartupEntry -Name $LdLegacyRunName) -or
                  (Get-Process -Name $LdLegacyProcessNames -ErrorAction SilentlyContinue))
}

# Stops an installation under the former name (daemon and service) without removing anything, so its
# files and the device are released. Returns what was running, for Restore-LegacyInstall.
function Stop-LegacyInstall([string]$Script = "install.ps1") {
    $svc = Get-Service -Name $LdLegacyServiceName -ErrorAction SilentlyContinue
    $state = @{
        DaemonWasRunning = [bool](Get-Process -Name "libre-deck" -ErrorAction SilentlyContinue)
        ServiceWasRunning = [bool]($svc -and $svc.Status -eq "Running")
    }
    Stop-DeckDaemon -WindowClass $LdLegacyWindowClass -ProcessName "libre-deck" | Out-Null
    if ($svc) { Stop-Service -Name $LdLegacyServiceName -Force -ErrorAction SilentlyContinue | Out-Null }
    Wait-ProcessesExit -Names $LdLegacyProcessNames -Script $Script | Out-Null
    return $state
}

# Restarts what Stop-LegacyInstall stopped (after a failed install), when it is still there.
function Restore-LegacyInstall([hashtable]$State) {
    if (-not $State) { return }
    if ($State.ServiceWasRunning -and (Get-Service -Name $LdLegacyServiceName -ErrorAction SilentlyContinue)) {
        Start-Service -Name $LdLegacyServiceName
    }
    $exe = Join-Path $LdLegacyInstallDir "libre-deck.exe"
    if ($State.DaemonWasRunning -and (Test-Path -LiteralPath $exe)) {
        Start-Process -FilePath "explorer.exe" -ArgumentList (Get-QuotedPath $exe)
    }
}

# Removes an installation under the former name: its daemon, autostart value, service, folder and
# registry key, in that order (no autostart value is left pointing at deleted files). The StreamDock
# flag of its registry key moves to the new key. %APPDATA% is left to the daemon, which moves the old
# data folder at its first start. Safe to call again after a partial run.
function Remove-LegacyInstall([string]$Script = "install.ps1") {
    if (-not (Test-LegacyInstall) -and -not (Test-Path -LiteralPath $LdLegacyStateKey)) { return }
    Write-Host "Removing the previous installation (libre-deck)..."
    Stop-LegacyInstall -Script $Script | Out-Null
    Remove-ItemProperty -LiteralPath $LdRunKey -Name $LdLegacyRunName -ErrorAction SilentlyContinue
    Remove-ItemProperty -LiteralPath $LdApprovedKey -Name $LdLegacyRunName -ErrorAction SilentlyContinue
    Remove-DeckService -Name $LdLegacyServiceName
    Remove-FolderWithRetry -Path $LdLegacyInstallDir
    $old = Get-ItemProperty -LiteralPath $LdLegacyStateKey -ErrorAction SilentlyContinue
    if ($old -and $old.StreamDockDisabledByInstaller -eq 1) {
        New-Item -Path $LdStateKey -Force | Out-Null
        Set-ItemProperty -LiteralPath $LdStateKey -Name "StreamDockDisabledByInstaller" -Value 1 -Type DWord
    }
    Remove-Item -LiteralPath $LdLegacyStateKey -Recurse -Force -ErrorAction SilentlyContinue
}

# True when the script can ask questions: an interactive session with a console whose input is not redirected.
function Test-CanPrompt {
    if (-not [Environment]::UserInteractive) { return $false }
    if ([Environment]::GetCommandLineArgs() | Where-Object { $_ -ieq "-NonInteractive" }) { return $false }
    try { return -not [Console]::IsInputRedirected } catch { return $false }
}

function Get-StartupEntry([string]$Name, [string]$RunKey = $LdRunKey) {
    $v = Get-ItemProperty -LiteralPath $RunKey -Name $Name -ErrorAction SilentlyContinue
    if ($v) { return $v.$Name } else { return $null }
}

# Task Manager's convention: first byte even (02) = enabled, odd (03) = disabled; no value = enabled.
function Test-StartupEntryEnabled([string]$Name, [string]$ApprovedKey = $LdApprovedKey) {
    $v = Get-ItemProperty -LiteralPath $ApprovedKey -Name $Name -ErrorAction SilentlyContinue
    if (-not $v) { return $true }
    $bytes = [byte[]]$v.$Name
    return ($bytes.Length -eq 0) -or (($bytes[0] % 2) -eq 0)
}

function Set-StartupApproved([string]$Name, [string]$ApprovedKey, [byte]$Flag, [long]$FileTime) {
    if (-not (Test-Path -LiteralPath $ApprovedKey)) { New-Item -Path $ApprovedKey -Force | Out-Null }
    $bytes = New-Object byte[] 12
    $bytes[0] = $Flag
    [BitConverter]::GetBytes($FileTime).CopyTo($bytes, 4)
    New-ItemProperty -LiteralPath $ApprovedKey -Name $Name -PropertyType Binary -Value $bytes -Force | Out-Null
}

function Disable-StartupEntry([string]$Name, [string]$ApprovedKey = $LdApprovedKey) {
    Set-StartupApproved $Name $ApprovedKey 3 ([DateTime]::UtcNow.ToFileTimeUtc())
}

function Enable-StartupEntry([string]$Name, [string]$ApprovedKey = $LdApprovedKey) {
    Set-StartupApproved $Name $ApprovedKey 2 0
}
