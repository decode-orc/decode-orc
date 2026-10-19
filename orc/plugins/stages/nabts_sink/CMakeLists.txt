set(STAGE_PLUGIN_SOURCES
    plugin.cpp
    nabts_sink_stage.cpp
    nabts_sink_deps.cpp
    nabts_frame_slicer.cpp
    nabts_scan_state.cpp
    nabts_block_scanner.cpp
    nabts_packet.cpp
    nabts_data_group.cpp
    nabts_service_census.cpp
    nabts_record.cpp
    nabts_record_catalogue.cpp
    nabts_navigation.cpp
    nabts_caption_stream.cpp
    naplps_code_env.cpp
    naplps_pdi.cpp
    naplps_state.cpp
    naplps_incremental.cpp
    naplps_interpreter.cpp
    naplps_raster.cpp
    naplps_font.cpp
    naplps_palette_timeline.cpp
    naplps_receiver_pace.cpp
    naplps_proportional.cpp
    naplps_dialect_model.cpp
    naplps_reconstruct.cpp
    nabts_record_library.cpp
    nabts_raster_view.cpp
    nabts_catalogue_view.cpp
)

orc_add_stage_plugin(
    orc-stage-plugin-nabts-sink
    OUTPUT_NAME orc-stage-plugin-nabts-sink
    PLUGIN_VERSION "${ORC_VERSION}"
    SOURCES ${STAGE_PLUGIN_SOURCES}
    # System C rides the same slicer as WST, and NAPLPS decoding needs the
    # shared display-list model (see orc/plugins/stages/common/vbi-services).
    LINK_LIBRARIES orc-vbi-services
)
