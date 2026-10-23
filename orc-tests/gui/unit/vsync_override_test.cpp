/*
 * File:        vsync_override_test.cpp
 * Module:      orc-gui-tests
 * Purpose:     Unit tests for the opt-in --no-vsync command-line override
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "gpu/vsync_override.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <vector>

using orc::gui::gpu::isNoVsyncRequested;
using orc::gui::gpu::noVsyncFlag;

namespace {

// The option is read from argv before anything Qt exists to parse it.
class Argv {
 public:
  explicit Argv(std::initializer_list<const char*> args) {
    for (const char* arg : args) {
      storage_.emplace_back(arg);
    }
    for (std::string& arg : storage_) {
      pointers_.push_back(arg.data());
    }
    pointers_.push_back(nullptr);
  }

  int argc() const { return static_cast<int>(storage_.size()); }
  char** argv() { return pointers_.data(); }

 private:
  std::vector<std::string> storage_;
  std::vector<char*> pointers_;
};

TEST(VsyncOverride, FindsTheFlagAmongOtherArguments) {
  Argv args({"orc-gui", "--log-level", "debug", noVsyncFlag(), "file.orcprj"});
  EXPECT_TRUE(isNoVsyncRequested(args.argc(), args.argv()));
}

// Vsync stays on unless asked for: without it, preview playback tears.
TEST(VsyncOverride, AnOrdinaryRunKeepsVsync) {
  Argv args({"orc-gui", "--no-gpu", "recording.orcprj"});
  EXPECT_FALSE(isNoVsyncRequested(args.argc(), args.argv()));

  Argv none({"orc-gui"});
  EXPECT_FALSE(isNoVsyncRequested(none.argc(), none.argv()));
  EXPECT_FALSE(isNoVsyncRequested(0, nullptr));
}

// argv[0] is the program, never an option.
TEST(VsyncOverride, IgnoresTheProgramName) {
  Argv args({noVsyncFlag()});
  EXPECT_FALSE(isNoVsyncRequested(args.argc(), args.argv()));
}

// A flag that merely starts with the option's name is a different flag.
TEST(VsyncOverride, DoesNotMatchOnAPrefix) {
  Argv args({"orc-gui", "--no-vsync-please", "--no-vsyn"});
  EXPECT_FALSE(isNoVsyncRequested(args.argc(), args.argv()));
}

}  // namespace
