/*
 * File:        sinks_test.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     The sinks on videosynth cases: the audio sink and the EFM
 *              sink against the source audio, sample for sample; RF64
 *              channel pairs against their RIFF twins; the CVBS
 *              sink's round trip; SECAM through the TBC sink and back
 *              through tbc_source; and the video sink's frame count, its
 *              deinterlacing, its weaving of the frames whose fields are one
 *              instant, its held-frame decoder schedule, and the disc
 *              metadata it remuxes into the file's header
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <decoded_comparison.h>
#include <gtest/gtest.h>
#include <orc/abi/orc_plugin_services.h>
#include <orc/stage/common_types.h>
#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/params/parameter_types.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/observers/colour_frame_phase_observer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "audio_sink_stage.h"
#include "cvbs_sink_stage_deps.h"
#include "efm_sink_stage.h"
#include "stacker_stage.h"
#include "tbc_sink_stage.h"
#include "tbc_source_stage.h"
#include "video_sink_stage.h"
#include "videosynth_cases.h"
#include "videosynth_harness.h"

namespace orc_videosynth_test {

namespace {

namespace fs = std::filesystem;

using orc_videosynth::CaseFile;
using orc_videosynth::VfrPtr;

using Parameters = std::map<std::string, orc::ParameterValue>;

CaseFile sinks_case(const std::string& system_name, orc::VideoSystem system) {
  return {system_name + "_sinks", "videosynth_" + system_name + "_sinks",
          system};
}

// A file the test writes, under <output dir>/work/; nullopt, with the test
// failed, outside ctest.
std::optional<fs::path> work_path(const std::string& name) {
  const auto dir = orc_videosynth::case_dir("work");
  if (!dir) {
    ADD_FAILURE() << "ORC_VIDEOSYNTH_OUTPUT_DIR is not set; run through ctest";
    return std::nullopt;
  }
  fs::create_directories(*dir);
  return *dir / name;
}

std::vector<uint8_t> read_bytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
}

// The bytes of a RIFF WAV file's data chunk; nullopt when there is none.
std::optional<std::vector<uint8_t>> wav_data(const fs::path& path) {
  const auto bytes = read_bytes(path);
  auto u32 = [&](size_t at) {
    return static_cast<uint32_t>(bytes[at] | (bytes[at + 1] << 8) |
                                 (bytes[at + 2] << 16) |
                                 (static_cast<uint32_t>(bytes[at + 3]) << 24));
  };
  if (bytes.size() < 12 ||
      std::string(bytes.begin(), bytes.begin() + 4) != "RIFF") {
    return std::nullopt;
  }
  size_t at = 12;
  while (at + 8 <= bytes.size()) {
    const std::string id(bytes.begin() + at, bytes.begin() + at + 4);
    const size_t size = u32(at + 4);
    if (id == "data") {
      const size_t end = std::min(bytes.size(), at + 8 + size);
      return std::vector<uint8_t>(bytes.begin() + at + 8, bytes.begin() + end);
    }
    at += 8 + size + (size & 1);
  }
  return std::nullopt;
}

std::vector<int16_t> as_int16(const std::vector<uint8_t>& bytes) {
  std::vector<int16_t> samples(bytes.size() / 2);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = static_cast<int16_t>(bytes[2 * i] | (bytes[2 * i + 1] << 8));
  }
  return samples;
}

// ---------------------------------------------------------------------------
// Audio sink
// ---------------------------------------------------------------------------

// Each channel pair the audio sink writes is the pair videosynth wrote: the
// same 48 kHz 24-bit samples, which cvbs_source passes through untouched.
void run_audio_case(const std::string& system_name, orc::VideoSystem system) {
  const auto file = sinks_case(system_name, system);
  const auto artifact = orc_videosynth::open_case(file);
  ASSERT_NE(artifact, nullptr);
  const auto base = orc_videosynth::case_base(file);
  ASSERT_TRUE(base.has_value());
  for (const int32_t pair : {0, 1}) {
    const auto out =
        work_path(system_name + "_audio_" + std::to_string(pair) + ".wav");
    ASSERT_TRUE(out.has_value());
    orc::AudioSinkStage sink;
    orc::ObservationContext ctx;
    ASSERT_TRUE(sink.trigger(
        {artifact}, {{"output_path", out->string()}, {"channel_pair", pair}},
        ctx))
        << sink.get_trigger_status();
    const auto written = wav_data(*out);
    const auto source =
        wav_data(base->string() + "_audio_" + std::to_string(pair) + ".wav");
    ASSERT_TRUE(written.has_value()) << "pair " << pair;
    ASSERT_TRUE(source.has_value()) << "pair " << pair;
    EXPECT_EQ(written->size(), source->size()) << "pair " << pair;
    EXPECT_TRUE(*written == *source) << "pair " << pair << " differs";
    fs::remove(*out);
  }
}

// The RF64 twin of a sink case (variants.json, "sinks-rf64"): the same
// project with output.audio_container: rf64, so its channel pair files carry
// the same samples behind an 80-byte RF64 header. cvbs_source must read the
// samples the RIFF case carries, frame for frame, and the audio sink must
// write them back out as the RIFF case's data chunk.
void run_rf64_audio_case(const std::string& system_name,
                         orc::VideoSystem system) {
  const CaseFile riff_file = sinks_case(system_name, system);
  const CaseFile rf64_file{system_name + "_sinks_rf64",
                           "videosynth_" + system_name + "_sinks_rf64", system};
  const auto rf64_base = orc_videosynth::case_base(rf64_file);
  const auto riff_base = orc_videosynth::case_base(riff_file);
  ASSERT_TRUE(rf64_base.has_value() && riff_base.has_value());
  for (const int pair : {0, 1}) {
    const auto header = read_bytes(rf64_base->string() + "_audio_" +
                                   std::to_string(pair) + ".wav");
    ASSERT_GE(header.size(), 80u) << "pair " << pair;
    EXPECT_EQ(std::string(header.begin(), header.begin() + 4), "RF64")
        << "pair " << pair << ": the variant did not force RF64";
    EXPECT_EQ(std::string(header.begin() + 12, header.begin() + 16), "ds64")
        << "pair " << pair;
  }

  const auto riff =
      orc_videosynth::as_vfr(orc_videosynth::open_case(riff_file));
  const auto rf64_artifact = orc_videosynth::open_case(rf64_file);
  ASSERT_NE(riff, nullptr);
  ASSERT_NE(rf64_artifact, nullptr);
  const auto rf64 = orc_videosynth::as_vfr(rf64_artifact);
  ASSERT_NE(rf64, nullptr);
  ASSERT_EQ(rf64->frame_count(), riff->frame_count());
  ASSERT_EQ(rf64->audio_channel_pair_count(), riff->audio_channel_pair_count());
  ASSERT_EQ(rf64->audio_channel_pair_count(), 2u);
  for (orc::FrameID f = 0; f < riff->frame_count(); ++f) {
    for (size_t pair = 0; pair < riff->audio_channel_pair_count(); ++pair) {
      ASSERT_EQ(rf64->get_audio_samples(pair, f),
                riff->get_audio_samples(pair, f))
          << "frame " << f << " pair " << pair;
    }
  }

  for (const int32_t pair : {0, 1}) {
    const auto out =
        work_path(system_name + "_rf64_audio_" + std::to_string(pair) + ".wav");
    ASSERT_TRUE(out.has_value());
    orc::AudioSinkStage sink;
    orc::ObservationContext ctx;
    ASSERT_TRUE(sink.trigger(
        {rf64_artifact},
        {{"output_path", out->string()}, {"channel_pair", pair}}, ctx))
        << sink.get_trigger_status();
    const auto written = wav_data(*out);
    const auto source = wav_data(riff_base->string() + "_audio_" +
                                 std::to_string(pair) + ".wav");
    ASSERT_TRUE(written.has_value()) << "pair " << pair;
    ASSERT_TRUE(source.has_value()) << "pair " << pair;
    EXPECT_TRUE(*written == *source) << "pair " << pair << " differs";
    fs::remove(*out);
  }
  orc_videosynth::note("RF64 channel pairs read as their RIFF twin for " +
                       system_name);
}

// ---------------------------------------------------------------------------
// EFM sink
// ---------------------------------------------------------------------------

// The programme's pair 0 is sink_audio.wav (44.1 kHz 16-bit stereo), which
// videosynth encodes as EFM track 2 unchanged; the EFM sink's decode must
// hold it sample for sample. The decode is aligned to the file by a window from
// its middle, so the latency of the decode does not matter.
constexpr size_t kAlignWindowSamples = 64;  // interleaved, 32 stereo pairs
constexpr double kMinimumCoverSeconds = 1.0;

void run_efm_case(const std::string& system_name, orc::VideoSystem system) {
  const auto file = sinks_case(system_name, system);
  const auto artifact = orc_videosynth::open_case(file);
  ASSERT_NE(artifact, nullptr);
  ASSERT_TRUE(orc_videosynth::as_vfr(artifact)->has_efm());
  const auto out = work_path(system_name + "_efm.wav");
  ASSERT_TRUE(out.has_value());
  orc::EFMSinkStage sink;
  orc::ObservationContext ctx;
  ASSERT_TRUE(sink.trigger({artifact},
                           {{"output_path", out->string()},
                            {"decode_mode", std::string("audio")},
                            {"video_sync", false}},
                           ctx))
      << sink.get_trigger_status();
  const auto decoded_bytes = wav_data(*out);
  ASSERT_TRUE(decoded_bytes.has_value());
  const auto decoded = as_int16(*decoded_bytes);

  const auto reference_bytes =
      wav_data(fs::path(ORC_VIDEOSYNTH_MEDIA_DIR) / "sink_audio.wav");
  ASSERT_TRUE(reference_bytes.has_value());
  const auto reference = as_int16(*reference_bytes);

  // Where the reference's 0.5 s mark (44100 interleaved samples) sits in the
  // decode.
  const size_t anchor = 44100;
  ASSERT_GT(reference.size(), anchor + kAlignWindowSamples);
  const auto window = reference.begin() + static_cast<std::ptrdiff_t>(anchor);
  const auto found = std::search(decoded.begin(), decoded.end(), window,
                                 window + kAlignWindowSamples);
  ASSERT_NE(found, decoded.end())
      << "the reference's samples are not in the EFM decode";
  const auto offset = static_cast<std::ptrdiff_t>(found - decoded.begin()) -
                      static_cast<std::ptrdiff_t>(anchor);

  // The run of matching samples around the anchor, and any mismatch inside
  // the span the decode and the reference share.
  size_t first = anchor;
  while (first > 0 && static_cast<std::ptrdiff_t>(first) + offset > 0 &&
         decoded[static_cast<size_t>(first - 1 + offset)] ==
             reference[first - 1]) {
    --first;
  }
  size_t last = anchor;
  while (last + 1 < reference.size() &&
         static_cast<size_t>(static_cast<std::ptrdiff_t>(last + 1) + offset) <
             decoded.size() &&
         decoded[static_cast<size_t>(static_cast<std::ptrdiff_t>(last + 1) +
                                     offset)] == reference[last + 1]) {
    ++last;
  }
  const double covered = static_cast<double>(last - first + 1) / 2.0 / 44100.0;
  EXPECT_GE(covered, kMinimumCoverSeconds)
      << "only " << covered << " s of the programme decodes sample for "
      << "sample (reference samples " << first << "-" << last << ")";
  orc_videosynth::note("EFM decode matches the source audio for " +
                       std::to_string(covered) + " s");
  // Kept when the case fails, to be looked at.
  if (!::testing::Test::HasFailure()) fs::remove(*out);
}

// ---------------------------------------------------------------------------
// CVBS sink round trip
// ---------------------------------------------------------------------------

using RunShape = std::tuple<orc::FrameID, uint64_t, uint32_t, uint8_t>;

std::vector<RunShape> runs_of(const orc::VideoFrameRepresentation& vfr,
                              orc::FrameID f) {
  std::vector<RunShape> runs;
  for (const auto& run : vfr.get_dropout_hints(f)) {
    runs.emplace_back(f, run.sample_start, run.sample_count, run.severity);
  }
  std::sort(runs.begin(), runs.end());
  return runs;
}

// Written by the CVBS sink and read back by cvbs_source, a case is what it
// was: samples, dropouts, audio, EFM and the signal levels.
void run_cvbs_round_trip(const std::string& system_name,
                         orc::VideoSystem system) {
  const auto original = orc_videosynth::as_vfr(
      orc_videosynth::open_case(sinks_case(system_name, system)));
  ASSERT_NE(original, nullptr);
  const std::string stem = system_name + "_round_trip";
  const auto base = work_path(stem);
  ASSERT_TRUE(base.has_value());

  std::atomic<bool> cancel{false};
  orc::CVBSSinkStageDeps deps;
  deps.init({}, &cancel);
  orc::CVBSSinkWriteConfig config;
  config.output_base_path = base->string();
  const auto result = deps.write_cvbs(original.get(), config);
  ASSERT_TRUE(result.success) << result.status_message;
  ASSERT_EQ(result.frames_written, original->frame_count());

  const auto copy = orc_videosynth::as_vfr(
      orc_videosynth::open_case(CaseFile{"work", stem, system}));
  ASSERT_NE(copy, nullptr);
  ASSERT_EQ(copy->frame_count(), original->frame_count());
  const auto a = original->get_video_parameters();
  const auto b = copy->get_video_parameters();
  ASSERT_TRUE(a.has_value() && b.has_value());
  EXPECT_EQ(a->system, b->system);
  EXPECT_EQ(a->blanking_level, b->blanking_level);
  EXPECT_EQ(a->black_level, b->black_level);
  EXPECT_EQ(a->white_level, b->white_level);
  EXPECT_EQ(a->sync_tip_level, b->sync_tip_level);
  EXPECT_EQ(a->is_subcarrier_locked, b->is_subcarrier_locked);
  EXPECT_EQ(original->audio_channel_pair_count(),
            copy->audio_channel_pair_count());
  EXPECT_EQ(original->has_efm(), copy->has_efm());

  size_t frames_differing = 0;
  for (orc::FrameID f = 0; f < original->frame_count(); ++f) {
    if (original->get_frame_copy(f) != copy->get_frame_copy(f)) {
      ++frames_differing;
    }
    EXPECT_EQ(runs_of(*copy, f), runs_of(*original, f)) << "frame " << f;
    for (size_t pair = 0; pair < original->audio_channel_pair_count(); ++pair) {
      EXPECT_EQ(copy->get_audio_samples(pair, f),
                original->get_audio_samples(pair, f))
          << "frame " << f << " pair " << pair;
    }
    if (original->has_efm()) {
      EXPECT_EQ(copy->get_efm_samples(f), original->get_efm_samples(f))
          << "frame " << f;
    }
  }
  EXPECT_EQ(frames_differing, 0u);
  for (const char* suffix : {".cvbs", ".meta", ".dropouts.meta", "_audio_0.wav",
                             "_audio_1.wav", ".efm", ".efm.meta"}) {
    fs::remove(base->string() + suffix);
  }
}

// ---------------------------------------------------------------------------
// TBC sink round trip (SECAM)
// ---------------------------------------------------------------------------

// The colour_frame_index the colour-frame observer gives each frame of |vfr|.
std::vector<int32_t> colour_frame_indices(
    const orc::VideoFrameRepresentation& vfr) {
  orc::ColourFramePhaseObserver observer;
  orc::ObservationContext ctx;
  std::vector<int32_t> indices;
  for (orc::FrameID f = 0; f < vfr.frame_count(); ++f) {
    observer.process_frame(vfr, f, ctx);
    const auto value = ctx.get(orc::FieldID(f * 2), "colour_frame_phase",
                               "colour_frame_index");
    indices.push_back(value && std::holds_alternative<int32_t>(*value)
                          ? std::get<int32_t>(*value)
                          : 0);
  }
  return indices;
}

// A TBC cannot say it carries SECAM: the TBC sink writes it as PAL, on
// PAL's line-locked grid, with a capture note. Read back by tbc_source in a
// SECAM project it is SECAM again, unlocked, on the 4fsc lattice. The
// lattice and the line-locked grid differ by under 2 samples a line, so the
// round trip resamples each line twice; the samples come back within the
// interpolation's error (within 1 code outside the dropouts, measured), the
// dropouts flagged where they were, and the D'R/D'B sequence, which lives in
// each line's sub-carrier frequency, unchanged.
constexpr double kTbcRoundTripRmsCodes = 0.5;
constexpr int kTbcRoundTripMaxCodes = 2;
constexpr size_t kTbcDropoutMargin = 16;
constexpr size_t kTbcDropoutWidening = 2;

void run_tbc_round_trip(const std::string& system_name,
                        orc::VideoSystem system) {
  const auto original_artifact =
      orc_videosynth::open_case(sinks_case(system_name, system));
  const auto original = orc_videosynth::as_vfr(original_artifact);
  ASSERT_NE(original, nullptr);
  const auto base = work_path(system_name + "_tbc_round_trip");
  ASSERT_TRUE(base.has_value());
  const std::string tbc = base->string() + ".tbc";

  // The stage writes through the host's services (videosynth_main.cpp).
  orc::TBCSinkStage sink(orc::plugin::get_stage_services());
  orc::ObservationContext sink_ctx;
  ASSERT_TRUE(
      sink.trigger({original_artifact}, {{"output_path", tbc}}, sink_ctx))
      << sink.get_trigger_status();

  orc::TBCSourceStage source;
  const Parameters source_parameters{
      {"input_path", tbc},
      {orc::kProjectVideoSystemParameter,
       std::string(orc::video_system_to_string(system))}};
  ASSERT_TRUE(source.set_parameters(source_parameters));
  orc::ObservationContext source_ctx;
  const auto outputs = source.execute({}, source_parameters, source_ctx);
  ASSERT_EQ(outputs.size(), 1u);
  const auto copy = orc_videosynth::as_vfr(outputs[0]);
  ASSERT_NE(copy, nullptr);
  ASSERT_EQ(copy->frame_count(), original->frame_count());
  const auto params = copy->get_video_parameters();
  ASSERT_TRUE(params.has_value());
  EXPECT_EQ(params->system, system);
  EXPECT_FALSE(params->is_subcarrier_locked);

  // The active window of every line, outside the flagged dropouts: a
  // dropout's samples sit at the rails, and the resampling rings across
  // its edges, so each run is widened by the interpolation's reach.
  const auto active = original->get_video_parameters();
  ASSERT_TRUE(active.has_value());
  double energy = 0.0;
  size_t samples = 0;
  int max_error = 0;
  size_t original_dropout_samples = 0;
  size_t copy_dropout_samples = 0;
  size_t original_runs = 0;
  size_t copy_runs = 0;
  size_t uncovered_dropout_samples = 0;
  for (orc::FrameID f = 0; f < original->frame_count(); ++f) {
    const auto frame_a = original->get_frame_copy(f);
    const auto frame_b = copy->get_frame_copy(f);
    ASSERT_EQ(frame_a.size(), frame_b.size()) << "frame " << f;
    std::vector<uint8_t> skip(frame_a.size(), 0);
    for (const auto& run : original->get_dropout_hints(f)) {
      original_dropout_samples += run.sample_count;
      const size_t from = run.sample_start > kTbcDropoutMargin
                              ? run.sample_start - kTbcDropoutMargin
                              : 0;
      const size_t to = std::min<size_t>(
          skip.size(), run.sample_start + run.sample_count + kTbcDropoutMargin);
      std::fill(skip.begin() + static_cast<std::ptrdiff_t>(from),
                skip.begin() + static_cast<std::ptrdiff_t>(to), 1);
    }
    std::vector<uint8_t> flagged(frame_a.size(), 0);
    for (const auto& run : copy->get_dropout_hints(f)) {
      copy_dropout_samples += run.sample_count;
      ++copy_runs;
      const size_t to =
          std::min<size_t>(flagged.size(), run.sample_start + run.sample_count);
      for (size_t n = run.sample_start; n < to; ++n) flagged[n] = 1;
    }
    for (const auto& run : original->get_dropout_hints(f)) {
      ++original_runs;
      for (size_t n = run.sample_start;
           n < run.sample_start + run.sample_count && n < flagged.size(); ++n) {
        if (flagged[n] == 0) ++uncovered_dropout_samples;
      }
    }
    for (size_t line = 0; line < static_cast<size_t>(active->frame_height);
         ++line) {
      const size_t offset = orc::frame_line_sample_offset(
          active->system, static_cast<size_t>(active->frame_width_nominal),
          line);
      for (int32_t x = active->active_video_start; x < active->active_video_end;
           ++x) {
        const size_t n = offset + static_cast<size_t>(x);
        if (n >= frame_a.size() || skip[n] != 0) continue;
        const int d =
            static_cast<int>(frame_a[n]) - static_cast<int>(frame_b[n]);
        energy += static_cast<double>(d) * d;
        max_error = std::max(max_error, std::abs(d));
        ++samples;
      }
    }
  }
  ASSERT_GT(samples, 0u);
  const double rms = std::sqrt(energy / static_cast<double>(samples));
  EXPECT_LT(rms, kTbcRoundTripRmsCodes);
  EXPECT_LE(max_error, kTbcRoundTripMaxCodes);
  // The runs come back on the lattice still covering every sample flagged,
  // each at most kTbcDropoutWidening samples wider at either end, the
  // difference between the two grids (a run that crosses a line's end comes
  // back as one run per line).
  EXPECT_EQ(uncovered_dropout_samples, 0u);
  EXPECT_GE(copy_runs, original_runs);
  EXPECT_LE(copy_dropout_samples,
            original_dropout_samples + (2 * kTbcDropoutWidening * copy_runs));
  EXPECT_EQ(colour_frame_indices(*copy), colour_frame_indices(*original));
  orc_videosynth::expect_metrics(
      {{"active_rms_codes", rms, orc_videosynth::Better::Lower,
        orc_videosynth::new_tolerance(rms, 0.02, 0.01)},
       {"active_max_codes", static_cast<double>(max_error),
        orc_videosynth::Better::Lower, 1.0}});
  for (const char* suffix : {".tbc", ".tbc.db", ".pcm"}) {
    fs::remove(base->string() + suffix);
  }
}

// ---------------------------------------------------------------------------
// Video sink
// ---------------------------------------------------------------------------

// The frames ffprobe counts in |path|; nullopt when it cannot.
std::optional<size_t> count_frames(const fs::path& path) {
  const std::string command =
      std::string("\"") + ORC_FFPROBE_EXECUTABLE +
      "\" -v error -count_frames -select_streams v:0 -show_entries "
      "stream=nb_read_frames -of csv=p=0 \"" +
      path.string() + "\"";
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) return std::nullopt;
  char buffer[64] = {};
  const bool read = fgets(buffer, sizeof(buffer), pipe) != nullptr;
  pclose(pipe);
  if (!read) return std::nullopt;
  try {
    return static_cast<size_t>(std::stoul(buffer));
  } catch (...) {
    return std::nullopt;
  }
}

// The patterns case (projects/decoders/*_patterns.yaml) is 12 frames.
constexpr size_t kPatternFrames = 12;

const char* two_d(orc::VideoSystem system) {
  if (system == orc::VideoSystem::SECAM) return "secam";
  return system == orc::VideoSystem::NTSC ? "ntsc2d" : "transform2d";
}

const char* three_d(orc::VideoSystem system) {
  return system == orc::VideoSystem::NTSC ? "ntsc3d" : "transform3d";
}

// Woven, the export has a frame per frame; deinterlaced (bwdif), one per
// field.
void run_video_frames_case(const std::string& system_name,
                           orc::VideoSystem system) {
  const auto artifact = orc_videosynth::open_case(
      {system_name + "_patterns", "videosynth_" + system_name + "_patterns",
       system});
  ASSERT_NE(artifact, nullptr);
  for (const bool deinterlace : {false, true}) {
    const auto out =
        work_path(system_name + (deinterlace ? "_bwdif" : "_weave") + ".mkv");
    ASSERT_TRUE(out.has_value());
    orc::VideoSinkStage sink;
    orc::ObservationContext ctx;
    ASSERT_TRUE(sink.trigger({artifact},
                             {{"output_path", out->string()},
                              {"output_mode", std::string("ffmpeg")},
                              {"ffmpeg_format", std::string("mkv-ffv1")},
                              {"decoder_type", std::string(two_d(system))},
                              {"apply_deinterlace", deinterlace},
                              {"deinterlace_mode", std::string("all")}},
                             ctx))
        << sink.get_trigger_status();
    EXPECT_EQ(count_frames(*out),
              deinterlace ? 2 * kPatternFrames : kPatternFrames)
        << (deinterlace ? "bwdif" : "weave");
    fs::remove(*out);
  }
}

// |input| decoded to raw YUV444P16 with |parameters|.
std::optional<orc_videosynth::DecodedFile> decode_raw(
    const orc::ArtifactPtr& input, Parameters parameters,
    const std::string& name, size_t frames) {
  const auto out = work_path(name + ".yuv");
  if (!out) return std::nullopt;
  parameters["output_path"] = out->string();
  parameters["output_mode"] = std::string("raw");
  parameters["raw_format"] = std::string("yuv");
  orc::VideoSinkStage sink;
  orc::ObservationContext ctx;
  if (!sink.trigger({input}, parameters, ctx)) {
    ADD_FAILURE() << name << ": " << sink.get_trigger_status();
    return std::nullopt;
  }
  std::string error;
  auto decoded = orc_videosynth::DecodedFile::read(*out, frames, &error);
  if (!decoded) ADD_FAILURE() << error;
  fs::remove(*out);
  return decoded;
}

// A held still (projects/stacking/*_held_still.yaml) through the stacker,
// whose held hints mark every frame held, then the video sink with the
// schedule "<2D>, held:<3D>": every frame is the 3D decoder's.
void run_held_schedule_case(const std::string& system_name,
                            orc::VideoSystem system) {
  const auto capture = orc_videosynth::open_case(
      {system_name + "_held_still", "videosynth_" + system_name + "_held_still",
       system});
  ASSERT_NE(capture, nullptr);
  orc::StackerStage stacker;
  const Parameters stack_parameters{{"still_frame_stacking", true},
                                    {"still_frame_search", int32_t{8}}};
  ASSERT_TRUE(stacker.set_parameters(stack_parameters));
  orc::ObservationContext stack_ctx;
  const auto outputs = stacker.execute({capture}, stack_parameters, stack_ctx);
  ASSERT_EQ(outputs.size(), 1u);
  const orc::ArtifactPtr input = outputs[0];
  const auto stacked = orc_videosynth::as_vfr(input);
  ASSERT_NE(stacked, nullptr);
  const size_t frames = stacked->frame_count();
  for (orc::FrameID f = 0; f < frames; ++f) {
    const auto desc = stacked->get_frame_descriptor(f);
    ASSERT_TRUE(desc.has_value());
    ASSERT_GT(desc->held_frames_before + desc->held_frames_after, 0u)
        << "frame " << f << " is not held";
  }

  const std::string schedule =
      std::string(two_d(system)) + ", held:" + three_d(system);
  const auto scheduled = decode_raw(input,
                                    {{"decoder_type", std::string("multiple")},
                                     {"decoder_schedule", schedule}},
                                    system_name + "_held_scheduled", frames);
  const auto all_3d =
      decode_raw(input, {{"decoder_type", std::string(three_d(system))}},
                 system_name + "_held_3d", frames);
  const auto all_2d =
      decode_raw(input, {{"decoder_type", std::string(two_d(system))}},
                 system_name + "_held_2d", frames);
  ASSERT_TRUE(scheduled && all_3d && all_2d);
  // Each frame is wholly one decoder's: the 3D decoder's where the hold
  // reaches its look-behind and look-ahead, the 2D decoder's elsewhere, and
  // so always at the ends of the hold.
  size_t held_frames = 0;
  for (orc::FrameID f = 0; f < frames; ++f) {
    const auto vs_3d = orc_videosynth::plane_rms(*scheduled, *all_3d, f, 1);
    const auto vs_2d = orc_videosynth::plane_rms(*scheduled, *all_2d, f, 1);
    ASSERT_TRUE(vs_3d && vs_2d);
    const bool is_3d = vs_3d->y + vs_3d->u + vs_3d->v == 0.0;
    const bool is_2d = vs_2d->y + vs_2d->u + vs_2d->v == 0.0;
    EXPECT_TRUE(is_3d != is_2d)
        << "frame " << f << " is " << (is_3d ? "both decoders'" : "neither's");
    const auto desc = stacked->get_frame_descriptor(f);
    if (desc->held_frames_before == 0 || desc->held_frames_after == 0) {
      EXPECT_TRUE(is_2d) << "frame " << f << " ends the hold";
    }
    if (is_3d) ++held_frames;
  }
  EXPECT_GT(held_frames, 0u) << "no frame was decoded as held";
  orc_videosynth::note(std::to_string(held_frames) + " of " +
                       std::to_string(frames) + " frames decoded as held");
}

// The MD5 of each video frame of |path|, decoded by FFmpeg; empty on
// failure.
std::vector<std::string> frame_md5s(const fs::path& path) {
  const std::string command = std::string("\"") + ORC_FFMPEG_EXECUTABLE +
                              "\" -v error -i \"" + path.string() +
                              "\" -map 0:v:0 -f framemd5 -";
  std::vector<std::string> md5s;
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) return md5s;
  char line[512];
  while (fgets(line, sizeof(line), pipe) != nullptr) {
    if (line[0] == '#') continue;
    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
      text.pop_back();
    }
    const auto comma = text.rfind(',');
    if (comma != std::string::npos) md5s.push_back(text.substr(comma + 1));
  }
  pclose(pipe);
  return md5s;
}

// projects/sinks/*_weave.yaml: a still, film and interlaced motion, six frames
// each, and on NTSC two cycles of film with 2:3 pulldown.
constexpr size_t kWeaveSectionFrames = 6;

// Whether each frame of the system's weave case must be woven: the still and
// the film yes, the interlaced motion no, and of each five pulldown frames
// (A A, B B, B C, C D, D D) the two that mix film frames no.
std::vector<bool> weave_expected(orc::VideoSystem system) {
  std::vector<bool> woven;
  for (const bool section_woven : {true, true, false}) {
    woven.insert(woven.end(), kWeaveSectionFrames, section_woven);
  }
  if (system == orc::VideoSystem::NTSC) {
    for (int cycle = 0; cycle < 2; ++cycle) {
      woven.insert(woven.end(), {true, true, false, false, true});
    }
  }
  return woven;
}

const char* weave_section(size_t frame) {
  const char* const kSections[] = {"still", "film", "interlaced", "pulldown"};
  return kSections[std::min<size_t>(frame / kWeaveSectionFrames, 3)];
}

// deinterlace_mode motion_only: the output keeps the field rate, two frames
// per frame. A woven frame is both of them and is the frame exactly as an
// export without deinterlacing writes it; a deinterlaced one is two
// different bwdif frames.
void run_weave_case(const std::string& system_name, orc::VideoSystem system) {
  const auto artifact = orc_videosynth::open_case(
      {system_name + "_weave", "videosynth_" + system_name + "_weave", system});
  ASSERT_NE(artifact, nullptr);

  // The reference goes through a filter graph too ("null"), so its
  // conversion to the encoder's format is the one the woven frames get.
  const auto export_case =
      [&](const std::string& name,
          const Parameters& extra) -> std::vector<std::string> {
    const auto out = work_path(system_name + "_" + name + ".mkv");
    if (!out) return {};
    Parameters parameters{{"output_path", out->string()},
                          {"output_mode", std::string("ffmpeg")},
                          {"ffmpeg_format", std::string("mkv-ffv1")},
                          {"decoder_type", std::string(two_d(system))}};
    for (const auto& [key, value] : extra) parameters[key] = value;
    orc::VideoSinkStage sink;
    orc::ObservationContext ctx;
    if (!sink.trigger({artifact}, parameters, ctx)) {
      ADD_FAILURE() << name << ": " << sink.get_trigger_status();
      return {};
    }
    auto md5s = frame_md5s(*out);
    fs::remove(*out);
    return md5s;
  };
  const auto woven = export_case(
      "woven",
      {{"apply_deinterlace", false}, {"video_filter", std::string("null")}});
  const auto motion_only = export_case(
      "motion_only", {{"apply_deinterlace", true},
                      {"deinterlace_mode", std::string("motion_only")}});
  const auto expected = weave_expected(system);
  ASSERT_EQ(woven.size(), expected.size());
  ASSERT_EQ(motion_only.size(), 2 * expected.size());

  size_t right = 0;
  for (size_t f = 0; f < expected.size(); ++f) {
    const bool expect_woven = expected[f];
    const bool pair_same = motion_only[2 * f] == motion_only[(2 * f) + 1];
    const bool as_woven = motion_only[2 * f] == woven[f];
    const bool is_woven = pair_same && as_woven;
    EXPECT_EQ(is_woven, expect_woven)
        << weave_section(f) << " frame " << f + 1
        << (is_woven ? " was woven" : " was deinterlaced");
    if (pair_same != as_woven) {
      ADD_FAILURE() << weave_section(f) << " frame " << f + 1
                    << (pair_same ? " was repeated but is not the woven frame"
                                  : " is the woven frame only once");
    }
    if (is_woven == expect_woven) ++right;
  }
  orc_videosynth::expect_metrics(
      {{"frames_decided_right", static_cast<double>(right),
        orc_videosynth::Better::Higher, 0.0}});
}

// The output of |command|, whole; empty when it cannot run.
std::string command_output(const std::string& command) {
  std::string text;
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) return text;
  char buffer[512];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) text += buffer;
  pclose(pipe);
  return text;
}

// A CAV case's disc metadata and chapters are gathered while the frames are
// exported and remuxed into the header afterwards: the file carries the
// document, its chapters and the frames a plain export writes, and nothing is
// left of the file written before the remux.
void run_disc_metadata_case(const CaseFile& disc, size_t frames,
                            bool chapters) {
  const auto artifact = orc_videosynth::open_case(disc);
  ASSERT_NE(artifact, nullptr);
  const auto plain = work_path(disc.case_name + "_plain.mkv");
  const auto tagged = work_path(disc.case_name + "_disc_metadata.mkv");
  ASSERT_TRUE(plain && tagged);
  const Parameters parameters{
      {"output_mode", std::string("ffmpeg")},
      {"ffmpeg_format", std::string("mkv-ffv1")},
      {"decoder_type", std::string(two_d(disc.system))}};
  for (const bool embed : {false, true}) {
    Parameters run = parameters;
    run["output_path"] = (embed ? *tagged : *plain).string();
    run["embed_disc_metadata"] = embed;
    run["embed_chapter_metadata"] = embed;
    orc::VideoSinkStage sink;
    orc::ObservationContext ctx;
    ASSERT_TRUE(sink.trigger({artifact}, run, ctx))
        << sink.get_trigger_status();
  }

  EXPECT_FALSE(fs::exists(tagged->string() + ".partial"));
  const auto expected = frame_md5s(*plain);
  EXPECT_EQ(expected.size(), frames);
  EXPECT_EQ(frame_md5s(*tagged), expected);

  const std::string ffprobe = std::string("\"") + ORC_FFPROBE_EXECUTABLE +
                              "\" -v error -of default=nw=1 ";
  const std::string streams = command_output(
      ffprobe + "-show_entries stream=codec_type:stream_tags=filename \"" +
      tagged->string() + "\"");
  EXPECT_NE(streams.find("codec_type=attachment"), std::string::npos)
      << streams;
  EXPECT_NE(streams.find("TAG:filename=orc-disc-metadata.yaml"),
            std::string::npos)
      << streams;
  const std::string tags = command_output(
      ffprobe + "-show_entries format_tags=ORC_FIRST_PICTURE \"" +
      tagged->string() + "\"");
  EXPECT_NE(tags.find("ORC_FIRST_PICTURE=1"), std::string::npos) << tags;
  const std::string found =
      command_output(ffprobe + "-show_chapters \"" + tagged->string() + "\"");
  EXPECT_EQ(found.find("TAG:title=Chapter 1") != std::string::npos, chapters)
      << found;
  fs::remove(*plain);
  fs::remove(*tagged);
}

}  // namespace

// projects/relabel/pal_relabel_disc.yaml: 20 frames numbered from 1, no
// chapter numbers.
TEST(SinkTruth, PAL_DiscMetadata) {
  run_disc_metadata_case({"pal_relabel_disc", "videosynth_pal_relabel_disc",
                          orc::VideoSystem::PAL},
                         20, false);
}

// projects/pulldown/ntsc_film_disc.yaml: 40 frames whose pictures are
// numbered from 1, the frames between them carrying chapter 1.
TEST(SinkTruth, NTSC_DiscMetadata) {
  run_disc_metadata_case(
      {"ntsc_film_disc_clean", "videosynth_ntsc_film_disc_clean",
       orc::VideoSystem::NTSC},
      40, true);
}

TEST(SinkTruth, PAL_Audio) { run_audio_case("pal", orc::VideoSystem::PAL); }
TEST(SinkTruth, NTSC_Audio) { run_audio_case("ntsc", orc::VideoSystem::NTSC); }
TEST(SinkTruth, PALM_Audio) {
  run_audio_case("pal_m", orc::VideoSystem::PAL_M);
}

// SECAM's audio is PAL's: 48 kHz at 25 frames a second, 1920 stereo samples
// a frame.
TEST(SinkTruth, SECAM_Audio) {
  run_audio_case("secam", orc::VideoSystem::SECAM);
  const auto vfr = orc_videosynth::as_vfr(
      orc_videosynth::open_case(sinks_case("secam", orc::VideoSystem::SECAM)));
  ASSERT_NE(vfr, nullptr);
  ASSERT_EQ(vfr->audio_channel_pair_count(), 2u);
  for (orc::FrameID f = 0; f < vfr->frame_count(); ++f) {
    for (size_t pair = 0; pair < 2; ++pair) {
      EXPECT_EQ(vfr->get_audio_samples(pair, f).size(), 2u * 1920u)
          << "frame " << f << " pair " << pair;
    }
  }
}

TEST(SinkTruth, PAL_Rf64Audio) {
  run_rf64_audio_case("pal", orc::VideoSystem::PAL);
}
TEST(SinkTruth, NTSC_Rf64Audio) {
  run_rf64_audio_case("ntsc", orc::VideoSystem::NTSC);
}
TEST(SinkTruth, PALM_Rf64Audio) {
  run_rf64_audio_case("pal_m", orc::VideoSystem::PAL_M);
}
TEST(SinkTruth, SECAM_Rf64Audio) {
  run_rf64_audio_case("secam", orc::VideoSystem::SECAM);
}

TEST(SinkTruth, PAL_Efm) { run_efm_case("pal", orc::VideoSystem::PAL); }
TEST(SinkTruth, NTSC_Efm) { run_efm_case("ntsc", orc::VideoSystem::NTSC); }
TEST(SinkTruth, PALM_Efm) { run_efm_case("pal_m", orc::VideoSystem::PAL_M); }

TEST(SinkTruth, PAL_CvbsRoundTrip) {
  run_cvbs_round_trip("pal", orc::VideoSystem::PAL);
}
TEST(SinkTruth, NTSC_CvbsRoundTrip) {
  run_cvbs_round_trip("ntsc", orc::VideoSystem::NTSC);
}
TEST(SinkTruth, PALM_CvbsRoundTrip) {
  run_cvbs_round_trip("pal_m", orc::VideoSystem::PAL_M);
}
TEST(SinkTruth, SECAM_CvbsRoundTrip) {
  run_cvbs_round_trip("secam", orc::VideoSystem::SECAM);
}

TEST(SinkTruth, SECAM_TbcRoundTrip) {
  run_tbc_round_trip("secam", orc::VideoSystem::SECAM);
}

TEST(SinkTruth, PAL_VideoFrames) {
  run_video_frames_case("pal", orc::VideoSystem::PAL);
}
TEST(SinkTruth, NTSC_VideoFrames) {
  run_video_frames_case("ntsc", orc::VideoSystem::NTSC);
}
TEST(SinkTruth, PALM_VideoFrames) {
  run_video_frames_case("pal_m", orc::VideoSystem::PAL_M);
}
TEST(SinkTruth, SECAM_VideoFrames) {
  run_video_frames_case("secam", orc::VideoSystem::SECAM);
}

TEST(SinkTruth, PAL_HeldSchedule) {
  run_held_schedule_case("pal", orc::VideoSystem::PAL);
}
TEST(SinkTruth, NTSC_HeldSchedule) {
  run_held_schedule_case("ntsc", orc::VideoSystem::NTSC);
}
TEST(SinkTruth, PALM_HeldSchedule) {
  run_held_schedule_case("pal_m", orc::VideoSystem::PAL_M);
}

TEST(SinkTruth, PAL_Weave) { run_weave_case("pal", orc::VideoSystem::PAL); }
TEST(SinkTruth, NTSC_Weave) { run_weave_case("ntsc", orc::VideoSystem::NTSC); }

}  // namespace orc_videosynth_test
