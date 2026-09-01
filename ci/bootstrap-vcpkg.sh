#!/usr/bin/env bash

set -euo pipefail

: "${VCPKG_ROOT:?VCPKG_ROOT must name the CI vcpkg checkout}"

manifest_baseline="$(
  python3 - "${GITHUB_WORKSPACE:-.}/vcpkg.json" <<'PY'
import json
import pathlib
import sys

manifest = pathlib.Path(sys.argv[1])
print(json.loads(manifest.read_text(encoding="utf-8"))["builtin-baseline"])
PY
)"

if [[ -n "${VCPKG_BASELINE:-}" && "${VCPKG_BASELINE}" != "${manifest_baseline}" ]]; then
  printf 'VCPKG_BASELINE (%s) disagrees with vcpkg.json (%s)\n' \
    "${VCPKG_BASELINE}" "${manifest_baseline}" >&2
  exit 1
fi

git init --quiet "${VCPKG_ROOT}"
git -C "${VCPKG_ROOT}" remote add origin https://github.com/microsoft/vcpkg.git
git -C "${VCPKG_ROOT}" fetch --quiet --depth 1 origin "${manifest_baseline}"
git -C "${VCPKG_ROOT}" checkout --quiet --detach FETCH_HEAD

actual_baseline="$(git -C "${VCPKG_ROOT}" rev-parse HEAD)"
if [[ "${actual_baseline}" != "${manifest_baseline}" ]]; then
  printf 'vcpkg checkout is %s, expected %s\n' \
    "${actual_baseline}" "${manifest_baseline}" >&2
  exit 1
fi

"${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics
