# Presenters Module - MVP Architecture Layer
# Provides clean interface between GUI/CLI and Core
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(orc-presenters STATIC
    src/project_presenter.cpp
    src/plugin_details.cpp
    src/plugin_load_state.cpp
    src/plugin_selector.cpp
    src/stage_details.cpp
    src/filtergraph_parser.cpp
    src/filtergraph_import.cpp
    src/node_editing.cpp
    src/tool_running.cpp
    src/analysis_sink_runner.cpp
    src/tool_refresh.cpp
    src/render_presenter.cpp
    src/representation_audio_stream_reader.cpp
    src/analysis_presenter.cpp
    src/analysis_tool_presenter.cpp
    src/analysis_tool_presenter_factory.cpp
    src/frame_corruption_presenter.cpp
    src/source_alignment_presenter.cpp
    src/disc_mapper_presenter.cpp
    src/frame_map_range_presenter.cpp
    src/mask_line_presenter.cpp
    src/ffmpeg_preset_presenter.cpp
    src/dropout_editor_presenter.cpp
    src/hints_view_models.cpp
    src/dropout_presenter.cpp
    src/metrics_presenter.cpp
    src/vbi_presenter.cpp
    src/ntsc_observation_presenter.cpp
    src/closed_caption_observation_presenter.cpp
    src/video_parameter_observation_presenter.cpp
    src/core_init.cpp
)

# =============================================================================
# MVP Architecture - Presenter Layer
# =============================================================================
# Presenters form the bridge between GUI/CLI (high-level) and Core (low-level).
# 
# PUBLIC dependencies (exposed to GUI/CLI):
#   - orc-common: Common types used across layers
#
# PRIVATE dependencies (hidden from GUI/CLI):
#   - orc-core: Core implementation and business logic
#
# This ensures GUI/CLI code cannot directly access core headers, even if they
# try to include them - the headers are simply not in their include path.
# =============================================================================
target_include_directories(orc-presenters
    PUBLIC
        # Presenter headers are public - GUI/CLI can include them
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include/orc/presenters>
    PRIVATE
        # Core headers are private - only presenters can see them
        ${CMAKE_SOURCE_DIR}/orc/core/include
        ${CMAKE_SOURCE_DIR}/orc/core/analysis
        ${CMAKE_SOURCE_DIR}/orc/view-types
)

# =============================================================================
# Link Dependencies
# =============================================================================
# PUBLIC links expose symbols and headers to GUI/CLI
# PRIVATE links hide implementation details
# =============================================================================
target_link_libraries(orc-presenters
    PUBLIC
        orc-view-types  # Shared presentation layer types
        orc-common      # Common types
        # These stage plugins are consumed directly by presenter code.
        # Because orc-presenters is a static library, its downstream
        # executables must inherit these dependencies for final linking,
        # especially on Windows/MSVC.
        orc-stage-plugin-dropout-map
        orc-stage-plugin-dropout-analysis-sink
        orc-stage-plugin-snr-analysis-sink
        orc-stage-plugin-burst-level-analysis-sink
    PRIVATE
        orc-core    # Core implementation hidden from GUI/CLI
)

# Ensure C++17 standard
target_compile_features(orc-presenters PUBLIC cxx_std_17)

# Install presenter headers for GUI/CLI use
install(DIRECTORY include/
    DESTINATION include/orc/presenters
    FILES_MATCHING PATTERN "*.h"
)
