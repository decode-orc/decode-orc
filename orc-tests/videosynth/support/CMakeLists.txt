# Measurement and baseline helpers for the videosynth suite. Pure C++ with no
# videosynth dependency, so the unit tests in orc-tests/core/unit exercise
# them in every build that has unit tests, CI included.
add_library(orc-videosynth-support STATIC
    truth_comparison.cpp
    decoded_comparison.cpp
    baselines.cpp
)
target_include_directories(orc-videosynth-support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
find_package(yaml-cpp REQUIRED)
if(TARGET yaml-cpp AND NOT TARGET yaml-cpp::yaml-cpp)
    add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
endif()
target_link_libraries(orc-videosynth-support
    PUBLIC orc-plugin-sdk
    PRIVATE yaml-cpp::yaml-cpp
)

# FFTW3, found as the chroma decoders find it.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFTW3 IMPORTED_TARGET fftw3)
endif()
if(TARGET PkgConfig::FFTW3)
    target_link_libraries(orc-videosynth-support PRIVATE PkgConfig::FFTW3)
else()
    find_package(FFTW3 CONFIG REQUIRED)
    target_link_libraries(orc-videosynth-support PRIVATE FFTW3::fftw3)
endif()
