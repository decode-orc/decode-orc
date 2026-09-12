add_library(orc-core SHARED
    # Phase 1: Foundations
    field_id.cpp
    artifact.cpp
    dag_executor.cpp
    frame_provenance.cpp
    dag_frame_renderer.cpp
    preview_frame_layout.cpp
    preview_renderer.cpp
    preview_view_registry.cpp

    # Observation system
    observation_context.cpp
    core_observation_service.cpp
    pipeline_validator.cpp
    
    # Abstract factories
    factories.cpp

    
    # VBI utilities (vbi_utilities.cpp lives in orc-sdk-support)
    vbi_decoder.cpp

    # Observers (host-internal). The standard observer set is host-owned and
    # reached by plugins through IObservationService (see core_observation_service);
    # the concrete classes and their headers no longer cross the plugin boundary.
    observers/biphase_observer.cpp
    observers/closed_caption_observer.cpp
    observers/burst_level_observer.cpp
    observers/white_snr_observer.cpp
    observers/black_psnr_observer.cpp
    observers/fm_code_observer.cpp
    observers/white_flag_observer.cpp
    observers/colour_frame_phase_observer.cpp
    observers/field_quality_observer.cpp

    # Analysis framework
    analysis/analysis_registry.cpp
    analysis/analysis_init.cpp

    # Shared display decimation for analysis-sink graph paths
    analysis/analysis_series_decimator.cpp

    # Parallel observer sweep shared by the whole-recording analyses
    analysis/observation_pass.cpp

    # Vectorscope analysis
    analysis/vectorscope/composite_vectorscope.cpp
    analysis/vectorscope/vectorscope_analysis.cpp
    
    # Field/frame mapping analysis
    analysis/disc_mapper/disc_mapper_analysis.cpp
    analysis/disc_mapper/disc_mapper_analyzer.cpp
    analysis/disc_mapper/frame_quality_score.cpp
    analysis/frame_map_range/frame_map_range_analysis.cpp
    analysis/frame_map_range/frame_map_range_search.cpp
    
    # Frame corruption analysis (testing tool)
    analysis/frame_corruption/frame_corruption_analyzer.cpp
    analysis/frame_corruption/frame_corruption_analysis.cpp
    
    # Dropout editor tool
    analysis/dropout/dropout_editor_tool.cpp
    
    # Source alignment analysis
    analysis/source_alignment/source_alignment_analysis.cpp
    
    # Mask line configuration
    analysis/mask_line/mask_line_analysis.cpp
    
    # FFmpeg preset configuration
    analysis/ffmpeg_preset/ffmpeg_preset_analysis.cpp
    
    # Core stage interfaces and factories only (implementations live in runtime plugins)
    stages/stage_factories.cpp

    # Dropout decision utilities
    dropout_decision.cpp
    
    # Observation cache (shared by all analysis decoders)
    carrier_scope_extraction.cpp
    observation_cache.cpp

    # Provenance-keyed observation store (survives DAG rebuilds)
    observation_store.cpp

    # Read-through context over the store (per-field lazy loading for
    # triggered sinks)
    store_backed_observation_context.cpp

    # SQLite-backed observation sidecar (survives application restarts)
    sqlite_observation_persistence.cpp

    # Retention policy for the per-source sidecar cache directory
    observation_cache_retention.cpp

    # Observation change propagation and store garbage collection
    observation_invalidation.cpp

    # Background observation scheduler (computes missing observations ahead of
    # demand on a worker thread)
    observation_scheduler.cpp

    # Node type system
    node_type.cpp
    
    # Stage parameters
    stage_parameter.cpp

    # Stage plugin runtime loading
    stage_plugin_loader.cpp
    stage_plugin_registry.cpp
    plugin_remote_loader.cpp
    plugin_artifact_name.cpp
    plugin_release_manifest.cpp
    sha256_hash.cpp

    # Curated plugin index discovery
    curl_http_fetcher.cpp
    plugin_index.cpp
    plugin_index_client.cpp
    plugin_update_checker.cpp

    # Project file I/O
    project.cpp
    
    # Project to DAG conversion
    stage_registry.cpp
    project_to_dag.cpp
)

# the scope needs to be PUBLIC so the unit test project can share these include
# directory rules. All entries are BUILD_INTERFACE-only: orc-core is exported
# in the decode-orc-plugin-sdk package as a link-only target (plugins compile
# against the SDK headers, never against core's private headers), so its
# installed interface deliberately carries no include directories.
target_include_directories(orc-core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/stages>
    # Access to view types (MVP architecture allows core -> view dependency)
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../view-types>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/observers>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/analysis>
    # Plugin SDK headers (<orc/plugin/...>): stage plugin headers include them
    # by angle path, and core (stage_factories.cpp) and the test tree compile
    # against plugin stage headers directly.
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../sdk/include>
    $<BUILD_INTERFACE:${EZPWD_INCLUDE_DIR}>
)

# =============================================================================
# MVP Architecture - Core Headers are PRIVATE
# =============================================================================
# Core headers are marked PRIVATE to prevent transitive inclusion.
# Only orc-presenters should directly access core headers.
# GUI/CLI code accesses core functionality through presenters only.
# =============================================================================
# Note: All include directories above are PRIVATE by default
# =============================================================================

# Link against common types
target_link_libraries(orc-core PUBLIC orc-common)

# Link the SDK support-tier helpers (eia608_decoder, dropout_util,
# preview_helpers, colour_preview_conversion). These sources moved out of
# orc-core into the standalone orc-sdk-support static library (orc/sdk/src) so
# stage plugins can resolve them without linking the host; orc-core links the
# same archive so host behaviour is unchanged. PUBLIC so the in-tree unit tests
# that link orc-core also resolve these helper symbols. Defined in orc/sdk (a
# later add_subdirectory); the reference is resolved at generate time.
target_link_libraries(orc-core PUBLIC orc-sdk-support)

# Link fmt (used by EFM decoder pipeline sources compiled directly into orc-core)
target_link_libraries(orc-core PUBLIC fmt::fmt)

# Warning flags
# Note: -Wno-cpp suppresses #warning pragmas from system headers in Debug builds.
# On NixOS the gcc-wrapper unconditionally injects -D_FORTIFY_SOURCE=2 (hardening),
# but Debug builds use -O0, which causes glibc features.h to emit a #warning.
# Since this is a Nix environment issue and not a code quality problem, we silence
# it with -Wno-cpp (applies only to #warning/#pragma directives in system headers).
target_compile_options(orc-core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:/Od /Zi>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/O2>
    $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<CONFIG:Debug>>:-g -O0 -Wno-cpp>
    $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<CONFIG:Release>>:-O3>
)

# Set compile-time spdlog filter level per build type.
# Release builds compile down to DEBUG level (info/warn/error remain available
# and debug output can be enabled at runtime via --log-level).  Debug/unset
# builds go all the way to TRACE so every severity is reachable at runtime.
target_compile_definitions(orc-core PRIVATE
    $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG>
    $<$<NOT:$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
    # MSVC does not define ssize_t (a POSIX type); define it as ptrdiff_t so
    # the ezpwd-reed-solomon headers (used by reedsolomon.cpp / rspc.cpp) compile
    # correctly on Windows.
    $<$<CXX_COMPILER_ID:MSVC>:ssize_t=ptrdiff_t>
    ORC_STAGE_PLUGIN_BUILD_DIR="${ORC_STAGE_PLUGIN_BUILD_DIR}"
)

set_target_properties(orc-core PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

if(APPLE)
    set_target_properties(orc-core PROPERTIES
        MACOSX_RPATH ON
        INSTALL_NAME_DIR "@rpath"
    )
elseif(UNIX AND NOT WIN32)
    set_target_properties(orc-core PROPERTIES
        INSTALL_RPATH "\$ORIGIN"
    )
endif()

# Find and link SQLite3. Promoted to a global target so stage plugins
# (sibling directory scope) can declare it in their own LINK_LIBRARIES:
# the SDK no longer propagates orc-core's third-party dependencies.
find_package(SQLite3 REQUIRED)
set_target_properties(SQLite::SQLite3 PROPERTIES IMPORTED_GLOBAL TRUE)
target_link_libraries(orc-core PUBLIC SQLite::SQLite3)

# Find and link yaml-cpp
find_package(yaml-cpp REQUIRED)
# Create an alias if yaml-cpp::yaml-cpp doesn't exist (for compatibility)
if(TARGET yaml-cpp AND NOT TARGET yaml-cpp::yaml-cpp)
    add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
endif()
target_link_libraries(orc-core PUBLIC yaml-cpp::yaml-cpp)

# Find and link spdlog
find_package(spdlog REQUIRED)
target_link_libraries(orc-core PUBLIC spdlog::spdlog)

# Find and link curl (for downloading remote plugin binaries)
find_package(CURL REQUIRED)
target_link_libraries(orc-core PRIVATE CURL::libcurl)

if(WIN32)
    target_link_libraries(orc-core PRIVATE dbghelp)
endif()

# Find and link libpng
find_package(PNG REQUIRED)
target_link_libraries(orc-core PUBLIC PNG::PNG)

# Find and link FFmpeg libraries (for MP4 output). GLOBAL so the video sink
# stage plugins (sibling directory scope) can declare PkgConfig::FFMPEG in
# their own LINK_LIBRARIES: the SDK no longer propagates orc-core's
# third-party dependencies.
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
    libavcodec
    libavfilter
    libavformat
    libavutil
    libswscale
)

# Create aggregate PkgConfig::FFMPEG target if it doesn't exist
# (pkg_check_modules may create individual targets like PkgConfig::libavcodec)
if(NOT TARGET PkgConfig::FFMPEG)
    add_library(PkgConfig::FFMPEG INTERFACE IMPORTED GLOBAL)
    set(_ffmpeg_pc_libs "")
    foreach(_ffmpeg_pc_target libavcodec libavfilter libavformat libavutil libswscale)
        if(TARGET PkgConfig::${_ffmpeg_pc_target})
            list(APPEND _ffmpeg_pc_libs PkgConfig::${_ffmpeg_pc_target})
        endif()
    endforeach()

    if(_ffmpeg_pc_libs)
        set_target_properties(PkgConfig::FFMPEG PROPERTIES
            INTERFACE_LINK_LIBRARIES "${_ffmpeg_pc_libs}"
        )
    else()
        set_target_properties(PkgConfig::FFMPEG PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES "${FFMPEG_LIBRARIES}"
        )
        if(FFMPEG_LIBRARY_DIRS)
            set_property(TARGET PkgConfig::FFMPEG PROPERTY
                INTERFACE_LINK_DIRECTORIES "${FFMPEG_LIBRARY_DIRS}"
            )
        endif()
    endif()
    unset(_ffmpeg_pc_libs)
endif()

# orc-core is the host runtime library, installed for the app bundle. It is no
# longer part of the decode-orc-plugin-sdk export set: out-of-tree plugins link
# the SDK alone (orc::plugin-sdk + orc::orc-sdk-support), never the host.
if(APPLE)
    install(TARGETS orc-core
        LIBRARY DESTINATION "orc-gui.app/Contents/Frameworks" COMPONENT Runtime
        RUNTIME DESTINATION "orc-gui.app/Contents/MacOS" COMPONENT Runtime
    )
elseif(WIN32)
    install(TARGETS orc-core
        RUNTIME DESTINATION bin COMPONENT Runtime
        LIBRARY DESTINATION lib COMPONENT Runtime
        ARCHIVE DESTINATION lib COMPONENT Runtime
    )
else()
    install(TARGETS orc-core
        LIBRARY DESTINATION lib COMPONENT Runtime
        RUNTIME DESTINATION bin COMPONENT Runtime
    )
endif()

target_compile_definitions(orc-core PUBLIC HAVE_FFMPEG)
target_link_libraries(orc-core PUBLIC PkgConfig::FFMPEG)
