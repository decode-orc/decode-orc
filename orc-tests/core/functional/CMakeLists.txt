cmake_minimum_required(VERSION 3.20)

project(orc-core-functional-tests)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(GTest REQUIRED)

include(GoogleTest)

function(orc_add_core_functional_tests target labels)
    string(REPLACE ";" "\\;" escaped_labels "${labels}")

    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/orc/plugins/stages/cvbs_sink
        ${CMAKE_SOURCE_DIR}/orc/plugins/stages/cvbs_source
        ${CMAKE_SOURCE_DIR}/orc/plugins/stages/daphne_vbi_sink
        ${CMAKE_SOURCE_DIR}/orc/plugins/stages/tbc_sink
        ${CMAKE_SOURCE_DIR}/orc/sdk/include
    )
    target_link_libraries(${target}
        orc-core
        GTest::gmock_main
    )
    target_compile_definitions(${target} PRIVATE
        ORC_STAGE_PLUGIN_BUILD_DIR="${CMAKE_BINARY_DIR}/lib/orc-stage-plugins"
    )

    if(TARGET orc_chroma_decoders)
        target_link_libraries(${target} orc_chroma_decoders)
    endif()

    foreach(_orc_stage_plugin_target
            orc-stage-plugin-ntsc-comp-source
            orc-stage-plugin-ntsc-yc-source
            orc-stage-plugin-pal-comp-source
            orc-stage-plugin-pal-yc-source
            orc-stage-plugin-cvbs-source
            orc-stage-plugin-cvbs-sink
            orc-stage-plugin-stacker
            orc-stage-plugin-field-invert
            orc-stage-plugin-field-map
            orc-stage-plugin-video-params
            orc-stage-plugin-dropout-correct
            orc-stage-plugin-dropout-map
            orc-stage-plugin-source-align
            orc-stage-plugin-mask-line
            orc-stage-plugin-video-sink
            orc-stage-plugin-daphne-vbi-sink
            orc-stage-plugin-audio-sink
            orc-stage-plugin-cc-sink
            orc-stage-plugin-tbc-sink
            orc-stage-plugin-efm-sink
            orc-stage-plugin-raw-efm-sink
            orc-stage-plugin-hackdac-sink
            orc-stage-plugin-ac3rf-sink
            orc-stage-plugin-dropout-analysis-sink
            orc-stage-plugin-snr-analysis-sink
            orc-stage-plugin-burst-level-analysis-sink)
        if(TARGET ${_orc_stage_plugin_target})
            target_link_libraries(${target} ${_orc_stage_plugin_target})
        endif()
    endforeach()

    if(TARGET orc-stage-plugins)
        add_dependencies(${target} orc-stage-plugins)
    endif()

    gtest_discover_tests(${target}
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "${escaped_labels}")
endfunction()

orc_add_core_functional_tests(
    orc-core-functional-tests-contracts
    "functional;contracts"

    contracts/stage_plugin_loader_smoke_test.cpp
    contracts/runtime_stage_contracts_test.cpp
    contracts/multi_track_audio_roundtrip_test.cpp
)

# Plugin isolation tests get a dedicated executable that deliberately does
# NOT link any stage plugin target: linking a plugin would map its library
# (and unify its symbols/typeinfo) before StagePluginLoader runs, making the
# RTLD_LOCAL RTTI and keep-alive lifetime checks pass trivially.
add_executable(orc-core-functional-tests-plugin-isolation
    contracts/stage_plugin_isolation_test.cpp
)
target_include_directories(orc-core-functional-tests-plugin-isolation PRIVATE
    ${CMAKE_SOURCE_DIR}/orc/sdk/include
)
target_link_libraries(orc-core-functional-tests-plugin-isolation
    orc-core
    GTest::gmock_main
)
target_compile_definitions(orc-core-functional-tests-plugin-isolation PRIVATE
    ORC_STAGE_PLUGIN_BUILD_DIR="${CMAKE_BINARY_DIR}/lib/orc-stage-plugins"
)
if(TARGET orc-stage-plugins)
    add_dependencies(orc-core-functional-tests-plugin-isolation orc-stage-plugins)
endif()
gtest_discover_tests(orc-core-functional-tests-plugin-isolation
    DISCOVERY_MODE POST_BUILD
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "functional\;contracts")

# Observation sidecar tests exercise the real SQLite backend against temporary
# files on disk, so they are functional (not unit). No stage plugins needed.
add_executable(orc-core-functional-tests-observation
    observation_persistence_sqlite_test.cpp
    observation_cache_retention_fs_test.cpp
)
target_include_directories(orc-core-functional-tests-observation PRIVATE
    ${CMAKE_SOURCE_DIR}/orc/core/include
    ${CMAKE_SOURCE_DIR}/orc/sdk/include
)
target_link_libraries(orc-core-functional-tests-observation
    orc-core
    GTest::gmock_main
)
gtest_discover_tests(orc-core-functional-tests-observation
    DISCOVERY_MODE POST_BUILD
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "functional\;observation")

# Executable-relative plugin search path resolution: creates real directories
# and symlinks on disk, so functional (not unit). No stage plugins needed.
add_executable(orc-core-functional-tests-plugin-search-path
    plugin_search_path_fs_test.cpp
)
target_include_directories(orc-core-functional-tests-plugin-search-path PRIVATE
    ${CMAKE_SOURCE_DIR}/orc/core/include
    ${CMAKE_SOURCE_DIR}/orc/sdk/include
)
target_link_libraries(orc-core-functional-tests-plugin-search-path
    orc-core
    GTest::gmock_main
)
gtest_discover_tests(orc-core-functional-tests-plugin-search-path
    DISCOVERY_MODE POST_BUILD
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "functional\;contracts")

# Named pipe (FIFO) detection in pipe validation and the GUI execution guard:
# creates real FIFOs on disk, so functional (not unit). POSIX only; the tests
# skip themselves on Windows. No stage plugins needed.
add_executable(orc-core-functional-tests-named-pipe
    named_pipe_detection_fs_test.cpp
)
target_include_directories(orc-core-functional-tests-named-pipe PRIVATE
    ${CMAKE_SOURCE_DIR}/orc/core/include
    ${CMAKE_SOURCE_DIR}/orc/presenters/include
    ${CMAKE_SOURCE_DIR}/orc/sdk/include
)
target_link_libraries(orc-core-functional-tests-named-pipe
    orc-core
    orc-presenters
    GTest::gmock_main
)
gtest_discover_tests(orc-core-functional-tests-named-pipe
    DISCOVERY_MODE POST_BUILD
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "functional\;contracts")

# Raw VBI capture transport and the stage over it: both are verified against
# the reference bt8x8 PAL sample, which is real media on disk and therefore
# functional. The sample is not checked in (test-data/ is ignored), so the
# tests skip when it is absent. The stage plugin target is linked directly so
# the stage class can be driven in-process.
if(TARGET orc-vbi-source)
    add_executable(orc-core-functional-tests-vbi-source
        stages/vbi_source/vbi_flac_transport_test.cpp
        stages/vbi_source/vbi_calibration_test.cpp
        stages/vbi_source/vbi_source_pipeline_test.cpp
        stages/vbi_source/vbi_ntsc_source_test.cpp
        stages/vbi_source/vbi_secam_source_test.cpp
        # The teletext sink's frame slicer is what the 525-line
        # recovery check drives; compiled in directly rather than linked
        # because it is a self-contained plugin-local module. It learns
        # through the scan state, so that comes with it.
        ${CMAKE_SOURCE_DIR}/orc/plugins/stages/teletext_sink/teletext_frame_slicer.cpp
        ${CMAKE_SOURCE_DIR}/orc/plugins/stages/teletext_sink/teletext_scan_state.cpp
    )
    target_include_directories(orc-core-functional-tests-vbi-source PRIVATE
        ${CMAKE_SOURCE_DIR}/orc/plugins/stages/teletext_sink
    )
    target_link_libraries(orc-core-functional-tests-vbi-source
        orc-vbi-source
        # The slicer and page decoder the 525-line recovery check verifies
        # against; the PUBLIC include directory comes with the link.
        orc-vbi-services
        GTest::gmock_main
    )
    if(TARGET orc-stage-plugin-vbi-source)
        # orc-core supplies the concrete ObservationContext the stage's
        # execute() signature takes; at runtime the host provides it.
        target_link_libraries(orc-core-functional-tests-vbi-source
            orc-stage-plugin-vbi-source
            orc-core)
    endif()
    target_compile_definitions(orc-core-functional-tests-vbi-source PRIVATE
        ORC_VBI_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test-data"
    )
    gtest_discover_tests(orc-core-functional-tests-vbi-source
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "functional\;sources")
endif()

# The teletext sink end to end: the reference 625-line and 525-line
# WST captures, read through the VBI source stage and decoded by the sink, with
# the exported packet stream compared against golden data. Real media on disk,
# so functional; the samples are not checked in (test-data/ is ignored), so the
# tests skip when they are absent.
if(TARGET orc-vbi-source AND TARGET orc-stage-plugin-nabts-sink)
    add_executable(orc-core-functional-tests-nabts-sink
        stages/nabts_sink/nabts_sink_pipeline_test.cpp
    )
    target_include_directories(orc-core-functional-tests-nabts-sink PRIVATE
        ${CMAKE_SOURCE_DIR}/orc/core/include
        ${CMAKE_SOURCE_DIR}/orc/plugins/stages/nabts_sink
        ${CMAKE_SOURCE_DIR}/orc/plugins/stages/vbi_source
        ${CMAKE_SOURCE_DIR}/orc/sdk/include
    )
    target_link_libraries(orc-core-functional-tests-nabts-sink
        orc-core
        orc-vbi-source
        orc-stage-plugin-nabts-sink
        orc-vbi-services
        GTest::gmock_main
    )
    if(TARGET orc-stage-plugin-vbi-source)
        target_link_libraries(orc-core-functional-tests-nabts-sink
            orc-stage-plugin-vbi-source)
    endif()
    target_compile_definitions(orc-core-functional-tests-nabts-sink PRIVATE
        ORC_VBI_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test-data"
    )
    gtest_discover_tests(orc-core-functional-tests-nabts-sink
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "functional\;sinks")
endif()

if(TARGET orc-vbi-source AND TARGET orc-stage-plugin-teletext-sink)
    add_executable(orc-core-functional-tests-teletext-sink
        stages/teletext_sink/teletext_sink_pipeline_test.cpp
    )
    target_include_directories(orc-core-functional-tests-teletext-sink PRIVATE
        ${CMAKE_SOURCE_DIR}/orc/core/include
        ${CMAKE_SOURCE_DIR}/orc/plugins/stages/teletext_sink
        ${CMAKE_SOURCE_DIR}/orc/plugins/stages/vbi_source
        ${CMAKE_SOURCE_DIR}/orc/sdk/include
    )
    target_link_libraries(orc-core-functional-tests-teletext-sink
        orc-core
        orc-vbi-source
        orc-stage-plugin-teletext-sink
        orc-vbi-services
        GTest::gmock_main
    )
    if(TARGET orc-stage-plugin-vbi-source)
        target_link_libraries(orc-core-functional-tests-teletext-sink
            orc-stage-plugin-vbi-source)
    endif()
    target_compile_definitions(orc-core-functional-tests-teletext-sink PRIVATE
        ORC_VBI_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/test-data"
    )
    gtest_discover_tests(orc-core-functional-tests-teletext-sink
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "functional\;sinks")
endif()

# TBCMetadataWriter::update_sequential_field_count() (the correction that
# keeps a piped/unbounded tbc_stream_source export's .tbc.db from claiming
# its huge placeholder frame count) writes a real SQLite database to a
# temporary file on disk, so functional, not unit. tbc_metadata_writer.cpp
# is a private implementation detail of the tbc_sink plugin (not part of the
# public SDK), so it is compiled directly into the test binary rather than
# linked — the same "self-contained plugin-local module" pattern the
# teletext frame slicer above uses.
add_executable(orc-core-functional-tests-tbc-metadata-writer
    stages/tbc_sink/tbc_metadata_writer_test.cpp
    ${CMAKE_SOURCE_DIR}/orc/plugins/stages/tbc_sink/tbc_metadata_writer.cpp
)
target_include_directories(orc-core-functional-tests-tbc-metadata-writer PRIVATE
    ${CMAKE_SOURCE_DIR}/orc/plugins/stages/tbc_sink
    ${CMAKE_SOURCE_DIR}/orc/plugins/stages/tbc_source
    ${CMAKE_SOURCE_DIR}/orc/sdk/include
)
target_link_libraries(orc-core-functional-tests-tbc-metadata-writer
    orc-core
    SQLite::SQLite3
    GTest::gmock_main
)
gtest_discover_tests(orc-core-functional-tests-tbc-metadata-writer
    DISCOVERY_MODE POST_BUILD
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "functional\;sinks")

# VideoSinkStage's streaming (unbounded-source) export path writes a real
# video file to disk through the real (non-FFmpeg) RawOutputBackend — a
# hand-rolled VideoFrameRepresentation drives a genuine multi-frame,
# stateful streaming sequence through it that a mock's canned EXPECT_CALL
# returns cannot easily express — so functional, not unit.
if(TARGET orc-stage-plugin-video-sink)
    add_executable(orc-core-functional-tests-video-sink
        stages/video_sink/video_sink_streaming_test.cpp
    )
    target_include_directories(orc-core-functional-tests-video-sink PRIVATE
        ${CMAKE_SOURCE_DIR}/orc/core/include
        ${CMAKE_SOURCE_DIR}/orc/plugins/stages/sinks/common
        ${CMAKE_SOURCE_DIR}/orc/sdk/include
    )
    target_link_libraries(orc-core-functional-tests-video-sink
        orc-core
        orc-stage-plugin-video-sink
        GTest::gmock_main
    )
    gtest_discover_tests(orc-core-functional-tests-video-sink
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "functional\;sinks")
endif()
