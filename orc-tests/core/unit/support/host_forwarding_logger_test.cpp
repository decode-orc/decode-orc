/*
 * File:        host_forwarding_logger_test.cpp
 * Module:      orc-tests/core/unit
 * Purpose:     Tests for the logger a privately-linked plugin forwards to the
 *              host through OrcPluginServices::log
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/support/logging.h>

#include <string>
#include <utility>
#include <vector>

namespace orc {
namespace tests {
namespace {

// The host callback is a plain function pointer, so the records it receives
// are collected in a file-scope list each test clears first.
std::vector<std::pair<OrcPluginLogLevel, std::string>>& received() {
  static std::vector<std::pair<OrcPluginLogLevel, std::string>> records;
  return records;
}

void record_host_log(OrcPluginLogLevel level, const char* message) {
  received().emplace_back(level, message);
}

class HostForwardingLogger : public ::testing::Test {
 protected:
  void SetUp() override { received().clear(); }
};

TEST_F(HostForwardingLogger, HandsTheHostTheMessageTextWithoutAPattern) {
  auto logger = make_host_forwarding_logger(&record_host_log);
  logger->info("frame {} of {}", 3, 10);

  ASSERT_EQ(received().size(), 1U);
  EXPECT_EQ(received()[0].first, OrcPluginLogLevel::Info);
  // The host adds its own timestamp, logger name and level.
  EXPECT_EQ(received()[0].second, "frame 3 of 10");
}

TEST_F(HostForwardingLogger, KeepsEachRecordsLevel) {
  auto logger = make_host_forwarding_logger(&record_host_log);
  logger->trace("t");
  logger->debug("d");
  logger->info("i");
  logger->warn("w");
  logger->error("e");
  logger->critical("c");

  const std::vector<std::pair<OrcPluginLogLevel, std::string>> expected = {
      {OrcPluginLogLevel::Trace, "t"}, {OrcPluginLogLevel::Debug, "d"},
      {OrcPluginLogLevel::Info, "i"},  {OrcPluginLogLevel::Warn, "w"},
      {OrcPluginLogLevel::Error, "e"}, {OrcPluginLogLevel::Critical, "c"},
  };
  EXPECT_EQ(received(), expected);
}

TEST_F(HostForwardingLogger, PassesEveryLevelForTheHostToFilter) {
  // A plugin cannot ask the host which levels are on, so nothing is dropped
  // on the plugin's side.
  auto logger = make_host_forwarding_logger(&record_host_log);
  EXPECT_TRUE(logger->should_log(spdlog::level::trace));
}

TEST_F(HostForwardingLogger, IsNamedLikeTheSharedCoreLogger) {
  auto logger = make_host_forwarding_logger(&record_host_log);
  EXPECT_EQ(logger->name(), "core");
}

}  // namespace
}  // namespace tests
}  // namespace orc
