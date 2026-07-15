#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${BUILD_DIR:-${ROOT}/build-linux}"
SDK_DIR="${ARACHNEL_SDK_DIR:-${ROOT}/.ci/arachnel-sdk}"
SDK_REF="${ARACHNEL_SDK_REF:-master}"
QT_VERSION="${QT_VERSION:-6.8.2}"
QT_PATH="${QT_INSTALL_DIR:-${ROOT}/.ci/qt/${QT_VERSION}/gcc_64}"
DIST="${ROOT}/dist/linux"

cd "${ROOT}"

if [[ -n "${CI_COMMIT_TAG:-}" ]]; then
  python3 "${ROOT}/scripts/ci/set_plugin_version.py" "${CI_COMMIT_TAG}"
fi

if [[ ! -f "${SDK_DIR}/cmake/ArachnelPluginSdk.cmake" ]]; then
  echo "==> Clone Arachnel SDK (${SDK_REF})"
  rm -rf "${SDK_DIR}"
  git clone --depth 1 --branch "${SDK_REF}" https://github.com/BadKiko/Arachnel.git "${SDK_DIR}"
fi

if [[ ! -f "${QT_PATH}/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
  echo "==> Install Qt ${QT_VERSION} (gcc_64)"
  AQT_VENV="${ROOT}/.ci/aqt-venv"
  if [[ ! -x "${AQT_VENV}/bin/aqt" ]]; then
    python3 -m venv "${AQT_VENV}"
    "${AQT_VENV}/bin/pip" install --upgrade pip aqtinstall
  fi
  export PATH="${AQT_VENV}/bin:${PATH}"
  aqt install-qt linux desktop "${QT_VERSION}" gcc_64 \
    -O "$(dirname "$(dirname "${QT_PATH}")")" \
    --archives qtbase qtdeclarative qttools
fi

export ARACHNEL_SKIP_FREETP_CATALOG_FETCH="${ARACHNEL_SKIP_FREETP_CATALOG_FETCH:-1}"

echo "==> Configure"
cmake -S "${ROOT}" -B "${BUILD}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="${QT_PATH}" \
  -DARACHNEL_SDK_DIR="${SDK_DIR}"

echo "==> Build freetp_plugin"
cmake --build "${BUILD}" --target freetp_plugin -j"$(nproc)"

mkdir -p "${DIST}"
cp -f "${BUILD}/dist/freetp.arach" "${DIST}/freetp.arach"
echo "Done: ${DIST}/freetp.arach"
