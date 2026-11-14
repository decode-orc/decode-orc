/*
 * File:        orc_view_plugin_abi.h
 * Module:      decode-orc Plugin SDK (view tier)
 * Purpose:     Binary contract for a plugin's view binary: descriptor,
 *              entry points and the widget-factory registration
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 *
 * STABILITY: PUBLIC — This header is part of the stable plugin SDK.
 */

#pragma once

// SDK TIER: view — the contract between orc-gui and a plugin's view binary.
// A layout change here bumps kViewPluginAbiVersion, not the stage host ABI.
//
// No Qt header is included: QWidget is only ever handled by pointer here. A
// view binary includes Qt itself, and ORC_VIEW_PLUGIN_DESCRIPTOR (in
// <orc/view/orc_view_registration.h>) reads QT_VERSION_MAJOR/MINOR from it.

#include <orc/view/stage_tool_session.h>

#include <cstdint>

class QWidget;

namespace orc::view {

/// Version of the view contract. The host loads a view binary only when its
/// descriptor carries exactly this value. Bumped when anything in the
/// orc/view/ tier changes layout: the descriptor, the entry points, the host
/// services table or IStageToolSession.
///
/// Virtuals appended to IStageToolSession before the first host release
/// that loads views (run_job() onwards) are part of version 1: a view built
/// against the shorter interface calls only the slots it knew.
inline constexpr uint32_t kViewPluginAbiVersion = 1;

inline constexpr const char* kGetViewPluginDescriptorSymbol =
    "orc_get_view_plugin_descriptor";
inline constexpr const char* kRegisterViewPluginSymbol =
    "orc_register_view_plugin";

/**
 * @brief What a view binary says about itself before it is registered
 *
 * Every pointer must reference static storage. The host checks
 * view_abi_version, the Qt version and toolchain_tag before it calls the
 * registration entry point, and refuses the binary if any of them does not
 * match (see plugin-architecture.md, "View Plugins").
 */
struct ViewPluginDescriptor {
  /// The plugin id of the stage binary this view belongs to.
  const char* plugin_id;
  const char* plugin_version;
  /// Must equal kViewPluginAbiVersion at the host.
  uint32_t view_abi_version;
  /// QT_VERSION_MAJOR the view was built against. Must equal the host's.
  uint32_t qt_version_major;
  /// QT_VERSION_MINOR the view was built against. Must not exceed the host's.
  uint32_t qt_version_minor;
  /// ORC_SDK_TOOLCHAIN_TAG of the build. Must equal the host's.
  const char* toolchain_tag;
};

/// Severity of a message a view logs through the host.
enum class ViewLogLevel : int32_t {
  Trace = 0,
  Debug = 1,
  Info = 2,
  Warn = 3,
  Error = 4,
};

/**
 * @brief What the host gives a view binary at registration
 *
 * Append-only within one kViewPluginAbiVersion; check @ref size before
 * reading a field added later.
 */
struct ViewHostServices {
  /// sizeof(ViewHostServices) at host build time.
  uint32_t size;
  /// Write a message to the host's log. The message is copied.
  void (*log)(ViewLogLevel level, const char* message);
};

/**
 * @brief Make the widget for one opened tool
 *
 * Called on the GUI thread. Return a top-level widget (normally a QDialog)
 * parented to @p request.parent; the host shows it non-modally, deletes it
 * when it closes, and closes the session when it is destroyed. Keep the
 * session for as long as the widget needs it. Return nullptr to refuse.
 */
using OrcViewFactoryFn = QWidget* (*)(const ViewOpenRequest& request);

using OrcGetViewPluginDescriptorFn = const ViewPluginDescriptor* (*)();

/// Register one factory per tool contract id. Returns true when every
/// factory was accepted; @p error_message (static storage) says why not.
using OrcRegisterViewPluginFn =
    bool (*)(const ViewHostServices* services, void* context,
             bool (*register_view)(void* context, const char* contract_id,
                                   OrcViewFactoryFn factory),
             const char** error_message);

}  // namespace orc::view

#if defined(_WIN32)
#define ORC_VIEW_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define ORC_VIEW_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif
