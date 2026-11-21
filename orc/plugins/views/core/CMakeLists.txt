# orc-view_core: the view binary for the tools of Decode-Orc's own stages.
# It sits beside the stage plugins, where the view loader finds it by name,
# so it is loaded through the same path as any plugin's view. Tool windows
# move into it as their stages answer tool requests (issue #382).
#
# The windows are built into a static library of their own, which the GUI
# unit tests link to drive them through a fake session; the view binary is
# that library plus its entry points.

add_library(orc-view-core-windows STATIC
    analysis_display_series.cpp
    analysis_display_series.h
    analysis_session_controller.cpp
    analysis_session_controller.h
    analysisdialogbase.cpp
    analysisdialogbase.h
    burstlevelanalysisdialog.cpp
    burstlevelanalysisdialog.h
    config_session_controller.cpp
    config_session_controller.h
    configdialogbase.cpp
    configdialogbase.h
    dropout_editor_dialog.cpp
    dropout_editor_dialog.h
    dropoutanalysisdialog.cpp
    dropoutanalysisdialog.h
    ffmpegpresetdialog.cpp
    ffmpegpresetdialog.h
    frame_map_range_window.cpp
    frame_map_range_window.h
    frame_marker_slider.cpp
    frame_marker_slider.h
    masklineconfigdialog.cpp
    masklineconfigdialog.h
    snranalysisdialog.cpp
    snranalysisdialog.h
)
set_target_properties(orc-view-core-windows PROPERTIES
    AUTOMOC ON
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_include_directories(orc-view-core-windows PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(orc-view-core-windows PUBLIC orc-view-support Qt6::Widgets)

add_library(orc-view_core SHARED
    core_view.cpp
)
set_target_properties(orc-view_core PROPERTIES
    # A view binary is named orc-view_<name>, with no lib prefix.
    PREFIX ""
    AUTOMOC ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${ORC_STAGE_PLUGIN_BUILD_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${ORC_STAGE_PLUGIN_BUILD_DIR}"
)
target_link_libraries(orc-view_core PRIVATE orc-view-core-windows)

# At run time the view uses the orc-view-support orc-gui has loaded; the
# search path finds it beside the application should the loader look.
if(APPLE)
    set_target_properties(orc-view_core PROPERTIES
        INSTALL_RPATH "@loader_path/../../Frameworks")
elseif(UNIX)
    set_target_properties(orc-view_core PROPERTIES INSTALL_RPATH "\$ORIGIN/..")
endif()

install(TARGETS orc-view_core
    LIBRARY DESTINATION "${ORC_STAGE_PLUGIN_INSTALL_DIR}" COMPONENT Runtime
    RUNTIME DESTINATION "${ORC_STAGE_PLUGIN_INSTALL_DIR}" COMPONENT Runtime
)

if(TARGET orc-gui)
    add_dependencies(orc-gui orc-view_core)
endif()
