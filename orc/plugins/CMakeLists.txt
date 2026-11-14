# Runtime stage plugins are organized as per-stage bundles so they can be
# moved to separate repositories without changing host-side build structure.

add_subdirectory(stages)

# Plugin source trees from outside this repository, built in for development
# and testing only (never packaged); see cmake/PluginSourceTrees.cmake.
if(ORC_PLUGIN_SOURCE_DIRS)
    include(${CMAKE_SOURCE_DIR}/cmake/PluginSourceTrees.cmake)
endif()
