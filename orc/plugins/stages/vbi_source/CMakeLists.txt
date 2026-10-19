# VBI Source Stage Plugin
#
# Ingests third-party raw VBI teletext captures (bt8x8 and family card dumps,
# cropped .tbc VBI files) and presents them to the teletext decoders as CVBS.
#
# The container model, transports, reader, calibration and frame builder build
# as a plugin-side static library so they can be unit tested directly. It
# compiles against the public SDK headers only and is linked privately into the
# stage plugin.
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Simon Inns

add_library(orc-vbi-source STATIC
    vbi_source_format.cpp
    vbi_line_reader.cpp
    vbi_source_validation.cpp
    vbi_transport.cpp
    vbi_teletext_service.cpp
    vbi_output_frame.cpp
    vbi_line_mapping.cpp
    vbi_level_mapper.cpp
    vbi_line_placement.cpp
    vbi_resampler.cpp
    vbi_cri_template.cpp
    vbi_cri_correlator.cpp
    vbi_offset_calibration.cpp
    vbi_frame_builder.cpp
    vbi_frame_index.cpp
    vbi_frame_read_ahead.cpp
)

# The library is linked into the plugin's shared object, so it has to be
# position-independent even though it is itself static.
set_target_properties(orc-vbi-source PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(orc-vbi-source
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

set_target_properties(orc-vbi-source PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# The plugin SDK provides the <orc/support/...> contract headers and the
# spdlog/fmt usage requirements needed by <orc/support/logging.h>.
target_link_libraries(orc-vbi-source PUBLIC orc-plugin-sdk)

# FFmpeg unwraps FLAC-compressed captures, and captures stored as the
# pictures of a Matroska/FFV1 video (vbi_transport.cpp). FLAC is used
# here as a generic lossless byte compressor, not as audio, which is why
# libavformat/libavcodec are preferred to adding a libFLAC dependency.
target_link_libraries(orc-vbi-source PUBLIC PkgConfig::FFMPEG)

# The read-ahead decodes on threads of its own (vbi_frame_read_ahead.cpp).
find_package(Threads REQUIRED)
target_link_libraries(orc-vbi-source PUBLIC Threads::Threads)

# The stage itself: the DAG-facing class and the plugin bundle, over the
# library above.
orc_add_stage_plugin(
    orc-stage-plugin-vbi-source
    OUTPUT_NAME orc-stage-plugin-vbi-source
    PLUGIN_VERSION "${ORC_VERSION}"
    SOURCES
        vbi_source_stage.cpp
        plugin.cpp
    LINK_LIBRARIES orc-vbi-source
)

install(TARGETS orc-stage-plugin-vbi-source
    LIBRARY DESTINATION lib/orc/stages
)
