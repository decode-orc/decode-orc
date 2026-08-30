# GUI application
# Requires Qt6

find_package(Qt6 COMPONENTS Core Widgets QUIET)

if(NOT Qt6_FOUND)
    message(WARNING "Qt6 not found, skipping GUI build")
    return()
endif()

# =============================================================================
# Preview audio playback
# =============================================================================
# Audio playback in the preview dialogue needs Qt Multimedia for the output
# device. Everything above the device (the IAudioOutput seam, the chase clock
# and PreviewAudioController) is device-free and always built, so the feature
# can be compiled out on a platform that cannot provide the module without
# disturbing the rest of the GUI: only the QAudioSink-backed implementation
# and the factory that returns it are guarded.
# =============================================================================
option(ORC_GUI_AUDIO_PLAYBACK "Build preview audio playback (needs Qt6 Multimedia)" ON)

if(ORC_GUI_AUDIO_PLAYBACK)
    find_package(Qt6 COMPONENTS Multimedia QUIET)
    if(NOT Qt6Multimedia_FOUND)
        message(WARNING "Qt6 Multimedia not found, building without preview audio playback")
        set(ORC_GUI_AUDIO_PLAYBACK OFF)
    endif()
endif()

# Enable Qt's MOC (Meta-Object Compiler)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_INCLUDE_CURRENT_DIR_IN_INTERFACE ON)

# Generate platform-specific icons from source PNG
include(${CMAKE_SOURCE_DIR}/cmake/GenerateIcons.cmake)
set(ICON_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/icons")
generate_platform_icons(
    "${CMAKE_SOURCE_DIR}/assets/orc-gui-icon-256.png"
    "${ICON_OUTPUT_DIR}"
)

# =============================================================================
# GUI Application Sources
# =============================================================================

set(ORC_GUI_LIB_SOURCES
    mainwindow.cpp
    mainwindow.h
    mainwindow_coordinator_callbacks.cpp
    render_coordinator.cpp
    render_coordinator.h
    guiproject.cpp
    guiproject.h
    orcgraphmodel.cpp
    orcgraphmodel.h
    orcgraphicsscene.cpp
    orcgraphicsscene.h
    orcgraphicsview.cpp
    orcgraphicsview.h
    orcnodepainter.cpp
    orcnodepainter.h
    fieldpreviewwidget.cpp
    fieldpreviewwidget.h
    frame_marker_slider.cpp
    frame_marker_slider.h
    frame_view_geometry.cpp
    frame_view_geometry.h
    frame_viewport_widget.cpp
    frame_viewport_widget.h
    preview_image_qt.cpp
    preview_image_qt.h
    previewdialog.cpp
    previewdialog.h

    # Preview audio playback (device-free part; see ORC_GUI_AUDIO_PLAYBACK)
    audio_output.cpp
    audio_output.h
    preview_audio_chase.h
    preview_audio_controller.cpp
    preview_audio_controller.h

    framescopedialog.cpp
    framescopedialog.h
    line_navigation_mapper.cpp
    line_navigation_mapper.h
    node_navigation_mapper.cpp
    node_navigation_mapper.h
    stageparameterdialog.cpp
    stageparameterdialog.h
    projectpropertiesdialog.cpp
    projectpropertiesdialog.h
    node_type_helper.cpp
    node_type_helper.h
    quick_project_planner.cpp
    quick_project_planner.h

    # Field/Frame presentation helpers
    field_frame_presentation.cpp
    field_frame_presentation.h
    
    # Preview dialogs
    preview/vectorscope_dialog.cpp
    preview/vectorscope_dialog.h
    preview/vectorscope_geometry.h
    preview/histogram_dialog.cpp
    preview/histogram_dialog.h
    generic_analysis_dialog.cpp
    generic_analysis_dialog.h
    
    # VBI dialog
    vbidialog.cpp
    vbidialog.h

    # Video parameter observer dialog
    videoparameterobserverdialog.cpp
    videoparameterobserverdialog.h

    # NTSC observer dialog
    ntscobserverdialog.cpp
    ntscobserverdialog.h

    # Generic catalogue browser: the viewer for any stage advertising
    # StageToolKind::CatalogueBrowser, plus the two payload renderers it
    # switches between and the flash phase they share.
    catalogue_flash_clock.cpp
    catalogue_flash_clock.h
    cataloguecellgridwidget.cpp
    cataloguecellgridwidget.h
    cataloguedisplaylistwidget.cpp
    cataloguedisplaylistwidget.h
    cataloguedialog.cpp
    cataloguedialog.h

    # The teletext character generator's face, which the cell grid draws
    # alphanumeric cells from. teletext_font.cpp is generated; see
    # tools/generate_teletext_font.py.
    teletext_font.cpp
    teletext_font.h
    teletext_glyph_painter.cpp
    teletext_glyph_painter.h

    # Closed caption preview dialog
    closed_caption_assembler.cpp
    closed_caption_assembler.h
    closedcaptiondialog.cpp
    closedcaptiondialog.h

    # Frame timing dialog
    frametimingdialog.cpp
    frametimingdialog.h
    frametimingwidget.cpp
    frametimingwidget.h

    # Waveform monitor dialog
    waveformmonitordialog.cpp
    waveformmonitordialog.h
    waveformmonitorwidget.cpp
    waveformmonitorwidget.h
    
    # Plot widget
    plotwidget.cpp
    plotwidget.h
    theme_color_tokens.h
    theme_controller.cpp
    theme_controller.h
    theme_manager.cpp
    theme_manager.h
    
    # Analysis dialog base class
    analysisdialogbase.cpp
    analysisdialogbase.h
    
    # Config dialog base class
    configdialogbase.cpp
    configdialogbase.h
    
    # Mask line config dialog
    masklineconfigdialog.cpp
    masklineconfigdialog.h
    
    # FFmpeg preset config dialog
    ffmpegpresetdialog.cpp
    ffmpegpresetdialog.h
    
    # Dropout analysis dialog
    dropoutanalysisdialog.cpp
    dropoutanalysisdialog.h
    
    # Dropout editor dialog
    dropout_editor_dialog.cpp
    dropout_editor_dialog.h
    
    # SNR analysis dialog
    snranalysisdialog.cpp
    snranalysisdialog.h
    
    # Burst level analysis dialog
    burstlevelanalysisdialog.cpp
    burstlevelanalysisdialog.h
    
    # Plugin manager dialog
    pluginmanagerdialog.cpp
    pluginmanagerdialog.h

    # Plugin registry management model (presenter-boundary seam)
    pluginmanagermodel.h

    # Curated plugin index browse dialog and model
    pluginbrowsemodel.cpp
    pluginbrowsemodel.h
    pluginbrowsedialog.cpp
    pluginbrowsedialog.h

    # Plugin trust confirmation dialog
    plugintrustdialog.cpp
    plugintrustdialog.h

    # Stage help dialog
    stage_help_dialog.cpp
    stage_help_dialog.h

    # Diagnostic logging configuration (Tools > Logging)
    logging_settings.cpp
    logging_settings.h
    logging_controller.cpp
    logging_controller.h
    logging_settings_dialog.cpp
    logging_settings_dialog.h

    # Resources
    orc_gui_resources.qrc
)

if(ORC_GUI_AUDIO_PLAYBACK)
    list(APPEND ORC_GUI_LIB_SOURCES
        qt_audio_output.cpp
        qt_audio_output.h
    )
endif()

add_library(orc-gui-lib STATIC ${ORC_GUI_LIB_SOURCES})

add_executable(orc-gui MACOSX_BUNDLE
    main.cpp
)

if(APPLE)
    set_target_properties(orc-gui PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
elseif(UNIX AND NOT WIN32)
    set_target_properties(orc-gui PROPERTIES INSTALL_RPATH "\$ORIGIN/../lib;\$ORIGIN/../lib/orc-stage-plugins")
endif()

# =============================================================================
# Link Libraries
# =============================================================================
# The GUI accesses core functionality exclusively through the presenter layer.
# Direct linking to orc-core is intentionally avoided to enforce architectural
# boundaries. The presenter layer handles all conversions between public API
# types and internal core types.
# =============================================================================
if(TARGET QtNodes::QtNodes)
    set(QTNODES_TARGET QtNodes::QtNodes)
elseif(TARGET QtNodes)
    set(QTNODES_TARGET QtNodes)
else()
    message(FATAL_ERROR "QtNodes target not found")
endif()

target_link_libraries(orc-gui-lib PUBLIC
    orc-common           # Common types shared across all layers
    orc-view-types       # Shared presentation layer data structures
    orc-presenters       # Presenter layer (privately links orc-core)
    Qt6::Core
    Qt6::Widgets
    ${QTNODES_TARGET}
)

if(ORC_GUI_AUDIO_PLAYBACK)
    # PUBLIC so the GUI test targets that link orc-gui-lib inherit the module.
    target_link_libraries(orc-gui-lib PUBLIC Qt6::Multimedia)
    target_compile_definitions(orc-gui-lib PUBLIC ORC_GUI_AUDIO_PLAYBACK)
endif()

target_link_libraries(orc-gui PRIVATE orc-gui-lib)

# QtNodes may be provided as a static library (e.g., Nix dev shells).
# Ensure NODE_EDITOR_STATIC is defined for consumers in that case.
if(TARGET ${QTNODES_TARGET})
    get_target_property(QTNODES_IMPORTED ${QTNODES_TARGET} IMPORTED)
    if(QTNODES_IMPORTED)
        get_target_property(QTNODES_LOCATION ${QTNODES_TARGET} IMPORTED_LOCATION)
        if(NOT QTNODES_LOCATION)
            get_target_property(QTNODES_LOCATION ${QTNODES_TARGET} IMPORTED_LOCATION_RELEASE)
        endif()
        if(QTNODES_LOCATION MATCHES "\\.a$")
            target_compile_definitions(orc-gui-lib PUBLIC NODE_EDITOR_STATIC)
        endif()
    else()
        get_target_property(QTNODES_TYPE ${QTNODES_TARGET} TYPE)
        if(QTNODES_TYPE STREQUAL "STATIC_LIBRARY")
            target_compile_definitions(orc-gui-lib PUBLIC NODE_EDITOR_STATIC)
        endif()
    endif()
endif()

# =============================================================================
# Compile Definitions
# =============================================================================
# ORC_GUI_BUILD: Enables compile-time guards in core headers that prevent
#                direct inclusion from GUI code. If GUI code attempts to
#                include a core header, compilation will fail with a message
#                directing the developer to use the appropriate presenter or
#                public API type instead.
# =============================================================================
target_compile_definitions(orc-gui-lib PRIVATE
    $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG>
    $<$<NOT:$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
    ORC_GUI_BUILD
)

target_compile_definitions(orc-gui PRIVATE
    $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG>
    $<$<NOT:$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
    ORC_GUI_BUILD
)

# =============================================================================
# Include Directories
# =============================================================================
# The GUI has access to:
# - Public API (orc/public): Stable interface types for rendering, analysis, etc.
# - Presenters (orc/presenters/include): Application logic layer
# - Common types (orc/common/include): Types shared across all layers
# - Generated files: Build version information
#
# Core implementation headers (orc/core) are intentionally NOT in the include
# path. This ensures GUI code cannot directly access internal implementation
# details. If you need access to core functionality, add it to the appropriate
# presenter or create a new public API type.
# =============================================================================
target_include_directories(orc-gui-lib PUBLIC
    ${CMAKE_SOURCE_DIR}/orc/public
    ${CMAKE_SOURCE_DIR}/orc/presenters/include
    ${CMAKE_SOURCE_DIR}/orc/common/include
    ${CMAKE_SOURCE_DIR}/orc
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(orc-gui-lib PRIVATE
    ${CMAKE_BINARY_DIR}/generated
)

target_include_directories(orc-gui PRIVATE
    ${CMAKE_BINARY_DIR}/generated
)

if(APPLE)
    # Configure the custom Info.plist from template
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
        "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
        @ONLY
    )
    
    # Set bundle properties for macOS
    set_target_properties(orc-gui PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "Decode Orc"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.decode_orc.decode-orc"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_LONG_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "orc-gui.icns"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
        MACOSX_BUNDLE_INFO_STRING "Cross-platform orchestration framework for LaserDisc and tape decoding workflows"
        MACOSX_BUNDLE_COPYRIGHT "Copyright © 2024-2026 Simon Inns"
    )
    
    # Copy the icon into the bundle's Resources directory if available
    if(MACOS_ICON_FILE)
        set_source_files_properties(${MACOS_ICON_FILE} PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources"
        )
        target_sources(orc-gui PRIVATE ${MACOS_ICON_FILE})
        
        add_custom_target(generate_macos_icon DEPENDS ${MACOS_ICON_FILE})
        add_dependencies(orc-gui generate_macos_icon)
    endif()
endif()

if(WIN32)
    set(_qt_deploy_hints)
    if(DEFINED VCPKG_INSTALLED_DIR)
        if(DEFINED VCPKG_TARGET_TRIPLET)
            list(APPEND _qt_deploy_hints
                "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/Qt6/bin"
                "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/Qt/bin"
            )
        endif()
        list(APPEND _qt_deploy_hints
            "${VCPKG_INSTALLED_DIR}/x64-windows/tools/Qt6/bin"
            "${VCPKG_INSTALLED_DIR}/x64-windows/tools/Qt/bin"
        )
    endif()

    find_program(WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt
        HINTS ${_qt_deploy_hints}
    )

    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET orc-gui POST_BUILD
            COMMAND "${CMAKE_COMMAND}"
                "-DWINDEPLOYQT_EXECUTABLE=${WINDEPLOYQT_EXECUTABLE}"
                "-DWINDEPLOYQT_CONFIG=$<IF:$<CONFIG:Debug>,debug,release>"
                "-DWINDEPLOYQT_TARGET=$<TARGET_FILE:orc-gui>"
                -P "${CMAKE_SOURCE_DIR}/cmake/RunWindeployqtSafe.cmake"
            COMMENT "Deploying Qt runtime dependencies for orc-gui"
            VERBATIM
        )
    else()
        message(WARNING "windeployqt not found; orc-gui may fail at runtime due to missing Qt plugins")
    endif()

    # Windows-specific executable properties
    if(WINDOWS_ICON_FILE)
        # Create a Windows resource file to embed the icon
        set(WINDOWS_RC_FILE "${CMAKE_CURRENT_BINARY_DIR}/orc-gui.rc")
        file(WRITE ${WINDOWS_RC_FILE}
            "IDI_ICON1 ICON \"${WINDOWS_ICON_FILE}\"\n"
        )
        target_sources(orc-gui PRIVATE ${WINDOWS_RC_FILE})
        
        add_custom_target(generate_windows_icon DEPENDS ${WINDOWS_ICON_FILE})
        add_dependencies(orc-gui generate_windows_icon)
    endif()
    
    # Set Windows executable properties
    set_target_properties(orc-gui PROPERTIES
        WIN32_EXECUTABLE TRUE
        OUTPUT_NAME "orc-gui"
    )

    # Run with UTF-8 as the process code page so the UTF-8 std::string paths
    # the GUI hands to the presenter layer survive the trip to the OS.
    orc_apply_utf8_manifest(orc-gui)
endif()

# Install target
install(TARGETS orc-gui
    BUNDLE DESTINATION . COMPONENT Runtime
    RUNTIME DESTINATION bin COMPONENT Runtime
)

# Linux desktop integration files
if(UNIX AND NOT APPLE)
    install(FILES io.github.decode_orc.decode-orc.desktop
        DESTINATION share/applications
    )

    install(FILES ${CMAKE_SOURCE_DIR}/assets/orc-gui-icon-256.png
        DESTINATION share/icons/hicolor/256x256/apps
        RENAME io.github.decode_orc.decode-orc.png
    )

    if(EXISTS ${CMAKE_SOURCE_DIR}/assets/orc-gui-icon.svg)
        install(FILES ${CMAKE_SOURCE_DIR}/assets/orc-gui-icon.svg
            DESTINATION share/icons/hicolor/scalable/apps
            RENAME io.github.decode_orc.decode-orc.svg
        )
    endif()

    install(FILES io.github.decode_orc.decode-orc.metainfo.xml
        DESTINATION share/metainfo
    )
endif()

if(BUILD_UNIT_TESTS)
    add_test(
        NAME GUI.HelpShowsSafeCorePluginsOption
        COMMAND $<TARGET_FILE:orc-gui> --help
    )
    set_tests_properties(GUI.HelpShowsSafeCorePluginsOption PROPERTIES
        LABELS "unit;gui"
        PASS_REGULAR_EXPRESSION "--safe-core-plugins"
    )
endif()
