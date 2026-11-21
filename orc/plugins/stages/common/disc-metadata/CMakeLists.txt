# LaserDisc disc metadata: the per-frame VBI collector and the document built
# from it. The video sink embeds the document in its output; the Frame Map's
# Disc Mapper publishes the document's disc-level vote as statistics, so the
# two read a disc's identity the same way.
#
# This is NOT part of the SDK contract: it compiles against the public SDK
# headers only and is linked privately by the video sink and Frame Map
# plugins.
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Simon Inns

add_library(orc-disc-metadata STATIC
    disc_metadata_document.cpp
    disc_metadata_collect.cpp
)

# Consumers include the headers as "disc-metadata/disc_metadata_document.h"
# so the shared-library provenance is visible at every include site,
# matching the audio-resample and efm-decode convention.
target_include_directories(orc-disc-metadata
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Linked into stage plugins, which are shared libraries.
set_target_properties(orc-disc-metadata PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# orc-sdk-support carries the SDK include directory (<orc/stage/...>,
# <orc/support/...>).
target_link_libraries(orc-disc-metadata PUBLIC orc-sdk-support)
