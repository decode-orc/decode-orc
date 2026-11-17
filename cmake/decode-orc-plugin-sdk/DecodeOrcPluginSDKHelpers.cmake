# DecodeOrcPluginSDKHelpers.cmake
#
# Helper macros for building decode-orc stage plugins, both inside this
# repository and in external (third-party) plugin projects.
#
# Provides:
#   orc_add_stage_plugin()   — create a properly configured MODULE target
#   orc_sdk_enable_private_logging()
#                            — build third-party plugins with their own private
#                              spdlog/fmt (see below)
#   orc_sdk_hide_private_logging_symbols(<target>)
#                            — keep a binary's spdlog/fmt symbols to itself

include_guard(GLOBAL)
include(CMakeParseArguments)

# Platform-specific plugin installation directory.
# These paths are used both by the in-tree orc_add_stage_plugin() (via
# StagePlugins.cmake) and by the installed helper for external plugin authors.
if(NOT DEFINED ORC_STAGE_PLUGIN_INSTALL_DIR)
    if(APPLE)
        set(ORC_STAGE_PLUGIN_INSTALL_DIR "orc-gui.app/Contents/PlugIns/orc-stage-plugins")
    elseif(WIN32)
        set(ORC_STAGE_PLUGIN_INSTALL_DIR "bin/orc-stage-plugins")
    else()
        set(ORC_STAGE_PLUGIN_INSTALL_DIR "lib/orc-stage-plugins")
    endif()
endif()

# ---------------------------------------------------------------------------
# Private logging for third-party plugins
#
# A plugin released on its own must not depend on the host's spdlog and fmt
# shared libraries. The host bundles whatever versions its platform's package
# manager had when it was built (Homebrew on macOS, the Flatpak runtime's on
# Linux, vcpkg's on Windows), so a plugin linked against another build's copy
# fails to load, or loads against a different version, whenever the two drift.
# On macOS a plugin built in a Nix shell even names its spdlog by a /nix/store
# path that exists on no user's machine (issue #391).
#
# orc_sdk_enable_private_logging() switches a build to private logging:
#   - spdlog and fmt are compiled into the plugin header-only, so it links no
#     spdlog or fmt library at all;
#   - orc_add_stage_plugin() (and orc_sdk_hide_private_logging_symbols() for a
#     view binary) keeps every spdlog/fmt symbol out of the binary's exported
#     symbol table, so the plugin's copy never binds to the host's, nor the
#     host's to the plugin's;
#   - <orc/support/logging.h>'s get_logger() then finds no shared "core" logger
#     in the plugin's private registry and forwards every record to the host's
#     OrcPluginServices::log callback instead.
#
# Only the spdlog/fmt symbols are hidden, not the whole plugin: the host
# dynamic_casts the stage objects a plugin hands it, so the SDK's types must
# keep their default visibility.
#
# decode-orc's own build never enables this: bundled plugins share the host's
# spdlog. The installed SDK package and a plugin repository's in-tree SDK build
# (DecodeOrcInTreeSDK.cmake) enable it.
#
# Defines the INTERFACE target orc-sdk-private-logging (alias
# orc::sdk-private-logging) carrying the header-only spdlog/fmt usage
# requirements. Needs spdlog and fmt found first (find_package(... CONFIG)).
# ---------------------------------------------------------------------------
function(orc_sdk_define_private_logging_target)
    if(TARGET orc-sdk-private-logging)
        return()
    endif()
    if(NOT TARGET spdlog::spdlog_header_only OR NOT TARGET fmt::fmt-header-only)
        message(FATAL_ERROR
            "Private plugin logging needs the spdlog::spdlog_header_only and "
            "fmt::fmt-header-only targets: find_package(spdlog CONFIG) and "
            "find_package(fmt CONFIG) first.")
    endif()
    find_package(Threads REQUIRED)

    add_library(orc-sdk-private-logging INTERFACE)
    add_library(orc::sdk-private-logging ALIAS orc-sdk-private-logging)

    # spdlog::spdlog_header_only's include path, definitions
    # (SPDLOG_FMT_EXTERNAL and so on) and compile options (/utf-8 under
    # MSVC), but not its link interface: a spdlog
    # built against an external fmt links the shared fmt::fmt there, and the
    # point is to link no fmt library.
    get_target_property(_orc_spdlog_includes spdlog::spdlog_header_only
        INTERFACE_INCLUDE_DIRECTORIES)
    get_target_property(_orc_spdlog_definitions spdlog::spdlog_header_only
        INTERFACE_COMPILE_DEFINITIONS)
    if(_orc_spdlog_includes)
        target_include_directories(orc-sdk-private-logging SYSTEM INTERFACE
            ${_orc_spdlog_includes})
    endif()
    if(_orc_spdlog_definitions)
        target_compile_definitions(orc-sdk-private-logging INTERFACE
            ${_orc_spdlog_definitions})
    endif()
    get_target_property(_orc_spdlog_options spdlog::spdlog_header_only
        INTERFACE_COMPILE_OPTIONS)
    if(_orc_spdlog_options)
        target_compile_options(orc-sdk-private-logging INTERFACE
            ${_orc_spdlog_options})
    endif()
    target_link_libraries(orc-sdk-private-logging INTERFACE
        fmt::fmt-header-only Threads::Threads)
    # Turns on get_logger()'s forwarding to the host in the orc-sdk-support
    # sources compiled against this target.
    target_compile_definitions(orc-sdk-private-logging INTERFACE
        ORC_SDK_PRIVATE_LOGGING)
endfunction()

function(orc_sdk_enable_private_logging)
    orc_sdk_define_private_logging_target()
    set_property(GLOBAL PROPERTY ORC_SDK_PRIVATE_LOGGING ON)
endfunction()

# Keep a binary's spdlog and fmt symbols out of its exported symbol table, and
# its calls into its own code bound to that code. A no-op unless
# orc_sdk_enable_private_logging() has been called, and on Windows, where a DLL
# exports only what it marks for export and binds its own calls.
function(orc_sdk_hide_private_logging_symbols target)
    get_property(_orc_private GLOBAL PROPERTY ORC_SDK_PRIVATE_LOGGING)
    if(NOT _orc_private)
        return()
    endif()
    set(_orc_dir "${CMAKE_BINARY_DIR}/orc-sdk-private-logging")
    if(APPLE)
        # ld64 matches these against mangled names. Each spdlog or fmt name
        # carries its length-prefixed namespace: N6spdlog..., N3fmt3v12...;
        # a std:: template instantiated on one of their types carries it too.
        set(_orc_list "${_orc_dir}/unexported_symbols.txt")
        file(CONFIGURE OUTPUT "${_orc_list}" CONTENT "*6spdlog*\n*3fmt*\n")
        target_link_options(${target} PRIVATE
            "LINKER:-unexported_symbols_list,${_orc_list}")
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${_orc_list}")
    elseif(UNIX)
        # Demangled patterns: hidden symbols bind inside the plugin, so the
        # host's libspdlog/libfmt (already in the process's global scope) can
        # never interpose on the plugin's copy.
        set(_orc_script "${_orc_dir}/private_logging.map")
        file(CONFIGURE OUTPUT "${_orc_script}" CONTENT
            "{\n  local:\n    extern \"C++\" {\n      *spdlog::*;\n      *fmt::*;\n    };\n};\n")
        # -Bsymbolic-functions: the host's liborc-core carries its own copy of
        # the orc-sdk-support code, and a function the host exports would
        # otherwise take the place of the plugin's copy of the same name: the
        # plugin's get_logger() would be the host's, handing back a logger made
        # by the host's spdlog version. Data and typeinfo are left as they are,
        # so the host's dynamic_casts on plugin objects are unaffected. On
        # macOS a binary's calls to its own non-weak functions are bound when it
        # is linked.
        target_link_options(${target} PRIVATE
            "LINKER:--version-script=${_orc_script}"
            "LINKER:-Bsymbolic-functions")
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${_orc_script}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# orc_add_stage_plugin(<target>
#     OUTPUT_NAME    <name>           # Plugin shared library output name
#     PLUGIN_VERSION <version>        # Semantic version string ("1.0.0")
#     SOURCES        <file> ...       # Source files for this plugin
#     [LINK_LIBRARIES <lib> ...]      # Additional link dependencies
#     [RUNTIME_DEPENDENCIES <path>...])
# ---------------------------------------------------------------------------
#
# Creates a SHARED plugin library target configured for decode-orc stage
# plugin conventions (SHARED rather than MODULE so in-tree test executables
# can link stage classes directly; the host loads plugins via dlopen):
#
#   - Links to orc::plugin-sdk (or orc-plugin-sdk for in-tree builds) which
#     provides the SDK include paths (<orc/abi/...>, <orc/stage/...>,
#     <orc/support/...>), spdlog/fmt, and the orc::orc-sdk-support static
#     library (support-tier helpers). The host is not linked; declare any
#     other third-party dependency the plugin uses via LINK_LIBRARIES.
#   - Sets ORC_STAGE_PLUGIN_VERSION compile definition.
#   - Places the output in the standard plugin directory.
#   - Installs to the platform-appropriate plugin location.
#   - Registers the target as a dependency of the orc-stage-plugins meta-target.
#
# USAGE (external plugin project, after find_package(decode-orc-plugin-sdk)):
#
#   orc_add_stage_plugin(
#       my-orc-stage-my-filter
#       OUTPUT_NAME    my-orc-stage-my-filter
#       PLUGIN_VERSION "1.2.3"
#       SOURCES        plugin.cpp my_filter_stage.cpp
#   )
#
function(orc_add_stage_plugin target)
    set(options)
    set(oneValueArgs OUTPUT_NAME PLUGIN_VERSION)
    set(multiValueArgs SOURCES LINK_LIBRARIES RUNTIME_DEPENDENCIES)
    cmake_parse_arguments(ORCSP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ORCSP_SOURCES)
        message(FATAL_ERROR "orc_add_stage_plugin(${target}) requires SOURCES")
    endif()

    # SHARED (not MODULE) is deliberate: plugins are dlopen'd by the host at
    # runtime, but the in-tree unit/functional test executables also link
    # stage plugin targets directly to exercise stage classes in-process
    # (see orc-tests/core/unit/CMakeLists.txt). MODULE targets cannot be
    # linked, so SHARED is required for that dual use.
    add_library(${target} SHARED ${ORCSP_SOURCES})

    if(ORCSP_OUTPUT_NAME)
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "${ORCSP_OUTPUT_NAME}")
    endif()

    # Prefer orc::plugin-sdk (installed package target namespace) if available;
    # fall back to orc-plugin-sdk (in-tree target name).
    if(TARGET orc::plugin-sdk)
        set(_orc_sdk_target orc::plugin-sdk)
    elseif(TARGET orc-plugin-sdk)
        set(_orc_sdk_target orc-plugin-sdk)
    else()
        message(FATAL_ERROR
            "orc_add_stage_plugin: Neither orc::plugin-sdk nor orc-plugin-sdk is defined. "
            "Did you call find_package(decode-orc-plugin-sdk REQUIRED) or "
            "add_subdirectory(orc/sdk)?")
    endif()

    target_link_libraries(${target} PRIVATE ${_orc_sdk_target} ${ORCSP_LINK_LIBRARIES})

    orc_sdk_hide_private_logging_symbols(${target})

    # ORC_STAGE_INSTRUCTIONS_MD uses dladdr() on Linux to locate the plugin .so.
    if(UNIX AND NOT APPLE)
        target_link_libraries(${target} PRIVATE dl)
    endif()

    # Always expose the plugin directory itself for local stage headers.
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    if(ORCSP_PLUGIN_VERSION)
        target_compile_definitions(${target} PRIVATE
            ORC_STAGE_PLUGIN_VERSION="${ORCSP_PLUGIN_VERSION}")
    endif()

    # Compile-time log level, matching the host (orc/core, orc/gui, orc/sdk).
    # spdlog defaults SPDLOG_ACTIVE_LEVEL to info, which strips every
    # ORC_LOG_DEBUG and ORC_LOG_TRACE from the plugin at compile time — the
    # statement is gone, so no runtime level can bring it back. Plugins carry
    # the diagnostics a difficult source is judged on, so they need the same
    # level the host is built with.
    target_compile_definitions(${target} PRIVATE
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG>
        $<$<NOT:$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
    )

    # Output directory during builds (so the host can discover plugins at runtime)
    if(NOT DEFINED ORC_STAGE_PLUGIN_BUILD_DIR)
        set(ORC_STAGE_PLUGIN_BUILD_DIR "${CMAKE_BINARY_DIR}/lib/orc-stage-plugins")
    endif()

    set_target_properties(${target} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY  "${ORC_STAGE_PLUGIN_BUILD_DIR}"
        RUNTIME_OUTPUT_DIRECTORY  "${ORC_STAGE_PLUGIN_BUILD_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY  "${CMAKE_BINARY_DIR}/lib"
    )

    # RPATH for bundled deployment
    if(APPLE)
        set_target_properties(${target} PROPERTIES
            INSTALL_RPATH "@loader_path/../../Frameworks")
    elseif(UNIX AND NOT WIN32)
        set_target_properties(${target} PROPERTIES
            INSTALL_RPATH "\$ORIGIN/..")
    endif()

    install(TARGETS ${target}
        LIBRARY DESTINATION "${ORC_STAGE_PLUGIN_INSTALL_DIR}" COMPONENT Runtime
        RUNTIME DESTINATION "${ORC_STAGE_PLUGIN_INSTALL_DIR}" COMPONENT Runtime
        BUNDLE  DESTINATION "${ORC_STAGE_PLUGIN_INSTALL_DIR}" COMPONENT Runtime
    )

    # If instructions.md exists, copy it alongside the plugin .so at build time
    # and install it so ORC_STAGE_INSTRUCTIONS_MD can find it at runtime.
    # Uses add_custom_command(OUTPUT/DEPENDS) so a change to instructions.md
    # triggers the copy even when the stage .so does not need to be rebuilt.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/instructions.md")
        if(ORCSP_OUTPUT_NAME)
            set(_orc_md_base "${ORCSP_OUTPUT_NAME}")
        else()
            set(_orc_md_base "${target}")
        endif()
        if(WIN32)
            set(_orc_md_filename "${_orc_md_base}.md")
        else()
            set(_orc_md_filename "lib${_orc_md_base}.md")
        endif()

        set(_orc_md_src "${CMAKE_CURRENT_SOURCE_DIR}/instructions.md")
        set(_orc_md_dst "${ORC_STAGE_PLUGIN_BUILD_DIR}/${_orc_md_filename}")

        add_custom_command(
            OUTPUT "${_orc_md_dst}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_orc_md_src}" "${_orc_md_dst}"
            DEPENDS "${_orc_md_src}"
            COMMENT "Copying instructions.md for ${target}"
            VERBATIM
        )
        add_custom_target(${target}-instructions ALL DEPENDS "${_orc_md_dst}")

        install(FILES "${_orc_md_src}"
            RENAME "${_orc_md_filename}"
            DESTINATION "${ORC_STAGE_PLUGIN_INSTALL_DIR}"
            COMPONENT Runtime
        )
    endif()

    if(ORCSP_RUNTIME_DEPENDENCIES)
        foreach(_dep IN LISTS ORCSP_RUNTIME_DEPENDENCIES)
            if(EXISTS "${_dep}")
                install(FILES "${_dep}"
                    DESTINATION "${ORC_STAGE_PLUGIN_INSTALL_DIR}"
                    COMPONENT Runtime
                )
            else()
                message(WARNING "Stage plugin runtime dependency not found: ${_dep}")
            endif()
        endforeach()
    endif()

    # Register this plugin with the orc-stage-plugins meta-target so that
    # building orc-gui or orc-cli builds all plugins automatically.
    if(NOT TARGET orc-stage-plugins)
        add_custom_target(orc-stage-plugins)
    endif()
    add_dependencies(orc-stage-plugins ${target})
endfunction()
