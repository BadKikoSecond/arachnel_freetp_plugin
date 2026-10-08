#Requires -Version 5.1
param(
    [string]$BuildDir = "build-win"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
. (Join-Path $PSScriptRoot "read-launcher-toolchain.ps1")

$BuildPath = Join-Path $Root $BuildDir
$QtRoot = if ($env:QT_INSTALL_DIR) { $env:QT_INSTALL_DIR } else { Join-Path $Root ".ci\qt" }
$QtVersion = $env:QT_VERSION
$QtArch = $env:QT_WINDOWS_ARCH
$QtKit = if ($env:QT_WINDOWS_KIT) { $env:QT_WINDOWS_KIT } else {
    if ($QtArch -match 'mingw') { 'mingw_64' } else { 'msvc2022_64' }
}
$QtModules = $env:QT_MODULES -split '\s+'
$SdkRef = $env:ARACHNEL_SDK_REF
$QtPrefix = Join-Path $QtRoot "$QtVersion\$QtKit"
$DistDir = Join-Path $Root "dist\windows"
$useMingw = $QtKit -match 'mingw'

Write-Host "Toolchain: Qt $QtVersion $QtArch ($QtKit), SDK $SdkRef, modules $($env:QT_MODULES)"

$ReleaseTag = if ($env:RELEASE_TAG) { $env:RELEASE_TAG } else { $env:CI_COMMIT_TAG }
if ($ReleaseTag) {
    python (Join-Path $Root "scripts\ci\set_plugin_version.py") $ReleaseTag
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# CI always reclones into .ci\arachnel-sdk (ignore stale ARACHNEL_SDK_DIR / D:\Work\Arachnel).
# Stale SDK checkouts shipped CatalogEntry 592 after core shrank to 544.
if ($env:GITHUB_ACTIONS -or $env:GITLAB_CI) {
    $SdkDir = Join-Path $Root ".ci\arachnel-sdk"
    Write-Host "==> Sync Arachnel SDK ($SdkRef) for CI"
    if (Test-Path -LiteralPath $SdkDir) { Remove-Item -LiteralPath $SdkDir -Recurse -Force }
    git clone --depth 1 --branch $SdkRef https://github.com/BadKiko/Arachnel.git $SdkDir
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} else {
    $SdkDir = if ($env:ARACHNEL_SDK_DIR) { $env:ARACHNEL_SDK_DIR } else { Join-Path $Root ".ci\arachnel-sdk" }
    if (-not (Test-Path -LiteralPath (Join-Path $SdkDir "cmake\ArachnelPluginSdk.cmake"))) {
        Write-Host "==> Clone Arachnel SDK ($SdkRef)"
        if (Test-Path -LiteralPath $SdkDir) { Remove-Item -LiteralPath $SdkDir -Recurse -Force }
        git clone --depth 1 --branch $SdkRef https://github.com/BadKiko/Arachnel.git $SdkDir
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
}
$sdkHead = (git -C $SdkDir rev-parse --short HEAD).Trim()
Write-Host "SDK HEAD=$sdkHead path=$SdkDir"

function Find-MingwBin {
    param([string]$Prefix, [string]$InstallRoot)
    $candidates = @(
        (Join-Path $InstallRoot "Tools\mingw1310_64\bin"),
        (Join-Path $InstallRoot "Tools\mingw1120_64\bin"),
        (Join-Path (Split-Path -Parent (Split-Path -Parent $Prefix)) "Tools\mingw1310_64\bin")
    )
    foreach ($bin in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $bin "g++.exe")) { return $bin }
    }
    $fromPath = Get-Command g++.exe -ErrorAction SilentlyContinue
    if ($fromPath) { return (Split-Path -Parent $fromPath.Source) }
    return $null
}

if (-not (Test-Path -LiteralPath (Join-Path $QtPrefix "lib\cmake\Qt6\Qt6Config.cmake"))) {
    Write-Host "==> Install Qt $QtVersion ($QtArch) modules: $($env:QT_MODULES)"
    $AqtVenv = Join-Path $Root ".ci\aqt-venv"
    $AqtPython = Join-Path $AqtVenv "Scripts\python.exe"
    if (-not (Test-Path -LiteralPath $AqtPython)) {
        python -m venv $AqtVenv
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    # PyPI 3.3.0 cannot resolve Qt 6.11+ Windows layout — install from git master.
    & $AqtPython -m pip install --upgrade pip setuptools wheel
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $AqtPython -m pip install 'setuptools_scm[toml]>=9.2.0'
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $AqtPython -m pip install --no-build-isolation "git+https://github.com/miurahr/aqtinstall.git"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $moduleArgs = @()
    foreach ($module in $QtModules) {
        if ($module) { $moduleArgs += @("-m", $module) }
    }
    & $AqtPython -m aqt install-qt windows desktop $QtVersion $QtArch @moduleArgs -O $QtRoot
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    if ($useMingw) {
        & $AqtPython -m aqt install-tool windows desktop tools_mingw1310 -O $QtRoot
    }
}

if (-not (Test-Path -LiteralPath (Join-Path $QtPrefix "lib\cmake\Qt6\Qt6Config.cmake"))) {
    throw "Qt kit not found at $QtPrefix"
}

$env:ARACHNEL_SKIP_FREETP_CATALOG_FETCH = if ($env:ARACHNEL_SKIP_FREETP_CATALOG_FETCH) { $env:ARACHNEL_SKIP_FREETP_CATALOG_FETCH } else { "1" }

$configureArgs = @(
    "-S", $Root,
    "-B", $BuildPath,
    "-DCMAKE_PREFIX_PATH=$QtPrefix",
    "-DARACHNEL_SDK_DIR=$SdkDir",
    "-DCMAKE_BUILD_TYPE=Release"
)

if ($useMingw) {
    $mingwBin = Find-MingwBin -Prefix $QtPrefix -InstallRoot $QtRoot
    if (-not $mingwBin) { throw "MinGW bin not found (expected Tools\mingw1310_64\bin next to Qt)" }
    $env:Path = "$mingwBin;D:\Qt\Tools\Ninja;$env:Path"
    $gcc = (Join-Path $mingwBin "gcc.exe") -replace '\\', '/'
    $gxx = (Join-Path $mingwBin "g++.exe") -replace '\\', '/'
    Write-Host "==> Configure (MinGW/Ninja, Release) gcc=$gcc"
    $configureArgs += @(
        "-G", "Ninja",
        "-DCMAKE_C_COMPILER=$gcc",
        "-DCMAKE_CXX_COMPILER=$gxx"
    )
} else {
    Write-Host "==> Configure (MSVC 2022 x64, Release)"
    $configureArgs += @("-G", "Visual Studio 17 2022", "-A", "x64")
}

& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "==> Build freetp_plugin ($($env:BUILD_TYPE))"
if ($useMingw) {
    & cmake --build $BuildPath --target freetp_plugin -j $env:NUMBER_OF_PROCESSORS
} else {
    & cmake --build $BuildPath --config Release --target freetp_plugin -j $env:NUMBER_OF_PROCESSORS
}
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$ArachPath = Join-Path $BuildPath "dist\freetp.arach"
if (-not (Test-Path -LiteralPath $ArachPath)) {
    throw "Expected artifact not found: $ArachPath"
}

New-Item -ItemType Directory -Force -Path $DistDir | Out-Null
Copy-Item -LiteralPath $ArachPath -Destination (Join-Path $DistDir "freetp.arach") -Force
Write-Host "Done: $(Join-Path $DistDir 'freetp.arach')"
