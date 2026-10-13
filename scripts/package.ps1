# Builds Release in its own directory (build\dist-build, never build\Release which a running daemon may lock),
# with tests unless -SkipTests, and assembles the distribution folder dist\lite-deck.
param([switch]$SkipTests)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "install-lib.ps1")
$root = Split-Path $PSScriptRoot -Parent

$buildArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "build.ps1"), "-Config", "Release", "-BuildDir", "dist-build")
if (-not $SkipTests) { $buildArgs += "-Test" }
& powershell @buildArgs
if ($LASTEXITCODE) { throw "build.ps1 failed (code $LASTEXITCODE)" }

$dist = Join-Path $root "dist\lite-deck"
if (Test-Path -LiteralPath $dist) { Remove-Item -LiteralPath $dist -Recurse -Force }
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$release = Join-Path $root "build\dist-build"
Copy-Item -LiteralPath (Join-Path $release "lite-deck.exe") -Destination $dist
Copy-Item -LiteralPath (Join-Path $release "lite-deck-sensors.exe") -Destination $dist
Copy-Item -LiteralPath (Join-Path $root "third_party\pawnio-modules\AMDFamily17.bin") -Destination $dist
Copy-Item -LiteralPath (Join-Path $root "third_party\pawnio-modules\COPYING") -Destination (Join-Path $dist "AMDFamily17.COPYING.txt")
foreach ($s in "install.ps1", "uninstall.ps1", "install-lib.ps1") { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $s) -Destination $dist }
$notices = @("LICENSE", "THIRD_PARTY_NOTICES.md")
foreach ($n in $notices) { Copy-Item -LiteralPath (Join-Path $root $n) -Destination $dist }

$cache = Join-Path $root "build\downloads\PawnIO_setup.exe"
if (-not (Test-Path -LiteralPath $cache)) {
    New-Item -ItemType Directory -Force -Path (Split-Path $cache) | Out-Null
    Invoke-WebRequest -UseBasicParsing -Uri $PawnIOSetupUrl -OutFile $cache
}
Assert-SignedBy -Path $cache -Cn $PawnIOSignerCn
Copy-Item -LiteralPath $cache -Destination $dist

foreach ($f in $LdFiles + @("PawnIO_setup.exe")) {  # $LdFiles includes the notices
    if (-not (Test-Path -LiteralPath (Join-Path $dist $f))) { throw "Incomplete dist folder: $f" }
}
Get-ChildItem -LiteralPath $dist | Select-Object Name, Length | Format-Table -AutoSize | Out-Host
Write-Host "Distribution folder: $dist" -ForegroundColor Green
