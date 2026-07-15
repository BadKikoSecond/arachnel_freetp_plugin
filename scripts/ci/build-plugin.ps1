#Requires -Version 5.1
param(
    [string]$BuildDir = "build-win",
    [string]$SdkRef = $(if ($env:ARACHNEL_SDK_REF) { $env:ARACHNEL_SDK_REF } else { "master" }),
    [string]$QtVersion = $(if ($env:QT_VERSION) { $env:QT_VERSION } else { "6.8.2" })
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$BuildPath = Join-Path $Root $BuildDir
$SdkDir = if ($env:ARACHNEL_SDK_DIR) { $env:ARACHNEL_SDK_DIR } else { Join-Path $Root ".ci\arachnel-sdk" }
$QtRoot = if ($env:QT_INSTALL_DIR) { $env:QT_INSTALL_DIR } else { Join-Path $Root ".ci\qt" }
$QtPrefix = Join-Path $QtRoot "$QtVersion\msvc2022_64"
$DistDir = Join-Path $Root "dist\windows"

if ($env:CI_COMMIT_TAG) {
    python (Join-Path $Root "scripts\ci\set_plugin_version.py") $env:CI_COMMIT_TAG
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Test-Path -LiteralPath (Join-Path $SdkDir "cmake\ArachnelPluginSdk.cmake"))) {
    Write-Host "==> Clone Arachnel SDK ($SdkRef)"
    if (Test-Path -LiteralPath $SdkDir) { Remove-Item -LiteralPath $SdkDir -Recurse -Force }
    git clone --depth 1 --branch $SdkRef https://github.com/BadKiko/Arachnel.git $SdkDir
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Test-Path -LiteralPath (Join-Path $QtPrefix "lib\cmake\Qt6\Qt6Config.cmake"))) {
    Write-Host "==> Install Qt $QtVersion (msvc2022_64)"
    pip install aqtinstall
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    aqt install-qt windows desktop $QtVersion win64_msvc2022_64 -O $QtRoot --modules qtbase
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$env:ARACHNEL_SKIP_FREETP_CATALOG_FETCH = if ($env:ARACHNEL_SKIP_FREETP_CATALOG_FETCH) { $env:ARACHNEL_SKIP_FREETP_CATALOG_FETCH } else { "1" }

Write-Host "==> Configure"
cmake -S $Root -B $BuildPath -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_PREFIX_PATH="$QtPrefix" `
    -DARACHNEL_SDK_DIR="$SdkDir"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "==> Build freetp_plugin"
cmake --build $BuildPath --config Release --target freetp_plugin -j $env:NUMBER_OF_PROCESSORS
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$ArachPath = Join-Path $BuildPath "dist\freetp.arach"
if (-not (Test-Path -LiteralPath $ArachPath)) {
    throw "Expected artifact not found: $ArachPath"
}

New-Item -ItemType Directory -Force -Path $DistDir | Out-Null
Copy-Item -LiteralPath $ArachPath -Destination (Join-Path $DistDir "freetp.arach") -Force
Write-Host "Done: $(Join-Path $DistDir 'freetp.arach')"
