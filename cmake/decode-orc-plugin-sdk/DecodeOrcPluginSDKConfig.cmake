# DecodeOrcPluginSDKConfig.cmake
#
# CMake package configuration for the decode-orc Plugin SDK.
# Installed as lib/cmake/decode-orc-plugin-sdk/decode-orc-plugin-sdkConfig.cmake
# (the name find_package(decode-orc-plugin-sdk) resolves on case-sensitive
# filesystems); the repository copy keeps this descriptive filename.
#
# After find_package(decode-orc-plugin-sdk REQUIRED), the following are
# available:
#
#   orc::plugin-sdk   — INTERFACE imported target. Link your plugin against
#                       this target to get the SDK include directories
#                       (<orc/abi/...>, <orc/stage/...>, <orc/support/...>),
#                       spdlog/fmt, and the orc::orc-sdk-support static library
#                       (support-tier helper symbols). No host link required.
#
#   orc::view-support — SHARED imported target, defined when the package was
#                       built with orc-gui and Qt6 Widgets is found: the
#                       <orc/view-support/...> widgets for a view binary.
#                       At run time the view uses orc-gui's own copy.
#
#   orc_add_stage_plugin()  — CMake helper macro (from DecodeOrcPluginSDKHelpers.cmake)
#                             for creating properly configured plugin targets.
#
# Example (external plugin CMakeLists.txt):
#
#   cmake_minimum_required(VERSION 3.20)
#   project(my-orc-plugin CXX)
#
#   find_package(decode-orc-plugin-sdk REQUIRED)
#
#   orc_add_stage_plugin(
#       my-stage-plugin
#       OUTPUT_NAME  my-stage-plugin
#       PLUGIN_VERSION "1.0.0"
#       SOURCES plugin.cpp my_stage.cpp
#   )

cmake_minimum_required(VERSION 3.20)

include(CMakeFindDependencyMacro)

# Dependencies referenced by the exported targets' link interfaces:
#   orc::plugin-sdk      → orc::orc-sdk-support → spdlog, fmt
#   orc::orc-sdk-support → spdlog, fmt (SDK logging surface only)
# The host libraries (orc-core, orc-common) are no longer in the export set, so
# their heavy transitive dependencies (SQLite3, yaml-cpp, PNG, FFmpeg) are NOT
# required to configure a plugin against the installed SDK — spdlog and fmt are
# the only packages that must resolve before the Targets file is included.
find_dependency(spdlog REQUIRED)
find_dependency(fmt REQUIRED)

# Import orc::plugin-sdk and its link dependency orc::orc-sdk-support.
include("${CMAKE_CURRENT_LIST_DIR}/decode-orc-plugin-sdkTargets.cmake")

# orc::view-support links Qt, which a stage-only plugin need not have: import
# it only when the package carries it and Qt6 Widgets resolves.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/decode-orc-view-supportTargets.cmake")
    find_package(Qt6 QUIET COMPONENTS Core Gui Widgets)
    if(Qt6Widgets_FOUND)
        include("${CMAKE_CURRENT_LIST_DIR}/decode-orc-view-supportTargets.cmake")
    endif()
endif()

# Load the orc_add_stage_plugin() helper macro
include("${CMAKE_CURRENT_LIST_DIR}/DecodeOrcPluginSDKHelpers.cmake")
