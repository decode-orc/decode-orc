/*
 * File:        cc_sink_output_integration_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Integration test proving CCSinkStageDeps drives the host
 *              observation service and produces real closed-caption output.
 *
 * Unit tests may not touch the file system (see TESTING.md), and the CC sink
 * writes its output file directly through std::ofstream with no writer seam,
 * so end-to-end output coverage lives here in the integration tier where real
 * files are permitted.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "cc_sink_stage_deps.h"
#include "mock_video_frame_representation.h"
#include "observation_service_interface_mock.h"

namespace orc_unit_test {
namespace {
using testing::_;
using testing::ByMove;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;

constexpr orc::FrameID kFrameCount = 4;

// Stand-in for the host "closed_caption" observer: writes a decodable EIA-608
// pair onto field 0 of every processed frame (data0 = 0x14 caption control
// code, data1 = a printable byte), mirroring what ClosedCaptionObserver would
// deposit for a frame that carries captions.
void write_caption_observations(const orc::VideoFrameRepresentation& /*rep*/,
                                orc::FrameID frame_id,
                                orc::IObservationContext& context) {
  const orc::FieldID field0(frame_id * 2);
  context.set(field0, "closed_caption", "present", true);
  context.set(field0, "closed_caption", "data0", static_cast<int32_t>(0x14));
  context.set(field0, "closed_caption", "data1", static_cast<int32_t>(0x2C));
  context.set(field0, "closed_caption", "parity0_valid", true);
  context.set(field0, "closed_caption", "parity1_valid", true);

  // Field 1 carries no NTSC captions.
  const orc::FieldID field1(frame_id * 2 + 1);
  context.set(field1, "closed_caption", "present", false);
}

// One byte pair of a scripted line 21 stream.
struct ScriptedPair {
  int32_t data0;
  int32_t data1;
};

// A field 1 stream carrying a CC1 pop-on caption and a TEXT2 page at the same
// time, the two alternating pair for pair — the shape of the recording issue
// #273 was reported against. One frame per pair.
//
// CC1 says "HI", TEXT2 says "42".
const std::vector<ScriptedPair>& interleaved_script() {
  static const std::vector<ScriptedPair> script = {
      {0x14, 0x20},  // CC1: Resume Caption Loading (pop-on)
      {0x14, 0x50},  // CC1: PAC, row 14 column 0
      {0x1C, 0x2A},  // TEXT2: Text Restart
      {'4', '2'},    // TEXT2 characters
      {0x14, 0x50},  // back to CC1 (its PAC again)
      {'H', 'I'},    // CC1 characters
      {0x14, 0x2F},  // CC1: End of Caption - put it up
      {0x14, 0x2C},  // CC1: Erase Displayed Memory - take it down again
  };
  return script;
}

// Observer stand-in that plays interleaved_script() out, one pair per frame.
void write_interleaved_observations(
    const orc::VideoFrameRepresentation& /*rep*/, orc::FrameID frame_id,
    orc::IObservationContext& context) {
  const orc::FieldID field0(frame_id * 2);
  const orc::FieldID field1(frame_id * 2 + 1);
  context.set(field1, "closed_caption", "present", false);

  const auto& script = interleaved_script();
  if (frame_id >= script.size()) {
    context.set(field0, "closed_caption", "present", false);
    return;
  }

  context.set(field0, "closed_caption", "present", true);
  context.set(field0, "closed_caption", "data0", script[frame_id].data0);
  context.set(field0, "closed_caption", "data1", script[frame_id].data1);
  context.set(field0, "closed_caption", "parity0_valid", true);
  context.set(field0, "closed_caption", "parity1_valid", true);
}

// RAII cleanup for a temporary output file.
struct ScopedTempFile {
  std::filesystem::path path;
  explicit ScopedTempFile(const std::string& name)
      : path(std::filesystem::temp_directory_path() / name) {
    std::filesystem::remove(path);
  }
  ~ScopedTempFile() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  std::string str() const { return path.string(); }
};

std::string read_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

void configure_ntsc_frames(NiceMock<MockVideoFrameRepresentation>& vfr,
                           orc::FrameID frame_count = kFrameCount) {
  ON_CALL(vfr, frame_range())
      .WillByDefault(Return(orc::FrameIDRange{0, frame_count - 1}));
  ON_CALL(vfr, has_frame(_)).WillByDefault(Return(true));
  orc::FrameDescriptor desc;
  desc.system = orc::VideoSystem::NTSC;
  ON_CALL(vfr, get_frame_descriptor(_))
      .WillByDefault(Return(std::optional<orc::FrameDescriptor>(desc)));
}
}  // namespace

TEST(CCSinkOutputIntegrationTest, Scc_ProducesCaptionBytesAndReusesOneSession) {
  NiceMock<MockObservationService> service;
  NiceMock<MockVideoFrameRepresentation> vfr;
  configure_ntsc_frames(vfr);

  // The observer session must be created exactly once and reused for every
  // frame (the field-pairing contract Phase 2 called for).
  auto handle = std::make_unique<NiceMock<MockObserverHandle>>();
  auto* handle_ptr = handle.get();
  EXPECT_CALL(*handle_ptr, process_frame(_, _, _))
      .Times(static_cast<int>(kFrameCount))
      .WillRepeatedly(Invoke(&write_caption_observations));
  std::unique_ptr<orc::IObserverHandle> handle_base = std::move(handle);
  EXPECT_CALL(service, create_observer("closed_caption"))
      .Times(1)
      .WillOnce(Return(ByMove(std::move(handle_base))));

  orc::ObservationContext context;
  orc::CCSinkStageDeps deps(&service);
  deps.init(orc::TriggerProgressCallback{}, nullptr);

  ScopedTempFile out("orc_cc_sink_scc_it.scc");
  orc::CCExportOptions options;
  options.output_path = out.str();
  options.export_format = orc::CCExportFormat::SCC;

  const orc::CCExportResult result = deps.export_cc(&vfr, context, options);

  EXPECT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.cc_frames_exported, static_cast<int32_t>(kFrameCount));

  const std::string contents = read_file(out.path);
  EXPECT_THAT(contents, testing::HasSubstr("Scenarist_SCC V1.0"));
  // 0x14 -> odd-parity 0x94, 0x2C -> odd-parity 0x2C.
  EXPECT_THAT(contents, testing::HasSubstr("942c"));
}

TEST(CCSinkOutputIntegrationTest, PlainText_RunsObserverPerFrameAndWritesFile) {
  NiceMock<MockObservationService> service;
  NiceMock<MockVideoFrameRepresentation> vfr;
  configure_ntsc_frames(vfr);

  auto handle = std::make_unique<NiceMock<MockObserverHandle>>();
  auto* handle_ptr = handle.get();
  EXPECT_CALL(*handle_ptr, process_frame(_, _, _))
      .Times(static_cast<int>(kFrameCount))
      .WillRepeatedly(Invoke(&write_caption_observations));
  std::unique_ptr<orc::IObserverHandle> handle_base = std::move(handle);
  EXPECT_CALL(service, create_observer("closed_caption"))
      .Times(1)
      .WillOnce(Return(ByMove(std::move(handle_base))));

  orc::ObservationContext context;
  orc::CCSinkStageDeps deps(&service);
  deps.init(orc::TriggerProgressCallback{}, nullptr);

  ScopedTempFile out("orc_cc_sink_text_it.txt");
  orc::CCExportOptions options;
  options.output_path = out.str();
  options.export_format = orc::CCExportFormat::PLAIN_TEXT;

  const orc::CCExportResult result = deps.export_cc(&vfr, context, options);

  EXPECT_TRUE(result.success) << result.message;
  // The fixture writes the same control pair (Erase Displayed Memory) on all
  // four frames. Decoding is where an encoder's duplicate copy of a control
  // pair must be dropped, so the four pairs are two commands.
  EXPECT_EQ(result.cc_frames_exported, static_cast<int32_t>(kFrameCount) / 2);
  EXPECT_TRUE(std::filesystem::exists(out.path));
}

TEST(CCSinkOutputIntegrationTest,
     NullService_WritesValidHeaderWithoutObserver) {
  NiceMock<MockVideoFrameRepresentation> vfr;
  configure_ntsc_frames(vfr);

  orc::ObservationContext context;
  // Older host: no observation service available.
  orc::CCSinkStageDeps deps(nullptr);
  deps.init(orc::TriggerProgressCallback{}, nullptr);

  ScopedTempFile out("orc_cc_sink_nullsvc_it.scc");
  orc::CCExportOptions options;
  options.output_path = out.str();
  options.export_format = orc::CCExportFormat::SCC;

  const orc::CCExportResult result = deps.export_cc(&vfr, context, options);

  EXPECT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.cc_frames_exported, 0);

  const std::string contents = read_file(out.path);
  EXPECT_THAT(contents, testing::HasSubstr("Scenarist_SCC V1.0"));
}
namespace {
// Run an export over the interleaved script and hand back the file contents.
std::string run_interleaved_export(const std::string& file_name,
                                   orc::CCExportFormat format,
                                   orc::EIA608Service service,
                                   int32_t* pairs_exported = nullptr) {
  const auto frame_count =
      static_cast<orc::FrameID>(interleaved_script().size());

  NiceMock<MockObservationService> service_mock;
  NiceMock<MockVideoFrameRepresentation> vfr;
  configure_ntsc_frames(vfr, frame_count);

  auto handle = std::make_unique<NiceMock<MockObserverHandle>>();
  ON_CALL(*handle, process_frame(_, _, _))
      .WillByDefault(Invoke(&write_interleaved_observations));
  std::unique_ptr<orc::IObserverHandle> handle_base = std::move(handle);
  EXPECT_CALL(service_mock, create_observer("closed_caption"))
      .WillOnce(Return(ByMove(std::move(handle_base))));

  orc::ObservationContext context;
  orc::CCSinkStageDeps deps(&service_mock);
  deps.init(orc::TriggerProgressCallback{}, nullptr);

  ScopedTempFile out(file_name);
  orc::CCExportOptions options;
  options.output_path = out.str();
  options.export_format = format;
  options.service = service;

  const orc::CCExportResult result = deps.export_cc(&vfr, context, options);
  EXPECT_TRUE(result.success) << result.message;
  if (pairs_exported != nullptr) {
    *pairs_exported = result.cc_frames_exported;
  }
  return read_file(out.path);
}
}  // namespace

TEST(CCSinkOutputIntegrationTest, PlainText_ExportsOnlyTheSelectedService) {
  // Issue #273: exported together, the two services' characters interleave and
  // the caption reads "42HI" (or worse, in a real recording).
  const std::string cc1 = run_interleaved_export(
      "orc_cc_sink_service_cc1_it.txt", orc::CCExportFormat::PLAIN_TEXT,
      orc::EIA608Service::CC1);
  EXPECT_THAT(cc1, testing::HasSubstr("HI"));
  EXPECT_THAT(cc1, testing::Not(testing::HasSubstr("42")));

  const std::string text2 = run_interleaved_export(
      "orc_cc_sink_service_t2_it.txt", orc::CCExportFormat::PLAIN_TEXT,
      orc::EIA608Service::T2);
  EXPECT_THAT(text2, testing::HasSubstr("42"));
  EXPECT_THAT(text2, testing::Not(testing::HasSubstr("HI")));
}

TEST(CCSinkOutputIntegrationTest, Scc_WritesOnlyTheSelectedServicesBytes) {
  int32_t pairs = 0;
  const std::string scc = run_interleaved_export(
      "orc_cc_sink_service_cc1_it.scc", orc::CCExportFormat::SCC,
      orc::EIA608Service::CC1, &pairs);

  EXPECT_THAT(scc, testing::HasSubstr("Scenarist_SCC V1.0"));
  // CC1's six pairs, and not TEXT2's two.
  EXPECT_EQ(pairs, 6);
  // 0x1C (TEXT2's control byte) with odd parity is 0x9c; "42" is 0x34 0x32.
  EXPECT_THAT(scc, testing::Not(testing::HasSubstr("9c")));
  EXPECT_THAT(scc, testing::Not(testing::HasSubstr("3432")));
  // 'H' 'I' with odd parity: 0xc8 0x49.
  EXPECT_THAT(scc, testing::HasSubstr("c849"));
}

TEST(CCSinkOutputIntegrationTest, Srt_WritesNumberedCuesWithMillisecondTimes) {
  const std::string srt = run_interleaved_export(
      "orc_cc_sink_it.srt", orc::CCExportFormat::SRT, orc::EIA608Service::CC1);

  EXPECT_THAT(srt, testing::StartsWith("1\n00:00:00,"));
  EXPECT_THAT(srt, testing::HasSubstr(" --> 00:00:00,"));
  EXPECT_THAT(srt, testing::HasSubstr("HI"));
  EXPECT_THAT(srt, testing::Not(testing::HasSubstr("42")));
}

namespace {
// RAII cleanup for a temporary output folder.
struct ScopedTempFolder {
  std::filesystem::path path;
  explicit ScopedTempFolder(const std::string& name)
      : path(std::filesystem::temp_directory_path() / name) {
    std::filesystem::remove_all(path);
  }
  ~ScopedTempFolder() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};
}  // namespace

// Every service in one pass: one observer session over the frames, a file for
// each service that carried anything, each the one its own export writes, and
// the report beside them.
TEST(CCSinkOutputIntegrationTest, AllServices_WritesEachServiceInOnePass) {
  const auto frame_count =
      static_cast<orc::FrameID>(interleaved_script().size());
  NiceMock<MockObservationService> service_mock;
  NiceMock<MockVideoFrameRepresentation> vfr;
  configure_ntsc_frames(vfr, frame_count);
  auto handle = std::make_unique<NiceMock<MockObserverHandle>>();
  EXPECT_CALL(*handle, process_frame(_, _, _))
      .Times(static_cast<int>(frame_count))
      .WillRepeatedly(Invoke(&write_interleaved_observations));
  std::unique_ptr<orc::IObserverHandle> handle_base = std::move(handle);
  EXPECT_CALL(service_mock, create_observer("closed_caption"))
      .WillOnce(Return(ByMove(std::move(handle_base))));

  orc::ObservationContext context;
  orc::CCSinkStageDeps deps(&service_mock);
  deps.init(orc::TriggerProgressCallback{}, nullptr);
  ScopedTempFolder folder("orc_cc_sink_all_it");
  orc::CCExportOptions options;
  options.output_path = (folder.path / "line21").string();
  options.all_services = true;
  options.write_report = true;
  const orc::CCExportResult result = deps.export_cc(&vfr, context, options);
  ASSERT_TRUE(result.success) << result.message;

  const std::filesystem::path out = folder.path / "line21";
  EXPECT_EQ(read_file(out / "CC1.srt"),
            run_interleaved_export("orc_cc_sink_all_cc1_it.srt",
                                   orc::CCExportFormat::SRT,
                                   orc::EIA608Service::CC1));
  EXPECT_EQ(read_file(out / "TEXT2.txt"),
            run_interleaved_export("orc_cc_sink_all_t2_it.txt",
                                   orc::CCExportFormat::PLAIN_TEXT,
                                   orc::EIA608Service::T2));
  // Nothing else carried anything.
  for (const char* name : {"CC2.srt", "CC3.srt", "CC4.srt", "TEXT1.txt",
                           "TEXT3.txt", "TEXT4.txt", "XDS.txt"}) {
    EXPECT_FALSE(std::filesystem::exists(out / name)) << name;
  }
  EXPECT_EQ(read_file(out / "line21.report.txt"), result.report);
}

TEST(CCSinkOutputIntegrationTest, Html_WritesAMonospacedTranscript) {
  const std::string html = run_interleaved_export(
      "orc_cc_sink_it.html", orc::CCExportFormat::HTML, orc::EIA608Service::T2);

  EXPECT_THAT(html, testing::StartsWith("<!DOCTYPE html>"));
  // Named as the service parameter names it.
  EXPECT_THAT(html, testing::HasSubstr("<title>Closed captions — TEXT2"));
  // The column layout only survives inside a preformatted, monospaced block.
  EXPECT_THAT(html, testing::HasSubstr("<pre>"));
  EXPECT_THAT(html, testing::HasSubstr("42"));
  EXPECT_THAT(html, testing::EndsWith("</html>\n"));
}

namespace {
// One field's line 21 as the observer would report it.
struct ScriptedField {
  bool present = false;
  int32_t data0 = 0;
  int32_t data1 = 0;
  bool parity0_valid = true;
  bool parity1_valid = true;
};

// A run of frames, both fields of each scripted, exported as plain text with
// the report written to the result.
struct ScriptedRun {
  std::vector<std::pair<ScriptedField, ScriptedField>> frames;
};

void write_scripted(const ScriptedRun& run, orc::FrameID frame_id,
                    orc::IObservationContext& context) {
  const auto& [first, second] = run.frames.at(frame_id);
  int field = 0;
  for (const ScriptedField* scripted : {&first, &second}) {
    const orc::FieldID id(frame_id * 2 + static_cast<uint64_t>(field++));
    context.set(id, "closed_caption", "present", scripted->present);
    if (scripted->present) {
      context.set(id, "closed_caption", "data0", scripted->data0);
      context.set(id, "closed_caption", "data1", scripted->data1);
      context.set(id, "closed_caption", "parity0_valid",
                  scripted->parity0_valid);
      context.set(id, "closed_caption", "parity1_valid",
                  scripted->parity1_valid);
    }
  }
}

orc::CCExportResult run_scripted_export(const ScriptedRun& run,
                                        const std::string& file_name,
                                        orc::EIA608Service service,
                                        std::string* contents) {
  NiceMock<MockObservationService> service_mock;
  NiceMock<MockVideoFrameRepresentation> vfr;
  configure_ntsc_frames(vfr, static_cast<orc::FrameID>(run.frames.size()));
  auto handle = std::make_unique<NiceMock<MockObserverHandle>>();
  ON_CALL(*handle, process_frame(_, _, _))
      .WillByDefault(Invoke([&run](const orc::VideoFrameRepresentation&,
                                   orc::FrameID frame_id,
                                   orc::IObservationContext& context) {
        write_scripted(run, frame_id, context);
      }));
  std::unique_ptr<orc::IObserverHandle> handle_base = std::move(handle);
  EXPECT_CALL(service_mock, create_observer("closed_caption"))
      .WillOnce(Return(ByMove(std::move(handle_base))));

  orc::ObservationContext context;
  orc::CCSinkStageDeps deps(&service_mock);
  deps.init(orc::TriggerProgressCallback{}, nullptr);
  ScopedTempFile out(file_name);
  orc::CCExportOptions options;
  options.output_path = out.str();
  options.export_format = orc::CCExportFormat::PLAIN_TEXT;
  options.service = service;
  const orc::CCExportResult result = deps.export_cc(&vfr, context, options);
  *contents = read_file(out.path);
  return result;
}

ScriptedField pair(int32_t data0, int32_t data1) {
  ScriptedField field;
  field.present = true;
  field.data0 = data0;
  field.data1 = data1;
  return field;
}

// A pop-on caption, "HI", on one field of a run of frames, each control pair
// sent twice as encoders send them [CTA-608-E §D.2], then null padding; the
// other field carries nothing.
ScriptedRun caption_run(bool on_second_field) {
  const std::vector<std::pair<int32_t, int32_t>> script = {
      {0x14, 0x2E}, {0x14, 0x2E}, {0x14, 0x20}, {0x14, 0x20}, {0x14, 0x50},
      {0x14, 0x50}, {'H', 'I'},   {0x14, 0x2F}, {0x14, 0x2F}, {0x00, 0x00},
      {0x14, 0x2C}, {0x14, 0x2C}, {0x00, 0x00}, {0x00, 0x00}};
  ScriptedRun run;
  for (const auto& [data0, data1] : script) {
    ScriptedField caption = pair(data0, data1);
    ScriptedField other;
    run.frames.emplace_back(on_second_field ? other : caption,
                            on_second_field ? caption : other);
  }
  return run;
}
}  // namespace

// Each byte's parity is its own: a character whose partner's parity failed is
// kept, the damaged byte dropped; a pair whose first byte failed is dropped
// whole, since it may have been a control code. Observer 1.x's rule (either
// byte) took both pairs whole.
TEST(CCSinkOutputIntegrationTest, TakesEachByteOnlyWhereItsParityHolds) {
  ScriptedRun run = caption_run(/*on_second_field=*/false);
  // "HI" with its second byte's parity failed.
  run.frames[6].first.parity1_valid = false;
  // "OK" with its first byte's parity failed, where null padding was.
  run.frames[9].first = pair('O', 'K');
  run.frames[9].first.parity0_valid = false;
  std::string contents;
  const auto result = run_scripted_export(run, "orc_cc_sink_parity_it.txt",
                                          orc::EIA608Service::CC1, &contents);
  EXPECT_TRUE(result.success) << result.message;
  EXPECT_THAT(contents, testing::HasSubstr("H"));
  EXPECT_THAT(contents, testing::Not(testing::HasSubstr("HI")));
  EXPECT_THAT(contents, testing::Not(testing::HasSubstr("K")));
  EXPECT_THAT(result.report, testing::HasSubstr("1 by their first character"));
}

// The captions of a capture whose first stored field is television field 2
// arrive on the second stored field, spelled as field 1 spells them: they are
// read as CC1, and the report says why.
TEST(CCSinkOutputIntegrationTest, ReadsACaptureThatStartsOnTheSecondField) {
  std::string contents;
  const auto result = run_scripted_export(caption_run(/*on_second_field=*/true),
                                          "orc_cc_sink_reversed_it.txt",
                                          orc::EIA608Service::CC1, &contents);
  EXPECT_TRUE(result.success) << result.message;
  EXPECT_THAT(contents, testing::HasSubstr("HI"));
  EXPECT_THAT(result.report, testing::HasSubstr("Field order:     reversed"));
}

// A line of noise that reads as a pair, alone among frames that carry no line
// 21 data, is not taken.
TEST(CCSinkOutputIntegrationTest, RefusesAPairThatStandsAlone) {
  ScriptedRun run;
  for (int frame = 0; frame < 100; ++frame) {
    run.frames.emplace_back(ScriptedField{}, ScriptedField{});
  }
  run.frames[50].first = pair(0x14, 0x2C);
  run.frames[51].first = pair('W', 'E');
  std::string contents;
  const auto result = run_scripted_export(run, "orc_cc_sink_alone_it.txt",
                                          orc::EIA608Service::CC1, &contents);
  EXPECT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.cc_frames_exported, 0);
  EXPECT_THAT(result.report, testing::HasSubstr("standing alone"));
}

}  // namespace orc_unit_test
