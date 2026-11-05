# Chroma Decoder Library (Pure C++17)

# Define decoder library sources
set(DECODER_SOURCES
    decoder.cpp
    sourcefield.cpp
    paldecoder.cpp
    palcolour.cpp
    transformpal.cpp
    transformpal2d.cpp
    transformpal3d.cpp
    ntscdecoder.cpp
    comb.cpp
    monodecoder.cpp
    secamdecoder.cpp
    palstilldecoder.cpp
    componentframe.cpp
    noise_reducer.cpp
    framecanvas.cpp
    outputwriter.cpp
    vectorscope_extract.cpp
)

# Create static library
add_library(orc_chroma_decoders STATIC
    ${DECODER_SOURCES}
)

# Include directories: only the decoder tree itself. Contract types
# (<orc/stage/...>) come from the plugin SDK; private host include trees
# must not be visible to plugin-side code.
target_include_directories(orc_chroma_decoders
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Set C++ standard
set_target_properties(orc_chroma_decoders PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_definitions(orc_chroma_decoders PUBLIC
    _USE_MATH_DEFINES
)

# Link FFTW3 (required by transformpal2d.cpp and transformpal3d.cpp)
# Try pkg-config first (more reliable across platforms), then CONFIG mode
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFTW3 IMPORTED_TARGET fftw3)
    if(FFTW3_FOUND)
        target_link_libraries(orc_chroma_decoders PUBLIC PkgConfig::FFTW3)
    else()
        find_package(FFTW3 CONFIG REQUIRED)
        target_link_libraries(orc_chroma_decoders PUBLIC FFTW3::fftw3)
    endif()
else()
    find_package(FFTW3 CONFIG REQUIRED)
    target_link_libraries(orc_chroma_decoders PUBLIC FFTW3::fftw3)
endif()

# The plugin SDK provides the <orc/stage/...> contract headers and the
# spdlog/fmt usage requirements needed by <orc/stage/logging.h>.
target_link_libraries(orc_chroma_decoders PUBLIC orc-plugin-sdk)

# PAL Still's loops take square roots. With errno set by sqrt() (the C++
# default) each one is a libm call that must be able to set errno, so those
# loops cannot vectorise; nothing reads errno after them. This changes no
# result (cmake/HostOptimization.cmake). MSVC's sqrt() vectorises anyway.
#
# The vertical fit works in single precision, where a fused multiply-add
# moves the output by up to 5e-7 (a fortieth of a 16-bit code). Clang fuses
# by default on targets with FMA (arm64), and x86-64's builds have none, so
# fusing is turned off: every platform then computes the same sums.
# MSVC does not fuse under its default /fp:precise.
if(NOT MSVC)
    set_source_files_properties(palstilldecoder.cpp PROPERTIES
        COMPILE_OPTIONS "-fno-math-errno;-ffp-contract=off")
endif()
