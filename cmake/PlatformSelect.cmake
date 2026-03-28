include_guard(GLOBAL)

function(app_list_platform source_dir out_platform_list)
    file(GLOB _platform_dir_candidates LIST_DIRECTORIES true "${source_dir}/platform/*")
    set(_platforms "")

    foreach(_candidate IN LISTS _platform_dir_candidates)
        if(IS_DIRECTORY "${_candidate}" AND EXISTS "${_candidate}/CMakeLists.txt")
            get_filename_component(_name "${_candidate}" NAME)
            list(APPEND _platforms "${_name}")
        endif()
    endforeach()

    list(SORT _platforms)
    set(${out_platform_list} "${_platforms}" PARENT_SCOPE)
endfunction()

function(app_resolve_platform source_dir requested_platform out_platform)
    app_list_platform("${source_dir}" _platforms)

    if(_platforms STREQUAL "")
        message(FATAL_ERROR
            "No platform targets found under ${source_dir}/platform. "
            "Add at least one platform/<name>/CMakeLists.txt")
    endif()

    set(_resolved "${requested_platform}")

    if((_resolved STREQUAL "") OR (_resolved STREQUAL "auto"))
        list(GET _platforms 0 _resolved)
    endif()

    list(FIND _platforms "${_resolved}" _index)
    if(_index EQUAL -1)
        list(JOIN _platforms ", " _supported_csv)
        message(FATAL_ERROR
            "Unsupported APP_PLATFORM='${requested_platform}'. "
            "Available targets: ${_supported_csv}")
    endif()

    set(${out_platform} "${_resolved}" PARENT_SCOPE)
endfunction()

function(app_get_platform_dir source_dir platform out_dir)
    set(_platform_dir "${source_dir}/platform/${platform}")

    if(NOT EXISTS "${_platform_dir}/CMakeLists.txt")
        message(FATAL_ERROR "Platform '${platform}' is missing CMakeLists.txt at ${_platform_dir}")
    endif()

    set(${out_dir} "${_platform_dir}" PARENT_SCOPE)
endfunction()
