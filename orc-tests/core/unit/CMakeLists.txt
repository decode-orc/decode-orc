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
                ${CMAKE_SOURCE_DIR}/orc/plugins/stages/nabts_sink
                ${CMAKE_SOURCE_DIR}/orc/plugins/stages/teletext_sink
                ${CMAKE_SOURCE_DIR}/orc/plugins/stages/vbi_source
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

        # WST/NABTS decoding chain. Formerly resolved through orc-core (which
        # linked orc-sdk-support); now a shared plugin-side library, so the
        # test binaries link it directly.
        if(TARGET orc-vbi-services)
                target_link_libraries(${target} orc-vbi-services)
        endif()

        if(TARGET orc-vbi-source)
                target_link_libraries(${target} orc-vbi-source)
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
                        orc-stage-plugin-cvbs-source
                        orc-stage-plugin-vbi-source
                        orc-stage-plugin-stacker
                        orc-stage-plugin-frame-map
                        orc-stage-plugin-source-join
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
                        orc-stage-plugin-efm-audio-decode
                        orc-stage-plugin-audio-import
                        orc-stage-plugin-audio-channel-map
                        orc-stage-plugin-audio-align
                        orc-stage-plugin-raw-efm-sink
                        orc-stage-plugin-ac3rf-sink
                        orc-stage-plugin-dropout-analysis-sink
                        orc-stage-plugin-snr-analysis-sink
                        orc-stage-plugin-burst-level-analysis-sink
                        orc-stage-plugin-cvbs-sink
                        orc-stage-plugin-teletext-sink
                        orc-stage-plugin-nabts-sink)
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
        stages/vbi_source/vbi_source_stage_test.cpp
        stages/audio_resample/audio_resampler_test.cpp
)

orc_add_core_unit_tests(
        orc-core-unit-tests-transforms
        "unit;transforms"

        stages/stacker/stacker_stage_test.cpp
        stages/frame_map/frame_map_stage_test.cpp
        stages/source_join/source_join_stage_test.cpp
        analysis/frame_map_range_search_test.cpp
        stages/video_params/video_params_stage_test.cpp
        stages/dropout_correct/dropout_correct_stage_test.cpp
        stages/dropout_map/dropout_map_stage_test.cpp
        stages/source_align/source_align_stage_test.cpp
        stages/mask_line/mask_line_stage_test.cpp
        stages/efm_audio_decode/efm_audio_decode_stage_test.cpp
        stages/audio_import/audio_import_stage_test.cpp
        stages/audio_channel_map/audio_channel_map_stage_test.cpp
        stages/audio_align/audio_align_stage_test.cpp
)

orc_add_core_unit_tests(
        orc-core-unit-tests-sinks
        "unit;sinks"

        stages/video_sink/componentframe_test.cpp
        stages/video_sink/vectorscope_analysis_test.cpp
        stages/video_sink/av1_rate_control_test.cpp
        stages/video_sink/video_sink_stage_defaults_test.cpp
        stages/video_sink/video_sink_stage_safety_test.cpp
        stages/video_sink/comb_test.cpp
        stages/video_sink/video_sink_stage_test.cpp
        stages/video_sink/audio_pair_selection_test.cpp
        stages/video_sink/audio_sample_feed_test.cpp
        stages/video_sink/subtitle_embed_policy_test.cpp
        stages/video_sink/monodecoder_test.cpp
        stages/video_sink/ntsc_pal_decoder_wrapper_test.cpp
        stages/video_sink/palcolour_test.cpp
        stages/video_sink/sourcefield_test.cpp
        stages/daphne_vbi_sink/daphne_vbi_writer_util_test.cpp
        stages/daphne_vbi_sink/daphne_vbi_sink_stage_deps_test.cpp
        stages/daphne_vbi_sink/daphne_vbi_sink_stage_test.cpp
        stages/audio_sink/audio_sink_stage_test.cpp
        stages/audio_sink/audio_sink_stage_deps_test.cpp
        stages/ac3rf_sink/ac3rf_sink_stage_test.cpp
        stages/cc_sink/cc_sink_stage_test.cpp
        stages/tbc_sink/tbc_sink_stage_test.cpp
        stages/tbc_sink/tbc_sink_stage_deps_test.cpp
        stages/tbc_sink/tbc_sink_levels_test.cpp
        stages/efm_sink/efm_sink_stage_test.cpp
        stages/raw_efm_sink/raw_efm_sink_stage_test.cpp
        stages/raw_efm_sink/raw_efm_sink_stage_deps_test.cpp
        stages/cvbs_sink/cvbs_sink_stage_test.cpp
        stages/cvbs_sink/cvbs_sink_phase_sequence_test.cpp
        stages/dropout_analysis_sink/dropout_analysis_sink_stage_test.cpp
        stages/dropout_analysis_sink/dropout_analysis_sink_deps_test.cpp
        stages/dropout_analysis_sink/dropout_analysis_sink_map_contract_test.cpp
        stages/dropout_analysis_sink/dropout_analysis_sink_report_test.cpp
        stages/snr_analysis_sink/snr_analysis_sink_stage_test.cpp
        stages/snr_analysis_sink/snr_analysis_sink_deps_test.cpp
        stages/burst_level_analysis_sink/burst_level_analysis_sink_stage_test.cpp
        stages/burst_level_analysis_sink/burst_level_analysis_sink_deps_test.cpp
        stages/nabts_sink/nabts_sink_stage_test.cpp
        stages/nabts_sink/nabts_frame_slicer_test.cpp
        stages/nabts_sink/nabts_packet_test.cpp
        stages/nabts_sink/nabts_data_group_test.cpp
        stages/nabts_sink/nabts_record_test.cpp
        stages/nabts_sink/nabts_record_catalogue_test.cpp
        stages/nabts_sink/nabts_caption_publication_test.cpp
        stages/nabts_sink/nabts_catalogue_view_test.cpp
        stages/nabts_sink/naplps_code_env_test.cpp
        stages/nabts_sink/naplps_font_test.cpp
        stages/nabts_sink/naplps_lint_test.cpp
        stages/nabts_sink/naplps_lint_repair_test.cpp
        stages/nabts_sink/naplps_interpreter_test.cpp
        stages/nabts_sink/naplps_raster_test.cpp
        stages/nabts_sink/naplps_render_grid_test.cpp
        stages/nabts_sink/nabts_scan_state_test.cpp
        stages/teletext_sink/teletext_sink_stage_test.cpp
        stages/teletext_sink/teletext_sink_deps_test.cpp
        stages/teletext_sink/teletext_frame_slicer_test.cpp
        stages/teletext_sink/teletext_scan_state_test.cpp
        stages/teletext_sink/teletext_page_catalogue_test.cpp
        stages/teletext_sink/teletext_catalogue_view_test.cpp
        stages/teletext_sink/teletext_squash_stats_test.cpp
)

orc_add_core_unit_tests(
        orc-core-unit-tests-analysis
        "unit"

        analysis/analysis_series_decimator_test.cpp
        analysis/composite_vectorscope_test.cpp
        analysis/frame_quality_score_test.cpp
)

orc_add_core_unit_tests(
        orc-core-unit-tests-contracts
        "unit;contracts"

        contracts/public_stage_contract_test.cpp
        contracts/stage_registry_contract_test.cpp
        contracts/project_to_dag_contract_test.cpp
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
        presenters/filtergraph_import_test.cpp
        presenters/plugin_details_test.cpp
        presenters/plugin_load_state_test.cpp
        presenters/plugin_selector_test.cpp
        presenters/stage_details_test.cpp
        presenters/representation_audio_stream_reader_test.cpp
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
        orc-test-fixture-compatible)
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

        observers/colour_frame_phase_observer_test.cpp
        observers/core_observation_service_test.cpp
)

# SDK support-tier algorithm helpers (synthesis-based, no I/O).
orc_add_core_unit_tests(
        orc-core-unit-tests-support
        "unit"

        support/log_destination_test.cpp
        support/eia608_service_demux_test.cpp
        support/eia608_decoder_test.cpp
)

# orc-vbi-services: the WST/NABTS decoding chain shared by the teletext_sink
# and nabts_sink plugins. Plugin-side rather than SDK, so these live under
# common/ to mirror orc/plugins/stages/common/vbi-services.
orc_add_core_unit_tests(
        orc-core-unit-tests-vbi-services
        "unit"

        common/vbi-services/nabts_page_test.cpp
        common/vbi-services/nabts_repertoire_test.cpp
        common/vbi-services/teletext_slicer_test.cpp
        common/vbi-services/teletext_page_decoder_test.cpp
        common/vbi-services/teletext_recovery_stats_test.cpp
        common/vbi-services/teletext_row_squasher_test.cpp
        common/vbi-services/vbi_identity_attestation_test.cpp
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
        observation/observation_invalidation_test.cpp
        observation/observer_pass_test.cpp
        observation/observation_scheduler_test.cpp
        observation/sink_fallback_output_index_test.cpp
        observation/observation_cache_retention_test.cpp
)

        orc_add_core_unit_tests(
                orc-core-unit-tests-preview-foundation
                "unit;preview"

                preview_types/preview_types_test.cpp
                preview_types/preview_carrier_conversion_test.cpp
                preview_types/preview_frame_layout_test.cpp
                preview_types/preview_helpers_test.cpp
                preview_types/preview_navigation_hint_test.cpp
                preview_types/preview_view_registry_test.cpp
                preview_types/node_representation_resolution_test.cpp
        )
