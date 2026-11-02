/*
 * File:        vbi_export_writer_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for where a VBI data service's export may write
 *
 * Covers: the paths an export's writer accepts — relative, inside the export
 * — and those it refuses because a name a recording carried would climb out
 * of it, the tally that counts what was written and what was not, and the
 * README that says what each part written holds.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "vbi-services/vbi_export_writer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace orc_unit_test {
namespace {

TEST(VbiExportWriterTest, APathMustStayInsideTheExport) {
  EXPECT_TRUE(orc::vbi_export_path_contained("records/000/000-150-v3.rec"));
  EXPECT_TRUE(orc::vbi_export_path_contained("files/243/..bin"));
  EXPECT_TRUE(orc::vbi_export_path_contained("README.txt"));
  EXPECT_FALSE(orc::vbi_export_path_contained(""));
  EXPECT_FALSE(orc::vbi_export_path_contained("/etc/passwd"));
  EXPECT_FALSE(orc::vbi_export_path_contained("\\windows"));
  EXPECT_FALSE(orc::vbi_export_path_contained("../outside"));
  EXPECT_FALSE(orc::vbi_export_path_contained("web/host/../../../outside"));
  EXPECT_FALSE(orc::vbi_export_path_contained("web\\..\\outside"));
  EXPECT_FALSE(orc::vbi_export_path_contained("C:/outside"));
}

TEST(VbiExportWriterTest, TheTallyCountsWhatWasWrittenAndNamesWhatWasNot) {
  orc::VbiExportTally tally;
  const orc::VbiExportWriter write = [](const std::string& path, const uint8_t*,
                                        size_t) {
    return path != "refused.txt";
  };
  EXPECT_TRUE(orc::vbi_export_put_text(write, tally, "kept.txt", "text"));
  EXPECT_FALSE(orc::vbi_export_put_text(write, tally, "refused.txt", "text"));
  EXPECT_EQ(tally.files, 1U);
  ASSERT_EQ(tally.failures.size(), 1U);
  EXPECT_EQ(tally.failures[0], "refused.txt");
}

// The README names each part written, in the order written, with what it
// holds — a part never written is left out — then what the tally says.
TEST(VbiExportWriterTest, TheReadmeSaysWhatEachPartWrittenHolds) {
  constexpr orc::VbiExportPart kParts[] = {
      {"pages/", "every page"}, {"lines/", "every line"}, {"x.txt", "an x"}};
  std::map<std::string, std::string> files;
  const orc::VbiExportWriter write =
      [&files](const std::string& path, const uint8_t* data, size_t size) {
        files[path].assign(reinterpret_cast<const char*>(data), size);
        return true;
      };
  orc::VbiExportTally tally;
  tally.summary = {"three pages", "one x"};
  orc::vbi_export_readme(write, tally, "Opening.\n\n", {"x.txt", "pages/"},
                         kParts, std::size(kParts));
  EXPECT_EQ(files["README.txt"],
            "Opening.\n\nx.txt\n  an x\n\npages/\n  every page\n\n"
            "This export holds:\n  - three pages\n  - one x\n");
  EXPECT_EQ(tally.files, 1u);

  files.clear();
  orc::VbiExportTally none;
  orc::vbi_export_readme(write, none, "Opening.\n\n", {}, kParts,
                         std::size(kParts));
  EXPECT_TRUE(files.empty());
}

}  // namespace
}  // namespace orc_unit_test
