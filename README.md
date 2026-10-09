# arachnel-plugin-freetp

Source plugin for [Arachnel](https://github.com/PetWork/Arachnel): [FreeTP](https://freetp.org/) catalog — portable archives, Inno Setup, `.ftp` multi-part installers, add-on overlays.

Catalog JSON: [freetp-hydra-link](https://gitlab.com/BadKiko/freetp-hydra-link).

**General plugin development guide (new plugins, ABI, paths):** [Arachnel docs/PLUGIN_SDK.md](https://github.com/PetWork/Arachnel/blob/main/docs/PLUGIN_SDK.md)

---

## Requirements

- CMake 3.20+, C++20, Ninja
- Qt 6.8+ (**Core**, **Network**)
- **Arachnel** checkout (Plugin SDK — not necessarily a built app)

`build-win/` and `games-arachnel.json` are **not** in git — the catalog is loaded at runtime from [freetp-hydra-link](https://gitlab.com/BadKiko/freetp-hydra-link) via `catalogUrl` in `plugin.json`. CMake may optionally download a snapshot into `games-arachnel.json` for offline `.arach` bundles.

---

## Clone layout

```
your-workspace/
  Arachnel/
  arachnel-plugin-freetp/    ← this repo
```

---

## Build & install (Windows)

```powershell
git clone https://github.com/PetWork/Arachnel.git
git clone https://github.com/PetWork/arachnel-plugin-freetp.git

cd arachnel-plugin-freetp

# Required: path to Arachnel sources
$env:ARACHNEL_SDK_DIR = "C:\path\to\Arachnel"

# If Qt is not auto-detected under D:\Qt or C:\Qt:
# $env:CMAKE_PREFIX_PATH = "D:\Qt\6.11.1\mingw_64"

.\run.ps1
```

`run.ps1` will:

1. Configure `build-win/` (optionally downloads `games-arachnel.json` for the bundle)
2. Build `freetp_plugin.dll`
3. Fill `build-win/plugin-bundle/` (`plugin.json`, DLL, catalog, `linux/`)
4. Create `build-win/dist/freetp.arach`
5. Copy `plugin-bundle/*` → `%LOCALAPPDATA%\PetWork\Arachnel\plugins\freetp\`

### Manual CMake

```powershell
cmake -S . -B build-win -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DCMAKE_PREFIX_PATH="D:\Qt\6.11.1\mingw_64" `
  -DARACHNEL_SDK_DIR="C:\path\to\Arachnel"
cmake --build build-win --target freetp_plugin
```

Copy `build-win/plugin-bundle/*` to:

`%LOCALAPPDATA%\PetWork\Arachnel\plugins\freetp\`

Or install `build-win/dist/freetp.arach` in Arachnel: **Settings → Plugins**.

---

## Build (Linux)

```bash
export ARACHNEL_SDK_DIR=~/src/Arachnel
export CMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target freetp_plugin
```

Install bundle into:

`~/.local/share/PetWork/Arachnel/plugins/freetp/`

---

## Test with Arachnel

```powershell
cd ..\Arachnel
.\run.ps1
```

On Windows, `Arachnel/run.ps1` auto-deploys from `../arachnel-plugin-freetp/build-win/plugin-bundle` or `D:\PetWork\arachnel-plugin-freetp\build-win\plugin-bundle` if present.

Verify in `%LOCALAPPDATA%\PetWork\Arachnel\run.log`:

```
Plugin loaded: freetp v1.0.0 from ...
```

---

## Repository contents

| Path | In git | Purpose |
|------|--------|---------|
| `src/` | yes | Plugin implementation |
| `plugin.json` | yes | Manifest (`catalogUrl` → freetp-hydra-link) |
| `games-arachnel.json` | **no** | Optional local snapshot (CMake fetch or dev copy) |
| `linux/` | yes | Optional Linux fix assets |
| `build-win/` | **no** | Local build output |

---

## Plugin API

Must match Arachnel **plugin API v2** (`plugin.json` → `"apiVersion": 2`).  
Rebuild after Arachnel changes `ARACHNEL_PLUGIN_API_VERSION` or `CatalogEntry` layout.

---

## Releases (CI/CD)

Pushing a **semver tag** triggers [GitHub Actions](.github/workflows/release.yml) (or run **Actions → Release → Run workflow** and enter the version):

```bash
git tag v1.0.0
git push origin v1.0.0
```

The workflow builds platform-specific bundles, merges them into **one universal `freetp.arach`**, checks it on Ubuntu / Fedora / Arch, publishes a GitHub Release and notifies the sourcelist.

The sourcelist ([`arachnel_plugins_sourcelist`](https://github.com/BadKikoSecond/arachnel_plugins_sourcelist)) polls this repo's releases every 30 minutes, so no secret is required. Add the repository secret **`SOURCELIST_DISPATCH_TOKEN`** (fine-grained token with *Contents: write* on the sourcelist) to make the update instant.

Contents of the universal bundle:

| File | Platform |
|------|----------|
| `freetp_plugin.dll` | Windows (MSVC) |
| `libfreetp_plugin.so` | Linux |
| `plugin.json`, `games-arachnel.json`, `linux/` | shared |

Arachnel loads only the native library for the current OS.

| Tag format | Example |
|------------|---------|
| Semver | `v1.0.0`, `v1.0.1-beta` |

Windows builds on `windows-2022` (MinGW, installed via aqtinstall), Linux on `ubuntu-24.04`, both GitHub-hosted runners.

Manual/local CI scripts:

```powershell
# Windows (MSVC, same as Arachnel release)
pwsh scripts/ci/build-plugin.ps1

# Linux
bash scripts/ci/build-plugin.sh

# Merge both into dist/universal/freetp.arach
bash scripts/ci/merge-universal-arach.sh
```

Environment variable `ARACHNEL_SDK_REF` (default `master`) selects the Arachnel SDK git ref.

**Release builds:** CI reads `scripts/ci/launcher-toolchain.env` — a lockfile mirrored from [Arachnel `.github/workflows/release.yml`](https://github.com/BadKiko/Arachnel/blob/master/.github/workflows/release.yml) (Qt **6.8.2**, `win64_msvc2022_64` / `linux_gcc_64`, modules `qtshadertools qtmultimedia`, SDK tag `v0.1.x`). Bump this file when cutting a new Arachnel GitHub release so plugin DLLs match the published launcher.

Install the released `freetp.arach` into an Arachnel build made with the same toolchain (MinGW, Qt 6.11.1). Do not mix it with other local dev builds.

---

## Environment variables

| Variable | Description |
|----------|-------------|
| `ARACHNEL_SDK_DIR` | Path to Arachnel repository |
| `CMAKE_PREFIX_PATH` | Qt 6 installation (kit directory) |
| `ARACHNEL_SKIP_FREETP_CATALOG_FETCH` | `1` = do not download catalog at CMake configure (runtime `catalogUrl` still works) |
