/*
 * File:        orc_view_registration.h
 * Module:      decode-orc Plugin SDK (view tier)
 * Purpose:     Descriptor and entry-point boilerplate for a view binary
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 *
 * STABILITY: PUBLIC — This header is part of the stable plugin SDK.
 *
 * USAGE (include a Qt header first, so QT_VERSION_MAJOR/MINOR are defined):
 *
 *   #include <QDialog>
 *   #include <orc/view/orc_view_registration.h>
 *
 *   namespace {
 *   QWidget* make_browser(const orc::view::ViewOpenRequest& request) {
 *     return new MyBrowserDialog(request.session, request.parent);
 *   }
 *   constexpr orc::view::ViewPluginDescriptor kViewDescriptor =
 *       ORC_VIEW_PLUGIN_DESCRIPTOR("com.example.stage.demo", "1.0.0");
 *   }  // namespace
 *
 *   ORC_DEFINE_VIEW_PLUGIN(kViewDescriptor,
 *                          {"com.example.demo.browser.v1", &make_browser})
 */

#pragma once

#include <orc/abi/orc_plugin_abi.h>  // ORC_SDK_TOOLCHAIN_TAG
#include <orc/view/orc_view_plugin_abi.h>

#include <string>

namespace orc::view {

/// One factory and the tool contract id it serves.
struct ViewRegistration {
  const char* contract_id;
  OrcViewFactoryFn factory;
};

/// Services table the host passed at registration, or nullptr before it.
inline const ViewHostServices*& host_services_slot() {
  static const ViewHostServices* services = nullptr;
  return services;
}

/// Write to the host's log; does nothing before registration.
inline void log(ViewLogLevel level, const std::string& message) {
  const ViewHostServices* services = host_services_slot();
  if (services && services->log) {
    services->log(level, message.c_str());
  }
}

template <size_t N>
bool register_views(const ViewHostServices* services, void* context,
                    bool (*register_view)(void* context,
                                          const char* contract_id,
                                          OrcViewFactoryFn factory),
                    const char** error_message,
                    const ViewRegistration (&views)[N]) {
  host_services_slot() = services;
  if (!register_view) {
    if (error_message) {
      *error_message = "Missing view registration callback";
    }
    return false;
  }
  for (const ViewRegistration& view : views) {
    if (!register_view(context, view.contract_id, view.factory)) {
      if (error_message) {
        *error_message = "Host refused a view registration";
      }
      return false;
    }
  }
  return true;
}

}  // namespace orc::view

/// A descriptor for a view built against the Qt the translation unit
/// includes and the current SDK.
#define ORC_VIEW_PLUGIN_DESCRIPTOR(plugin_id, plugin_version)          \
  orc::view::ViewPluginDescriptor {                                    \
    (plugin_id), (plugin_version), orc::view::kViewPluginAbiVersion,   \
        static_cast<uint32_t>(QT_VERSION_MAJOR),                       \
        static_cast<uint32_t>(QT_VERSION_MINOR), ORC_SDK_TOOLCHAIN_TAG \
  }

/// Define both entry points: return @p descriptor, and register each
/// {contract_id, factory} pair that follows it.
#define ORC_DEFINE_VIEW_PLUGIN(descriptor, ...)                        \
  ORC_VIEW_PLUGIN_EXPORT const orc::view::ViewPluginDescriptor*        \
  orc_get_view_plugin_descriptor() {                                   \
    return &(descriptor);                                              \
  }                                                                    \
  ORC_VIEW_PLUGIN_EXPORT bool orc_register_view_plugin(                \
      const orc::view::ViewHostServices* services, void* context,      \
      bool (*register_view)(void* context, const char* contract_id,    \
                            orc::view::OrcViewFactoryFn factory),      \
      const char** error_message) {                                    \
    static const orc::view::ViewRegistration kViews[] = {__VA_ARGS__}; \
    return orc::view::register_views(services, context, register_view, \
                                     error_message, kViews);           \
  }
