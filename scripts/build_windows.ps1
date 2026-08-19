# Build the cn_ta DuckDB extension on Windows.
#
# Prerequisites (installed via winget/choco is fine; paths resolved by vcpkg/CMake):
#   - Visual Studio 2022 (with "Desktop development with C++") or Build Tools
#   - Git, CMake (>= 3.24), Ninja
#   - vcpkg is pulled automatically in manifest mode via the vcpkg.json in this repo
#
# Usage (run from a "Developer PowerShell for VS 2022" or plain PowerShell):
#   scripts\build_windows.ps1 [-BuildType release|debug] [-BuildDir <path>]
#
# Output:
#   build\<type>\extension\cn_ta\cn_ta.duckdb_extension   (loadable extension)
#   build\<type>\duckdb.exe                               (duckdb with ext linked)

param(
  [string]$BuildType = "release",
  [string]$BuildDir  = ""
)

$ErrorActionPreference = "Stop"
$ProjDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $BuildDir) { $BuildDir = Join-Path $ProjDir "build\$BuildType" }

# Pinned DuckDB / extension-ci-tools version.
# Keep these in sync with each other (DuckDB vX.Y.Z <-> ci-tools branch vX.Y.Z)
# and with the Python `duckdb` package used at runtime (see benchmark_py/pyproject.toml).
if (-not $env:DUCKDB_VERSION) { $env:DUCKDB_VERSION = "v1.5.5" }
if (-not $env:CI_TOOLS_VERSION) { $env:CI_TOOLS_VERSION = "v1.5.5" }

# Ensure submodules (duckdb + extension-ci-tools) are present.
if (-not (Test-Path (Join-Path $ProjDir "duckdb\CMakeLists.txt")) -or
    -not (Test-Path (Join-Path $ProjDir "extension-ci-tools\makefiles"))) {
  Write-Host ">> Initializing submodules (duckdb, extension-ci-tools)..."
  git -C $ProjDir submodule update --init --recursive
}

# Pin both submodules to the fixed version above, regardless of what commit
# the superproject gitlink currently records. This guarantees the extension is
# always compiled against the exact DuckDB version we intend, avoiding the
# "built for X / runtime is Y" load error.
Write-Host ">> Pinning submodules (duckdb=$env:DUCKDB_VERSION, extension-ci-tools=$env:CI_TOOLS_VERSION)..."
git -C (Join-Path $ProjDir "duckdb") fetch --tags origin
git -C (Join-Path $ProjDir "duckdb") checkout $env:DUCKDB_VERSION
git -C (Join-Path $ProjDir "extension-ci-tools") fetch --tags origin
git -C (Join-Path $ProjDir "extension-ci-tools") checkout $env:CI_TOOLS_VERSION

# Invalidate stale CMake cache when the DuckDB source or build configuration
# has changed. CMake otherwise reuses build/<type>/CMakeCache.txt across
# different DuckDB commits / generators, silently linking the wrong DuckDB
# version into the extension (the "built for X / runtime is Y" load error).
$DuckdbCommit = (git -C (Join-Path $ProjDir "duckdb") rev-parse HEAD 2>$null)
if (-not $DuckdbCommit) { $DuckdbCommit = "unknown" }
$CacheStamp = Join-Path $BuildDir ".duckdb_commit"
$NeedClean = $false

if (Test-Path (Join-Path $BuildDir "CMakeCache.txt")) {
  # (1) Generator mismatch (e.g. previous build used "Unix Makefiles").
  $CachedGen = (Select-String -Path (Join-Path $BuildDir "CMakeCache.txt") -Pattern '^CMAKE_GENERATOR:INTERNAL=(.*)$').Matches[0].Groups[1].Value
  if ($CachedGen -and $CachedGen -ne "Ninja") {
    Write-Host ">> Generator mismatch ($CachedGen != Ninja), clearing build cache..."
    $NeedClean = $true
  }

  # (2) DuckDB source commit changed since last build.
  $PrevCommit = ""
  if (Test-Path $CacheStamp) { $PrevCommit = (Get-Content $CacheStamp -Raw).Trim() }
  if (-not $PrevCommit) {
    Write-Host ">> No build stamp found (stale/foreign build dir), clearing build cache..."
    $NeedClean = $true
  } elseif ($PrevCommit -ne $DuckdbCommit) {
    Write-Host ">> DuckDB source changed ($PrevCommit -> $DuckdbCommit), clearing build cache..."
    $NeedClean = $true
  }

  if ($NeedClean) {
    Remove-Item -Recurse -Force $BuildDir -ErrorAction SilentlyContinue
  }
}

# Windows uses vcpkg manifest mode (vcpkg.json declares talib + eigen3).
# VCPKG_ROOT is auto-detected if vcpkg is on PATH; otherwise set it below.
$vcpkgRoot = $env:VCPKG_ROOT
if (-not $vcpkgRoot) {
  # Try a sibling clone, otherwise rely on CMake's built-in vcpkg bootstrap.
  $sibling = Join-Path $ProjDir "..\vcpkg"
  if (Test-Path (Join-Path $sibling "vcpkg.exe")) { $vcpkgRoot = $sibling }
}

$cmakeArgs = @(
  "-S", (Join-Path $ProjDir "duckdb")
  "-B", $BuildDir
  "-DCMAKE_BUILD_TYPE=$BuildType"
  "-DBUILD_UNITTESTS=0"
  "-DBUILD_SHELL=1"
  "-DDUCKDB_EXTENSION_CONFIGS=$((Join-Path $ProjDir 'extension_config.cmake'))"
)
if ($vcpkgRoot) {
  $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$((Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'))"
  $cmakeArgs += "-DVCPKG_MANIFEST_DIR=$ProjDir"
  $cmakeArgs += "-DVCPKG_TARGET_TRIPLET=x64-windows"
} else {
  Write-Host "WARNING: VCPKG_ROOT not set and no sibling vcpkg found; relying on system TA-Lib/Eigen3."
}

Write-Host ">> Configuring ($BuildType)..."
cmake @cmakeArgs

Write-Host ">> Building cn_ta (loadable extension)..."
cmake --build $BuildDir --target cn_ta_loadable_extension

# Record the DuckDB commit this build used, so the next run can detect changes.
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
Set-Content -Path $CacheStamp -Value $DuckdbCommit

Write-Host ">> Done. Artifacts in: $BuildDir\extension\cn_ta\"
Write-Host "   loadable: $BuildDir\extension\cn_ta\cn_ta.duckdb_extension"
