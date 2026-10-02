#!/usr/bin/env bash
# Compare arachnel_plugin_catalog_entry_size() to sizeof(CatalogEntry) from the SDK.
set -euo pipefail

SO="${1:?usage: check-catalog-entry-size.sh <plugin.so> <sdk-dir> [qt-prefix]}"
SDK_DIR="${2:?}"
QT_PREFIX="${3:-}"

if [[ ! -f "${SO}" ]]; then
  echo "missing plugin: ${SO}" >&2
  exit 1
fi
if [[ ! -f "${SDK_DIR}/src/core/catalog/catalog_types.h" ]]; then
  echo "missing SDK catalog_types.h under ${SDK_DIR}" >&2
  exit 1
fi

PROBE_DIR="$(mktemp -d)"
trap 'rm -rf "${PROBE_DIR}"' EXIT

cat > "${PROBE_DIR}/sizeof_probe.cpp" <<'CPP'
#include "catalog_types.h"
#include <cstdio>
int main() {
  std::printf("%d\n", static_cast<int>(sizeof(arachnel::core::CatalogEntry)));
  return 0;
}
CPP

INC=(
  -I"${SDK_DIR}/src/core/catalog"
  -I"${SDK_DIR}/src/core/install"
  -I"${SDK_DIR}/src/core"
)
LIBS=()
RPATH_ARGS=()

if [[ -n "${QT_PREFIX}" && -f "${QT_PREFIX}/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
  INC+=(-I"${QT_PREFIX}/include" -I"${QT_PREFIX}/include/QtCore")
  LIBS+=(-L"${QT_PREFIX}/lib" -lQt6Core)
  RPATH_ARGS+=(-Wl,-rpath,"${QT_PREFIX}/lib")
  export LD_LIBRARY_PATH="${QT_PREFIX}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
elif command -v pkg-config >/dev/null 2>&1 && pkg-config --exists Qt6Core; then
  # shellcheck disable=SC2206
  INC+=($(pkg-config --cflags Qt6Core))
  # shellcheck disable=SC2206
  LIBS+=($(pkg-config --libs Qt6Core))
else
  echo "Qt6Core not found (pass qt-prefix or install pkg-config Qt6)" >&2
  exit 1
fi

# Avoid qt_version_tag link requirement on some distro Qt builds.
g++ -std=c++20 -fPIC -DQT_NO_VERSION_TAGGING \
  "${INC[@]}" "${PROBE_DIR}/sizeof_probe.cpp" -o "${PROBE_DIR}/sizeof_probe" \
  "${LIBS[@]}" "${RPATH_ARGS[@]}"
EXPECTED="$("${PROBE_DIR}/sizeof_probe")"

PLUGIN_DIR="$(cd "$(dirname "${SO}")" && pwd)"
export LD_LIBRARY_PATH="${PLUGIN_DIR}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

ACTUAL="$(python3 - "${SO}" <<'PY'
import ctypes, sys
so = sys.argv[1]
lib = ctypes.CDLL(so)
lib.arachnel_plugin_catalog_entry_size.restype = ctypes.c_int
print(lib.arachnel_plugin_catalog_entry_size())
PY
)"

echo "CatalogEntry sizeof: sdk_headers=${EXPECTED} plugin_export=${ACTUAL}"
if [[ "${EXPECTED}" != "${ACTUAL}" ]]; then
  echo "CatalogEntry size mismatch between SDK headers and plugin export" >&2
  exit 1
fi
echo "CatalogEntry ABI check OK (${ACTUAL} bytes)"
