#!/usr/bin/env bash
# Build and run the cn_ta sqllogic tests on Linux.
#
# Unlike scripts/build_linux.sh (which builds only the loadable extension with
# BUILD_UNITTESTS=0), this script builds the DuckDB unittest binary with the
# extension statically linked, then runs test/sql/*.test.
#
# Usage:
#   scripts/test_linux.sh [BUILD_DIR] [TEST_FILTER]
#
# Examples:
#   scripts/test_linux.sh                              # run all tests
#   scripts/test_linux.sh build/test "test_scalar"     # run matching tests
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$PROJ_DIR/build/test}"
TEST_FILTER="${2:-"*atm_talib/test/sql/*"}"

# Pinned DuckDB / extension-ci-tools version (same as build_linux.sh).
DUCKDB_VERSION="${DUCKDB_VERSION:-v1.5.5}"
CI_TOOLS_VERSION="${CI_TOOLS_VERSION:-v1.5.5}"

# Ensure submodules (duckdb + extension-ci-tools) are present.
if [ ! -f "$PROJ_DIR/duckdb/CMakeLists.txt" ] || [ ! -d "$PROJ_DIR/extension-ci-tools/makefiles" ]; then
  echo ">> Initializing submodules (duckdb, extension-ci-tools)..."
  git -C "$PROJ_DIR" submodule update --init --recursive
fi

# Pin both submodules to the fixed version (same policy as build_linux.sh).
echo ">> Pinning submodules (duckdb=$DUCKDB_VERSION, extension-ci-tools=$CI_TOOLS_VERSION)..."
git -C "$PROJ_DIR/duckdb" fetch --tags origin
git -C "$PROJ_DIR/duckdb" checkout "$DUCKDB_VERSION"
git -C "$PROJ_DIR/extension-ci-tools" fetch --tags origin
git -C "$PROJ_DIR/extension-ci-tools" checkout "$CI_TOOLS_VERSION"

# Invalidate stale cache when DuckDB source changed.
DUCKDB_COMMIT="$(git -C "$PROJ_DIR/duckdb" rev-parse HEAD 2>/dev/null || echo unknown)"
CACHE_STAMP="$BUILD_DIR/.duckdb_commit"
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
  PREV_COMMIT="$(cat "$CACHE_STAMP" 2>/dev/null || true)"
  if [ "$PREV_COMMIT" != "$DUCKDB_COMMIT" ]; then
    echo ">> DuckDB source changed ($PREV_COMMIT -> $DUCKDB_COMMIT), clearing build cache..."
    rm -rf "$BUILD_DIR"
  fi
fi

# Configure a unittest build: main DuckDB library + extension statically linked,
# so the unittest binary can exercise the extension's registered sqllogic tests.
#
# In CI the build runs inside the DuckDB docker image, which has no system
# eigen3/ta-lib; when vcpkg is present (/vcpkg) enable it so the manifest
# dependencies (eigen3 + talib overlay port) are installed. Locally (no /vcpkg)
# the system-installed dependencies are used, as before.
VCPKG_ARGS=()
if [ -d "/vcpkg/scripts/buildsystems" ]; then
    VCPKG_ARGS=(
        -DVCPKG_BUILD=1
        -DCMAKE_TOOLCHAIN_FILE="/vcpkg/scripts/buildsystems/vcpkg.cmake"
        -DVCPKG_TARGET_TRIPLET="${VCPKG_TARGET_TRIPLET:-x64-linux-release}"
        -DVCPKG_MANIFEST_DIR="$PROJ_DIR"
    )
fi

echo ">> Configuring unittest build..."
cmake -S "$PROJ_DIR/duckdb" \
  -B "$BUILD_DIR" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_UNITTESTS=1 \
  -DBUILD_SHELL=1 \
  -DBUILD_MAIN_DUCKDB_LIBRARY=1 \
  -DEXTENSION_STATIC_BUILD=1 \
  -DDUCKDB_EXTENSION_CONFIGS="$PROJ_DIR/extension_config.cmake" \
  -DCMAKE_PREFIX_PATH="$PROJ_DIR/vcpkg_installed;$PROJ_DIR/vcpkg_installed/x64-linux" \
  "${VCPKG_ARGS[@]}"

echo ">> Building unittest..."
cmake --build "$BUILD_DIR" --target unittest

mkdir -p "$BUILD_DIR"
echo "$DUCKDB_COMMIT" > "$CACHE_STAMP"

echo ">> Running tests (filter: $TEST_FILTER)..."
"$BUILD_DIR/test/unittest" "$TEST_FILTER"
