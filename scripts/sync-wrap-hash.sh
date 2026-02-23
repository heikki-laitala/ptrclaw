#!/usr/bin/env bash
set -euo pipefail

WRAP_FILE="${1:-subprojects/sqlite3.wrap}"

if [[ ! -f "$WRAP_FILE" ]]; then
  echo "[wrap-sync] wrap file not found: $WRAP_FILE" >&2
  exit 1
fi

source_url="$(awk -F ' = ' '/^source_url = /{print $2}' "$WRAP_FILE")"
source_hash="$(awk -F ' = ' '/^source_hash = /{print $2}' "$WRAP_FILE")"

if [[ -z "$source_url" || -z "$source_hash" ]]; then
  echo "[wrap-sync] source_url/source_hash missing in $WRAP_FILE" >&2
  exit 1
fi

tmp_file="$(mktemp)"
trap 'rm -f "$tmp_file"' EXIT

curl -fsSL "$source_url" -o "$tmp_file"
actual_hash="$(sha256sum "$tmp_file" | awk '{print $1}')"

if [[ "$actual_hash" == "$source_hash" ]]; then
  echo "[wrap-sync] $WRAP_FILE hash already up to date"
  exit 0
fi

sed -i "s/^source_hash = .*/source_hash = $actual_hash/" "$WRAP_FILE"
echo "[wrap-sync] updated $WRAP_FILE"
echo "[wrap-sync] old: $source_hash"
echo "[wrap-sync] new: $actual_hash"