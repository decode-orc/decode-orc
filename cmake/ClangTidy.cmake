# ClangTidy.cmake - clang-tidy integration
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Provides two layers of enforcement:
#
#   1. CMAKE_CXX_CLANG_TIDY -- CMake's built-in hook runs clang-tidy on
#      every .cpp source file that is recompiled, passing the exact compiler
#      flags so clang-tidy sees the same includes/defines.  The .clang-tidy
#      config in orc/ is picked up automatically by directory traversal.
#
#   2. clang-tidy-check target -- runs the full source tree via
#      run-clang-tidy using the compile_commands.json database.  Use this
#      in CI or to get a full-repo report.
#
# clang-tidy is optional; if not found everything is silently skipped.

# Opt-out switch for builds that ship the product rather than gate the source.
#
# The "silently skipped when absent" rule above does not hold in a Nix package
# build: on the Darwin stdenv clang-tidy lives in the same derivation as the
# compiler and is on PATH, so find_program() below succeeds and every
# translation unit is analysed.  That analysis then fails, because CMake runs
# the *unwrapped* clang-tidy while compiling goes through the cc-wrapper, and
# it is the wrapper -- not the command line CMake records -- that adds
# libc++'s include directory (-cxx-isystem <libcxx>/include/c++/v1).  Without
# it clang-tidy cannot find <cstdint>, <functional> or any other standard
# header, and the resulting clang-diagnostic-error is fatal under
# WarningsAsErrors: '*'.  A broken parse also produces bogus findings: a catch
# block whose body failed to resolve is dropped from the AST and reported as
# bugprone-empty-catch.
#
# flake.nix passes -DORC_ENABLE_CLANG_TIDY=OFF for that reason.  Static
# analysis stays on in the dev shell and in CI, where clang-tidy runs against
# a compile database produced by the same toolchain.
option(ORC_ENABLE_CLANG_TIDY "Run clang-tidy as part of the build" ON)

if(NOT ORC_ENABLE_CLANG_TIDY)
    message(STATUS "clang-tidy: disabled (ORC_ENABLE_CLANG_TIDY=OFF)")
    return()
endif()

# clang-tidy cannot parse MSVC (cl.exe) command lines when invoked through
# CMAKE_CXX_CLANG_TIDY: /EH and Windows SDK defines are misread, producing
# thousands of false "exceptions disabled" / unknown-type errors that fail
# the build. The Visual Studio generator never ran this hook, so skipping
# MSVC preserves long-standing Windows behaviour (analysis runs on Linux CI).
if(MSVC)
    message(STATUS "clang-tidy: skipped for MSVC toolchain")
    return()
endif()

# clang-tidy's own clang frontend does not know where MinGW-w64/gcc's
# standard headers live (<cstdint>, <stdio.h>, <atomic>, ...) unless told
# explicitly, and CMAKE_CXX_CLANG_TIDY does not forward that automatically.
# Without it every MinGW build fails immediately on missing standard headers,
# and any external dependency shipping its own permissive .clang-tidy (e.g.
# QtNodes) additionally fails with "no checks enabled". Skipping mirrors the
# existing MSVC exception above; static analysis still runs on Linux/macOS CI.
if(MINGW)
    message(STATUS "clang-tidy: skipped for MinGW toolchain (see cmake/ClangTidy.cmake)")
    return()
endif()

find_program(CLANG_TIDY_EXECUTABLE
    NAMES
        clang-tidy
        clang-tidy-21
        clang-tidy-20
        clang-tidy-19
        clang-tidy-18
    DOC "Path to clang-tidy executable"
)

if(NOT CLANG_TIDY_EXECUTABLE)
    message(STATUS "clang-tidy: not found -- static analysis disabled")
    return()
endif()

message(STATUS "clang-tidy: ${CLANG_TIDY_EXECUTABLE}")

# Export compile_commands.json so clang-tidy-check and IDEs can consume it.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# 1. Per-file analysis wired into the build dependency graph
# ---------------------------------------------------------------------------
# Every .cpp file that is recompiled is also analysed.  The build fails if
# any warning is emitted (.clang-tidy already sets WarningsAsErrors: '*').
set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE}")

# ---------------------------------------------------------------------------
# 2. Full-repo target (uses compile_commands.json via run-clang-tidy)
# ---------------------------------------------------------------------------
find_program(RUN_CLANG_TIDY_EXECUTABLE
    NAMES
        run-clang-tidy
        run-clang-tidy-21
        run-clang-tidy-20
        run-clang-tidy-19
        run-clang-tidy-18
    DOC "Path to run-clang-tidy script"
)

if(RUN_CLANG_TIDY_EXECUTABLE)
    message(STATUS "run-clang-tidy: ${RUN_CLANG_TIDY_EXECUTABLE}")
    add_custom_target(clang-tidy-check
        COMMAND ${RUN_CLANG_TIDY_EXECUTABLE}
            -clang-tidy-binary ${CLANG_TIDY_EXECUTABLE}
            -p ${CMAKE_BINARY_DIR}
            -header-filter ".*/(orc|orc-tests)/.*"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "clang-tidy: analysing all source files..."
        VERBATIM
    )
else()
    message(STATUS "run-clang-tidy: not found -- clang-tidy-check target disabled")
endif()
