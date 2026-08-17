#!/usr/bin/env bash
# Test: scripts/build_pages_site.sh builds a gzipped DuckDB custom-repo tree.
set -euo pipefail
cd "$(dirname "$0")/../.."   # repo root

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Fake CI artifacts: two arches, matching the reusable workflow's "<ext>-*extension-<arch>" naming.
mkdir -p "$tmp/artifacts/cn_ta-linux-extension-linux_amd64"
mkdir -p "$tmp/artifacts/cn_ta-osx-extension-osx_arm64"
printf 'DUMMY-BINARY' > "$tmp/artifacts/cn_ta-linux-extension-linux_amd64/cn_ta.duckdb_extension"
printf 'DUMMY-BINARY' > "$tmp/artifacts/cn_ta-osx-extension-osx_arm64/cn_ta.duckdb_extension"

bash scripts/build_pages_site.sh "$tmp/artifacts" v1.5.4 "$tmp/site" cn_ta

for p in v1.5.4/linux_amd64 v1.5.4/osx_arm64; do
  f="$tmp/site/$p/cn_ta.duckdb_extension.gz"
  [ -f "$f" ] || { echo "FAIL: missing $f"; exit 1; }
  gzip -t "$f" || { echo "FAIL: not valid gzip: $f"; exit 1; }
done

# Empty artifacts must fail loudly (guards against silently publishing nothing).
mkdir -p "$tmp/empty"
if bash scripts/build_pages_site.sh "$tmp/empty" v1.5.4 "$tmp/site2" cn_ta 2>/dev/null; then
  echo "FAIL: expected non-zero exit on empty artifacts"; exit 1
fi

echo "PASS"
