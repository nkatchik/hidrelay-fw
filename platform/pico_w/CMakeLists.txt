include(${CMAKE_CURRENT_LIST_DIR}/cmake/options.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/target.cmake)

if(NOT DEFINED PICO_SDK_PATH)
    message(FATAL_ERROR "PICO_SDK_PATH must point to a local Pico SDK checkout. Run make bootstrap first.")
endif()

if(NOT EXISTS "${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
    message(FATAL_ERROR "Missing ${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

pico_sdk_init()

add_executable(hidrelay_fw
    ${CMAKE_SOURCE_DIR}/src/main.c
    src/platform_api_pico_w.c
    src/platform_pico_w_hw.c
    src/platform_pico_w_state.c
)

target_include_directories(hidrelay_fw
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_LIST_DIR}/include
)

pico_w_configure_target(hidrelay_fw)
