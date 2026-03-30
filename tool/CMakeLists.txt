cmake_minimum_required(VERSION 3.23)

project(hidrelay_tools LANGUAGES C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(HIDRELAY_TOOL_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
)

add_library(tool_common STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/util/cleanup.c
)

target_compile_features(tool_common PRIVATE c_std_17)

target_compile_options(tool_common
    PRIVATE
        ${HIDRELAY_TOOL_WARNINGS}
)

target_include_directories(tool_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

add_executable(cache_probe
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cache_probe.c
)

target_compile_features(cache_probe PRIVATE c_std_17)

target_compile_options(cache_probe
    PRIVATE
        ${HIDRELAY_TOOL_WARNINGS}
)

target_link_libraries(cache_probe
    PRIVATE
        tool_common
)

add_executable(diag_capture
    ${CMAKE_CURRENT_SOURCE_DIR}/src/diag_capture.c
)

target_compile_features(diag_capture PRIVATE c_std_17)

target_compile_options(diag_capture
    PRIVATE
        ${HIDRELAY_TOOL_WARNINGS}
)

target_link_libraries(diag_capture
    PRIVATE
        tool_common
)
