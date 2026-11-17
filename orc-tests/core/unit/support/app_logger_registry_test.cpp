/*
 * File:        app_logger_registry_test.cpp
 * Module:      orc-tests/core/unit
 * Purpose:     The application logger tolerates a logger another module has
 *              already registered under its name
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <logging.h>
#include <spdlog/sinks/null_sink.h>

#include <memory>
#include <string>

namespace orc {
namespace tests {
namespace {

// On Windows orc-gui.exe and orc-core.dll each hold their own copy of the
// application logger code, sharing one spdlog registry. A logger registered
// straight into the registry stands in for the other copy's.
std::shared_ptr<spdlog::logger> register_foreign_logger(
    const std::string& name) {
  auto logger = std::make_shared<spdlog::logger>(
      name, std::make_shared<spdlog::sinks::null_sink_mt>());
  spdlog::register_logger(logger);
  return logger;
}

class AppLoggerRegistry : public ::testing::Test {
 protected:
  void SetUp() override { clear(); }
  void TearDown() override { clear(); }

 private:
  static void clear() {
    reset_logging();
    spdlog::drop("orc-app");
    spdlog::drop("cli");
  }
};

TEST_F(AppLoggerRegistry, FirstUseReusesALoggerAlreadyRegistered) {
  const auto foreign = register_foreign_logger("orc-app");

  std::shared_ptr<spdlog::logger> logger;
  EXPECT_NO_THROW(logger = get_app_logger());
  EXPECT_EQ(logger, foreign);
}

TEST_F(AppLoggerRegistry, FirstUseRegistersWhenNothingIsRegistered) {
  const auto logger = get_app_logger();

  ASSERT_NE(logger, nullptr);
  EXPECT_EQ(logger->name(), "orc-app");
  EXPECT_EQ(spdlog::get("orc-app"), logger);
}

TEST_F(AppLoggerRegistry, InitialisationReplacesALoggerAlreadyRegistered) {
  const auto foreign = register_foreign_logger("cli");

  EXPECT_NO_THROW(init_app_logging("warn", "[%n] [%l] %v", "", "cli",
                                   LogDestination::kConsole));

  const auto registered = spdlog::get("cli");
  ASSERT_NE(registered, nullptr);
  EXPECT_NE(registered, foreign);
  EXPECT_EQ(registered, get_app_logger());
  EXPECT_EQ(registered->level(), spdlog::level::warn);
}

}  // namespace
}  // namespace tests
}  // namespace orc
