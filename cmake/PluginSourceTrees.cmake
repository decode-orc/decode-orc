# PluginSourceTrees.cmake
#
# Builds plugin source trees from outside this repository as part of this
# build, for development and testing only. Plugins are not shipped with
# Decode-Orc: users install them with the Plugin Manager. This exists so that a
# change to the SDK can be checked against a plugin, and so that core's tests
# that use a plugin's code (the VBI source tests read their output back with
# the teletext plugin's decoders) can run.
#
#   -DORC_PLUGIN_SOURCE_DIRS=/path/to/orc-plugin_teletext[;/path/to/another]
#
# Each tree builds against this tree's SDK targets into the build's plugin
# directory (build/lib/orc-stage-plugins/), so orc-cli and orc-gui from the
# same build load it. It is an ordinary (non-core) plugin, exactly as an
# installed copy would be. Packaging never sets this.
#
# Included from orc/plugins/CMakeLists.txt, so the plugin directory variables
# (ORC_STAGE_PLUGIN_BUILD_DIR, ORC_STAGE_PLUGIN_INSTALL_DIR) and the
# orc-plugin-sdk target are already defined.

include_guard(GLOBAL)

# The teletext plugin's captures for its functional tests, laid out like
# test-data/. A cache entry, so the plugin's own default (its source tree)
# does not win.
set(ORC_TELETEXT_TEST_DATA_DIR "${CMAKE_SOURCE_DIR}/test-data" CACHE PATH
    "Directory holding the reference VBI captures for the teletext plugin's functional tests")

# Adds one plugin source tree. A function, so the variables set for the
# plugin stay in this scope and do not leak into decode-orc's own build.
function(orc_add_plugin_source_tree dir)
    get_filename_component(_dir "${dir}" ABSOLUTE)
    get_filename_component(_name "${_dir}" NAME)
    if(NOT EXISTS "${_dir}/CMakeLists.txt")
        message(FATAL_ERROR
            "ORC_PLUGIN_SOURCE_DIRS: ${_dir} is not a plugin source tree "
            "(no CMakeLists.txt)")
    endif()

    # decode-orc has its own BUILD_TESTS cache option (ON by default), which
    # the plugin would otherwise pick up. Its tests follow BUILD_UNIT_TESTS.
    set(BUILD_TESTS ${BUILD_UNIT_TESTS})
    # decode-orc's per-file clang-tidy (cmake/ClangTidy.cmake) is configured
    # for orc/ and orc-tests/; a plugin is analysed in its own repository.
    set(CMAKE_CXX_CLANG_TIDY "")

    add_subdirectory("${_dir}" "${CMAKE_BINARY_DIR}/plugin-source-trees/${_name}")

    # The SDK boundary gates, run over the plugin's tree the way its own CI
    # runs them: standalone mode, the whole tree as one plugin.
    add_test(
        NAME PluginPrivateIncludeScan.${_name}
        COMMAND ${BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/cmake/check_plugin_private_includes.sh "${_dir}"
    )
    add_test(
        NAME PluginPrivateLinkScan.${_name}
        COMMAND ${BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/cmake/check_plugin_private_links.sh "${_dir}"
    )
    set_tests_properties(PluginPrivateIncludeScan.${_name} PluginPrivateLinkScan.${_name}
        PROPERTIES LABELS "sdk")
endfunction()

foreach(_orc_plugin_source_dir IN LISTS ORC_PLUGIN_SOURCE_DIRS)
    orc_add_plugin_source_tree("${_orc_plugin_source_dir}")
endforeach()
