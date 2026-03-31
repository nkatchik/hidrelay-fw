cmake_minimum_required(VERSION 3.23)

set(APP_PLATFORM "" CACHE STRING "Target platform folder name (required).")
set(APP_AUTO_FETCH_DEPS ON CACHE BOOL "Fetch missing platform dependencies during configure.")
set(APP_CACHE_DIR "${CMAKE_SOURCE_DIR}/.cache" CACHE PATH "Local cache directory for SDK/toolchain artifacts.")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(PlatformSelect)
include(PlatformBootstrap)

app_resolve_platform("${CMAKE_SOURCE_DIR}" "${APP_PLATFORM}" APP_PLATFORM_RESOLVED)
set(APP_PLATFORM "${APP_PLATFORM_RESOLVED}" CACHE STRING "Target platform folder name (required)." FORCE)

app_bootstrap_platform_environment(
    SOURCE_DIR "${CMAKE_SOURCE_DIR}"
    PLATFORM "${APP_PLATFORM}"
    CACHE_DIR "${APP_CACHE_DIR}"
    ALLOW_FETCH "${APP_AUTO_FETCH_DEPS}"
)

project(hidrelay_fw VERSION 0.1.0 LANGUAGES C CXX ASM)

include(CommonOptions)

add_library(hidrelay_core STATIC
    src/app.c
    src/button_fsm.c
    src/led_ui.c
    src/pair_db.c
    src/bt_manager.c
    src/hid_report_policy.c
    src/hid_report_remap.c
    src/operator_command.c
    src/usb_bridge.c
    src/util/cleanup.c
)

target_include_directories(hidrelay_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

app_target_enable_project_warnings(hidrelay_core)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/platform/${APP_PLATFORM}")
