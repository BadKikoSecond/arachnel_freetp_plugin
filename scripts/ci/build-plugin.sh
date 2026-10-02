#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck disable=SC1091
source "${ROOT}/scripts/ci/read-launcher-toolchain.sh"

BUILD="${BUILD_DIR:-${ROOT}/build-linux}"
SDK_REF="${ARACHNEL_SDK_REF}"
QT_VERSION="${QT_VERSION}"
QT_ARCH="${QT_LINUX_ARCH}"
QT_PATH="${QT_INSTALL_DIR:-${ROOT}/.ci/qt/${QT_VERSION}/gcc_64}"
DIST="${ROOT}/dist/linux"

cd "${ROOT}"

echo "Toolchain: Qt ${QT_VERSION} ${QT_ARCH}, SDK ${SDK_REF}, modules ${QT_MODULES}"

if [[ -n "${CI_COMMIT_TAG:-}" ]]; then
  python3 "${ROOT}/scripts/ci/set_plugin_version.py" "${CI_COMMIT_TAG}"
fi

# CI always reclones into .ci/arachnel-sdk (ignore stale ARACHNEL_SDK_DIR / local trees).
# Stale SDK checkouts shipped CatalogEntry 592 after core shrank to 544.
if [[ -n "${GITLAB_CI:-}" ]]; then
  SDK_DIR="${ROOT}/.ci/arachnel-sdk"
  echo "==> Sync Arachnel SDK (${SDK_REF}) for CI"
  rm -rf "${SDK_DIR}"
  git clone --depth 1 --branch "${SDK_REF}" https://github.com/BadKiko/Arachnel.git "${SDK_DIR}"
else
  SDK_DIR="${ARACHNEL_SDK_DIR:-${ROOT}/.ci/arachnel-sdk}"
  if [[ ! -f "${SDK_DIR}/cmake/ArachnelPluginSdk.cmake" ]]; then
    echo "==> Clone Arachnel SDK (${SDK_REF})"
    rm -rf "${SDK_DIR}"
    git clone --depth 1 --branch "${SDK_REF}" https://github.com/BadKiko/Arachnel.git "${SDK_DIR}"
  fi
fi
echo "SDK HEAD=$(git -C "${SDK_DIR}" rev-parse --short HEAD) path=${SDK_DIR}"

if [[ ! -f "${QT_PATH}/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
  echo "==> Install Qt ${QT_VERSION} (${QT_ARCH}) modules: ${QT_MODULES}"
  AQT_VENV="${ROOT}/.ci/aqt-venv"
  if [[ ! -x "${AQT_VENV}/bin/aqt" ]]; then
    python3 -m venv "${AQT_VENV}"
    # Qt 6.11+ needs aqtinstall newer than PyPI 3.3.0. Preinstall build deps so
    # git install works when pip build-isolation can't resolve setuptools_scm.
    "${AQT_VENV}/bin/pip" install --upgrade pip setuptools wheel
    "${AQT_VENV}/bin/pip" install 'setuptools_scm[toml]>=9.2.0'
    "${AQT_VENV}/bin/pip" install --no-build-isolation "git+https://github.com/miurahr/aqtinstall.git"
  fi
  export PATH="${AQT_VENV}/bin:${PATH}"
  # shellcheck disable=SC2206
  MODULE_ARGS=(${QT_MODULES})
  aqt install-qt linux desktop "${QT_VERSION}" "${QT_ARCH}" \
    $(printf -- '-m %s ' "${MODULE_ARGS[@]}") \
    -O "$(dirname "$(dirname "${QT_PATH}")")"
fi

export ARACHNEL_SKIP_FREETP_CATALOG_FETCH="${ARACHNEL_SKIP_FREETP_CATALOG_FETCH:-1}"

echo "==> Configure (Ninja, ${BUILD_TYPE})"
cmake -S "${ROOT}" -B "${BUILD}" -G Ninja \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DCMAKE_PREFIX_PATH="${QT_PATH}" \
  -DARACHNEL_SDK_DIR="${SDK_DIR}"

echo "==> Build freetp_plugin"
cmake --build "${BUILD}" --target freetp_plugin -j"$(nproc)"
bash "${ROOT}/scripts/ci/check-linux-runtime.sh" "${BUILD}/plugin-bundle/libfreetp_plugin.so"
bash "${ROOT}/scripts/ci/check-catalog-entry-size.sh" \
  "${BUILD}/plugin-bundle/libfreetp_plugin.so" "${SDK_DIR}" "${QT_PATH}"

mkdir -p "${DIST}"
cp -f "${BUILD}/dist/freetp.arach" "${DIST}/freetp.arach"
echo "Done: ${DIST}/freetp.arach"
