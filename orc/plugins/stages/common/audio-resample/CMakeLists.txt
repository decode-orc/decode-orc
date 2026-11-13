# Shared audio resampling library (free-running 44100 Hz ↔ frame-locked
# rates) used by the tbc_source, cvbs_source, and audio_sink stage plugins.
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Simon Inns

add_library(orc-audio-resample STATIC
    audio_resampler.cpp
    synchronous_audio_cache.cpp
)

# Consumers include the header as "audio-resample/audio_resampler.h" so the
# shared-library provenance is visible at every include site.
target_include_directories(orc-audio-resample
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

set_target_properties(orc-audio-resample PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# SoXR provides the HQ rate conversion. Try pkg-config first (more reliable
# across platforms), then CONFIG mode, then a plain library search.
set(_SOXR_LINK_TARGET "")
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SOXR IMPORTED_TARGET soxr)
    if(SOXR_FOUND)
        set(_SOXR_LINK_TARGET PkgConfig::SOXR)
    endif()
endif()
if(NOT _SOXR_LINK_TARGET)
    find_package(soxr CONFIG QUIET)
    if(soxr_FOUND OR TARGET soxr)
        set(_SOXR_LINK_TARGET soxr)
    endif()
endif()
if(NOT _SOXR_LINK_TARGET)
    # Fallback: try SoXR with capitalised name (some distro packages).
    find_library(SOXR_LIBRARY NAMES soxr)
    find_path(SOXR_INCLUDE_DIR NAMES soxr.h)
    if(SOXR_LIBRARY AND SOXR_INCLUDE_DIR)
        add_library(soxr_fallback_audio_resample INTERFACE)
        target_link_libraries(soxr_fallback_audio_resample INTERFACE "${SOXR_LIBRARY}")
        target_include_directories(soxr_fallback_audio_resample INTERFACE "${SOXR_INCLUDE_DIR}")
        set(_SOXR_LINK_TARGET soxr_fallback_audio_resample)
    endif()
endif()
if(NOT _SOXR_LINK_TARGET)
    message(FATAL_ERROR
        "orc-audio-resample: SoXR library not found. "
        "Install libsoxr-dev (Debian/Ubuntu), soxr (Homebrew/vcpkg), "
        "or ensure it is available via pkg-config.")
endif()

# PUBLIC so plugins linking this library inherit the SoXR link requirement.
target_link_libraries(orc-audio-resample PUBLIC ${_SOXR_LINK_TARGET})

# The plugin SDK provides the <orc/stage/...> contract headers and the
# spdlog/fmt usage requirements needed by <orc/stage/logging.h>.
target_link_libraries(orc-audio-resample PUBLIC orc-plugin-sdk)
