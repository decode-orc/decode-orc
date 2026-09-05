set(CHROMA_DECODER_SHARED_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/../common/output_backend.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../common/raw_output_backend.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../common/ffmpeg_output_backend.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../common/video_sink_stage.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../common/disc_metadata_document.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../common/disc_metadata_collect.cpp
)

orc_add_stage_plugin(
    orc-stage-plugin-video-sink
    OUTPUT_NAME orc-stage-plugin-video-sink
    PLUGIN_VERSION "${ORC_VERSION}"
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/plugin.cpp
        ${CHROMA_DECODER_SHARED_SOURCES}
    # FFmpeg is used by the shared output backends (ffmpeg_output_backend.cpp);
    # declared here because the SDK no longer propagates host third-party
    # dependencies to plugins.
    LINK_LIBRARIES
        orc_chroma_decoders
        PkgConfig::FFMPEG
)

target_compile_definitions(orc-stage-plugin-video-sink PRIVATE HAVE_FFMPEG)

target_include_directories(orc-stage-plugin-video-sink PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../common
)
