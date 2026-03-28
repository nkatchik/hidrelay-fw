include_guard(GLOBAL)

include(CMakeParseArguments)

function(app_require_git out_git_executable)
    find_program(_git_executable git)

    if(NOT _git_executable)
        message(FATAL_ERROR "git is required for dependency bootstrap")
    endif()

    set(${out_git_executable} "${_git_executable}" PARENT_SCOPE)
endfunction()

function(app_git_clone_if_missing)
    cmake_parse_arguments(ARG "" "REPO_URL;GIT_TAG;DEST_DIR;ALLOW_FETCH" "" ${ARGN})

    if(NOT ARG_REPO_URL)
        message(FATAL_ERROR "app_git_clone_if_missing requires REPO_URL")
    endif()

    if(NOT ARG_DEST_DIR)
        message(FATAL_ERROR "app_git_clone_if_missing requires DEST_DIR")
    endif()

    set(_allow_fetch OFF)
    if(ARG_ALLOW_FETCH)
        set(_allow_fetch ON)
    endif()

    if(EXISTS "${ARG_DEST_DIR}/.git")
        return()
    endif()

    if(EXISTS "${ARG_DEST_DIR}" AND NOT EXISTS "${ARG_DEST_DIR}/.git")
        message(FATAL_ERROR
            "Path exists but is not a git checkout: ${ARG_DEST_DIR}. "
            "Remove it and rerun bootstrap.")
    endif()

    if(NOT _allow_fetch)
        message(FATAL_ERROR "Missing dependency checkout at ${ARG_DEST_DIR}")
    endif()

    app_require_git(_git)
    get_filename_component(_parent_dir "${ARG_DEST_DIR}" DIRECTORY)
    file(MAKE_DIRECTORY "${_parent_dir}")

    if(ARG_GIT_TAG)
        message(STATUS "Cloning ${ARG_REPO_URL} (${ARG_GIT_TAG}) into ${ARG_DEST_DIR}")
        execute_process(
            COMMAND "${_git}" clone --depth 1 --branch "${ARG_GIT_TAG}" "${ARG_REPO_URL}" "${ARG_DEST_DIR}"
            RESULT_VARIABLE _clone_result
            OUTPUT_VARIABLE _clone_output
            ERROR_VARIABLE _clone_error
        )
    else()
        message(STATUS "Cloning ${ARG_REPO_URL} into ${ARG_DEST_DIR}")
        execute_process(
            COMMAND "${_git}" clone --depth 1 "${ARG_REPO_URL}" "${ARG_DEST_DIR}"
            RESULT_VARIABLE _clone_result
            OUTPUT_VARIABLE _clone_output
            ERROR_VARIABLE _clone_error
        )
    endif()

    if(NOT _clone_result EQUAL 0)
        message(FATAL_ERROR
            "git clone failed (exit ${_clone_result})\n"
            "stdout:\n${_clone_output}\n"
            "stderr:\n${_clone_error}")
    endif()
endfunction()

function(app_git_submodules_ensure_paths)
    cmake_parse_arguments(ARG "" "ROOT_DIR;ALLOW_FETCH" "CHECK_PATHS" ${ARGN})

    if(NOT ARG_ROOT_DIR)
        message(FATAL_ERROR "app_git_submodules_ensure_paths requires ROOT_DIR")
    endif()

    set(_missing_path "")
    foreach(_path IN LISTS ARG_CHECK_PATHS)
        if(NOT EXISTS "${_path}")
            set(_missing_path "${_path}")
            break()
        endif()
    endforeach()

    if(_missing_path STREQUAL "")
        return()
    endif()

    set(_allow_fetch OFF)
    if(ARG_ALLOW_FETCH)
        set(_allow_fetch ON)
    endif()

    if(NOT _allow_fetch)
        message(FATAL_ERROR "Dependency submodule path missing: ${_missing_path}")
    endif()

    app_require_git(_git)

    message(STATUS "Fetching git submodules under ${ARG_ROOT_DIR}")
    execute_process(
        COMMAND "${_git}" -C "${ARG_ROOT_DIR}" submodule update --init --recursive --depth 1
        RESULT_VARIABLE _submodule_result
        OUTPUT_VARIABLE _submodule_output
        ERROR_VARIABLE _submodule_error
    )

    if(NOT _submodule_result EQUAL 0)
        message(FATAL_ERROR
            "git submodule update failed (exit ${_submodule_result})\n"
            "stdout:\n${_submodule_output}\n"
            "stderr:\n${_submodule_error}")
    endif()

    foreach(_path IN LISTS ARG_CHECK_PATHS)
        if(NOT EXISTS "${_path}")
            message(FATAL_ERROR "Submodule bootstrap completed but path is still missing: ${_path}")
        endif()
    endforeach()
endfunction()
