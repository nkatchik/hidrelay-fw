include_guard(GLOBAL)

function(app_collect_platform_roots source_dir extra_platform_dirs out_platform_roots)
    set(_platform_roots "${source_dir}/platform")

    foreach(_extra_dir IN LISTS extra_platform_dirs)
        if(_extra_dir STREQUAL "")
            continue()
        endif()

        if(IS_ABSOLUTE "${_extra_dir}")
            set(_platform_root "${_extra_dir}")
        else()
            get_filename_component(_platform_root "${source_dir}/${_extra_dir}" ABSOLUTE)
        endif()

        list(APPEND _platform_roots "${_platform_root}")
    endforeach()

    list(REMOVE_DUPLICATES _platform_roots)
    set(${out_platform_roots} "${_platform_roots}" PARENT_SCOPE)
endfunction()

function(app_list_platform source_dir)
    if(ARGC EQUAL 2)
        set(_extra_platform_dirs "")
        set(_out_platform_list "${ARGV1}")
    elseif(ARGC EQUAL 3)
        set(_extra_platform_dirs "${ARGV1}")
        set(_out_platform_list "${ARGV2}")
    else()
        message(FATAL_ERROR "app_list_platform expects SOURCE_DIR [EXTRA_PLATFORM_DIRS] OUT_PLATFORM_LIST")
    endif()

    set(_platforms "")
    set(_platform_dirs "")

    app_collect_platform_roots("${source_dir}" "${_extra_platform_dirs}" _platform_roots)

    foreach(_platform_root IN LISTS _platform_roots)
        file(GLOB _platform_dir_candidates LIST_DIRECTORIES true "${_platform_root}/*")

        foreach(_candidate IN LISTS _platform_dir_candidates)
            if(IS_DIRECTORY "${_candidate}" AND EXISTS "${_candidate}/CMakeLists.txt")
                get_filename_component(_name "${_candidate}" NAME)
                list(FIND _platforms "${_name}" _existing_index)
                if(NOT _existing_index EQUAL -1)
                    list(GET _platform_dirs ${_existing_index} _existing_dir)
                    message(FATAL_ERROR
                        "Duplicate platform '${_name}' found in ${_existing_dir} and ${_candidate}")
                endif()

                list(APPEND _platforms "${_name}")
                list(APPEND _platform_dirs "${_candidate}")
            endif()
        endforeach()
    endforeach()

    list(SORT _platforms)
    set(${_out_platform_list} "${_platforms}" PARENT_SCOPE)
endfunction()

function(app_resolve_platform source_dir requested_platform)
    if(ARGC EQUAL 3)
        set(_extra_platform_dirs "")
        set(_out_platform "${ARGV2}")
    elseif(ARGC EQUAL 4)
        set(_extra_platform_dirs "${ARGV2}")
        set(_out_platform "${ARGV3}")
    else()
        message(FATAL_ERROR "app_resolve_platform expects SOURCE_DIR REQUESTED_PLATFORM [EXTRA_PLATFORM_DIRS] OUT_PLATFORM")
    endif()

    app_list_platform("${source_dir}" "${_extra_platform_dirs}" _platforms)

    if(_platforms STREQUAL "")
        message(FATAL_ERROR
            "No platform targets found under ${source_dir}/platform or APP_EXTRA_PLATFORM_DIRS. "
            "Add at least one platform/<name>/CMakeLists.txt")
    endif()

    set(_resolved "${requested_platform}")

    if((_resolved STREQUAL "") OR (_resolved STREQUAL "auto"))
        list(JOIN _platforms ", " _supported_csv)
        if(_resolved STREQUAL "auto")
            message(FATAL_ERROR
                "APP_PLATFORM='auto' is not supported. "
                "Set APP_PLATFORM to one of: ${_supported_csv}")
        else()
            message(FATAL_ERROR
                "APP_PLATFORM must be specified. "
                "Set APP_PLATFORM to one of: ${_supported_csv}")
        endif()
    endif()

    list(FIND _platforms "${_resolved}" _index)
    if(_index EQUAL -1)
        list(JOIN _platforms ", " _supported_csv)
        message(FATAL_ERROR
            "Unsupported APP_PLATFORM='${requested_platform}'. "
            "Available targets: ${_supported_csv}")
    endif()

    set(${_out_platform} "${_resolved}" PARENT_SCOPE)
endfunction()

function(app_get_platform_dir source_dir platform)
    if(ARGC EQUAL 3)
        set(_extra_platform_dirs "")
        set(_out_dir "${ARGV2}")
    elseif(ARGC EQUAL 4)
        set(_extra_platform_dirs "${ARGV2}")
        set(_out_dir "${ARGV3}")
    else()
        message(FATAL_ERROR "app_get_platform_dir expects SOURCE_DIR PLATFORM [EXTRA_PLATFORM_DIRS] OUT_DIR")
    endif()

    app_collect_platform_roots("${source_dir}" "${_extra_platform_dirs}" _platform_roots)

    set(_resolved_platform_dir "")
    foreach(_platform_root IN LISTS _platform_roots)
        set(_candidate "${_platform_root}/${platform}")
        if(EXISTS "${_candidate}/CMakeLists.txt")
            if(NOT _resolved_platform_dir STREQUAL "")
                message(FATAL_ERROR
                    "Duplicate platform '${platform}' found in ${_resolved_platform_dir} and ${_candidate}")
            endif()
            set(_resolved_platform_dir "${_candidate}")
        endif()
    endforeach()

    if(_resolved_platform_dir STREQUAL "")
        message(FATAL_ERROR
            "Platform '${platform}' is missing CMakeLists.txt under ${source_dir}/platform or APP_EXTRA_PLATFORM_DIRS")
    endif()

    set(${_out_dir} "${_resolved_platform_dir}" PARENT_SCOPE)
endfunction()
