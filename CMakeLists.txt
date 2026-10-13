cmake_minimum_required(VERSION 3.16)
project(weflow_wcdb_api VERSION 2.1.15 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Default path to project deps directory containing Tencent WCDB v2.1.15 binaries
set(WCDB_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps" CACHE PATH "Path to deps directory")

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Source files
set(WCDB_API_SOURCES
    src/core_account.cpp
    src/session_contact.cpp
    src/message_query.cpp
    src/stats_reports.cpp
    src/media_sns_ai.cpp
    src/crud_antirevoke_snapshot.cpp
    src/cloud_telemetry.cpp
    src/compat_exports.cpp
)

add_library(wcdb_api SHARED ${WCDB_API_SOURCES})

# Base properties
set_target_properties(wcdb_api PROPERTIES
    OUTPUT_NAME "wcdb_api"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# -----------------------------------------------------------------------------
# Platform-specific configurations
# -----------------------------------------------------------------------------
if(APPLE)
    message(STATUS "Configuring for Apple macOS (${CMAKE_SYSTEM_PROCESSOR})")

    # Homebrew prefix
    set(HOMEBREW_PREFIX "/opt/homebrew" CACHE PATH "Homebrew prefix")
    include_directories(${HOMEBREW_PREFIX}/include)

    # Locate libWCDB.dylib (prefer project ./deps/macos/universal/libWCDB.dylib)
    set(DEFAULT_MAC_WCDB "${WCDB_DEPS_DIR}/macos/universal/libWCDB.dylib")
    if(NOT EXISTS "${DEFAULT_MAC_WCDB}")
        set(DEFAULT_MAC_WCDB "/Applications/WeFlow.app/Contents/Resources/resources/wcdb/macos/universal/libWCDB.dylib")
    endif()

    set(WCDB_DYLIB_PATH "${DEFAULT_MAC_WCDB}" CACHE FILEPATH "Path to libWCDB.dylib")

    # Locate zstd (prefer static library to eliminate runtime brew dependencies)
    if(EXISTS "${HOMEBREW_PREFIX}/lib/libzstd.a")
        set(ZSTD_LIB "${HOMEBREW_PREFIX}/lib/libzstd.a")
    else()
        find_library(ZSTD_LIB NAMES zstd PATHS ${HOMEBREW_PREFIX}/lib REQUIRED)
    endif()

    find_package(CURL REQUIRED)

    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(SECURITY_FRAMEWORK Security REQUIRED)
    find_library(APPLICATIONSERVICES_FRAMEWORK ApplicationServices REQUIRED)
    find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    find_library(CORESERVICES_FRAMEWORK CoreServices REQUIRED)

    set_target_properties(wcdb_api PROPERTIES
        INSTALL_NAME_DIR "@rpath"
        BUILD_WITH_INSTALL_RPATH ON
    )

    target_link_libraries(wcdb_api PRIVATE
        "${WCDB_DYLIB_PATH}"
        "${ZSTD_LIB}"
        CURL::libcurl
        "${COREFOUNDATION_FRAMEWORK}"
        "${SECURITY_FRAMEWORK}"
        "${APPLICATIONSERVICES_FRAMEWORK}"
        "${COREGRAPHICS_FRAMEWORK}"
        "${CORESERVICES_FRAMEWORK}"
    )

    # Rewrite LC_LOAD_DYLIB for libWCDB.dylib to @loader_path/libWCDB.dylib
    add_custom_command(TARGET wcdb_api POST_BUILD
        COMMAND install_name_tool -change
            "@rpath/WCDB.framework/Versions/2.1.15/WCDB"
            "@loader_path/libWCDB.dylib"
            $<TARGET_FILE:wcdb_api>
        COMMENT "Setting @loader_path/libWCDB.dylib on libwcdb_api.dylib"
    )

elseif(WIN32)
    message(STATUS "Configuring for Windows (${CMAKE_SYSTEM_PROCESSOR})")

    # Locate WCDB import library (.lib) from project ./deps
    set(DEFAULT_WIN_WCDB "")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        set(DEFAULT_WIN_WCDB "${WCDB_DEPS_DIR}/win32/arm64/WCDB.lib")
    else()
        set(DEFAULT_WIN_WCDB "${WCDB_DEPS_DIR}/win32/x64/WCDB.lib")
    endif()
    set(WCDB_LIB_PATH "${DEFAULT_WIN_WCDB}" CACHE FILEPATH "Path to WCDB.lib")

    # Dependencies: curl and zstd via vcpkg or system
    find_package(CURL REQUIRED)
    find_package(zstd QUIET)
    if(NOT zstd_FOUND)
        find_library(ZSTD_LIB NAMES zstd zstd_static REQUIRED)
    else()
        set(ZSTD_LIB zstd::libzstd_shared)
    endif()

    target_link_libraries(wcdb_api PRIVATE
        "${WCDB_LIB_PATH}"
        "${ZSTD_LIB}"
        CURL::libcurl
        ws2_32
        crypt32
        advapi32
        user32
        ole32
        oleaut32
    )

elseif(UNIX)
    message(STATUS "Configuring for Linux / Unix (${CMAKE_SYSTEM_PROCESSOR})")

    set(WCDB_LIB_PATH "" CACHE FILEPATH "Path to libWCDB.a (static) or libWCDB.so (shared)")

    find_package(CURL REQUIRED)
    find_package(zstd QUIET)
    if(NOT zstd_FOUND)
        find_library(ZSTD_LIB NAMES zstd REQUIRED)
    else()
        set(ZSTD_LIB zstd::libzstd)
    endif()

    target_link_libraries(wcdb_api PRIVATE
        ${ZSTD_LIB}
        CURL::libcurl
        pthread
        dl
    )
    if(WCDB_LIB_PATH AND EXISTS "${WCDB_LIB_PATH}")
        target_link_libraries(wcdb_api PRIVATE "${WCDB_LIB_PATH}")
    endif()

    set_target_properties(wcdb_api PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON
    )
endif()
