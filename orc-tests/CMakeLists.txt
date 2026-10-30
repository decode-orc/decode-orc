cmake_minimum_required(VERSION 3.20)

# Per-file clang-tidy (cmake/ClangTidy.cmake sets CMAKE_CXX_CLANG_TIDY at the
# top level) is a host-tree gate: it enforces orc/.clang-tidy, whose check list
# runs with WarningsAsErrors: '*' and which test sources do not satisfy. Leaving
# the hook enabled here did not analyse tests against that list -- clang-tidy
# found no configuration above orc-tests/ and fell back to its built-in default,
# the full clang-analyzer suite, at tens of seconds per translation unit with
# nothing enforcing the result. Clearing the variable for this directory (and,
# by inheritance, every test subdirectory) keeps the analysis where it is
# enforced. See orc-tests/.clang-tidy for the editor/run-clang-tidy side.
set(CMAKE_CXX_CLANG_TIDY "")

function(orc_fail_if_integration_tests_are_in_unit_tree unit_tree_path)
    file(GLOB_RECURSE _misplaced_integration_tests
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/${unit_tree_path}/*_integration_test.cpp")

    if(_misplaced_integration_tests)
        list(JOIN _misplaced_integration_tests "\n  - " _misplaced_list)
        message(FATAL_ERROR
            "Found integration-style tests in unit tree '${unit_tree_path}':\n"
            "  - ${_misplaced_list}\n"
            "Move these files under an integration test subtree.")
    endif()
endfunction()

orc_fail_if_integration_tests_are_in_unit_tree("orc-tests/core/unit")
orc_fail_if_integration_tests_are_in_unit_tree("orc-tests/gui/unit")

# The videosynth suite's measurement helpers: built for the suite and for
# their own unit tests, which run with the rest of the unit tree.
if(BUILD_UNIT_TESTS OR BUILD_VIDEOSYNTH_TESTS)
    add_subdirectory(videosynth/support)
endif()

if(BUILD_UNIT_TESTS)
    add_subdirectory(core/unit)
endif()

if(BUILD_INTEGRATION_TESTS)
    add_subdirectory(core/integration)
endif()

if(BUILD_FUNCTIONAL_TESTS)
    add_subdirectory(core/functional)
endif()

if(BUILD_VIDEOSYNTH_TESTS)
    add_subdirectory(videosynth)
endif()

if(BUILD_GUI_TESTS)
    add_subdirectory(gui/unit)
endif()

if(BUILD_INTEGRATION_TESTS AND BUILD_GUI_TESTS)
    add_subdirectory(gui/integration)
endif()