# Shared VBI data-service library (WST teletext and NABTS) used by the
# teletext_sink and nabts_sink stage plugins, and by the host presenters that
# read their catalogues.
#
# NABTS rides the same NRZ line-slicing machinery as WST teletext — 0xE7
# framing instead of 0xE4, otherwise the same clock run-in, bit rate, lines,
# levels and bit order (CEA-516 §1-§3 against ETSI EN 300 706) — so both
# plugins slice with teletext_slicer and share the Hamming 8/4 decode.
#
# This is NOT part of the SDK contract. These sources were public SDK headers
# under orc/support/ until they were moved here: format-specific decoding for
# two stages has no third-party value, and vbi_analysis_results.h sitting in
# the ABI-frozen stage tier meant a teletext catalogue change bumped the host
# ABI for every plugin in the ecosystem (abi 12 and 13 were both exactly that).
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Simon Inns

add_library(orc-vbi-services STATIC
    teletext_slicer.cpp
    teletext_bit_clock.cpp
    teletext_page_decoder.cpp
    teletext_recovery_stats.cpp
    teletext_row_squasher.cpp
    nabts_page.cpp
    nabts_captions.cpp
    vbi_identity_attestation.cpp
)

# Consumers include the headers as "vbi-services/teletext_slicer.h" so the
# shared-library provenance is visible at every include site, matching the
# audio-resample and efm-decode convention.
target_include_directories(orc-vbi-services
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Linked into stage plugins, which are shared libraries.
set_target_properties(orc-vbi-services PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# The plugin SDK provides the <orc/stage/...> contract headers and the
# spdlog/fmt usage requirements; teletext_recovery_stats.cpp formats its
# profile with fmt directly.
target_link_libraries(orc-vbi-services PUBLIC orc-plugin-sdk)
