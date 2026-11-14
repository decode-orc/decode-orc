/*
 * File:        view_plugin.cpp
 * Module:      orc-core unit tests (fixture)
 * Purpose:     A view binary for the view loader tests, built without Qt:
 *              the descriptor's Qt version and contracts come from compile
 *              definitions, and the factories make no widget
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

// Stand-ins for <QtGlobal>: ORC_VIEW_PLUGIN_DESCRIPTOR reads these.
#define QT_VERSION_MAJOR ORC_TEST_VIEW_QT_MAJOR
#define QT_VERSION_MINOR ORC_TEST_VIEW_QT_MINOR

#include <orc/view/orc_view_registration.h>

#include <cstdlib>

namespace {

// orc-cli must never open a view binary. With this set, loading the fixture
// at all ends the process, so a test that runs orc-cli over a folder holding
// it fails if the stage loader ever opens it.
struct AbortIfLoaded {
  AbortIfLoaded() {
    if (std::getenv("ORC_TEST_VIEW_ABORT_ON_LOAD") != nullptr) {
      std::abort();
    }
  }
};
const AbortIfLoaded kAbortIfLoaded;

QWidget* make_nothing(const orc::view::ViewOpenRequest& /*request*/) {
  return nullptr;
}

constexpr orc::view::ViewPluginDescriptor kDescriptor{
    ORC_TEST_VIEW_PLUGIN_ID,
    "0.0.1",
    orc::view::kViewPluginAbiVersion + ORC_TEST_VIEW_ABI_OFFSET,
    static_cast<uint32_t>(QT_VERSION_MAJOR),
    static_cast<uint32_t>(QT_VERSION_MINOR),
    ORC_SDK_TOOLCHAIN_TAG,
};

}  // namespace

ORC_DEFINE_VIEW_PLUGIN(kDescriptor, {ORC_TEST_VIEW_CONTRACT_A, &make_nothing},
                       {ORC_TEST_VIEW_CONTRACT_B, &make_nothing})
