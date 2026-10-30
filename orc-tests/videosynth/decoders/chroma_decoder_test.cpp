/*
 * File:        chroma_decoder_test.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     The chroma decoders through the video sink: each decoder's
 *              composite decode of bars, ramps and multiburst against its
 *              decode of the Y/C rendering, the NTSC 3D field phase of issue
 *              #324, and the PAL herringbone each decoder passes
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <decoded_comparison.h>
#include <gtest/gtest.h>
#include <orc/stage/common_types.h>
#include <orc/stage/observation/observation_context.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "video_sink_stage.h"
#include "videosynth_cases.h"
#include "videosynth_harness.h"

namespace orc_videosynth_test {

namespace {

namespace fs = std::filesystem;

using orc_videosynth::Better;
using orc_videosynth::CaseFile;
using orc_videosynth::DecodedFile;
using orc_videosynth::Metric;

using Parameters = std::map<std::string, orc::ParameterValue>;

// The sections of projects/decoders/*.yaml, four frames each.
constexpr const char* kSections[] = {"bars", "ramps", "multiburst"};
constexpr size_t kSectionFrames = 4;
constexpr size_t kFrames = std::size(kSections) * kSectionFrames;

// |file| decoded by the video sink with |decoder| and |extra| parameters into
// raw YUV444P16; nullopt, with the test failed, when it cannot be.
std::optional<DecodedFile> decode(const CaseFile& file,
                                  const std::string& decoder,
                                  const Parameters& extra,
                                  const std::string& label,
                                  size_t frames = kFrames) {
  const auto artifact = orc_videosynth::open_case(file);
  if (!artifact) return std::nullopt;
  const auto root = orc_videosynth::case_dir("decoded");
  if (!root) return std::nullopt;
  fs::create_directories(*root);
  const fs::path out = *root / (label + ".yuv");
  Parameters parameters = extra;
  parameters["output_path"] = out.string();
  parameters["output_mode"] = std::string("raw");
  parameters["raw_format"] = std::string("yuv");
  parameters["decoder_type"] = decoder;
  orc::VideoSinkStage sink;
  orc::ObservationContext ctx;
  if (!sink.trigger({artifact}, parameters, ctx)) {
    ADD_FAILURE() << "the video sink failed on " << file.case_name << " with "
                  << decoder << ": " << sink.get_trigger_status();
    return std::nullopt;
  }
  std::string error;
  auto decoded = DecodedFile::read(out, frames, &error);
  if (!decoded) ADD_FAILURE() << error;
  fs::remove(out);
  return decoded;
}

// Per section, the Y, U and V difference between |a| and |b|, in percent of
// full scale, named <section>_<plane>_rms_pct after |prefix|.
std::vector<Metric> section_metrics(const DecodedFile& a, const DecodedFile& b,
                                    const std::string& prefix) {
  std::vector<Metric> metrics;
  for (size_t s = 0; s < std::size(kSections); ++s) {
    const auto rms =
        orc_videosynth::plane_rms(a, b, s * kSectionFrames, kSectionFrames);
    if (!rms) {
      ADD_FAILURE() << "the decodes differ in geometry";
      return {};
    }
    const std::string name = prefix + kSections[s];
    for (const auto& [plane, value] :
         {std::pair<const char*, double>{"y", rms->y},
          {"u", rms->u},
          {"v", rms->v}}) {
      metrics.push_back({name + "_" + plane + "_rms_pct", value, Better::Lower,
                         orc_videosynth::new_tolerance(value, 0.05, 0.01)});
    }
  }
  return metrics;
}

CaseFile patterns(const std::string& system_name, orc::VideoSystem system,
                  bool yc) {
  return {yc ? system_name + "_patterns_yc" : system_name + "_patterns",
          "videosynth_" + system_name + "_patterns", system, yc};
}

// |decoder|'s composite decode against its decode of the Y/C rendering: the
// error of its luma and chroma separation, the demodulation both share
// cancelling.
std::vector<Metric> separation_metrics(const std::string& system_name,
                                       orc::VideoSystem system,
                                       const std::string& decoder,
                                       const Parameters& extra = {}) {
  const auto composite =
      decode(patterns(system_name, system, false), decoder, extra,
             system_name + "_" + decoder + "_composite");
  const auto yc = decode(patterns(system_name, system, true), decoder, extra,
                         system_name + "_" + decoder + "_yc");
  if (!composite || !yc) return {};
  return section_metrics(*composite, *yc, "");
}

void run_decoder_case(const std::string& system_name, orc::VideoSystem system,
                      const std::string& decoder) {
  const auto metrics = separation_metrics(system_name, system, decoder);
  ASSERT_FALSE(metrics.empty());
  orc_videosynth::expect_metrics(metrics);
}

}  // namespace

#define ORC_DECODER_TEST(SYSTEM, NAME, ENUM, DECODER)         \
  TEST(DecoderTruth, SYSTEM##_##DECODER) {                    \
    run_decoder_case(NAME, orc::VideoSystem::ENUM, #DECODER); \
  }

ORC_DECODER_TEST(PAL, "pal", PAL, pal2d)
ORC_DECODER_TEST(PAL, "pal", PAL, transform2d)
ORC_DECODER_TEST(PAL, "pal", PAL, transform3d)
ORC_DECODER_TEST(NTSC, "ntsc", NTSC, ntsc1d)
ORC_DECODER_TEST(NTSC, "ntsc", NTSC, ntsc2d)
ORC_DECODER_TEST(NTSC, "ntsc", NTSC, ntsc3d)
ORC_DECODER_TEST(NTSC, "ntsc", NTSC, ntsc3dnoadapt)
ORC_DECODER_TEST(PALM, "pal_m", PAL_M, pal2d)
ORC_DECODER_TEST(PALM, "pal_m", PAL_M, transform2d)
ORC_DECODER_TEST(PALM, "pal_m", PAL_M, transform3d)

#undef ORC_DECODER_TEST

// Issue #324: the NTSC 3D comb needs each field's own colour phase. A wrong
// one shows on chroma that moves (projects/decoders/ntsc_motion.yaml), and
// with phase compensation off, which would otherwise mask it, as hue
// inverted on every other line. Measured as the composite decode against the
// decode of the Y/C rendering, both with ntsc3d.
constexpr size_t kMotionFrames = 8;

// Measured on MOVING_BARS_75, % of full scale: luma 0.70 and V 0.43 with each
// field's phase; 0.88 and 0.80 with the frame-level colour_frame_index both
// fields had before the fix. The bounds sit midway, so a build that reverts
// the fix fails here.
constexpr double kFieldPhaseLumaBoundPct = 0.79;
constexpr double kFieldPhaseVBoundPct = 0.62;

TEST(DecoderTruth, NTSC_ntsc3d_FieldPhase) {
  const Parameters no_phase_comp{{"ntsc_phase_comp", false}};
  const auto composite = decode(
      {"ntsc_motion", "videosynth_ntsc_motion", orc::VideoSystem::NTSC},
      "ntsc3d", no_phase_comp, "ntsc_motion_ntsc3d_composite", kMotionFrames);
  const auto yc =
      decode({"ntsc_motion_yc", "videosynth_ntsc_motion",
              orc::VideoSystem::NTSC, true},
             "ntsc3d", no_phase_comp, "ntsc_motion_ntsc3d_yc", kMotionFrames);
  ASSERT_TRUE(composite.has_value());
  ASSERT_TRUE(yc.has_value());
  const auto rms = orc_videosynth::plane_rms(*composite, *yc, 0, kMotionFrames);
  ASSERT_TRUE(rms.has_value());
  EXPECT_LT(rms->y, kFieldPhaseLumaBoundPct)
      << "ntsc3d leaves subcarrier in luma on moving colour: a wrong field "
         "phase (issue #324)";
  EXPECT_LT(rms->v, kFieldPhaseVBoundPct)
      << "ntsc3d loses chroma on moving colour: a wrong field phase (issue "
         "#324)";
  orc_videosynth::expect_metrics({
      {"motion_y_rms_pct", rms->y, Better::Lower,
       orc_videosynth::new_tolerance(rms->y, 0.05, 0.01)},
      {"motion_u_rms_pct", rms->u, Better::Lower,
       orc_videosynth::new_tolerance(rms->u, 0.05, 0.01)},
      {"motion_v_rms_pct", rms->v, Better::Lower,
       orc_videosynth::new_tolerance(rms->v, 0.05, 0.01)},
  });
}

// The herringbone of a PAL pressing's FM beat, as each decoder passes it:
// its decode of the herringbone pressing against its decode of the same
// patterns without the beat. Recorded only, until a stage cancels it.
TEST(DecoderTruth, PAL_Herringbone) {
  std::vector<Metric> metrics;
  for (const char* decoder : {"pal2d", "transform2d", "transform3d"}) {
    const auto beat =
        decode({"pal_herringbone", "videosynth_pal_herringbone",
                orc::VideoSystem::PAL},
               decoder, {}, std::string("pal_herringbone_") + decoder);
    const auto clean = decode(patterns("pal", orc::VideoSystem::PAL, false),
                              decoder, {}, std::string("pal_clean_") + decoder);
    ASSERT_TRUE(beat.has_value());
    ASSERT_TRUE(clean.has_value());
    const auto m = section_metrics(*beat, *clean, std::string(decoder) + "_");
    metrics.insert(metrics.end(), m.begin(), m.end());
  }
  orc_videosynth::note("Baseline only: no stage cancels the herringbone yet.");
  orc_videosynth::expect_metrics(metrics);
}

}  // namespace orc_videosynth_test
