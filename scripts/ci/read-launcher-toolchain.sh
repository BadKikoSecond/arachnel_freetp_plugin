#!/usr/bin/env bash
set -euo pipefail

ENV_FILE="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/launcher-toolchain.env}"
if [[ ! -f "${ENV_FILE}" ]]; then
  echo "Missing toolchain lock file: ${ENV_FILE}" >&2
  exit 1
fi

while IFS= read -r line || [[ -n "${line}" ]]; do
  line="${line%%#*}"
  line="$(echo "${line}" | xargs)"
  [[ -z "${line}" ]] && continue
  if [[ "${line}" == *=* ]]; then
    export "${line?}"
  fi
done < "${ENV_FILE}"
