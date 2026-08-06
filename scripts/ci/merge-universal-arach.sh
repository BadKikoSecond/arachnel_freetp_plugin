#!/usr/bin/env bash
# Merge platform-specific freetp.arach builds into one universal bundle.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WIN_ARCH="${1:-${ROOT}/dist/windows/freetp.arach}"
LIN_ARCH="${2:-${ROOT}/dist/linux/freetp.arach}"
OUT_ARCH="${3:-${ROOT}/dist/universal/freetp.arach}"

STAGING="${ROOT}/dist/universal/.staging"
WIN_DIR="${STAGING}/win"
LIN_DIR="${STAGING}/linux"
MERGED="${STAGING}/merged/freetp"

if [[ ! -f "${WIN_ARCH}" ]]; then
  echo "Windows bundle not found: ${WIN_ARCH}" >&2
  exit 1
fi
if [[ ! -f "${LIN_ARCH}" ]]; then
  echo "Linux bundle not found: ${LIN_ARCH}" >&2
  exit 1
fi

rm -rf "${STAGING}"
mkdir -p "${WIN_DIR}" "${LIN_DIR}" "${MERGED}"

unzip -q "${WIN_ARCH}" -d "${WIN_DIR}"
unzip -q "${LIN_ARCH}" -d "${LIN_DIR}"

find_bundle_root() {
  local base="$1"
  if [[ -f "${base}/plugin.json" ]]; then
    printf '%s\n' "${base}"
    return 0
  fi
  local child
  for child in "${base}"/*; do
    if [[ -d "${child}" && -f "${child}/plugin.json" ]]; then
      printf '%s\n' "${child}"
      return 0
    fi
  done
  return 1
}

WIN_BUNDLE="$(find_bundle_root "${WIN_DIR}")"
LIN_BUNDLE="$(find_bundle_root "${LIN_DIR}")"

echo "Windows bundle: ${WIN_BUNDLE}"
echo "Linux bundle:   ${LIN_BUNDLE}"

shopt -s nullglob
cp -a "${WIN_BUNDLE}/." "${MERGED}/"
rm -rf "${MERGED}/Release" "${MERGED}/Debug" "${MERGED}/RelWithDebInfo" "${MERGED}/MinSizeRel"

for file in "${LIN_BUNDLE}"/*; do
  base="$(basename "${file}")"
  case "${base}" in
    freetp_plugin.dll|plugin.json)
      continue
      ;;
  esac
  if [[ -d "${file}" ]]; then
    rm -rf "${MERGED}/${base}"
    cp -a "${file}" "${MERGED}/${base}"
  elif [[ ! -e "${MERGED}/${base}" ]]; then
    cp -a "${file}" "${MERGED}/${base}"
  fi
done

for so in "${LIN_BUNDLE}"/lib*.so*; do
  [[ -f "${so}" ]] || continue
  cp -a "${so}" "${MERGED}/"
done

required=(
  "${MERGED}/plugin.json"
  "${MERGED}/freetp_plugin.dll"
  "${MERGED}/libfreetp_plugin.so"
)
for path in "${required[@]}"; do
  if [[ ! -f "${path}" ]]; then
    echo "Universal bundle is incomplete, missing: ${path}" >&2
    exit 1
  fi
done

mkdir -p "$(dirname "${OUT_ARCH}")"
rm -f "${OUT_ARCH}"
(
  cd "${STAGING}/merged"
  zip -qr "${OUT_ARCH}" freetp
)

echo "Universal bundle: ${OUT_ARCH}"
ls -lh "${OUT_ARCH}"
