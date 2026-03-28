include_guard(GLOBAL)

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico_w CACHE STRING "Pico board type for platform/pico_w" FORCE)
endif()

if(NOT DEFINED PICO_NO_PICOTOOL)
    set(PICO_NO_PICOTOOL ON CACHE BOOL "Skip picotool fetch/build in default local firmware builds." FORCE)
endif()

option(APP_PLATFORM_ENABLE_TINYUSB "Link TinyUSB device support through Pico SDK." OFF)
option(APP_PLATFORM_ENABLE_BTSTACK "Link BTstack support through Pico SDK." OFF)
