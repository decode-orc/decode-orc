# orc-view-support: widgets orc-gui ships in a shared library for view
# binaries (orc-view_*) to link, so a plugin's tool windows draw with the same
# plot as orc-gui's own and the process holds one copy of each widget class
# (which qobject_cast and the theme refresh rely on).
#
# Headers: <orc/view-support/...> in the SDK include tree (tier view-support
# in orc/sdk/sdk_headers.yaml). Built against the host's Qt; a view links it
# and, at run time, uses the copy orc-gui has already loaded.
#
# In-tree:      target_link_libraries(orc-view_x PRIVATE orc-view-support)
# Out-of-tree:  find_package(decode-orc-plugin-sdk REQUIRED)
#               target_link_libraries(orc-view_x PRIVATE orc::view-support)

add_library(orc-view-support SHARED
    ${CMAKE_SOURCE_DIR}/orc/sdk/include/orc/view-support/amplitude_units.h
    ${CMAKE_SOURCE_DIR}/orc/sdk/include/orc/view-support/analysis_series_decimator.h
    ${CMAKE_SOURCE_DIR}/orc/sdk/include/orc/view-support/analysis_tool_window.h
    ${CMAKE_SOURCE_DIR}/orc/sdk/include/orc/view-support/frame_surface.h
    ${CMAKE_SOURCE_DIR}/orc/sdk/include/orc/view-support/frame_view_geometry.h
    ${CMAKE_SOURCE_DIR}/orc/sdk/include/orc/view-support/frame_viewport_widget.h
    ${CMAKE_SOURCE_DIR}/orc/sdk/include/orc/view-support/overlay_primitive_builder.h
    ${CMAKE_SOURCE_DIR}/orc/sdk/include/orc/view-support/overlay_primitives.h
    ${CMAKE_SOURCE_DIR}/orc/sdk/include/orc/view-support/plot_series_decimation.h
    ${CMAKE_SOURCE_DIR}/orc/sdk/include/orc/view-support/plot_widget.h
    ${CMAKE_SOURCE_DIR}/orc/sdk/include/orc/view-support/theme_color_tokens.h
    ${CMAKE_SOURCE_DIR}/orc/sdk/include/orc/view-support/view_support_export.h
    src/analysis_series_decimator.cpp
    src/analysis_tool_window.cpp
    src/frame_surface.cpp
    src/frame_view_geometry.cpp
    src/frame_viewport_widget.cpp
    src/overlay_primitive_builder.cpp
    src/overlay_primitives.cpp
    src/plot_series_decimation.cpp
    src/plot_widget.cpp
)
set_target_properties(orc-view-support PROPERTIES
    EXPORT_NAME view-support
    AUTOMOC ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_compile_definitions(orc-view-support PRIVATE ORC_VIEW_SUPPORT_BUILDING)
target_include_directories(orc-view-support PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/orc/sdk/include>
    $<INSTALL_INTERFACE:include/decode-orc-plugin-sdk>
)
target_link_libraries(orc-view-support PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets)

if(APPLE)
    set_target_properties(orc-view-support PROPERTIES
        MACOSX_RPATH ON
        INSTALL_NAME_DIR "@rpath"
    )
elseif(UNIX)
    set_target_properties(orc-view-support PROPERTIES INSTALL_RPATH "\$ORIGIN")
endif()

# Installed with the application (the copy views use at run time) and, as
# part of the SDK package, for out-of-tree views to link.
if(APPLE)
    install(TARGETS orc-view-support
        EXPORT decode-orc-view-supportTargets
        LIBRARY DESTINATION "orc-gui.app/Contents/Frameworks" COMPONENT Runtime
        RUNTIME DESTINATION "orc-gui.app/Contents/MacOS" COMPONENT Runtime
    )
elseif(WIN32)
    install(TARGETS orc-view-support
        EXPORT decode-orc-view-supportTargets
        RUNTIME DESTINATION bin COMPONENT Runtime
        LIBRARY DESTINATION lib COMPONENT Runtime
        ARCHIVE DESTINATION lib COMPONENT Runtime
    )
else()
    install(TARGETS orc-view-support
        EXPORT decode-orc-view-supportTargets
        LIBRARY DESTINATION lib COMPONENT Runtime
        RUNTIME DESTINATION bin COMPONENT Runtime
    )
endif()
# A separate export set: only views need Qt, so a stage-only plugin can
# configure against the SDK package without it (see
# DecodeOrcPluginSDKConfig.cmake).
install(EXPORT decode-orc-view-supportTargets
    FILE decode-orc-view-supportTargets.cmake
    NAMESPACE orc::
    DESTINATION lib/cmake/decode-orc-plugin-sdk
)
