include_guard(GLOBAL)

include(CMakeParseArguments)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/ToolchainBootstrap.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/DependencyBootstrap.cmake")

set(PICO_W_SDK_GIT_TAG "2.0.0" CACHE STRING "Pinned Pico SDK git tag for platform/pico_w.")
set(PICO_W_SDK_GIT_URL "https://github.com/raspberrypi/pico-sdk.git" CACHE STRING "Pico SDK git URL for platform/pico_w.")
set(PICO_W_ARM_GCC_VERSION "13.2.Rel1" CACHE STRING "Pinned Arm GNU embedded toolchain version for platform/pico_w.")
set(PICO_W_ARM_GCC_BASE_URL "https://developer.arm.com/-/media/Files/downloads/gnu/${PICO_W_ARM_GCC_VERSION}/binrel"
    CACHE STRING "Base URL for Arm GNU toolchain downloads used by platform/pico_w.")

function(pico_w_select_arm_archive host_os host_arch out_archive)
    if(host_os STREQUAL "linux" AND host_arch STREQUAL "x86_64")
        set(_archive "arm-gnu-toolchain-${PICO_W_ARM_GCC_VERSION}-x86_64-arm-none-eabi.tar.xz")
    elseif(host_os STREQUAL "darwin" AND host_arch STREQUAL "x86_64")
        set(_archive "arm-gnu-toolchain-${PICO_W_ARM_GCC_VERSION}-darwin-x86_64-arm-none-eabi.tar.xz")
    elseif(host_os STREQUAL "darwin" AND host_arch STREQUAL "aarch64")
        set(_archive "arm-gnu-toolchain-${PICO_W_ARM_GCC_VERSION}-darwin-arm64-arm-none-eabi.tar.xz")
    else()
        message(FATAL_ERROR
            "Unsupported host '${host_os}/${host_arch}' for platform/pico_w Arm toolchain bootstrap")
    endif()

    set(${out_archive} "${_archive}" PARENT_SCOPE)
endfunction()

function(platform_bootstrap_dependencies)
    cmake_parse_arguments(ARG "" "SOURCE_DIR;PLATFORM;CACHE_DIR;ALLOW_FETCH" "" ${ARGN})

    if(NOT ARG_CACHE_DIR)
        message(FATAL_ERROR "platform_bootstrap_dependencies requires CACHE_DIR")
    endif()

    set(_sdk_dir "${ARG_CACHE_DIR}/sdk/pico-sdk-${PICO_W_SDK_GIT_TAG}")

    app_git_clone_if_missing(
        REPO_URL "${PICO_W_SDK_GIT_URL}"
        GIT_TAG "${PICO_W_SDK_GIT_TAG}"
        DEST_DIR "${_sdk_dir}"
        ALLOW_FETCH "${ARG_ALLOW_FETCH}"
    )

    app_git_submodules_ensure_paths(
        ROOT_DIR "${_sdk_dir}"
        ALLOW_FETCH "${ARG_ALLOW_FETCH}"
        CHECK_PATHS
            "${_sdk_dir}/lib/tinyusb/src/tusb.c"
            "${_sdk_dir}/lib/btstack/src/btstack_run_loop.c"
    )

    set(PICO_SDK_PATH "${_sdk_dir}" CACHE PATH "Local Pico SDK checkout." FORCE)
endfunction()

function(platform_bootstrap_toolchain)
    cmake_parse_arguments(ARG "" "SOURCE_DIR;PLATFORM;CACHE_DIR;ALLOW_FETCH" "" ${ARGN})

    if(NOT ARG_SOURCE_DIR)
        message(FATAL_ERROR "platform_bootstrap_toolchain requires SOURCE_DIR")
    endif()

    if(NOT ARG_PLATFORM)
        message(FATAL_ERROR "platform_bootstrap_toolchain requires PLATFORM")
    endif()

    if(NOT ARG_CACHE_DIR)
        message(FATAL_ERROR "platform_bootstrap_toolchain requires CACHE_DIR")
    endif()

    if(NOT DEFINED PICO_SDK_PATH)
        message(FATAL_ERROR "PICO_SDK_PATH must be set by platform_bootstrap_dependencies")
    endif()

    app_detect_host(_host_os _host_arch)
    pico_w_select_arm_archive("${_host_os}" "${_host_arch}" _archive_name)

    set(_download_dir "${ARG_CACHE_DIR}/download")
    set(_tool_dir "${ARG_CACHE_DIR}/tool")
    set(_toolchain_dir "${ARG_CACHE_DIR}/toolchain")

    file(MAKE_DIRECTORY "${_download_dir}" "${_tool_dir}" "${_toolchain_dir}")

    string(REGEX REPLACE "\\.tar\\.xz$" "" _toolchain_folder "${_archive_name}")
    set(_arm_gcc_root "${_tool_dir}/${_toolchain_folder}")
    set(_arm_gcc_bin "${_arm_gcc_root}/bin/arm-none-eabi-gcc")

    if(NOT EXISTS "${_arm_gcc_bin}")
        if(NOT ARG_ALLOW_FETCH)
            message(FATAL_ERROR
                "Arm GCC toolchain not found at ${_arm_gcc_root}. Run bootstrap with fetch enabled.")
        endif()

        set(_archive_path "${_download_dir}/${_archive_name}")
        set(_download_url "${PICO_W_ARM_GCC_BASE_URL}/${_archive_name}")

        app_download_file_if_missing("${_download_url}" "${_archive_path}")
        app_extract_archive_if_missing("${_archive_path}" "${_tool_dir}" "${_arm_gcc_bin}")
    endif()

    set(ARM_GCC_ROOT "${_arm_gcc_root}" CACHE PATH "Local Arm GNU toolchain root." FORCE)
    set(PICO_TOOLCHAIN_PATH "${ARM_GCC_ROOT}" CACHE PATH "Path searched by Pico SDK for arm-none-eabi tools." FORCE)

    set(PICO_SDK_TOOLCHAIN_FILE "${PICO_SDK_PATH}/cmake/preload/toolchains/pico_arm_cortex_m0plus_gcc.cmake")
    if(NOT EXISTS "${PICO_SDK_TOOLCHAIN_FILE}")
        message(FATAL_ERROR "Missing Pico SDK toolchain file: ${PICO_SDK_TOOLCHAIN_FILE}")
    endif()

    set(_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/toolchain.cmake.in")
    if(NOT EXISTS "${_template}")
        message(FATAL_ERROR "Missing platform toolchain template: ${_template}")
    endif()

    set(_generated_toolchain "${_toolchain_dir}/${ARG_PLATFORM}.cmake")
    configure_file("${_template}" "${_generated_toolchain}" @ONLY)

    set(CMAKE_TOOLCHAIN_FILE "${_generated_toolchain}" CACHE FILEPATH "Generated platform toolchain file." FORCE)
endfunction()

function(platform_bootstrap_cache_seed out_entries)
    set(_entries "")

    if(DEFINED CMAKE_TOOLCHAIN_FILE)
        list(APPEND _entries "CMAKE_TOOLCHAIN_FILE|FILEPATH|${CMAKE_TOOLCHAIN_FILE}")
    endif()

    if(DEFINED PICO_SDK_PATH)
        list(APPEND _entries "PICO_SDK_PATH|PATH|${PICO_SDK_PATH}")
    endif()

    if(DEFINED ARM_GCC_ROOT)
        list(APPEND _entries "ARM_GCC_ROOT|PATH|${ARM_GCC_ROOT}")
    endif()

    if(DEFINED PICO_TOOLCHAIN_PATH)
        list(APPEND _entries "PICO_TOOLCHAIN_PATH|PATH|${PICO_TOOLCHAIN_PATH}")
    endif()

    set(${out_entries} "${_entries}" PARENT_SCOPE)
endfunction()
