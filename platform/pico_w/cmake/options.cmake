include_guard(GLOBAL)

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico_w CACHE STRING "Pico board type for platform/pico_w" FORCE)
endif()

if(NOT DEFINED PICO_NO_PICOTOOL)
    set(PICO_NO_PICOTOOL ON CACHE BOOL "Skip picotool fetch/build in default local firmware builds." FORCE)
endif()

option(APP_PLATFORM_ENABLE_TINYUSB "Link TinyUSB device support through Pico SDK." OFF)
option(APP_PLATFORM_ENABLE_BTSTACK "Link BTstack support through Pico SDK." OFF)

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
