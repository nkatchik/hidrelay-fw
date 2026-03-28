include_guard(GLOBAL)

option(APP_WARNINGS_AS_ERRORS "Treat warnings as errors for project-owned targets." OFF)

function(app_target_enable_project_warnings target_name)
    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR "app_target_enable_project_warnings called with non-existent target '${target_name}'")
    endif()

    target_compile_features(${target_name} PRIVATE c_std_17)

    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )

        if(APP_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE -Werror)
        endif()
    endif()
endfunction()
