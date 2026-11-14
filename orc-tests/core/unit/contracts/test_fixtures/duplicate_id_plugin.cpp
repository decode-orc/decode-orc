/*
 * File:        duplicate_id_plugin.cpp
 * Module:      orc-tests / test fixtures
 * Purpose:     Two copies of one plugin id, built from this file into two
 *              libraries. Used by
 *              stage_plugin_loader_version_rejection_test.cpp to verify that
 *              the second copy is refused before it registers anything.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <orc/abi/orc_plugin_abi.h>

#include <memory>

#ifndef ORC_TEST_FIXTURE_COPY_VERSION
#error "Build with ORC_TEST_FIXTURE_COPY_VERSION set to the copy's version"
#endif

namespace {

constexpr orc::StagePluginDescriptor kDescriptor{
    "test.duplicate",  // plugin_id: the same in both copies
    ORC_TEST_FIXTURE_COPY_VERSION,
    orc::kStagePluginHostAbiVersion,
    orc::kStagePluginApiVersion,
    "GPL-3.0-or-later",  // license_spdx
    false,               // is_core_plugin
    ORC_SDK_TOOLCHAIN_TAG,
};

// Never called: the tests accept the registration without creating a stage.
std::shared_ptr<orc::DAGStage> create_no_stage() { return nullptr; }

}  // namespace

ORC_STAGE_PLUGIN_EXPORT const orc::StagePluginDescriptor*
orc_get_stage_plugin_descriptor() {
  return &kDescriptor;
}

// Registers one stage name, the same in both copies, so a second copy that
// got as far as registering would collide with the first.
ORC_STAGE_PLUGIN_EXPORT bool orc_register_stage_plugin(
    const orc::OrcPluginServices* /*services*/, void* host_context,
    bool (*register_stage)(void*, const char*, orc::OrcStageFactoryFn),
    const char** out_error) {
  if (!register_stage(host_context, "test_duplicate_stage", &create_no_stage)) {
    if (out_error != nullptr) {
      *out_error = "test_duplicate_stage was rejected";
    }
    return false;
  }
  return true;
}
