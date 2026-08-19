vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO "TA-Lib/ta-lib"
    REF "v${VERSION}"
    SHA512 189702beda83f9ebe16ef7d08d8bba76068a71b63409e2e00f1a5a4a06997037d54f048778323fcc6482fe1e5ce9125314b4d4b7a12dee5d64c5b0d3879fca45
    PATCHES
        fix-forced-install-prefix.patch
        no-system-cleanup.patch
)

if(VCPKG_TARGET_IS_WINDOWS)
    vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
endif()

# Since upstream 0.6.1 the only supported build systems are CMake and
# autotools on every platform; the make/ msvc tree this port used to
# drive on Windows no longer exists in the source archive.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    DISABLE_PARALLEL_CONFIGURE
    OPTIONS
        -DBUILD_DEV_TOOLS=OFF
)
vcpkg_cmake_install()

# Upstream always builds and installs both the shared and the static
# library. Keep only what matches the triplet's linkage.
if(VCPKG_TARGET_IS_WINDOWS)
    # Static-only on Windows: drop the DLL and its import library, and
    # let the static library take the canonical name.
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin" "${CURRENT_PACKAGES_DIR}/debug/bin")
    file(REMOVE "${CURRENT_PACKAGES_DIR}/lib/ta-lib.lib" "${CURRENT_PACKAGES_DIR}/debug/lib/ta-lib.lib")
    file(RENAME "${CURRENT_PACKAGES_DIR}/lib/ta-lib-static.lib" "${CURRENT_PACKAGES_DIR}/lib/ta-lib.lib")
    if(EXISTS "${CURRENT_PACKAGES_DIR}/debug/lib/ta-lib-static.lib")
        file(RENAME "${CURRENT_PACKAGES_DIR}/debug/lib/ta-lib-static.lib" "${CURRENT_PACKAGES_DIR}/debug/lib/ta-lib.lib")
    endif()
elseif(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    file(GLOB _talib_shared
        "${CURRENT_PACKAGES_DIR}/lib/libta-lib*.so*"
        "${CURRENT_PACKAGES_DIR}/lib/libta-lib*.dylib"
        "${CURRENT_PACKAGES_DIR}/debug/lib/libta-lib*.so*"
        "${CURRENT_PACKAGES_DIR}/debug/lib/libta-lib*.dylib")
    if(_talib_shared)
        file(REMOVE ${_talib_shared})
    endif()
else()
    file(REMOVE
        "${CURRENT_PACKAGES_DIR}/lib/libta-lib.a"
        "${CURRENT_PACKAGES_DIR}/debug/lib/libta-lib.a")
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL
    "${CMAKE_CURRENT_LIST_DIR}/vcpkg-cmake-wrapper.cmake"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
)

# License file
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
