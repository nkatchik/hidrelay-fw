if(NOT DEFINED APP_SOURCE_DIR)
    get_filename_component(APP_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(APP_CACHE_DIR "${APP_SOURCE_DIR}/.cache")

file(MAKE_DIRECTORY
    "${APP_CACHE_DIR}"
    "${APP_CACHE_DIR}/download"
    "${APP_CACHE_DIR}/tool"
    "${APP_CACHE_DIR}/sdk"
    "${APP_CACHE_DIR}/toolchain"
    "${APP_CACHE_DIR}/bootstrap"
)

list(APPEND CMAKE_MODULE_PATH "${APP_SOURCE_DIR}/cmake")

include(PlatformSelect)
include(PlatformBootstrap)

if((NOT DEFINED APP_PLATFORM) OR ("${APP_PLATFORM}" STREQUAL "") OR ("${APP_PLATFORM}" STREQUAL "auto"))
    app_list_platform("${APP_SOURCE_DIR}" _platforms)
    if(_platforms STREQUAL "")
        message(FATAL_ERROR
            "No platform targets found under ${APP_SOURCE_DIR}/platform. "
            "Add at least one platform/<name>/CMakeLists.txt")
    endif()

    list(JOIN _platforms ", " _supported_csv)
    message(FATAL_ERROR
        "APP_PLATFORM must be specified for bootstrap. "
        "Run with -DAPP_PLATFORM=<target>. Available targets: ${_supported_csv}")
endif()

app_resolve_platform("${APP_SOURCE_DIR}" "${APP_PLATFORM}" APP_PLATFORM_RESOLVED)

message(STATUS "Bootstrapping platform ${APP_PLATFORM_RESOLVED}")

app_bootstrap_platform_environment(
    SOURCE_DIR "${APP_SOURCE_DIR}"
    PLATFORM "${APP_PLATFORM_RESOLVED}"
    CACHE_DIR "${APP_CACHE_DIR}"
    ALLOW_FETCH ON
    OUT_CACHE_SEED APP_PLATFORM_CACHE_SEED
)

set(_bootstrap_cache "${APP_CACHE_DIR}/bootstrap/${APP_PLATFORM_RESOLVED}.cmake")

app_write_bootstrap_cache_file(
    OUTPUT_FILE "${_bootstrap_cache}"
    PLATFORM "${APP_PLATFORM_RESOLVED}"
    CACHE_SEED "${APP_PLATFORM_CACHE_SEED}"
)

message(STATUS "Bootstrap complete")
list(LENGTH APP_PLATFORM_CACHE_SEED _seed_count)
message(STATUS "  Cache entries:  ${_seed_count}")
message(STATUS "  Cache preset:   ${_bootstrap_cache}")
