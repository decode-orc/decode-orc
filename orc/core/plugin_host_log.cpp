/*
 * File:        plugin_host_log.cpp
 * Module:      orc-core
 * Purpose:     The host side of OrcPluginServices::log
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "include/plugin_host_log.h"

#include <orc/support/logging.h>

namespace orc {

void write_plugin_log_record(OrcPluginLogLevel level, const char* message) {
  if (message == nullptr) {
    return;
  }
  auto logger = get_logger();
  if (!logger) {
    return;
  }
  switch (level) {
    case OrcPluginLogLevel::Trace:
      logger->trace("{}", message);
      break;
    case OrcPluginLogLevel::Debug:
      logger->debug("{}", message);
      break;
    case OrcPluginLogLevel::Info:
      logger->info("{}", message);
      break;
    case OrcPluginLogLevel::Warn:
      logger->warn("{}", message);
      break;
    case OrcPluginLogLevel::Error:
      logger->error("{}", message);
      break;
    case OrcPluginLogLevel::Critical:
      logger->critical("{}", message);
      break;
  }
}

}  // namespace orc
