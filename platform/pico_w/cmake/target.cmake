include_guard(GLOBAL)

function(pico_w_configure_target target_name)
    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR "pico_w_configure_target called with missing target '${target_name}'")
    endif()

    target_compile_definitions(${target_name}
        PRIVATE
            APP_PLATFORM_PICO_W=1
    )
    set_target_properties(${target_name} PROPERTIES SUFFIX ".elf")

    if(APP_PLATFORM_ENABLE_TELEMETRY)
        target_compile_definitions(${target_name} PRIVATE APP_HAS_TELEMETRY=1)
    endif()

    if(APP_PLATFORM_DEBUG_WIPE_ALL_ON_BOOT)
        target_compile_definitions(${target_name} PRIVATE APP_DEBUG_WIPE_ALL_ON_BOOT=1)
    endif()

    target_link_libraries(${target_name}
        PRIVATE
            hidrelay_core
            pico_stdlib
            pico_cyw43_arch_none
            pico_usb_reset_interface_headers
    )

    if(APP_PLATFORM_ENABLE_TINYUSB)
        target_link_libraries(${target_name} PRIVATE tinyusb_device tinyusb_board)
        target_compile_definitions(${target_name} PRIVATE APP_HAS_TINYUSB=1)

        if(APP_PLATFORM_ENABLE_DIAG_CDC)
            target_compile_definitions(${target_name} PRIVATE APP_HAS_DIAG_CDC=1)
        endif()
    elseif(APP_PLATFORM_ENABLE_DIAG_CDC)
        message(WARNING "APP_PLATFORM_ENABLE_DIAG_CDC is ON but APP_PLATFORM_ENABLE_TINYUSB is OFF; diagnostics CDC is disabled.")
    endif()

    if(APP_PLATFORM_ENABLE_BTSTACK)
        target_link_libraries(${target_name} PRIVATE pico_btstack_classic pico_btstack_ble pico_btstack_cyw43)
        target_compile_definitions(${target_name} PRIVATE APP_HAS_BTSTACK=1)
    endif()

    pico_enable_stdio_uart(${target_name} 1)
    pico_enable_stdio_usb(${target_name} 0)

    pico_add_extra_outputs(${target_name})
endfunction()
