# CVBS Stream Source Stage Plugin
#
# Sequential (pipe-compatible) CVBS source for PAL, NTSC, and PAL-M systems —
# reads "-" (standard input) or a real named pipe instead of a random-access
# file. See orc/support/pipe_io.h for the "-" convention.
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors

file(GLOB_RECURSE STAGE_PLUGIN_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
)

orc_add_stage_plugin(
    orc-stage-plugin-cvbs-stream-source
    OUTPUT_NAME orc-stage-plugin-cvbs-stream-source
    PLUGIN_VERSION "${ORC_VERSION}"
    SOURCES ${STAGE_PLUGIN_SOURCES}
)

# Install plugin
install(TARGETS orc-stage-plugin-cvbs-stream-source
    LIBRARY DESTINATION lib/orc/stages
)
