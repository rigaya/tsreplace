#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VERSION_HEADER="$SCRIPT_DIR/../app/rgy_version.h"
mapfile -t VERSIONS < <(sed -nE 's/^[[:space:]]*#define[[:space:]]+VER_STR_FILEVERSION[[:space:]]+"([^"]+)".*/\1/p' "$VERSION_HEADER" | tr -d '\r')

if (( ${#VERSIONS[@]} != 1 )); then
  echo "ERROR: VER_STR_FILEVERSION must be defined exactly once in $VERSION_HEADER." >&2
  exit 1
fi
if [[ ! "${VERSIONS[0]}" =~ ^[0-9]+\.[0-9]+$ ]]; then
  echo "ERROR: invalid VER_STR_FILEVERSION: '${VERSIONS[0]}'" >&2
  exit 1
fi

printf '%s\n' "${VERSIONS[0]}"
