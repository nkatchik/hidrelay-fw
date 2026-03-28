include_guard(GLOBAL)

function(app_detect_host out_os out_arch)
    string(TOLOWER "${CMAKE_HOST_SYSTEM_NAME}" _os)
    string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" _arch)

    if(_os STREQUAL "")
        execute_process(
            COMMAND uname -s
            RESULT_VARIABLE _uname_os_result
            OUTPUT_VARIABLE _uname_os_output
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        if(_uname_os_result EQUAL 0)
            string(TOLOWER "${_uname_os_output}" _os)
        endif()
    endif()

    if(_arch STREQUAL "")
        execute_process(
            COMMAND uname -m
            RESULT_VARIABLE _uname_arch_result
            OUTPUT_VARIABLE _uname_arch_output
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        if(_uname_arch_result EQUAL 0)
            string(TOLOWER "${_uname_arch_output}" _arch)
        endif()
    endif()

    if(_arch STREQUAL "amd64")
        set(_arch "x86_64")
    elseif(_arch STREQUAL "arm64")
        set(_arch "aarch64")
    endif()

    set(${out_os} "${_os}" PARENT_SCOPE)
    set(${out_arch} "${_arch}" PARENT_SCOPE)
endfunction()

function(app_download_file_if_missing url output_path)
    if(EXISTS "${output_path}")
        return()
    endif()

    message(STATUS "Downloading ${url}")
    file(DOWNLOAD "${url}" "${output_path}" STATUS _status TLS_VERIFY ON)
    list(GET _status 0 _status_code)
    list(GET _status 1 _status_message)

    if(NOT _status_code EQUAL 0)
        message(FATAL_ERROR "Download failed for ${url}: ${_status_message}")
    endif()

    message(STATUS "Downloaded ${output_path}")
endfunction()

function(app_extract_archive_if_missing archive_path destination_dir expected_path)
    if(EXISTS "${expected_path}")
        return()
    endif()

    if(NOT EXISTS "${archive_path}")
        message(FATAL_ERROR "Archive not found: ${archive_path}")
    endif()

    message(STATUS "Extracting ${archive_path}")
    file(ARCHIVE_EXTRACT INPUT "${archive_path}" DESTINATION "${destination_dir}")

    if(NOT EXISTS "${expected_path}")
        message(FATAL_ERROR
            "Archive extraction completed but expected path is missing: ${expected_path}")
    endif()
endfunction()
