#!/usr/bin/env bash
# Build the cn_ta DuckDB extension on Linux.
#
# Dependencies (system packages):
#   sudo apt-get install -y ta-lib-dev libeigen3-dev ninja-build cmake git
#
# Usage:
#   scripts/build_linux.sh [release|debug] [BUILD_DIR]
#
# Output:
#   build/release/extension/cn_ta/cn_ta.duckdb_extension   (loadable extension)
#   build/release/duckdb_release                            (duckdb with ext linked)
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_TYPE="${1:-release}"
BUILD_DIR="${2:-$PROJ_DIR/build/$BUILD_TYPE}"

# Pinned DuckDB / extension-ci-tools version.
# Keep these in sync with each other (DuckDB vX.Y.Z <-> ci-tools branch vX.Y.Z)
# and with the Python `duckdb` package used at runtime (see benchmark_py/pyproject.toml).
DUCKDB_VERSION="${DUCKDB_VERSION:-v1.5.5}"
CI_TOOLS_VERSION="${CI_TOOLS_VERSION:-v1.5.5}"

# Ensure submodules (duckdb + extension-ci-tools) are present.
if [ ! -f "$PROJ_DIR/duckdb/CMakeLists.txt" ] || [ ! -d "$PROJ_DIR/extension-ci-tools/makefiles" ]; then
  echo ">> Initializing submodules (duckdb, extension-ci-tools)..."
  git -C "$PROJ_DIR" submodule update --init --recursive
fi

# Pin both submodules to the fixed version above, regardless of what commit
# the superproject gitlink currently records. This guarantees the extension is
# always compiled against the exact DuckDB version we intend, avoiding the
# "built for X / runtime is Y" load error.
echo ">> Pinning submodules (duckdb=$DUCKDB_VERSION, extension-ci-tools=$CI_TOOLS_VERSION)..."
git -C "$PROJ_DIR/duckdb" fetch --tags origin
git -C "$PROJ_DIR/duckdb" checkout "$DUCKDB_VERSION"
git -C "$PROJ_DIR/extension-ci-tools" fetch --tags origin
git -C "$PROJ_DIR/extension-ci-tools" checkout "$CI_TOOLS_VERSION"

# Invalidate stale CMake cache when the DuckDB source or build configuration
# has changed. CMake otherwise reuses build/<type>/CMakeCache.txt across
# different DuckDB commits / generators, silently linking the wrong DuckDB
# version into the extension (the "built for X / runtime is Y" load error).
DUCKDB_COMMIT="$(git -C "$PROJ_DIR/duckdb" rev-parse HEAD 2>/dev/null || echo unknown)"
CACHE_STAMP="$BUILD_DIR/.duckdb_commit"
NEED_CLEAN=0

if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
  # (1) Generator mismatch (e.g. previous build used "Unix Makefiles").
  CACHED_GEN="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt")"
  if [ -n "$CACHED_GEN" ] && [ "$CACHED_GEN" != "Ninja" ]; then
    echo ">> Generator mismatch ($CACHED_GEN != Ninja), clearing build cache..."
    NEED_CLEAN=1
  fi

  # (2) DuckDB source commit changed since last build.
  PREV_COMMIT="$(cat "$CACHE_STAMP" 2>/dev/null || true)"
  if [ -z "$PREV_COMMIT" ]; then
    echo ">> No build stamp found (stale/foreign build dir), clearing build cache..."
    NEED_CLEAN=1
  elif [ "$PREV_COMMIT" != "$DUCKDB_COMMIT" ]; then
    echo ">> DuckDB source changed ($PREV_COMMIT -> $DUCKDB_COMMIT), clearing build cache..."
    NEED_CLEAN=1
  fi

  if [ "$NEED_CLEAN" -eq 1 ]; then
    rm -rf "$BUILD_DIR"
  fi
fi

# On Linux we rely on system TA-Lib / Eigen3 (see CMakeLists.txt).
echo ">> Configuring ($BUILD_TYPE)..."
cmake -S "$PROJ_DIR/duckdb" \
  -B "$BUILD_DIR" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DBUILD_UNITTESTS=0 \
  -DBUILD_SHELL=1 \
  -DDUCKDB_EXTENSION_CONFIGS="$PROJ_DIR/extension_config.cmake" \
  -DCMAKE_PREFIX_PATH="$PROJ_DIR/vcpkg_installed;$PROJ_DIR/vcpkg_installed/x64-linux"

echo ">> Building cn_ta (loadable extension)..."
cmake --build "$BUILD_DIR" --target cn_ta_loadable_extension

# Record the DuckDB commit this build used, so the next run can detect changes.
mkdir -p "$BUILD_DIR"
echo "$DUCKDB_COMMIT" > "$CACHE_STAMP"

echo ">> Done. Artifacts in: $BUILD_DIR/extension/cn_ta/"
echo "   loadable: $BUILD_DIR/extension/cn_ta/cn_ta.duckdb_extension"
