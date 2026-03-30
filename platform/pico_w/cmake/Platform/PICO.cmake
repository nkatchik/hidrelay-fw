# Suppress CMake's "System is unknown" advisory for Pico SDK toolchains that
# intentionally set CMAKE_SYSTEM_NAME to "PICO".
#
# Pico is a bare-metal target, so Generic platform defaults are appropriate.
include(Platform/Generic)
