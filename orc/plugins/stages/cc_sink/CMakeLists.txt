set(STAGE_PLUGIN_SOURCES
    cc_sink_stage.cpp
    cc_sink_stage_deps.cpp
    cc_line21_report.cpp
    cc_xds_decoder.cpp
    plugin.cpp
)

orc_add_stage_plugin(
    orc-stage-plugin-cc-sink
    OUTPUT_NAME orc-stage-plugin-cc-sink
    PLUGIN_VERSION "${ORC_VERSION}"
    SOURCES ${STAGE_PLUGIN_SOURCES}
)
