cmake_minimum_required(VERSION 3.20)

project(orc-core-unit-tests)

# GoogleTest requires at least C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(GTest REQUIRED)

# This is already included in the top-level cmakelist so does not need to be included here
#enable_testing()

function(orc_add_core_unit_tests target labels)
        string(REPLACE ";" "\\;" escaped_labels "${labels}")

    add_executable(${target} ${ARGN})
        target_include_directories(${target} PRIVATE
                ${CMAKE_SOURCE_DIR}/orc/plugins/stages/audio_sink
                ${CMAKE_SOURCE_DIR}/orc/plugins/stages/common
                ${CMAKE_SOURCE_DIR}/orc/plugins/stages/daphne_vbi_sink
                ${CMAKE_SOURCE_DIR}/orc/plugins/stages/tbc_sink
                ${CMAKE_SOURCE_DIR}/orc/plugins/stages/raw_efm_sink
                ${CMAKE_SOURCE_DIR}/orc/plugins/stages/tbc_source
                ${CMAKE_SOURCE_DIR}/orc/plugins/stages/vbi_source
                ${CMAKE_SOURCE_DIR}/orc/plugins/stages/cvbs_sink
                ${CMAKE_SOURCE_DIR}/orc/plugins/stages/ac3rf_sink
                ${CMAKE_SOURCE_DIR}/orc/sdk/include
        )
    target_link_libraries(
            ${target}
            orc-core
            GTest::gmock_main
    )

        if(TARGET orc-audio-resample)
                target_link_libraries(${target} orc-audio-resample)
        endif()

        if(TARGET orc-vbi-source)
                target_link_libraries(${target} orc-vbi-source)
        endif()

        # The teletext plugin's WST/NABTS decoding library, present only when
        # a teletext plugin checkout is built in (ORC_PLUGIN_SOURCE_DIRS). The
        # VBI source tests use its slicer as their decoding oracle.
        if(TARGET orc-vbi-services)
                target_link_libraries(${target} orc-vbi-services)
                target_compile_definitions(${target} PRIVATE ORC_TELETEXT_PLUGIN_IN_BUILD=1)
        endif()

    target_compile_definitions(${target} PRIVATE
            ORC_STAGE_PLUGIN_BUILD_DIR="${CMAKE_BINARY_DIR}/lib/orc-stage-plugins"
    )

        if(TARGET orc_chroma_decoders)
                target_link_libraries(${target} orc_chroma_decoders)
        endif()

        # Stage instances in contract tests are obtained via StageRegistry, not by
        # direct concrete class construction. These static plugin links are required
        # so that each plugin's registration callback fires during static
        # initialisation, populating the registry before any test runs.
        # Dedicated functional tests cover the runtime dlopen/LoadLibrary path
        # (ABI matching, missing symbols, etc.) alongside these registry links.
        foreach(_orc_stage_plugin_target
                        orc-stage-plugin-tbc-source
                        orc-stage-plugin-tbc-stream-source
                        orc-stage-plugin-cvbs-source
                        orc-stage-plugin-cvbs-stream-source
                        orc-stage-plugin-vbi-source
                        orc-stage-plugin-stacker
                        orc-stage-plugin-frame-map
                        orc-stage-plugin-source-join
                        orc-stage-plugin-video-params
                        orc-stage-plugin-dropout-correct
                        orc-stage-plugin-dropout-map
                        orc-stage-plugin-source-align
                        orc-stage-plugin-mask-line
                        orc-stage-plugin-picture-band-limit
                        orc-stage-plugin-fm-beat-cancel
                        orc-stage-plugin-channel-equalise
                        orc-stage-plugin-video-sink
                        orc-stage-plugin-daphne-vbi-sink
                        orc-stage-plugin-audio-sink
                        orc-stage-plugin-cc-sink
                        orc-stage-plugin-tbc-sink
                        orc-stage-plugin-efm-sink
                        orc-stage-plugin-efm-audio-decode
                        orc-stage-plugin-audio-import
                        orc-stage-plugin-audio-channel-map
                        orc-stage-plugin-audio-align
                        orc-stage-plugin-raw-efm-sink
                        orc-stage-plugin-ac3rf-sink
                        orc-stage-plugin-dropout-analysis-sink
                        orc-stage-plugin-snr-analysis-sink
                        orc-stage-plugin-burst-level-analysis-sink
                        orc-stage-plugin-cvbs-sink)
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

include(GoogleTest)

orc_add_core_unit_tests(
        orc-core-unit-tests-sources
        "unit;sources"

        stages/source_common/source_stage_contract_test.cpp
        stages/tbc_source/tbc_source_stage_test.cpp
        stages/tbc_source/pal_tbc_converter_test.cpp
        stages/tbc_source/ntsc_tbc_converter_test.cpp
        stages/cvbs_source/cvbs_source_stage_test.cpp
        stages/vbi_source/vbi_source_format_test.cpp
        stages/vbi_source/vbi_line_reader_test.cpp
        stages/vbi_source/vbi_source_validation_test.cpp
        stages/vbi_source/vbi_transport_test.cpp
        stages/vbi_source/vbi_line_mapping_test.cpp
        stages/vbi_source/vbi_output_frame_test.cpp
        stages/vbi_source/vbi_level_mapper_test.cpp
        stages/vbi_source/vbi_resampler_test.cpp
        stages/vbi_source/vbi_line_placement_test.cpp
        stages/vbi_source/vbi_cri_template_test.cpp
        stages/vbi_source/vbi_cri_correlator_test.cpp
        stages/vbi_source/vbi_offset_calibration_test.cpp
        stages/vbi_source/vbi_frame_builder_test.cpp
        stages/vbi_source/vbi_frame_index_test.cpp
        stages/vbi_source/vbi_frame_read_ahead_test.cpp
        stages/vbi_source/vbi_source_stage_test.cpp
        stages/audio_resample/audio_resampler_test.cpp
)

orc_add_core_unit_tests(
        orc-core-unit-tests-transforms
        "unit;transforms"

        stages/stacker/stacker_stage_test.cpp
        stages/stacker/efm_confidence_stack_test.cpp
        stages/stacker/still_frame_stacking_test.cpp
        stages/stacker/pressing_repair_test.cpp
        stages/stacker/sample_median_test.cpp
        stages/stacker/segment_measurements_test.cpp
        stages/frame_map/frame_map_stage_test.cpp
        stages/frame_map/frame_map_range_search_test.cpp
        stages/frame_map/frame_map_range_tool_test.cpp
        stages/frame_map/frame_corruption_tool_test.cpp
        stages/frame_map/disc_mapper_tool_test.cpp
        stages/frame_map/disc_mapper_identity_test.cpp
        stages/frame_map/disc_mapper_lead_in_out_test.cpp
        stages/frame_map/disc_mapper_pn_guards_test.cpp
        stages/frame_map/disc_mapper_pulldown_test.cpp
        stages/frame_map/frame_quality_score_test.cpp
        stages/source_join/source_join_stage_test.cpp
        stages/video_params/video_params_stage_test.cpp
        stages/dropout_correct/dropout_correct_stage_test.cpp
        stages/dropout_map/dropout_map_stage_test.cpp
        stages/source_align/source_align_stage_test.cpp
        stages/source_align/source_alignment_tool_test.cpp
        stages/source_align/source_alignment_offsets_test.cpp
        stages/source_align/capture_comparison_test.cpp
        stages/source_align/content_offsets_test.cpp
        stages/source_align/vits_edge_tail_test.cpp
        stages/mask_line/mask_line_stage_test.cpp
        stages/picture_band_limit/picture_band_limit_stage_test.cpp
        stages/fm_beat_cancel/fm_beat_cancel_stage_test.cpp
        stages/fm_beat_cancel/fm_beat_calibration_test.cpp
        stages/fm_beat_cancel/fm_beat_calibration_tool_test.cpp
        stages/channel_equalise/channel_equalise_stage_test.cpp
        stages/channel_equalise/channel_calibration_test.cpp
        stages/channel_equalise/channel_calibration_tool_test.cpp
        stages/efm_audio_decode/efm_audio_decode_stage_test.cpp
        stages/audio_import/audio_import_stage_test.cpp
        stages/audio_channel_map/audio_channel_map_stage_test.cpp
        stages/audio_align/audio_align_stage_test.cpp
)

orc_add_core_unit_tests(
        orc-core-unit-tests-sinks
        "unit;sinks"

        stages/video_sink/componentframe_test.cpp
        stages/video_sink/carrier_plane_copy_test.cpp
        stages/video_sink/vectorscope_analysis_test.cpp
        stages/video_sink/av1_rate_control_test.cpp
        stages/video_sink/bt601_export_grid_test.cpp
        stages/video_sink/decoder_schedule_test.cpp
        stages/video_sink/weave_decision_test.cpp
        stages/video_sink/disc_metadata_document_test.cpp
        stages/video_sink/vbi_observation_digest_test.cpp
        stages/video_sink/frame_claim_window_test.cpp
        stages/video_sink/video_sink_stage_defaults_test.cpp
        stages/video_sink/video_sink_stage_safety_test.cpp
        stages/video_sink/comb_test.cpp
        stages/video_sink/noise_reducer_test.cpp
        stages/video_sink/video_sink_stage_test.cpp
        stages/video_sink/video_sink_stage_preview_test.cpp
        stages/video_sink/audio_pair_selection_test.cpp
        stages/video_sink/audio_sample_feed_test.cpp
        stages/video_sink/subtitle_embed_policy_test.cpp
        stages/video_sink/remux_dts_test.cpp
        stages/video_sink/closed_caption_cues_test.cpp
        stages/video_sink/monodecoder_test.cpp
        stages/video_sink/ntsc_pal_decoder_wrapper_test.cpp
        stages/video_sink/palcolour_test.cpp
        stages/video_sink/pal_still_test.cpp
        stages/video_sink/palstilldecoder_test.cpp
        stages/video_sink/secamdecoder_test.cpp
        stages/video_sink/sourcefield_test.cpp
        stages/daphne_vbi_sink/daphne_vbi_writer_util_test.cpp
        stages/daphne_vbi_sink/daphne_vbi_sink_stage_deps_test.cpp
        stages/daphne_vbi_sink/daphne_vbi_sink_stage_test.cpp
        stages/audio_sink/audio_sink_stage_test.cpp
        stages/audio_sink/audio_sink_stage_deps_test.cpp
        stages/ac3rf_sink/ac3rf_sink_stage_test.cpp
        stages/ac3rf_sink/ac3rf_sink_stage_deps_test.cpp
        stages/cc_sink/cc_sink_stage_test.cpp
        stages/cc_sink/cc_line21_test.cpp
        stages/tbc_sink/tbc_sink_stage_test.cpp
        stages/tbc_sink/tbc_sink_stage_deps_test.cpp
        stages/tbc_sink/tbc_sink_levels_test.cpp
        stages/efm_sink/efm_sink_stage_test.cpp
        stages/raw_efm_sink/raw_efm_sink_stage_test.cpp
        stages/raw_efm_sink/raw_efm_sink_stage_deps_test.cpp
        stages/cvbs_sink/cvbs_sink_stage_test.cpp
        stages/cvbs_sink/cvbs_sink_stage_deps_test.cpp
        stages/cvbs_sink/cvbs_sink_phase_sequence_test.cpp
        stages/dropout_analysis_sink/dropout_analysis_sink_stage_test.cpp
        stages/dropout_analysis_sink/dropout_analysis_sink_deps_test.cpp
        stages/dropout_analysis_sink/dropout_analysis_sink_map_contract_test.cpp
        stages/dropout_analysis_sink/dropout_analysis_sink_report_test.cpp
        stages/snr_analysis_sink/snr_analysis_sink_stage_test.cpp
        stages/snr_analysis_sink/snr_analysis_sink_deps_test.cpp
        stages/burst_level_analysis_sink/burst_level_analysis_sink_stage_test.cpp
        stages/burst_level_analysis_sink/burst_level_analysis_sink_deps_test.cpp
        stages/analysis_sink_tool_results_test.cpp
)

orc_add_core_unit_tests(
        orc-core-unit-tests-analysis
        "unit"

        analysis/composite_vectorscope_test.cpp
)

orc_add_core_unit_tests(
        orc-core-unit-tests-contracts
        "unit;contracts"

        contracts/public_stage_contract_test.cpp
        contracts/stage_registry_contract_test.cpp
        contracts/project_to_dag_contract_test.cpp
        contracts/dag_executor_inputs_test.cpp
        contracts/plugin_safe_call_test.cpp
        contracts/video_frame_representation_wrapper_contract_test.cpp
        contracts/audio_channel_pair_contract_test.cpp
        contracts/observation_service_accessor_test.cpp
        contracts/observation_service_identity_contract_test.cpp
        contracts/observation_store_parity_test.cpp
)

# Presenter-layer unit tests. These link orc-presenters in addition to the
# core/plugin libraries provided by orc_add_core_unit_tests().
orc_add_core_unit_tests(
        orc-core-unit-tests-presenters
        "unit;presenters"

        presenters/project_presenter_validation_test.cpp
        presenters/node_editing_test.cpp
        presenters/node_reset_test.cpp
        presenters/analysis_tool_presenter_test.cpp
        presenters/tool_running_test.cpp
        presenters/tool_refresh_test.cpp
        presenters/plugin_details_test.cpp
        presenters/plugin_view_registry_test.cpp
        presenters/stage_analysis_tools_test.cpp
        presenters/stage_tool_parameters_test.cpp
        presenters/plugin_load_state_test.cpp
        presenters/plugin_selector_test.cpp
        presenters/stage_details_test.cpp
        presenters/representation_audio_stream_reader_test.cpp
        presenters/video_parameter_observation_presenter_test.cpp
)
target_link_libraries(orc-core-unit-tests-presenters orc-presenters)

# ---------------------------------------------------------------------------
# Test-fixture plugin shared libraries for loader version rejection tests
# ---------------------------------------------------------------------------
set(ORC_TEST_FIXTURE_PLUGIN_BUILD_DIR "${CMAKE_BINARY_DIR}/lib/orc-test-fixture-plugins")

foreach(_fixture_name bad_abi_version bad_api_version abi_v4 bad_toolchain_tag
        compatible)
        set(_target "orc-test-fixture-${_fixture_name}")
        add_library(${_target} SHARED
                contracts/test_fixtures/${_fixture_name}_plugin.cpp)
        target_include_directories(${_target} PRIVATE
                ${CMAKE_SOURCE_DIR}/orc/sdk/include)
        set_target_properties(${_target} PROPERTIES
                LIBRARY_OUTPUT_DIRECTORY "${ORC_TEST_FIXTURE_PLUGIN_BUILD_DIR}"
                RUNTIME_OUTPUT_DIRECTORY "${ORC_TEST_FIXTURE_PLUGIN_BUILD_DIR}")
endforeach()

# Two copies of one plugin id, for the duplicate-id test.
foreach(_copy first second)
        set(_target "orc-test-fixture-duplicate_${_copy}")
        add_library(${_target} SHARED
                contracts/test_fixtures/duplicate_id_plugin.cpp)
        target_include_directories(${_target} PRIVATE
                ${CMAKE_SOURCE_DIR}/orc/sdk/include)
        target_compile_definitions(${_target} PRIVATE
                ORC_TEST_FIXTURE_COPY_VERSION="1.0.0-${_copy}")
        set_target_properties(${_target} PROPERTIES
                LIBRARY_OUTPUT_DIRECTORY "${ORC_TEST_FIXTURE_PLUGIN_BUILD_DIR}"
                RUNTIME_OUTPUT_DIRECTORY "${ORC_TEST_FIXTURE_PLUGIN_BUILD_DIR}")
endforeach()

# View binaries for the view loader tests (no Qt: see view_plugin.cpp). Named
# orc-view_* with no lib prefix, as a view binary must be.
set(ORC_TEST_FIXTURE_VIEW_BUILD_DIR "${CMAKE_BINARY_DIR}/lib/orc-test-fixture-views")
#   name             plugin id          abi+  qt     contract A        contract B
set(_view_fixtures
    "compatible|test.view.one|0|6|2|test.view.alpha.v1|test.view.beta.v1"
    "same_id|test.view.one|0|6|2|test.view.gamma.v1|test.view.delta.v1"
    "clashing|test.view.two|0|6|2|test.view.alpha.v1|test.view.epsilon.v1"
    "bad_abi|test.view.three|1|6|2|test.view.zeta.v1|test.view.eta.v1"
    "newer_qt|test.view.four|0|6|99|test.view.theta.v1|test.view.iota.v1")
set(_view_fixture_targets)
foreach(_spec IN LISTS _view_fixtures)
        string(REPLACE "|" ";" _fields "${_spec}")
        list(GET _fields 0 _name)
        list(GET _fields 1 _id)
        list(GET _fields 2 _abi_offset)
        list(GET _fields 3 _qt_major)
        list(GET _fields 4 _qt_minor)
        list(GET _fields 5 _contract_a)
        list(GET _fields 6 _contract_b)
        set(_target "orc-view_test_${_name}")
        add_library(${_target} SHARED contracts/test_fixtures/view_plugin.cpp)
        target_include_directories(${_target} PRIVATE
                ${CMAKE_SOURCE_DIR}/orc/sdk/include)
        target_compile_definitions(${_target} PRIVATE
                ORC_TEST_VIEW_PLUGIN_ID="${_id}"
                ORC_TEST_VIEW_ABI_OFFSET=${_abi_offset}
                ORC_TEST_VIEW_QT_MAJOR=${_qt_major}
                ORC_TEST_VIEW_QT_MINOR=${_qt_minor}
                ORC_TEST_VIEW_CONTRACT_A="${_contract_a}"
                ORC_TEST_VIEW_CONTRACT_B="${_contract_b}")
        set_target_properties(${_target} PROPERTIES
                PREFIX ""
                LIBRARY_OUTPUT_DIRECTORY "${ORC_TEST_FIXTURE_VIEW_BUILD_DIR}"
                RUNTIME_OUTPUT_DIRECTORY "${ORC_TEST_FIXTURE_VIEW_BUILD_DIR}")
        list(APPEND _view_fixture_targets ${_target})
endforeach()
add_custom_target(orc-test-fixture-views DEPENDS ${_view_fixture_targets})

add_executable(orc-core-unit-tests-view-loader
        contracts/view_plugin_loader_test.cpp)
target_include_directories(orc-core-unit-tests-view-loader PRIVATE
        ${CMAKE_SOURCE_DIR}/orc/sdk/include)
target_link_libraries(orc-core-unit-tests-view-loader
        orc-core
        GTest::gmock_main)
target_compile_definitions(orc-core-unit-tests-view-loader PRIVATE
        ORC_TEST_FIXTURE_VIEW_BUILD_DIR="${ORC_TEST_FIXTURE_VIEW_BUILD_DIR}")
add_dependencies(orc-core-unit-tests-view-loader orc-test-fixture-views)
gtest_discover_tests(orc-core-unit-tests-view-loader
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "unit\;contracts")

# orc-cli never opens a view binary: with ORC_TEST_VIEW_ABORT_ON_LOAD set the
# fixtures abort the process the moment they are loaded, so these pass only
# if the stage loader's folder scan skips them.
if(TARGET orc-cli)
        foreach(_command list doctor)
                set(_test "CLI.Plugins_${_command}_NeverOpensAViewBinary")
                add_test(NAME ${_test}
                        COMMAND $<TARGET_FILE:orc-cli> plugins ${_command})
                set_tests_properties(${_test} PROPERTIES
                        LABELS "unit;cli"
                        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-views;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-views;APPDATA=${CMAKE_CURRENT_BINARY_DIR}/test-home-views;ORC_STAGE_PLUGIN_PATHS=${ORC_TEST_FIXTURE_VIEW_BUILD_DIR};ORC_TEST_VIEW_ABORT_ON_LOAD=1")
        endforeach()
endif()

add_executable(orc-core-unit-tests-loader-rejection
        contracts/stage_plugin_loader_version_rejection_test.cpp)
target_include_directories(orc-core-unit-tests-loader-rejection PRIVATE
        ${CMAKE_SOURCE_DIR}/orc/sdk/include)
target_link_libraries(orc-core-unit-tests-loader-rejection
        orc-core
        GTest::gmock_main)
target_compile_definitions(orc-core-unit-tests-loader-rejection PRIVATE
        ORC_TEST_FIXTURE_PLUGIN_BUILD_DIR="${ORC_TEST_FIXTURE_PLUGIN_BUILD_DIR}")
add_dependencies(orc-core-unit-tests-loader-rejection
        orc-test-fixture-bad_abi_version
        orc-test-fixture-bad_api_version
        orc-test-fixture-abi_v4
        orc-test-fixture-bad_toolchain_tag
        orc-test-fixture-compatible
        orc-test-fixture-duplicate_first
        orc-test-fixture-duplicate_second)
gtest_discover_tests(orc-core-unit-tests-loader-rejection
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "unit\;contracts")

orc_add_core_unit_tests(
        orc-core-unit-tests-plugin-keepalive
        "unit;contracts"

        contracts/stage_plugin_keepalive_test.cpp
)

# Q-8: 50/15 us audio de-emphasis filter unit test. Links the shared EFM decode
# library directly, as the DSP class is internal to orc-efm-decode and not
# reachable through orc-core (orc-efm-decode's PUBLIC include dirs bring in the
# decoder/efm-lib headers).
add_executable(orc-core-unit-tests-efm-deemphasis
        stages/efm_common/audio_deemphasis_test.cpp)
target_link_libraries(orc-core-unit-tests-efm-deemphasis
        orc-efm-decode
        GTest::gmock_main)
gtest_discover_tests(orc-core-unit-tests-efm-deemphasis
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "unit\;sinks")

# CIRC codeword scoring: warm-up / drain padding must be excluded from the
# C1/C2 valid-fixed-error figures. Links the shared EFM decode library directly,
# as with the de-emphasis DSP above.
add_executable(orc-core-unit-tests-efm-reedsolomon-padding
        stages/efm_common/reedsolomon_padding_test.cpp)
target_link_libraries(orc-core-unit-tests-efm-reedsolomon-padding
        orc-efm-decode
        GTest::gmock_main)
gtest_discover_tests(orc-core-unit-tests-efm-reedsolomon-padding
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "unit\;sinks")

# Issue #307: the producer's per-t-value doubt must survive EFM framing and be
# attributed to the F3 symbols it contributed to, and the CIRC must seed C1/C2
# erasures from it without ever exceeding the code's erasure capacity.
add_executable(orc-core-unit-tests-efm-doubt-attribution
        stages/efm_common/efm_doubt_attribution_test.cpp)
target_link_libraries(orc-core-unit-tests-efm-doubt-attribution
        orc-efm-decode
        GTest::gmock_main)
gtest_discover_tests(orc-core-unit-tests-efm-doubt-attribution
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "unit\;sinks")

add_executable(orc-core-unit-tests-efm-doubt-erasure
        stages/efm_common/reedsolomon_doubt_erasure_test.cpp)
target_link_libraries(orc-core-unit-tests-efm-doubt-erasure
        orc-efm-decode
        GTest::gmock_main)
gtest_discover_tests(orc-core-unit-tests-efm-doubt-erasure
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "unit\;sinks")

# End-of-stream Q-channel metadata: a trailing run of CRC-failure sections must
# have its timeline continued from the last valid section instead of being
# emitted with the default (track 0, 00:00:00) metadata.
add_executable(orc-core-unit-tests-efm-f2-tail
        stages/efm_common/f2_section_correction_tail_test.cpp)
target_link_libraries(orc-core-unit-tests-efm-f2-tail
        orc-efm-decode
        GTest::gmock_main)
gtest_discover_tests(orc-core-unit-tests-efm-f2-tail
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "unit\;sinks")

# R-5 sector address integrity: an unverifiable header address must be rebuilt
# from the Q-channel timeline, and a gap filled only when that timeline agrees
# data is actually missing.
add_executable(orc-core-unit-tests-efm-sector-address
        stages/efm_common/sector_address_integrity_test.cpp)
target_link_libraries(orc-core-unit-tests-efm-sector-address
        orc-efm-decode
        GTest::gmock_main)
gtest_discover_tests(orc-core-unit-tests-efm-sector-address
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "unit\;sinks")

# Issue 342: sync acquisition must wait for a full sector after the sync
# before judging it, rather than reading past the end of the flag buffers.
add_executable(orc-core-unit-tests-efm-data24-sector-sync
        stages/efm_common/data24_to_raw_sector_sync_test.cpp)
target_link_libraries(orc-core-unit-tests-efm-data24-sector-sync
        orc-efm-decode
        GTest::gmock_main)
gtest_discover_tests(orc-core-unit-tests-efm-data24-sector-sync
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "unit\;sinks")

# Q-8 Q-channel control voting: a track's control flags must be the majority
# verdict of its sections, and an unassigned control nybble must not vote.
add_executable(orc-core-unit-tests-efm-control-vote
        stages/efm_common/q_channel_control_vote_test.cpp)
target_link_libraries(orc-core-unit-tests-efm-control-vote
        orc-efm-decode
        GTest::gmock_main)
gtest_discover_tests(orc-core-unit-tests-efm-control-vote
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "unit\;sinks")

# R-8 bad-sector map coordinates: the .bsm must index the image it accompanies,
# not the disc's address space, or it mis-indexes past any discontinuity.
add_executable(orc-core-unit-tests-efm-sector-map
        stages/efm_common/sector_map_coordinates_test.cpp)
target_link_libraries(orc-core-unit-tests-efm-sector-map
        orc-efm-decode
        GTest::gmock_main)
gtest_discover_tests(orc-core-unit-tests-efm-sector-map
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "unit\;sinks")

# R-3 timeline re-baselining: a forward jump the size of a real LaserDisc skip
# must be reconstructed rather than resynced, and a jump too large to justify
# must be counted and marked instead of silently shortening the output.
add_executable(orc-core-unit-tests-efm-f2-resync
        stages/efm_common/f2_section_correction_resync_test.cpp)
target_link_libraries(orc-core-unit-tests-efm-f2-resync
        orc-efm-decode
        GTest::gmock_main)
gtest_discover_tests(orc-core-unit-tests-efm-f2-resync
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "unit\;sinks")

# No Timecodes mode: a stream whose Q-channel never decodes (early CAV discs
# pre-dating the EFM timecode specification) must be given a synthesised
# timeline and emitted rather than being silently discarded.
add_executable(orc-core-unit-tests-efm-f2-no-timecodes
        stages/efm_common/f2_section_correction_no_timecodes_test.cpp)
target_link_libraries(orc-core-unit-tests-efm-f2-no-timecodes
        orc-efm-decode
        GTest::gmock_main)
gtest_discover_tests(orc-core-unit-tests-efm-f2-no-timecodes
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "unit\;sinks")

# F3-to-F2 section framing: boundary repair must never drop, reorder or pad
# frames when the underlying stream is intact. Links the shared EFM decode
# library directly, as with the de-emphasis DSP above.
add_executable(orc-core-unit-tests-efm-section-framing
        stages/efm_common/f3frametof2section_test.cpp)
target_link_libraries(orc-core-unit-tests-efm-section-framing
        orc-efm-decode
        GTest::gmock_main)
gtest_discover_tests(orc-core-unit-tests-efm-section-framing
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "unit\;sinks")

# Audio/video sync head alignment (issue #231): the writer-level pad/trim
# primitive and the decoder head-loss counters (t-value bits and F3 frames
# consumed before first output) that the alignment offset is computed from.
# Links the shared EFM decode library directly, as with the de-emphasis DSP.
add_executable(orc-core-unit-tests-efm-sync-head-offset
        stages/efm_common/audio_sync_head_offset_test.cpp)
target_link_libraries(orc-core-unit-tests-efm-sync-head-offset
        orc-efm-decode
        GTest::gmock_main)
gtest_discover_tests(orc-core-unit-tests-efm-sync-head-offset
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "unit\;sinks")

# End-to-end boundary attribution: a stream that is clean by construction is run
# through the real F2 -> F1 -> Data24 -> Audio -> concealment chain and must
# report no input defects at all. Guards the regression where drain codewords
# leaked genuine-looking corrupt bytes into the graded figures.
add_executable(orc-core-unit-tests-efm-circ-boundary
        stages/efm_common/circ_boundary_attribution_test.cpp)
target_link_libraries(orc-core-unit-tests-efm-circ-boundary
        orc-efm-decode
        GTest::gmock_main)
gtest_discover_tests(orc-core-unit-tests-efm-circ-boundary
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "unit\;sinks")

# Concealment loss attribution: structural de-interleave filler vs genuine loss,
# broken down by disc region on the latency-compensated audio timeline.
add_executable(orc-core-unit-tests-efm-audio-correction-regions
        stages/efm_common/audio_correction_regions_test.cpp)
target_link_libraries(orc-core-unit-tests-efm-audio-correction-regions
        orc-efm-decode
        GTest::gmock_main)
gtest_discover_tests(orc-core-unit-tests-efm-audio-correction-regions
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "unit\;sinks")

# P-9: bounded blocking queue used to hand F2 sections from the EFM front-end
# thread to the back-end thread. Links the shared EFM decode library directly
# (the queue header is internal to orc-efm-decode, as with the de-emphasis DSP).
add_executable(orc-core-unit-tests-efm-bounded-queue
        stages/efm_common/bounded_queue_test.cpp)
target_link_libraries(orc-core-unit-tests-efm-bounded-queue
        orc-efm-decode
        GTest::gmock_main)
gtest_discover_tests(orc-core-unit-tests-efm-bounded-queue
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "unit\;sinks")

orc_add_core_unit_tests(
        orc-core-unit-tests-metadata
        "unit"

        metadata/video_system_parsing_test.cpp
        metadata/project_plugin_metadata_test.cpp
        metadata/stage_plugin_registry_test.cpp
        metadata/stage_registry_trust_test.cpp
        metadata/plugin_remote_loader_test.cpp
        metadata/plugin_artifact_name_test.cpp
        metadata/plugin_release_manifest_test.cpp
        metadata/plugin_index_client_test.cpp
        metadata/plugin_update_checker_test.cpp
        metadata/sha256_hash_test.cpp
        metadata/skeleton_plugin_registry_test.cpp
        metadata/dropout_util_test.cpp
)

# Phase 11: project format version 2.0 enforcement tests (label: unit)
orc_add_core_unit_tests(
        orc-core-unit-tests-project-format
        "unit"

        project/project_format_test.cpp
        project/relocated_stages_test.cpp
        project/node_label_test.cpp
        project/stacker_mode_test.cpp
        project/tool_refresh_yaml_test.cpp
        project/trigger_node_parameters_test.cpp
        project/trigger_node_measurements_test.cpp
        project/trigger_nodes_test.cpp
        project/sink_read_pacer_test.cpp
)

# Phase 1: core type and signal constant tests (label: unit)
orc_add_core_unit_tests(
        orc-core-unit-tests-types
        "unit"

        types/frame_id_test.cpp
        types/cvbs_signal_constants_test.cpp
        types/line_numbering_test.cpp
        types/frame_numbering_test.cpp
        types/amplitude_conversion_test.cpp
        types/lru_cache_test.cpp
        types/efm_tvalue_packing_test.cpp
)

orc_add_core_unit_tests(
        orc-core-unit-tests-observers
        "unit"

        observers/biphase_observer_test.cpp
        observers/cav_picture_number_test.cpp
        observers/closed_caption_observer_test.cpp
        observers/clv_picture_number_test.cpp
        observers/colour_frame_phase_observer_test.cpp
        observers/core_observation_service_test.cpp
        observers/white_snr_observer_test.cpp
)

# SDK support-tier algorithm helpers (synthesis-based, no I/O).
orc_add_core_unit_tests(
        orc-core-unit-tests-support
        "unit"

        support/log_destination_test.cpp
        support/eia608_service_demux_test.cpp
        support/eia608_line21_pairs_test.cpp
        support/eia608_decoder_test.cpp
        support/frame_read_ahead_test.cpp
        support/frame_cache_test.cpp
        support/frame_content_test.cpp
        support/stack_measurement_test.cpp
        support/composite_split_test.cpp
        support/symmetric_fir_test.cpp
        support/fm_beat_test.cpp
        support/observation_pass_test.cpp
        support/pal_line_locked_resample_test.cpp
)

orc_add_core_unit_tests(
        orc-core-unit-tests-provenance
        "unit"

        provenance/frame_provenance_test.cpp
)

orc_add_core_unit_tests(
        orc-core-unit-tests-observation
        "unit"

        observation/observation_store_test.cpp
        observation/observation_store_persistence_test.cpp
        observation/store_backed_observation_context_test.cpp
        observation/shared_dag_copies_test.cpp
        observation/observation_invalidation_test.cpp
        observation/observer_pass_test.cpp
        observation/observation_scheduler_test.cpp
        observation/sink_fallback_output_index_test.cpp
        observation/observation_cache_retention_test.cpp
        observation/renderer_observation_bound_test.cpp
        observation/parallel_frame_observation_test.cpp
)

        orc_add_core_unit_tests(
                orc-core-unit-tests-preview-foundation
                "unit;preview"

                preview_types/preview_types_test.cpp
                preview_types/preview_carrier_conversion_test.cpp
                preview_types/preview_frame_layout_test.cpp
                preview_types/preview_helpers_test.cpp
                preview_types/preview_navigation_hint_test.cpp
                preview_types/preview_plane_delivery_test.cpp
                preview_types/preview_view_registry_test.cpp
                preview_types/node_representation_resolution_test.cpp
        )

# The videosynth suite's measurement and baseline helpers
# (orc-tests/videosynth/support). The suite itself is local only; its helpers
# are pure arithmetic, so they are checked here on synthetic arrays.
add_executable(orc-core-unit-tests-videosynth-support
        videosynth_support/truth_comparison_test.cpp
        videosynth_support/baselines_test.cpp
        videosynth_support/decoded_comparison_test.cpp
        videosynth_support/picture_truth_test.cpp
        videosynth_support/still_metrics_test.cpp)
target_link_libraries(orc-core-unit-tests-videosynth-support
        orc-videosynth-support
        GTest::gmock_main)
gtest_discover_tests(orc-core-unit-tests-videosynth-support
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "unit")
