# orc-common: Shared type definitions accessible to all layers
#
# This library contains shared types and utility functions that are
# used across core, public API, presenters, GUI, and CLI.
#
# One copy per process. orc-common's sources hold process-wide state (the app
# logger, the crash handler's configuration and saved exception/terminate
# handlers), so their code must exist exactly once in a running process. The
# sources are therefore compiled into orc-core (a shared library) and nowhere
# else:
#
#   orc-common-headers  INTERFACE  include directories + spdlog
#   orc-common-objects  OBJECT     the sources; linked into orc-core ONLY
#   orc-common          INTERFACE  what every other layer links: the headers
#                                  plus a link-only dependency on orc-core,
#                                  which provides the symbols
#
# A static orc-common linked into both orc-core and the executables gave each
# Windows executable its own copy alongside the one in orc-core.dll (issue
# #393): two app loggers, two crash-handler configurations. Linux and macOS
# hid the duplicate by resolving the executable's references from liborc-core.
#
# Do not link orc-common-objects from any target but orc-core. Link orc-common.

add_library(orc-common-headers INTERFACE)

# These directories are visible to all consumers
target_include_directories(orc-common-headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/orc/common>
    # Stage contract headers (<orc/stage/...>): shared vocabulary types
    # (common_types.h, node_type.h, parameter_types.h, ...) are re-homed in
    # the plugin SDK contract tree and consumed by every layer, so the SDK
    # include directory is published from here (headers only; no link
    # dependency on the SDK or core).
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/orc/sdk/include>
    $<INSTALL_INTERFACE:include/decode-orc-plugin-sdk>
)

# Find and link spdlog (which includes fmt)
# Note: spdlog bundles fmt, so we get both libraries
find_package(spdlog REQUIRED)
target_link_libraries(orc-common-headers INTERFACE spdlog::spdlog)

add_library(orc-common-objects OBJECT
    parameter_util.cpp
    node_id.cpp
    logging.cpp
    crash_handler.cpp
)

# The objects end up in a shared library.
set_target_properties(orc-common-objects PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_link_libraries(orc-common-objects PUBLIC orc-common-headers)

if(WIN32)
    target_link_libraries(orc-common-objects PRIVATE dbghelp)
endif()

# orc-common: the target every layer outside orc-core links. LINK_ONLY keeps
# orc-core's usage requirements (its private include directories) away from
# the GUI and CLI, which reach core only through the presenters.
add_library(orc-common INTERFACE)
target_link_libraries(orc-common INTERFACE
    orc-common-headers
    $<LINK_ONLY:orc-core>
)

# Install headers
install(DIRECTORY include/
    DESTINATION include/orc/common
    FILES_MATCHING PATTERN "*.h"
)

# Nothing else to install: orc-common's code ships inside orc-core. orc-common
# is not part of the decode-orc-plugin-sdk export set either: plugins link the
# SDK alone (orc::plugin-sdk + orc::orc-sdk-support), not the host.
