cmake_minimum_required(VERSION 3.20)

project(orc-core-integration-tests)

# GoogleTest requires at least C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(GTest REQUIRED)

include(GoogleTest)

function(orc_add_core_integration_tests target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/orc/plugins/stages/daphne_vbi_sink
        ${CMAKE_SOURCE_DIR}/orc/plugins/stages/tbc_sink
        ${CMAKE_SOURCE_DIR}/orc/sdk/include
    )
    target_link_libraries(${target}
        orc-core
        GTest::gmock_main
    )

    if(TARGET orc_chroma_decoders)
        target_link_libraries(${target} orc_chroma_decoders)
    endif()

    gtest_discover_tests(${target}
        DISCOVERY_MODE POST_BUILD
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "integration")
endfunction()

orc_add_core_integration_tests(
    orc-core-integration-tests-sinks
    stages/chroma_sink/pal_m_integration_test.cpp
)

# CC sink end-to-end output test. Compiles the real CCSinkStageDeps (which
# writes files directly) so the integration tier can exercise SCC / plain-text
# output against a mocked observation service.
add_executable(orc-core-integration-tests-cc-sink
    stages/cc_sink/cc_sink_output_integration_test.cpp
    ${CMAKE_SOURCE_DIR}/orc/plugins/stages/cc_sink/cc_sink_stage_deps.cpp
    ${CMAKE_SOURCE_DIR}/orc/plugins/stages/cc_sink/cc_line21_report.cpp
    ${CMAKE_SOURCE_DIR}/orc/plugins/stages/cc_sink/cc_xds_decoder.cpp
)
target_include_directories(orc-core-integration-tests-cc-sink PRIVATE
    ${CMAKE_SOURCE_DIR}/orc/sdk/include
    ${CMAKE_SOURCE_DIR}/orc/plugins/stages/cc_sink
    ${CMAKE_SOURCE_DIR}/orc-tests/core/unit/include
    ${CMAKE_SOURCE_DIR}/orc-tests/core/unit/mocks
)
target_link_libraries(orc-core-integration-tests-cc-sink
    orc-core
    GTest::gmock_main
)
gtest_discover_tests(orc-core-integration-tests-cc-sink
    DISCOVERY_MODE POST_BUILD
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "integration")
