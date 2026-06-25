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

if(APP_PLATFORM STREQUAL "pico_2_w")
    set(APP_PLATFORM_PICO_DEFAULT_BOARD pico2_w)
elseif(APP_PLATFORM STREQUAL "pico_w")
    set(APP_PLATFORM_PICO_DEFAULT_BOARD pico_w)
else()
    message(FATAL_ERROR "Unsupported Pico-family APP_PLATFORM='${APP_PLATFORM}'")
endif()

if(NOT DEFINED PICO_BOARD OR "${PICO_BOARD}" STREQUAL "")
    set(PICO_BOARD "${APP_PLATFORM_PICO_DEFAULT_BOARD}" CACHE STRING
        "Pico SDK board type for ${APP_PLATFORM}" FORCE)
endif()

option(APP_PLATFORM_ENABLE_TINYUSB "Link TinyUSB device support through Pico SDK." ON)
option(APP_PLATFORM_ENABLE_BTSTACK "Link BTstack support through Pico SDK." ON)
option(APP_PLATFORM_ALLOW_RELEASE_TELEMETRY
    "Allow telemetry/diagnostics options in Release builds (development use only)." OFF)
option(APP_PLATFORM_ALLOW_RELEASE_USB_RESET_INTERFACE
    "Allow the USB vendor reset interface in Release builds (development use only)." OFF)
option(APP_PLATFORM_DEBUG_WIPE_ALL_ON_BOOT
    "Debug only: erase all persisted Pair DB + BTstack bonding/security data on every boot." OFF)

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

if(NOT DEFINED APP_PLATFORM_ENABLE_USB_RESET_INTERFACE)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(APP_PLATFORM_ENABLE_USB_RESET_INTERFACE ON CACHE BOOL
            "Expose the Pico USB vendor reset interface for picotool reboot/flash commands." FORCE)
    else()
        set(APP_PLATFORM_ENABLE_USB_RESET_INTERFACE OFF CACHE BOOL
            "Expose the Pico USB vendor reset interface for picotool reboot/flash commands." FORCE)
    endif()
endif()

if(APP_PLATFORM_ENABLE_USB_RESET_INTERFACE AND (NOT APP_PLATFORM_ENABLE_TINYUSB))
    message(WARNING
        "APP_PLATFORM_ENABLE_USB_RESET_INTERFACE is ON but APP_PLATFORM_ENABLE_TINYUSB is OFF; "
        "USB reset interface is disabled.")
    set(APP_PLATFORM_ENABLE_USB_RESET_INTERFACE OFF CACHE BOOL
        "Expose the Pico USB vendor reset interface for picotool reboot/flash commands." FORCE)
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

if((APP_PLATFORM_BUILD_TYPE_UPPER STREQUAL "RELEASE")
    AND APP_PLATFORM_ENABLE_USB_RESET_INTERFACE
    AND (NOT APP_PLATFORM_ALLOW_RELEASE_USB_RESET_INTERFACE))
    message(FATAL_ERROR
        "Release guard: the USB reset interface is disabled for production builds. "
        "Unset APP_PLATFORM_ENABLE_USB_RESET_INTERFACE, or set "
        "APP_PLATFORM_ALLOW_RELEASE_USB_RESET_INTERFACE=ON only for explicit development/debug releases.")
endif()
