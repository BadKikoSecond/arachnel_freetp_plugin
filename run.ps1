#Requires -Version 5.1
param(
    [string]$BuildDir = "build-win",
    [string]$ArachnelSdkDir = $(if ($env:ARACHNEL_SDK_DIR) { $env:ARACHNEL_SDK_DIR } else { "" })
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot

function Find-QtPrefix {
    if ($env:CMAKE_PREFIX_PATH) { return $env:CMAKE_PREFIX_PATH }
    foreach ($root in @("D:\Qt", "C:\Qt")) {
        if (-not (Test-Path $root)) { continue }
        $kits = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d' } |
            Sort-Object Name -Descending
        foreach ($ver in $kits) {
            $candidates = Get-ChildItem $ver.FullName -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match 'mingw' }
            foreach ($kit in $candidates) {
                if (Test-Path (Join-Path $kit.FullName "lib\cmake\Qt6\Qt6Config.cmake")) {
                    return $kit.FullName
                }
            }
        }
    }
    return $null
}

if (-not $ArachnelSdkDir) {
    foreach ($candidate in @(
            (Join-Path $Root "..\Arachnel"),
            (Join-Path $Root "..\..\Work\Arachnel"),
            "D:\Work\Arachnel"
        )) {
        if (Test-Path (Join-Path $candidate "cmake\ArachnelPluginSdk.cmake")) {
            $ArachnelSdkDir = (Resolve-Path $candidate).Path
            break
        }
    }
}

if (-not $ArachnelSdkDir -or -not (Test-Path (Join-Path $ArachnelSdkDir "cmake\ArachnelPluginSdk.cmake"))) {
    Write-Error "Set ARACHNEL_SDK_DIR to an Arachnel checkout (contains cmake/ArachnelPluginSdk.cmake)."
}

$qtPrefix = Find-QtPrefix
if (-not $qtPrefix) {
    Write-Error "Qt 6 kit not found. Set CMAKE_PREFIX_PATH, e.g. D:\Qt\6.11.1\mingw_64"
}

$env:Path = "D:\Qt\Tools\mingw1310_64\bin;D:\Qt\Tools\Ninja;$env:Path"
$buildPath = Join-Path $Root $BuildDir
if (-not (Test-Path $buildPath)) {
    cmake -S $Root -B $buildPath -G Ninja `
        -DCMAKE_BUILD_TYPE=RelWithDebInfo `
        -DCMAKE_PREFIX_PATH="$qtPrefix" `
        -DARACHNEL_SDK_DIR="$ArachnelSdkDir"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

cmake --build $buildPath --target freetp_plugin
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$bundle = Join-Path $buildPath "plugin-bundle"
# Match Arachnel AppDataLocation (no organizationName → %APPDATA%\Arachnel).
$dest = Join-Path $env:APPDATA "Arachnel\plugins\freetp"
if (Test-Path (Join-Path $bundle "freetp_plugin.dll")) {
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Copy-Item -Path (Join-Path $bundle "*") -Destination $dest -Recurse -Force
    Write-Host "Deployed to $dest"
}

Write-Host "Done. .arach: $(Join-Path $buildPath 'dist\freetp.arach')"
