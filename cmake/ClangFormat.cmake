# ClangFormat.cmake - clang-format integration
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Provides four layers of enforcement:
#
#   1. Per-file stamp files (ALL) -- every .cpp/.h/.hpp file under orc/ and
#      orc-tests/ gets a stamp file under build/clang-format-stamps/.  The
#      stamp is regenerated only when its file changes, so each build checks
#      exactly the files that were edited (--dry-run --Werror) and fails on a
#      violation.  CMake has no compile-time clang-format hook (unlike
#      <LANG>_CLANG_TIDY), so sources are stamped the same way as headers.
#      The file lists are globbed with CONFIGURE_DEPENDS, so a new file is
#      picked up by the next build without a manual reconfigure.
#
#   2. clang-format-check target -- checks every file in one run;
#      clang-format target -- reformats every file in-place.
#
#   3. ClangFormatCheck test (label "format") -- the same full check as a
#      CTest, which CI's format gate runs.
#
#   4. The .githooks/pre-commit hook, which checks staged C++ files.
#
# clang-format is optional; if not found everything is silently skipped so
# that sandboxed (Nix derivation) builds are not affected.

find_program(CLANG_FORMAT_EXECUTABLE
    NAMES
        clang-format
        clang-format-21
        clang-format-20
        clang-format-19
        clang-format-18
    DOC "Path to clang-format executable"
)

if(NOT CLANG_FORMAT_EXECUTABLE)
    message(STATUS "clang-format: not found -- format checks disabled")
    return()
endif()

message(STATUS "clang-format: ${CLANG_FORMAT_EXECUTABLE}")

# ---------------------------------------------------------------------------
# 1. Incremental per-file checks wired into the default build (ALL)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE _clang_format_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/orc/*.cpp"
    "${CMAKE_SOURCE_DIR}/orc/*.h"
    "${CMAKE_SOURCE_DIR}/orc/*.hpp"
    "${CMAKE_SOURCE_DIR}/orc-tests/*.cpp"
    "${CMAKE_SOURCE_DIR}/orc-tests/*.h"
    "${CMAKE_SOURCE_DIR}/orc-tests/*.hpp"
)

set(_stamp_dir "${CMAKE_BINARY_DIR}/clang-format-stamps")
file(MAKE_DIRECTORY "${_stamp_dir}")

set(_format_stamps)
foreach(_file IN LISTS _clang_format_sources)
    file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}" "${_file}")
    string(REPLACE "/" "__" _stamp_name "${_rel}")
    set(_stamp "${_stamp_dir}/${_stamp_name}.stamp")
    add_custom_command(
        OUTPUT  "${_stamp}"
        COMMAND ${CLANG_FORMAT_EXECUTABLE} --style=file --dry-run --Werror "${_file}"
        COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
        DEPENDS "${_file}"
        COMMENT "clang-format: ${_rel}"
        VERBATIM
    )
    list(APPEND _format_stamps "${_stamp}")
endforeach()

add_custom_target(clang-format-files ALL
    DEPENDS ${_format_stamps}
)

# ---------------------------------------------------------------------------
# 2. Full-repo targets: check everything, or reformat everything in-place
# ---------------------------------------------------------------------------
add_custom_target(clang-format-check
    COMMAND
        ${CLANG_FORMAT_EXECUTABLE}
        --style=file
        --dry-run
        --Werror
        ${_clang_format_sources}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "clang-format: checking all source files..."
    VERBATIM
)

add_custom_target(clang-format
    COMMAND
        ${CLANG_FORMAT_EXECUTABLE}
        --style=file
        -i
        ${_clang_format_sources}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "clang-format: reformatting all source files in-place..."
    VERBATIM
)

# ---------------------------------------------------------------------------
# 3. CTest registration so the CI gate catches format violations
# ---------------------------------------------------------------------------
add_test(
    NAME ClangFormatCheck
    COMMAND
        ${CLANG_FORMAT_EXECUTABLE}
        --style=file
        --dry-run
        --Werror
        ${_clang_format_sources}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(ClangFormatCheck PROPERTIES LABELS "format")

# ---------------------------------------------------------------------------
# 4. Install the pre-commit hook once at configure time (local builds only)
# ---------------------------------------------------------------------------
# Sets git's hooksPath to .githooks/ so the checked-in hook is used by all
# contributors without any manual setup step.  This only touches .git/config
# and has no effect on sandboxed (Nix derivation) builds.
find_program(GIT_EXECUTABLE git DOC "Path to git executable")
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} config core.hooksPath .githooks
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        RESULT_VARIABLE _git_hooks_result
        OUTPUT_QUIET ERROR_QUIET
    )
    if(_git_hooks_result EQUAL 0)
        message(STATUS "clang-format: pre-commit hook installed (core.hooksPath = .githooks)")
    endif()
endif()
