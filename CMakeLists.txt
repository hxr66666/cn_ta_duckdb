# NOTE: This CMakeLists.txt is NOT a standalone project. It is included by
# DuckDB's build system via extension_config.cmake (duckdb_extension_load),
# which provides the `build_static_extension` / `build_loadable_extension`
# macros used below. An IDE that parses this file in isolation will falsely
# report those commands as unknown; ignore such diagnostics.
cmake_minimum_required(VERSION 3.5...3.29)

set(TARGET_NAME cn_ta)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

# ============================================================
# 第三方依赖：talib + eigen3
# 用标准 find_* 查找，依赖来自：
#   - Linux : 系统包（sudo apt install ta-lib-dev libeigen3-dev）
#   - Windows: vcpkg（vcpkg.json 声明，官方 port 原生支持 Windows）
# 若依赖不在默认路径，通过 CMAKE_PREFIX_PATH 指定。
# ============================================================

# Eigen（header-only，系统在 /usr/include/eigen3，vcpkg 在 include/eigen3）
find_path(EIGEN3_INCLUDE_DIR NAMES Eigen/Dense PATH_SUFFIXES eigen3)
if(NOT EIGEN3_INCLUDE_DIR)
    message(FATAL_ERROR "cn_ta: 未找到 Eigen3。Linux 请运行 'sudo apt install libeigen3-dev'，Windows 请用 vcpkg 安装。")
endif()
include_directories(${EIGEN3_INCLUDE_DIR})

# TA-Lib 头文件（系统在 /usr/include/ta-lib）
find_path(TA_LIB_INCLUDE_DIR NAMES ta_libc.h PATH_SUFFIXES ta-lib)
if(NOT TA_LIB_INCLUDE_DIR)
    message(FATAL_ERROR "cn_ta: 未找到 TA-Lib 头文件。Linux 请运行 'sudo apt install ta-lib-dev'，Windows 请用 vcpkg 安装。")
endif()
include_directories(${TA_LIB_INCLUDE_DIR})

# TA-Lib 库：Windows（官方预编译包/vcpkg）为 4 个库，Unix 为单一库
if(WIN32)
    find_library(TA_LIB_MAIN NAMES ta_libc ta-lib)
    find_library(TA_LIB_FUNC NAMES ta_func)
    find_library(TA_LIB_ABSTRACT NAMES ta_abstract)
    find_library(TA_LIB_COMMON NAMES ta_common)
    if(NOT TA_LIB_MAIN)
        message(FATAL_ERROR "cn_ta: 未找到 Windows TA-Lib 库。请用 vcpkg 安装 talib 或配置官方预编译包。")
    endif()
    set(TA_LIB_LIBRARIES ${TA_LIB_MAIN} ${TA_LIB_FUNC} ${TA_LIB_ABSTRACT} ${TA_LIB_COMMON})
else()
    find_library(TA_LIB_LIBRARIES NAMES ta-lib talib)
    if(NOT TA_LIB_LIBRARIES)
        message(FATAL_ERROR "cn_ta: 未找到 TA-Lib 库。Linux 请运行 'sudo apt install ta-lib-dev'。")
    endif()
endif()

include_directories(src/include)

set(EXTENSION_SOURCES
    src/cn_ta_extension.cpp
    src/cn_ta_adapter.cpp
    src/cn_ta_scalar.cpp
    src/cn_ta_aggregate.cpp
    src/cn_ta_multi_output.cpp
    src/cn_ta_multi_output_agg.cpp
    src/cn_ta_stat.cpp
    src/cn_ta_stat_agg.cpp
    src/cn_ta_ashare.cpp
    src/cn_ta_ts_agg.cpp
    src/cn_ta_indicators.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Eigen 需要 C++14+，统一用 C++17
set_property(TARGET ${EXTENSION_NAME} PROPERTY CXX_STANDARD 17)
set_property(TARGET ${LOADABLE_EXTENSION_NAME} PROPERTY CXX_STANDARD 17)
set_property(TARGET ${EXTENSION_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${LOADABLE_EXTENSION_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)

# Link TA-Lib
target_link_libraries(${EXTENSION_NAME} ${TA_LIB_LIBRARIES})
target_link_libraries(${LOADABLE_EXTENSION_NAME} ${TA_LIB_LIBRARIES})

install(
    TARGETS ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)
