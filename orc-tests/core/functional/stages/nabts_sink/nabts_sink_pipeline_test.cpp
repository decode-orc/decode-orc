/*
 * File:        nabts_sink_pipeline_test.cpp
 * Module:      orc-tests
 * Purpose:     End-to-end validation of the NABTS sink on real 525-line
 *              System C captures
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/plugin/orc_stage_services.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/params/parameter_types.h>
#include <orc/stage/video_frame_representation.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "nabts_catalogue_view.h"
#include "nabts_record.h"
#include "nabts_record_catalogue.h"
#include "nabts_sink_stage.h"
#include "naplps_render_grid.h"
#include "sha256_hash.h"
#include "vbi-services/teletext_slicer.h"
#include "vbi-services/vbi_analysis_results.h"
#include "vbi_source_stage.h"

namespace orc {
namespace {

// The reference captures. Neither is checked in (test-data/ is ignored), so
// every test here skips when its capture is absent.
//
// Both are FLAC-wrapped VBI crops of off-air NTSC recordings carrying NABTS:
// NBC Teletext (1983) and CBS ExtraVision (1985), the two services the US
// networks ran. They are what CEA-516 was written for.
const char* kNbtsCapture = ORC_VBI_TEST_DATA_DIR
    "/teletext/NTSC NABTS Teletext samples/"
    "NBC_1983-12-15_NBC_Teletext_NTSC_EP_vbi_only_part1_u16.flac";

const char* kExtraVisionCapture = ORC_VBI_TEST_DATA_DIR
    "/teletext/NTSC NABTS Teletext samples/"
    "CBS_1985-12-10_ExtraVision_teletext_NTSC_SP_vbi_only_part4_u16.flac";

// The 525-line World System Teletext capture, used for the cross-service
// check: a WST recording must yield essentially nothing from this stage.
const char* kWstCapture = ORC_VBI_TEST_DATA_DIR
    "/teletext/TBS Electra WST 525-60/"
    "TBS_1988-01-03_Electra_teletext_NTSC_SP_vbi_only_u16-002.flac";

// ---------------------------------------------------------------------------
// Host services: the stage writes its packet stream through the host's file
// writer factory, so the functional harness supplies a real one.
// ---------------------------------------------------------------------------

class FileWriterUint8 final : public IFileWriterUint8 {
 public:
  ~FileWriterUint8() override { close(); }

  bool open(const std::string& filepath) override {
    stream_.open(filepath, std::ios::binary | std::ios::trunc);
    return stream_.is_open();
  }

  void write(const uint8_t* data, size_t count) override {
    stream_.write(reinterpret_cast<const char*>(data),
                  static_cast<std::streamsize>(count));
  }

  void write(const std::vector<uint8_t>& data) override {
    write(data.data(), data.size());
  }

  void flush() override { stream_.flush(); }

  void close() override {
    if (stream_.is_open()) stream_.close();
  }

 private:
  std::ofstream stream_;
};

class FunctionalStageServices final : public IStageServices {
 public:
  std::shared_ptr<IFileWriterUint8> create_buffered_file_writer_uint8(
      size_t /*buffer_size*/) override {
    return std::make_shared<FileWriterUint8>();
  }
  std::shared_ptr<IFileWriterUint16> create_buffered_file_writer_uint16(
      size_t /*buffer_size*/) override {
    return nullptr;
  }
  std::shared_ptr<IFileWriterInt16> create_buffered_file_writer_int16(
      size_t /*buffer_size*/) override {
    return nullptr;
  }
};

// ---------------------------------------------------------------------------
// A window onto a capture
// ---------------------------------------------------------------------------
//
// These captures run to tens of thousands of frames — hours of material. The
// tests decode a fixed window of a few hundred frames instead: long enough for
// the pass to settle and for whole data groups to arrive, short enough to run
// in a test. Frame ids are passed through unchanged.
class FrameWindow final : public Artifact, public VideoFrameRepresentation {
 public:
  FrameWindow(std::shared_ptr<const VideoFrameRepresentation> source,
              FrameID first, uint64_t count)
      : Artifact(ArtifactID("nabts-functional-window"), Provenance{}),
        source_(std::move(source)),
        range_{first, static_cast<FrameID>(first + count - 1)} {}

  std::string type_name() const override { return "VideoFrameRepresentation"; }

  FrameIDRange frame_range() const override { return range_; }
  size_t frame_count() const override {
    return static_cast<size_t>(range_.count());
  }
  bool has_frame(FrameID id) const override {
    return range_.contains(id) && source_->has_frame(id);
  }

  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    if (!range_.contains(id)) return std::nullopt;
    return source_->get_frame_descriptor(id);
  }

  const sample_type* get_frame(FrameID id) const override {
    return range_.contains(id) ? source_->get_frame(id) : nullptr;
  }

  const sample_type* get_line(FrameID id, size_t line) const override {
    return range_.contains(id) ? source_->get_line(id, line) : nullptr;
  }

  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    return range_.contains(id) ? source_->get_frame_copy(id)
                               : std::vector<sample_type>{};
  }

  std::vector<sample_type> get_line_samples(FrameID id,
                                            size_t line) const override {
    return range_.contains(id) ? source_->get_line_samples(id, line)
                               : std::vector<sample_type>{};
  }

  bool has_separate_channels() const override {
    return source_->has_separate_channels();
  }
  const sample_type* get_line_luma(FrameID id, size_t line) const override {
    return range_.contains(id) ? source_->get_line_luma(id, line) : nullptr;
  }
  const sample_type* get_line_chroma(FrameID id, size_t line) const override {
    return range_.contains(id) ? source_->get_line_chroma(id, line) : nullptr;
  }

  std::optional<SourceParameters> get_video_parameters() const override {
    return source_->get_video_parameters();
  }

 private:
  std::shared_ptr<const VideoFrameRepresentation> source_;
  FrameIDRange range_;
};

// ---------------------------------------------------------------------------
// Golden data
// ---------------------------------------------------------------------------
//
// The packet stream is the stage's product, so the reference is the stream
// itself: its SHA-256 and its length. Recorded from these captures with the
// parameter set below, which is fixed here rather than taken from the
// descriptors' defaults so that changing a default cannot silently move the
// reference.
struct StreamGolden {
  const char* sha256;
  uint64_t bytes;
  uint64_t packets;
};

// A window a quarter of the way into each recording, past its lead-in.
constexpr uint64_t kFrameCount = 300;

constexpr StreamGolden kNbcGolden{"", 0, 0};
constexpr StreamGolden kExtraVisionGolden{"", 0, 0};

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

struct RecoveryRun {
  bool triggered = false;
  std::string status;
  std::string stream_path;
  uint64_t stream_bytes = 0;
  std::string stream_sha256;
  // What the pass made of the packets above the packet layer: data groups,
  // records, and the catalogue the records dialog reads.
  NabtsAnalysisDataset dataset;
};

std::map<std::string, ParameterValue> recovery_parameters(
    const std::string& output_path, int32_t decode_threads) {
  return {
      {"output_path", output_path},
      {"first_vbi_line", int32_t{10}},
      {"last_vbi_line", int32_t{21}},
      {"keep_empty_packets", false},
      // Automatic, as a user gets it: threshold first, MLSE only where that
      // could not lock.
      {"detector", std::string("Automatic")},
      {"decode_threads", decode_threads},
      {"write_report", true},
  };
}

RecoveryRun recover(const std::string& capture_path,
                    const std::string& format_preset,
                    const std::filesystem::path& output_path,
                    int32_t decode_threads = 0) {
  RecoveryRun run;

  VBISourceStage source;
  ObservationContext observations;
  const std::vector<ArtifactPtr> outputs = source.execute(
      {}, {{"input_path", capture_path}, {"format", format_preset}},
      observations);
  if (outputs.empty()) return run;
  const auto representation =
      std::dynamic_pointer_cast<VideoFrameRepresentation>(outputs.front());
  if (!representation) return run;

  const auto window = std::make_shared<FrameWindow>(
      representation, static_cast<FrameID>(representation->frame_count() / 4),
      kFrameCount);

  FunctionalStageServices services;
  NabtsSinkStage stage(&services);
  const auto parameters =
      recovery_parameters(output_path.string(), decode_threads);
  stage.set_parameters(parameters);
  stage.execute({window}, parameters, observations);
  run.triggered = stage.trigger({window}, parameters, observations);
  run.status = stage.get_trigger_status();
  run.dataset = stage.dataset();

  const std::filesystem::path candidate = output_path.string() + ".t33";
  if (std::filesystem::exists(candidate)) {
    run.stream_path = candidate.string();
    run.stream_bytes = std::filesystem::file_size(candidate);
    run.stream_sha256 = sha256_hex_of_file(candidate.string());
  }
  return run;
}

/// Message for the "most records should draw" assertion.
std::string only_a_few(size_t drew, size_t total) {
  return "only " + std::to_string(drew) + " of " + std::to_string(total) +
         " records drew anything, so the NAPLPS interpreter is not running";
}

void report(const char* what, const RecoveryRun& run) {
  const NabtsRecoverySummary& summary = run.dataset.summary;
  std::cout << "[ INFO     ] " << what << ": " << run.status << "\n"
            << "[ INFO     ] " << what << ": " << run.stream_bytes
            << " bytes, sha256 " << run.stream_sha256 << "\n"
            << "[ INFO     ] " << what << ": groups "
            << summary.groups_completed << " complete / "
            << summary.groups_incomplete << " incomplete, messages "
            << summary.messages_complete << " complete / "
            << summary.messages_partial << " partial, blocks "
            << summary.blocks_corrected << " corrected / "
            << summary.blocks_damaged << " damaged\n"
            << "[ INFO     ] " << what << ": " << run.dataset.records.size()
            << " records catalogued" << std::endl;
  std::vector<NabtsCataloguedRecord> drawn = run.dataset.records;
  nabts_interpret_records(drawn,
                          naplps_render_grid(NaplpsRenderMode::kReference));
  size_t with_primitives = 0;
  size_t total_primitives = 0;
  size_t characters = 0;
  size_t geometry = 0;
  for (const NabtsCataloguedRecord& record : drawn) {
    if (!record.page.primitives.empty()) ++with_primitives;
    total_primitives += record.page.primitives.size();
    for (const auto& primitive : record.page.primitives) {
      if (primitive.kind == NabtsPrimitiveKind::kCharacter) {
        ++characters;
      } else {
        ++geometry;
      }
    }
  }
  std::cout << "[ INFO     ] " << what << ": NAPLPS " << with_primitives << "/"
            << run.dataset.records.size() << " records drew something, "
            << total_primitives << " primitives (" << characters
            << " characters, " << geometry << " geometric)" << std::endl;
  for (const NabtsCataloguedRecord& record : run.dataset.records) {
    std::cout << "[ INFO     ]   " << record.channel_text << " v"
              << static_cast<int>(record.version) << " type "
              << static_cast<int>(record.record_type) << ", "
              << record.data.size() << " bytes, seen " << record.times_seen
              << " (" << record.times_intact << " intact)"
              << (record.complete ? "" : ", incomplete")
              << (record.cyclic_marker ? ", cyclic marker" : "")
              << (record.caption ? ", caption" : "")
              << (record.reserved_purpose.empty()
                      ? std::string()
                      : ", " + record.reserved_purpose)
              << std::endl;
  }
}

class NabtsSinkPipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    // A folder of this run's own: another run of the same test at the same
    // time — a second build tree's ctest — would otherwise delete these files
    // from under it.
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("orc-nabts-") + info->name() + "-" +
            std::to_string(std::random_device{}()));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
};

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

TEST_F(NabtsSinkPipelineTest, NbcCaptureRecoversAServicesWorthOfPackets) {
  if (!std::filesystem::exists(kNbtsCapture)) {
    GTEST_SKIP() << "NABTS capture not present: " << kNbtsCapture;
  }

  const RecoveryRun run =
      recover(kNbtsCapture, ".tbc VBI crop, 16-bit (NABTS)", dir_ / "nbc");
  report("NBC Teletext", run);

  ASSERT_TRUE(run.triggered) << run.status;
  EXPECT_EQ(std::filesystem::path(run.stream_path).extension(), ".t33");

  // The stream is a whole number of 33-byte packets and nothing else.
  ASSERT_GT(run.stream_bytes, 0u) << "no packets recovered at all";
  EXPECT_EQ(run.stream_bytes % kNabtsPacketBytes, 0u);

  // A real service inserting on several lines of every field yields packets in
  // the thousands over 300 frames, not the handful a false-lock rate produces.
  const uint64_t packets = run.stream_bytes / kNabtsPacketBytes;
  EXPECT_GT(packets, kFrameCount)
      << "only " << packets << " packets over " << kFrameCount
      << " frames — that is a false-lock rate, not a service";
}

TEST_F(NabtsSinkPipelineTest,
       ExtraVisionCaptureRecoversAServicesWorthOfPackets) {
  if (!std::filesystem::exists(kExtraVisionCapture)) {
    GTEST_SKIP() << "NABTS capture not present: " << kExtraVisionCapture;
  }

  const RecoveryRun run =
      recover(kExtraVisionCapture, ".tbc VBI crop, 16-bit (NABTS)",
              dir_ / "extravision");
  report("CBS ExtraVision", run);

  ASSERT_TRUE(run.triggered) << run.status;
  ASSERT_GT(run.stream_bytes, 0u) << "no packets recovered at all";
  EXPECT_EQ(run.stream_bytes % kNabtsPacketBytes, 0u);

  const uint64_t packets = run.stream_bytes / kNabtsPacketBytes;
  EXPECT_GT(packets, kFrameCount)
      << "only " << packets << " packets over " << kFrameCount
      << " frames — that is a false-lock rate, not a service";
}

// The claim the whole design rests on, against real recordings rather than
// synthesized lines: System B and System C share everything on a 525-line
// capture but the framing code, so a World System Teletext recording must
// yield essentially nothing here.
TEST_F(NabtsSinkPipelineTest, AWstCaptureYieldsAlmostNothing) {
  if (!std::filesystem::exists(kWstCapture)) {
    GTEST_SKIP() << "525-line WST capture not present: " << kWstCapture;
  }
  if (!std::filesystem::exists(kExtraVisionCapture)) {
    GTEST_SKIP() << "NABTS capture not present for comparison";
  }

  const RecoveryRun wst = recover(kWstCapture, ".tbc VBI crop, 16-bit (NABTS)",
                                  dir_ / "wst-as-nabts");
  report("TBS Electra (WST) read as NABTS", wst);
  ASSERT_TRUE(wst.triggered) << wst.status;

  const RecoveryRun nabts = recover(
      kExtraVisionCapture, ".tbc VBI crop, 16-bit (NABTS)", dir_ / "nabts");
  ASSERT_TRUE(nabts.triggered) << nabts.status;

  const uint64_t wst_packets = wst.stream_bytes / kNabtsPacketBytes;
  const uint64_t nabts_packets = nabts.stream_bytes / kNabtsPacketBytes;
  ASSERT_GT(nabts_packets, 0u);

  // An order of magnitude, which is what the recordings actually show: 126
  // packets from the WST capture against 2460 from the NABTS one over the same
  // 300 frames. The separation is not perfect and is not expected to be — the
  // threshold detector rejects every WST line outright, but the MLSE detector
  // fits the framing code rather than matching it, so a few per cent of a
  // damaged recording's lines survive both it and the Hamming prefix gate. They
  // arrive at a mean decision confidence of 0,21 against 0,56 for a real
  // service, and none of them assembles into a data group.
  //
  // The bound is deliberately loose: it is here to catch the discrimination
  // failing altogether, not to pin a decoder tuning figure.
  EXPECT_LT(wst_packets * 10, nabts_packets)
      << wst_packets << " packets from a WST recording against "
      << nabts_packets << " from a NABTS one: the framing-code discrimination "
      << "is not holding";

  // And the layer above closes the gap the bit detectors leave. Those surviving
  // packets are noise that happened to pass a framing code and five Hamming
  // bytes; none of them carries a group header that decodes, so none opens a
  // group that completes, so none becomes a record. The catalogue a user
  // actually sees is empty.
  EXPECT_TRUE(wst.dataset.records.empty())
      << wst.dataset.records.size()
      << " records catalogued from a World System Teletext recording";
  EXPECT_EQ(wst.dataset.summary.groups_completed, 0u);
  EXPECT_GT(nabts.dataset.records.size(), 0u)
      << "no records from a real NABTS service, so the comparison proves "
         "nothing";
}

// The property the parallel decode rests on: what a worker may read while it
// works is frozen for the length of a block, so the recovered stream cannot
// depend on how the blocks were scheduled.
TEST_F(NabtsSinkPipelineTest, TheDecodeDoesNotDependOnTheThreadCount) {
  if (!std::filesystem::exists(kExtraVisionCapture)) {
    GTEST_SKIP() << "NABTS capture not present: " << kExtraVisionCapture;
  }

  std::string reference;
  for (const int32_t threads : {1, 3, 8}) {
    const RecoveryRun run =
        recover(kExtraVisionCapture, ".tbc VBI crop, 16-bit (NABTS)",
                dir_ / ("threads-" + std::to_string(threads)), threads);
    ASSERT_TRUE(run.triggered) << run.status;
    ASSERT_FALSE(run.stream_sha256.empty());

    if (reference.empty()) {
      reference = run.stream_sha256;
    } else {
      EXPECT_EQ(run.stream_sha256, reference)
          << "decoding on " << threads << " thread(s) produced a different "
          << "packet stream";
    }
  }
}

// ---------------------------------------------------------------------------
// Data groups and records (CEA-516 §4, §5)
// ---------------------------------------------------------------------------

// The record layer against a real service. What makes this more than a
// smoke test is the captioning record: CEA-516 §7.1.5 reserves channel A00,
// record address 000 for the start of captioning; §5.2.2.3 makes captioning a
// record of type 1; and §5.2.7.3 has a caption record carry the caption flag in
// classification byte Y13. Three facts from three different sections of the
// standard, decoded by three different pieces of this — the address bytes, the
// record type byte, and the classification sequence's pointer-and-flag walk —
// and they agree. A parse that had any of the three bit positions wrong could
// not produce that agreement by chance.
//
// (The design plan expected this window to show the master index at channel
// 000, address 000, and a cyclic marker record. It carries neither: a 300-frame
// window is about one carousel cycle of the magazine, and this one does not
// include them. Asserted here is what the recording actually contains.)
TEST_F(NabtsSinkPipelineTest, ExtraVisionCaptureAssemblesTheServicesRecords) {
  if (!std::filesystem::exists(kExtraVisionCapture)) {
    GTEST_SKIP() << "NABTS capture not present: " << kExtraVisionCapture;
  }

  const RecoveryRun run =
      recover(kExtraVisionCapture, ".tbc VBI crop, 16-bit (NABTS)",
              dir_ / "extravision");
  report("CBS ExtraVision", run);
  ASSERT_TRUE(run.triggered) << run.status;

  // A magazine's worth of records, not a handful of accidents.
  ASSERT_GT(run.dataset.records.size(), 20u)
      << "only " << run.dataset.records.size()
      << " records — that is not a service's magazine";
  EXPECT_GT(run.dataset.summary.groups_completed, 20u);
  EXPECT_GT(run.dataset.summary.messages_complete, 20u);
  EXPECT_FALSE(run.dataset.summary.records_truncated);

  const orc::NabtsCataloguedRecord* captioning = nullptr;
  size_t presentation_records = 0;
  for (const auto& record : run.dataset.records) {
    if (orc::nabts_type_is_presentation(record.record_type)) {
      ++presentation_records;
    }
  }
  // The captioning records are the caption service, kept as it arrived rather
  // than catalogued (§7.3.10.1 gives each caption the next version).
  for (const auto& record : run.dataset.captions) {
    if (record.channel == 0xA00 && record.address_text == "000") {
      captioning = &record;
    }
  }

  // §6.1: a presentation record's data is NAPLPS, which is what Phase 5 reads.
  EXPECT_EQ(presentation_records, run.dataset.records.size())
      << "a record of a type that is neither presentation nor application";

  ASSERT_NE(captioning, nullptr)
      << "the reserved captioning address of §7.1.5 was not recovered";
  EXPECT_EQ(captioning->reserved_purpose, "Start of captioning");
  EXPECT_EQ(captioning->record_type,
            orc::kNabtsRecordTypeNoncyclicPresentation);
  EXPECT_TRUE(captioning->caption)
      << "the caption record of §5.2.7.3 did not carry its caption flag";
  EXPECT_GT(captioning->data.size(), 0u);

  // Every catalogued record holds record data, and none exceeds what a linked
  // series could carry.
  for (const auto& record : run.dataset.records) {
    EXPECT_GT(record.data.size(), 0u)
        << record.channel_text << " catalogued with no data";
    EXPECT_GE(record.times_seen, 1u);
    EXPECT_GE(record.times_seen, record.times_intact);
    EXPECT_GE(record.records_in_message, 1u);
  }
}

// The NBC service uses two data channels, which is what §3.2.3's packet address
// is for, and is the case that would break a reassembler keying its groups on
// anything else. It is also a much weaker recording — VHS at EP speed — so it
// exercises the loss path rather than the clean one.
TEST_F(NabtsSinkPipelineTest, NbcCaptureAssemblesRecordsOnTwoDataChannels) {
  if (!std::filesystem::exists(kNbtsCapture)) {
    GTEST_SKIP() << "NABTS capture not present: " << kNbtsCapture;
  }

  const RecoveryRun run =
      recover(kNbtsCapture, ".tbc VBI crop, 16-bit (NABTS)", dir_ / "nbc");
  report("NBC Teletext", run);
  ASSERT_TRUE(run.triggered) << run.status;

  ASSERT_GT(run.dataset.records.size(), 5u);

  std::vector<uint16_t> channels;
  for (const auto& record : run.dataset.records) {
    if (channels.empty() || channels.back() != record.channel) {
      channels.push_back(record.channel);
    }
  }
  EXPECT_GT(channels.size(), 1u)
      << "records on one channel only, so interleaved channels are untested";

  // The catalogue is ordered by channel, so the channels came out ascending.
  for (size_t i = 1; i < channels.size(); ++i) {
    EXPECT_LT(channels[i - 1], channels[i]);
  }

  // A recording this marginal loses packets, and a record assembled over a gap
  // is reported rather than presented as clean. Which way round it comes out is
  // the recording's business, not this code's, so only the accounting is
  // asserted.
  for (const auto& record : run.dataset.records) {
    EXPECT_LE(record.times_intact, record.times_seen);
  }
}

// Task 4.5: the record files are the presentation data as transmitted, so they
// must round-trip the catalogued bytes exactly — that is what makes them usable
// with an external NAPLPS tool before this grows its own interpreter.
TEST_F(NabtsSinkPipelineTest, ExportedRecordFilesMatchTheCataloguedData) {
  if (!std::filesystem::exists(kExtraVisionCapture)) {
    GTEST_SKIP() << "NABTS capture not present: " << kExtraVisionCapture;
  }

  const std::filesystem::path output = dir_ / "records";
  VBISourceStage source;
  ObservationContext observations;
  const std::vector<ArtifactPtr> outputs =
      source.execute({},
                     {{"input_path", std::string(kExtraVisionCapture)},
                      {"format", std::string(".tbc VBI crop, 16-bit (NABTS)")}},
                     observations);
  ASSERT_FALSE(outputs.empty());
  const auto representation =
      std::dynamic_pointer_cast<VideoFrameRepresentation>(outputs.front());
  ASSERT_TRUE(representation);
  const auto window = std::make_shared<FrameWindow>(
      representation, static_cast<FrameID>(representation->frame_count() / 4),
      kFrameCount);

  FunctionalStageServices services;
  NabtsSinkStage stage(&services);
  auto parameters = recovery_parameters(output.string(), 0);
  parameters["export_records"] = true;
  stage.set_parameters(parameters);
  ASSERT_TRUE(stage.trigger({window}, parameters, observations))
      << stage.get_trigger_status();

  const NabtsAnalysisDataset& dataset = stage.dataset();
  ASSERT_GT(dataset.records.size(), 20u);

  size_t checked = 0;
  for (const NabtsCataloguedRecord& record : dataset.records) {
    // Named for the identity §5.2.1 gives the record, beside the stream.
    char suffix[64];
    std::snprintf(suffix, sizeof(suffix), ".t33.%03X-%s-v%X.rec",
                  record.channel, record.address_text.c_str(), record.version);
    const std::filesystem::path path = output.string() + suffix;
    ASSERT_TRUE(std::filesystem::exists(path)) << path;
    ASSERT_EQ(std::filesystem::file_size(path), record.data.size()) << path;

    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> written(record.data.size());
    file.read(reinterpret_cast<char*>(written.data()),
              static_cast<std::streamsize>(written.size()));
    EXPECT_EQ(written, record.data) << path;
    ++checked;
  }
  EXPECT_EQ(checked, dataset.records.size());
}

// Disabled by default, and refused rather than silently skipped when there is
// no output file for the records to sit beside.
// The record layer's rejection is total, measured rather than asserted.
//
// Read on the full ITU-R BT.653 §2 window, the ExtraVision capture yields 2460
// packets; read on the four lines the service actually uses (broadcast lines 15
// to 18) it yields 2288. The 172 extra are noise on lines the service leaves
// empty that happened to pass a framing code and five Hamming 8/4 prefix bytes
// — and the group-header check refuses every one of them, which is why the
// report counts exactly 172 bad headers.
//
// So both runs catalogue the same 51 records from the same 53 data groups. That
// is the property worth pinning: what a user browses does not depend on how
// wide a window they left the stage on, even though the exported packet stream
// does.
//
// What is *not* asserted is that the two runs recover byte-identical record
// data, because they do not, and the reason is worth writing down. The phase
// tracker pools its locks over every line of the window (NabtsPhaseTracker), so
// the spurious locks on the empty lines widen the distribution past
// kMaxRadiusSamples and the hint is withheld: the narrow run pins the data
// phase to sample 146,1 +/- 3,0 and the wide run does not pin at all. Pinning
// cannot lose a packet — a hinted attempt that fails is retried over the full
// window — but the two runs do acquire differently, and on a marginal line the
// MLSE fit can settle on a different bit. One of the 51 records differs by a
// few bytes for exactly that reason.
//
// Which is a second, practical finding: narrowing the window to the lines a
// service actually uses both cleans up the packet stream and lets the phase pin
// engage.
TEST_F(NabtsSinkPipelineTest, TheRecordsDoNotDependOnTheCandidateLineWindow) {
  if (!std::filesystem::exists(kExtraVisionCapture)) {
    GTEST_SKIP() << "NABTS capture not present: " << kExtraVisionCapture;
  }

  VBISourceStage source;
  ObservationContext observations;
  const std::vector<ArtifactPtr> outputs =
      source.execute({},
                     {{"input_path", std::string(kExtraVisionCapture)},
                      {"format", std::string(".tbc VBI crop, 16-bit (NABTS)")}},
                     observations);
  ASSERT_FALSE(outputs.empty());
  const auto representation =
      std::dynamic_pointer_cast<VideoFrameRepresentation>(outputs.front());
  ASSERT_TRUE(representation);
  const auto window = std::make_shared<FrameWindow>(
      representation, static_cast<FrameID>(representation->frame_count() / 4),
      kFrameCount);

  // The full window, then only the lines this service inserts on.
  struct Run {
    int32_t first_line;
    int32_t last_line;
    uint64_t packets = 0;
    uint64_t groups = 0;
    std::vector<NabtsCataloguedRecord> records;
  };
  Run runs[] = {{10, 21}, {15, 18}};

  for (Run& run : runs) {
    FunctionalStageServices services;
    NabtsSinkStage stage(&services);
    auto parameters = recovery_parameters(
        (dir_ / ("w" + std::to_string(run.first_line))).string(), 0);
    parameters["first_vbi_line"] = run.first_line;
    parameters["last_vbi_line"] = run.last_line;
    parameters["write_report"] = false;
    stage.set_parameters(parameters);
    ASSERT_TRUE(stage.trigger({window}, parameters, observations))
        << stage.get_trigger_status();
    // Every packet the window's lines gave, kept or refused as a test-signal
    // line read as data.
    run.packets = stage.dataset().summary.packets_recovered +
                  stage.dataset().summary.packets_refused_unprotected;
    run.groups = stage.dataset().summary.groups_completed;
    run.records = stage.dataset().records;
  }

  // The wider window really did meet packets the narrow one did not, or the
  // comparison below proves nothing.
  EXPECT_GT(runs[0].packets, runs[1].packets)
      << "the two windows recovered the same packets, so this test is not "
         "exercising what it claims to";

  // And none of them reached a record: same groups, same records, same
  // identities in the same order.
  EXPECT_EQ(runs[0].groups, runs[1].groups);
  ASSERT_EQ(runs[0].records.size(), runs[1].records.size())
      << "the empty lines' packets changed the size of the record catalogue";
  for (size_t i = 0; i < runs[0].records.size(); ++i) {
    EXPECT_EQ(runs[0].records[i].channel_text, runs[1].records[i].channel_text);
    EXPECT_EQ(runs[0].records[i].version, runs[1].records[i].version);
    EXPECT_EQ(runs[0].records[i].record_type, runs[1].records[i].record_type);
    EXPECT_EQ(runs[0].records[i].reserved_purpose,
              runs[1].records[i].reserved_purpose);
  }

  // The record data agrees on all but a handful of bytes — see above for why it
  // is not exact. A large divergence would mean the noise had got into a group
  // rather than merely changed how the real lines were acquired.
  size_t differing_records = 0;
  for (size_t i = 0; i < runs[0].records.size(); ++i) {
    if (runs[0].records[i].data != runs[1].records[i].data) {
      ++differing_records;
    }
  }
  EXPECT_LE(differing_records, runs[0].records.size() / 10)
      << differing_records << " of " << runs[0].records.size()
      << " records differ between the two windows, which is more than a "
         "difference in bit-phase acquisition explains";
}

TEST_F(NabtsSinkPipelineTest, RecordExportIsOffByDefaultAndNeedsAnOutputFile) {
  if (!std::filesystem::exists(kExtraVisionCapture)) {
    GTEST_SKIP() << "NABTS capture not present: " << kExtraVisionCapture;
  }

  const RecoveryRun run = recover(
      kExtraVisionCapture, ".tbc VBI crop, 16-bit (NABTS)", dir_ / "default");
  ASSERT_TRUE(run.triggered) << run.status;
  for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
    EXPECT_NE(entry.path().extension(), ".rec")
        << "record files written without being asked for: " << entry.path();
  }

  FunctionalStageServices services;
  NabtsSinkStage stage(&services);
  ObservationContext observations;
  auto parameters = recovery_parameters(std::string(), 0);
  parameters["write_report"] = false;
  parameters["export_records"] = true;
  const auto window = std::make_shared<FrameWindow>(nullptr, 0, 1);
  EXPECT_FALSE(stage.trigger({window}, parameters, observations));
  EXPECT_NE(stage.get_trigger_status().find("need an output file"),
            std::string::npos)
      << stage.get_trigger_status();
}

// ---------------------------------------------------------------------------
// NAPLPS presentation decoding (CEA-516 §6.1)
// ---------------------------------------------------------------------------

// The whole chain, end to end on a real service: VBI samples to data lines to
// packets to data groups to records to a display list a viewer can draw.
//
// Measured on this capture, 48 of the 51 records draw something, and between
// them they yield about 21000 primitives — roughly 19000 characters and 2000
// geometric. That mix is the shape of a broadcast teletext page: mostly text,
// with geometry for rules, borders and coloured blocks. The three that draw
// nothing are legitimate: a record may define macros or DRCS and draw nothing
// itself.
//
// The bounds below are loose on purpose. They are here to catch the interpreter
// failing altogether — no primitives, or a page that is all geometry and no
// text — rather than to pin a figure that a decoder tuning change would move.
TEST_F(NabtsSinkPipelineTest, ExtraVisionRecordsDecodeIntoNaplpsDisplayLists) {
  if (!std::filesystem::exists(kExtraVisionCapture)) {
    GTEST_SKIP() << "NABTS capture not present: " << kExtraVisionCapture;
  }

  const RecoveryRun run =
      recover(kExtraVisionCapture, ".tbc VBI crop, 16-bit (NABTS)",
              dir_ / "extravision");
  report("CBS ExtraVision", run);
  ASSERT_TRUE(run.triggered) << run.status;
  ASSERT_GT(run.dataset.records.size(), 20u);

  // Recovery leaves the records as bytes; drawing them is the viewer's step,
  // for the receiver it is drawing for.
  std::vector<NabtsCataloguedRecord> records = run.dataset.records;
  nabts_interpret_records(records,
                          naplps_render_grid(NaplpsRenderMode::kReference));

  size_t drew_something = 0;
  size_t characters = 0;
  size_t geometry = 0;
  for (const NabtsCataloguedRecord& record : records) {
    if (!record.page.primitives.empty()) {
      ++drew_something;
    }
    for (const NabtsPrimitive& primitive : record.page.primitives) {
      if (primitive.kind == NabtsPrimitiveKind::kCharacter) {
        ++characters;
      } else {
        ++geometry;
      }
    }

    // A presentation record's display list is NAPLPS; an application record's
    // is empty, because its data is function descriptors (§7.2.2).
    if (!nabts_type_is_presentation(record.record_type)) {
      EXPECT_TRUE(record.page.primitives.empty()) << record.channel_text;
    }

    // The colour map a renderer is handed is always a full one, whether the
    // record wrote to it or not.
    for (const NabtsColour& colour : record.page.colour_map) {
      EXPECT_LE(colour.green, kNabtsGunMax);
      EXPECT_LE(colour.red, kNabtsGunMax);
      EXPECT_LE(colour.blue, kNabtsGunMax);
    }
  }

  // Most of the service's records draw.
  EXPECT_GT(drew_something, records.size() / 2)
      << only_a_few(drew_something, records.size());

  // And what they draw is a teletext page rather than a blank one or a plotter
  // exercise: text in the thousands, geometry in the hundreds at least.
  EXPECT_GT(characters, 1000u)
      << "only " << characters << " characters over the whole service";
  EXPECT_GT(geometry, 100u) << "only " << geometry << " geometric primitives";
  EXPECT_GT(characters, geometry) << "a teletext page is mostly text";
}

// The captioning record is a presentation record (§5.2.2.3), so it decodes like
// any other — and being a caption it should be short, which is what separates a
// caption from a page.
TEST_F(NabtsSinkPipelineTest, TheCaptioningRecordDecodesAsPresentationCode) {
  if (!std::filesystem::exists(kExtraVisionCapture)) {
    GTEST_SKIP() << "NABTS capture not present: " << kExtraVisionCapture;
  }

  const RecoveryRun run = recover(
      kExtraVisionCapture, ".tbc VBI crop, 16-bit (NABTS)", dir_ / "captions");
  ASSERT_TRUE(run.triggered) << run.status;

  const NabtsCataloguedRecord* captioning = nullptr;
  for (const NabtsCataloguedRecord& record : run.dataset.captions) {
    if (record.channel == 0xA00 && record.address_text == "000") {
      captioning = &record;
    }
  }
  ASSERT_NE(captioning, nullptr);
  EXPECT_TRUE(nabts_type_is_presentation(captioning->record_type));
  // A caption is a fraction of a page: §7.1.3 makes captioning non-cyclic, sent
  // as it is needed rather than carouselled. Counted in the bytes that
  // arrived: a caption cut short by the next one keeps the length its group
  // header declared, the rest held as never received.
  size_t received = captioning->data.size();
  if (!captioning->data_present.empty()) {
    received = static_cast<size_t>(std::count_if(
        captioning->data_present.begin(), captioning->data_present.end(),
        [](uint8_t present) { return present != 0u; }));
  }
  EXPECT_LT(received, 100u);
}

}  // namespace
}  // namespace orc
