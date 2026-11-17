# orc-plugin-sdk: Public stable plugin SDK for decode-orc stage plugins.
#
# This INTERFACE library provides:
#   - The public SDK include directory only: <orc/plugin/...> (ABI/services)
#     and <orc/stage/...> (stage contract headers). Private host include
#     trees (orc/core, orc/common, orc/view-types) are deliberately NOT
#     propagated: a plugin TU that includes a private host header must fail
#     to compile, matching the allowlist enforced by
#     cmake/check_plugin_private_includes.sh.
#   - The orc-sdk-support static library so plugins can resolve the
#     support-tier algorithm helper symbols (eia608_decoder, dropout_util,
#     preview_helpers, colour_preview_conversion) without linking the host.
#   - spdlog/fmt usage requirements for the SDK logging surface
#     (<orc/support/logging.h>, <orc/plugin/orc_plugin_services_helpers.h>).
#
# In-tree usage (plugins built inside this repository):
#   target_link_libraries(my-plugin PRIVATE orc-plugin-sdk)
#
# Out-of-tree usage (third-party plugins):
#   find_package(decode-orc-plugin-sdk REQUIRED)
#   target_link_libraries(my-plugin PRIVATE orc::plugin-sdk)
#   orc_add_stage_plugin(...)   # helper macro from DecodeOrcPluginSDKHelpers.cmake

add_library(orc-plugin-sdk INTERFACE)

# Exported as orc::plugin-sdk from the installed package (the in-tree target
# name keeps the orc- prefix; EXPORT_NAME controls only the imported name).
set_target_properties(orc-plugin-sdk PROPERTIES EXPORT_NAME plugin-sdk)

target_include_directories(orc-plugin-sdk INTERFACE
    # SDK headers, tiered: <orc/abi/...> (frozen binary contract),
    # <orc/stage/...> (stage contract, grouped by domain), and
    # <orc/support/...> (compiled-into-plugin utilities).
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/decode-orc-plugin-sdk>
)

# Deprecated pre-tier include-path shims (flat <orc/plugin/...> and
# <orc/stage/...> paths that forward to the tiered layout) are retained for one
# release so existing third-party plugin source keeps building. Turning this
# OFF makes any plugin TU that still includes a pre-tier path fail to compile
# (the shim headers #error on ORC_SDK_NO_DEPRECATED_INCLUDE_SHIMS). Bundled
# plugins must build with it OFF — CI exercises that configuration. The define
# is a usage requirement of orc-plugin-sdk, so it reaches every plugin/test
# target that links the SDK but not the host (which does not link the SDK).
option(ORC_SDK_DEPRECATED_INCLUDE_SHIMS
    "Keep deprecated pre-tier SDK include-path shims for third-party source compatibility"
    ON)
if(NOT ORC_SDK_DEPRECATED_INCLUDE_SHIMS)
    target_compile_definitions(orc-plugin-sdk INTERFACE
        ORC_SDK_NO_DEPRECATED_INCLUDE_SHIMS)
endif()

# The SDK headers need these on Windows, where decode-orc's root CMakeLists
# sets them for every in-tree target: NOMINMAX keeps windows.h's min()/max()
# macros from breaking std::min/std::max and std::numeric_limits<T>::max(),
# and _USE_MATH_DEFINES exposes M_PI. As usage requirements they also reach a
# plugin built against the installed package (find_package), which never sees
# the root CMakeLists.
if(WIN32)
    target_compile_definitions(orc-plugin-sdk INTERFACE NOMINMAX _USE_MATH_DEFINES)
endif()

# orc-sdk-support: the support-tier algorithm helpers (eia608_decoder,
# dropout_util, preview_helpers, colour_preview_conversion) built as a
# position-independent STATIC library. These were previously compiled into the
# orc-core shared library, which forced plugins to link the whole host to
# resolve them; shipping them as a small static lib severs that tether. Each
# plugin gets its own copy of the objects (RTLD_LOCAL loading already isolates
# per-plugin symbols), and orc-core links the same lib so host behaviour is
# unchanged. Not part of the binary ABI: changing these sources never requires
# an ABI bump, only a plugin recompile.
set(ORC_SDK_SUPPORT_SOURCES
    src/eia608_decoder.cpp
    src/eia608_line21_pairs.cpp
    src/eia608_service_demux.cpp
    src/dropout_util.cpp
    src/preview_helpers.cpp
    src/colour_preview_conversion.cpp
    src/vbi_utilities.cpp
    src/stack_measurement.cpp
    src/fm_beat.cpp
    src/channel_equalise.cpp
    src/frame_content.cpp
    src/scratch_file.cpp
    src/read_only_file.cpp
    # The observation store and the observers stage tools sweep a disc with
    # (the Frame Map's Disc Mapper, Source Align); orc-core's background
    # observation pipeline runs the same copies.
    src/observation_context.cpp
    src/observation_pass.cpp
    src/observers/biphase_observer.cpp
    src/observers/black_psnr_observer.cpp
    src/observers/burst_level_observer.cpp
    src/observers/colour_frame_phase_observer.cpp
    src/observers/white_snr_observer.cpp
    # The support-tier logging surface (<orc/support/logging.h>): plugins and
    # host-free test binaries reach get_logger()/init_logging() through the SDK,
    # so the implementation must live here rather than in the host.
    src/logging.cpp
)
add_library(orc-sdk-support STATIC ${ORC_SDK_SUPPORT_SOURCES})
set_target_properties(orc-sdk-support PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Compile-time spdlog verbosity for the support sources' own ORC_LOG_* sites,
# matching orc-core: Release family filters to DEBUG; Debug/unset reaches TRACE.
target_compile_definitions(orc-sdk-support PRIVATE
    $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG>
    $<$<NOT:$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
)
target_include_directories(orc-sdk-support PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/decode-orc-plugin-sdk>
)
# The support sources use only the SDK header surface plus the spdlog/fmt
# logging shim (<orc/support/logging.h>). These are the sole install-time
# dependencies the exported SDK package must resolve.
target_link_libraries(orc-sdk-support PUBLIC spdlog::spdlog fmt::fmt)

# Plugins resolve the support-tier helper symbols from orc-sdk-support (a static
# archive), NOT from the host. This replaces the former $<LINK_ONLY:orc-core>
# tether: plugins now compile and link against the SDK alone.
#
# The installed package exports the standalone build of the library instead
# (orc-sdk-support-standalone below): a third-party plugin gets its own private
# spdlog and fmt rather than linking the host's.
target_link_libraries(orc-plugin-sdk INTERFACE
    $<BUILD_INTERFACE:orc-sdk-support>
    $<INSTALL_INTERFACE:orc::orc-sdk-support>)

# The SDK headers themselves use spdlog (<orc/stage/logging.h>) and fmt
# (<orc/plugin/orc_plugin_services_helpers.h>), so their usage requirements
# are part of the SDK contract.
# In the build tree these are the host's shared spdlog and fmt (bundled plugins
# share the host's logger); the installed package's orc::orc-sdk-support brings
# the header-only private copies instead.
target_link_libraries(orc-plugin-sdk INTERFACE
    $<BUILD_INTERFACE:spdlog::spdlog>
    $<BUILD_INTERFACE:fmt::fmt>)

# orc-sdk-support-standalone: the same sources, built for third-party plugins
# with spdlog and fmt header-only (orc_sdk_define_private_logging_target() in
# DecodeOrcPluginSDKHelpers.cmake). A plugin linking it carries no dependency
# on any spdlog or fmt shared library, so it loads whatever versions the host
# was built with (issue #391), and its logging reaches the host through
# OrcPluginServices::log. Installed and exported as orc::orc-sdk-support; never
# linked by anything in this build.
add_library(orc-sdk-support-standalone STATIC ${ORC_SDK_SUPPORT_SOURCES})
set_target_properties(orc-sdk-support-standalone PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME orc-sdk-support
    EXPORT_NAME orc-sdk-support
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/standalone")
target_compile_definitions(orc-sdk-support-standalone PRIVATE
    ORC_SDK_PRIVATE_LOGGING
    $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG>
    $<$<NOT:$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
)
target_include_directories(orc-sdk-support-standalone PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/decode-orc-plugin-sdk>
)
# Compiled with header-only spdlog and fmt: the include paths and definitions
# of their header-only targets, but none of their link interfaces (a spdlog
# built against an external fmt links the shared fmt::fmt from its header-only
# target too). orc_sdk_define_private_logging_target() makes the same choice for
# a plugin; it cannot simply be linked here, because a static library's link
# dependencies must all be exportable.
target_include_directories(orc-sdk-support-standalone SYSTEM PRIVATE
    $<TARGET_PROPERTY:spdlog::spdlog_header_only,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:fmt::fmt-header-only,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(orc-sdk-support-standalone PRIVATE
    $<TARGET_PROPERTY:spdlog::spdlog_header_only,INTERFACE_COMPILE_DEFINITIONS>
    $<TARGET_PROPERTY:fmt::fmt-header-only,INTERFACE_COMPILE_DEFINITIONS>)
# The installed package config recreates orc::sdk-private-logging from the
# consumer's own spdlog and fmt before it loads the exported targets.
set_property(TARGET orc-sdk-support-standalone APPEND PROPERTY
    INTERFACE_LINK_LIBRARIES "$<INSTALL_INTERFACE:orc::sdk-private-logging>")
# The same sources orc-sdk-support already puts through clang-tidy.
set_target_properties(orc-sdk-support-standalone PROPERTIES CXX_CLANG_TIDY "")

# =============================================================================
# Install rules
# =============================================================================

# SDK public headers
install(DIRECTORY include/
    DESTINATION include/decode-orc-plugin-sdk
    FILES_MATCHING PATTERN "*.h"
)

# Exported targets for out-of-tree plugin builds. The export set collects
# orc-plugin-sdk (this file) and orc-sdk-support-standalone (the support-tier
# static lib, built with private spdlog/fmt, exported as orc-sdk-support);
# the generated Targets file defines orc::plugin-sdk plus the
# orc::orc-sdk-support link target it references. The host libraries (orc-core,
# orc-common) are deliberately NOT exported: plugins link the SDK alone.
install(TARGETS orc-plugin-sdk orc-sdk-support-standalone
    EXPORT decode-orc-plugin-sdkTargets
    ARCHIVE DESTINATION lib COMPONENT Runtime
)
install(EXPORT decode-orc-plugin-sdkTargets
    FILE decode-orc-plugin-sdkTargets.cmake
    NAMESPACE orc::
    DESTINATION lib/cmake/decode-orc-plugin-sdk
)

# CMake package config files (see cmake/decode-orc-plugin-sdk/). The Config
# file is installed under the exact name find_package(decode-orc-plugin-sdk)
# looks for on case-sensitive filesystems: <package-name>Config.cmake.
install(FILES
    ${CMAKE_SOURCE_DIR}/cmake/decode-orc-plugin-sdk/DecodeOrcPluginSDKConfig.cmake
    DESTINATION lib/cmake/decode-orc-plugin-sdk
    RENAME decode-orc-plugin-sdkConfig.cmake
)
install(FILES
    ${CMAKE_SOURCE_DIR}/cmake/decode-orc-plugin-sdk/DecodeOrcPluginSDKHelpers.cmake
    DESTINATION lib/cmake/decode-orc-plugin-sdk
)

# SDK enforcement gate scripts — installed alongside the CMake package so that
# third-party plugin authors can run the same private-include and private-link
# scans locally without needing access to the host source tree.
install(FILES
    ${CMAKE_SOURCE_DIR}/cmake/check_plugin_private_includes.sh
    ${CMAKE_SOURCE_DIR}/cmake/check_plugin_private_links.sh
    DESTINATION lib/cmake/decode-orc-plugin-sdk
    PERMISSIONS OWNER_READ OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)

# The generated SDK header allowlist consumed by check_plugin_private_includes.sh.
# Installed next to the script so the standalone (out-of-tree) scan resolves it
# via its own directory without a decode-orc checkout.
install(FILES
    ${CMAKE_SOURCE_DIR}/cmake/sdk_header_allowlist.txt
    DESTINATION lib/cmake/decode-orc-plugin-sdk
)
