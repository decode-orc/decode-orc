set(STAGE_PLUGIN_SOURCES
    plugin.cpp
    teletext_sink_stage.cpp
    teletext_sink_deps.cpp
    teletext_frame_slicer.cpp
    teletext_scan_state.cpp
    teletext_block_scanner.cpp
    teletext_page_catalogue.cpp
    teletext_squash_stats.cpp
    teletext_unplaced_rows.cpp
    teletext_data_channels.cpp
    teletext_page_format_clear.cpp
    teletext_catalogue_view.cpp
)

orc_add_stage_plugin(
    orc-stage-plugin-teletext-sink
    OUTPUT_NAME orc-stage-plugin-teletext-sink
    PLUGIN_VERSION "${ORC_VERSION}"
    SOURCES ${STAGE_PLUGIN_SOURCES}
    # WST slicing, page decoding, row squashing and recovery statistics, shared
    # with nabts_sink (see orc/plugins/stages/common/vbi-services).
    LINK_LIBRARIES orc-vbi-services
)
