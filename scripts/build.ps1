param([string]$Config = "Debug", [switch]$Test, [string]$TestArgs = "", [string]$BuildDir = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "MSVC not found: install the 'Desktop development with C++' workload" }
Import-Module (Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
$build = Join-Path $root ("build\" + $(if ($BuildDir) { $BuildDir } else { $Config }))
cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" | Out-Host
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build $build | Out-Host
if ($LASTEXITCODE) { exit $LASTEXITCODE }
if ($Test) {
    $exe = Join-Path $build "lite-deck-tests.exe"
    if ($TestArgs) { & $exe $TestArgs.Split(" ") } else { & $exe }
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    # The page's Node tests (tests/web/*.test.js) with the full suite. Edge scripts (*.edge.js) are
    # run by hand. LD_REQUIRE_NODE=1 (CI) makes a missing Node an error instead of a skip.
    if ($TestArgs) { Write-Host "Node tests skipped: a test filter was given"; exit 0 }
    if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
        if ($env:LD_REQUIRE_NODE -eq "1") { Write-Host "Node tests: node not found"; exit 1 }
        Write-Host "Node tests skipped: node not found in PATH"
        exit 0
    }
    $failed = 0
    foreach ($t in Get-ChildItem (Join-Path $root "tests\web") -Filter "*.test.js" | Sort-Object Name) {
        & node $t.FullName
        if ($LASTEXITCODE) { Write-Host "FAILED: tests/web/$($t.Name)"; $failed += 1 } else { Write-Host "ok: tests/web/$($t.Name)" }
    }
    if ($failed) { Write-Host "Node tests: $failed failed"; exit 1 }
    Write-Host "Node tests: all passed"
    exit 0
}
