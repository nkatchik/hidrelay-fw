include_guard(GLOBAL)

# Pico SDK 2.0 still uses FetchContent_Populate for picotool.
# Keep compatibility local to this platform instead of root build glue.
if(NOT DEFINED CMAKE_POLICY_DEFAULT_CMP0169)
    set(CMAKE_POLICY_DEFAULT_CMP0169 OLD)
endif()

if(NOT DEFINED PICOTOOL_FETCH_FROM_GIT_PATH)
    set(PICOTOOL_FETCH_FROM_GIT_PATH
        "${APP_CACHE_DIR}/picotool"
        CACHE PATH
        "Shared picotool fetch cache used by Pico SDK projects.")
endif()

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico_w CACHE STRING "Pico board type for platform/pico_w" FORCE)
endif()

if(PICO_NO_PICOTOOL)
    message(FATAL_ERROR
        "PICO_NO_PICOTOOL=ON is not supported in this repository; UF2 output is required for flashing.")
endif()

set(PICO_NO_PICOTOOL OFF CACHE BOOL "Enable picotool post-processing so UF2 is produced." FORCE)

option(APP_PLATFORM_ENABLE_TINYUSB "Link TinyUSB device support through Pico SDK." ON)
option(APP_PLATFORM_ENABLE_BTSTACK "Link BTstack support through Pico SDK." ON)
option(APP_PLATFORM_ALLOW_RELEASE_TELEMETRY
    "Allow telemetry/diagnostics options in Release builds (development use only)." OFF)

if(NOT DEFINED APP_PLATFORM_ENABLE_TELEMETRY)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(APP_PLATFORM_ENABLE_TELEMETRY ON CACHE BOOL "Enable diagnostics/telemetry surfaces on Pico W." FORCE)
    else()
        set(APP_PLATFORM_ENABLE_TELEMETRY OFF CACHE BOOL "Enable diagnostics/telemetry surfaces on Pico W." FORCE)
    endif()
endif()

if(NOT DEFINED APP_PLATFORM_ENABLE_DIAG_CDC)
    if(APP_PLATFORM_ENABLE_TELEMETRY)
        set(APP_PLATFORM_ENABLE_DIAG_CDC ON CACHE BOOL "Expose diagnostics over TinyUSB CDC on Pico W." FORCE)
    else()
        set(APP_PLATFORM_ENABLE_DIAG_CDC OFF CACHE BOOL "Expose diagnostics over TinyUSB CDC on Pico W." FORCE)
    endif()
endif()

if(APP_PLATFORM_ENABLE_DIAG_CDC AND (NOT APP_PLATFORM_ENABLE_TELEMETRY))
    message(WARNING
        "APP_PLATFORM_ENABLE_DIAG_CDC requires APP_PLATFORM_ENABLE_TELEMETRY=ON; forcing diagnostics CDC OFF.")
    set(APP_PLATFORM_ENABLE_DIAG_CDC OFF CACHE BOOL "Expose diagnostics over TinyUSB CDC on Pico W." FORCE)
endif()

string(TOUPPER "${CMAKE_BUILD_TYPE}" APP_PLATFORM_BUILD_TYPE_UPPER)
if((APP_PLATFORM_BUILD_TYPE_UPPER STREQUAL "RELEASE")
    AND (NOT APP_PLATFORM_ALLOW_RELEASE_TELEMETRY))
    if(APP_PLATFORM_ENABLE_TELEMETRY OR APP_PLATFORM_ENABLE_DIAG_CDC)
        message(FATAL_ERROR
            "Release guard: telemetry/diagnostics are disabled for production builds. "
            "Unset APP_PLATFORM_ENABLE_TELEMETRY/APP_PLATFORM_ENABLE_DIAG_CDC, or set "
            "APP_PLATFORM_ALLOW_RELEASE_TELEMETRY=ON only for explicit development/debug releases.")
    endif()
endif()
