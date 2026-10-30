/*
 * File:        stacker_truth_test.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     The stacker against the truth: Mean and Weighted on two
 *              pressings of four captures each and on four captures with
 *              the lone capture of another pressing, Mean, Median and
 *              Weighted on the Domesday noise model, sample_align on a capture
 *              read on another grid, chroma_gain_match on captures with
 *              another chroma gain, held stills, and the per-frame choice of
 *              the source whose EFM passes through, and captures of film
 *              with 2:3 pulldown mapped, aligned and stacked
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/common_types.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <truth_comparison.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "alignment_support.h"
#include "content_offsets.h"
#include "source_align_stage.h"
#include "stacker_stage.h"
#include "videosynth_cases.h"
#include "videosynth_harness.h"

namespace orc_videosynth_test {

namespace {

using orc_videosynth::Better;
using orc_videosynth::CaseFile;
using orc_videosynth::Metric;
using orc_videosynth::VfrPtr;

using Parameters = std::map<std::string, orc::ParameterValue>;

CaseFile case_file(const std::string& name, orc::VideoSystem system) {
  return {name, "videosynth_" + name, system};
}

VfrPtr open_vfr(const CaseFile& file) {
  return orc_videosynth::as_vfr(orc_videosynth::open_case(file));
}

// The error of |output| against |truth| per composite band, over every frame
// both hold; empty, with the test failed, when they cannot be compared.
std::vector<double> band_rms(const orc::VideoFrameRepresentation& output,
                             const orc::VideoFrameRepresentation& truth) {
  const auto params = truth.get_video_parameters();
  if (!params || output.frame_count() != truth.frame_count()) {
    ADD_FAILURE() << "output and truth cannot be compared ("
                  << output.frame_count() << " and " << truth.frame_count()
                  << " frames)";
    return {};
  }
  orc_videosynth::BandErrorMeter meter(params->system,
                                       orc_videosynth::ire_per_code(*params));
  for (orc::FrameID f = 0; f < truth.frame_count(); ++f) {
    // Owned copies: a representation's frame pointer lasts only until its
    // next call.
    const auto out = output.get_frame_copy(f);
    const auto ref = truth.get_frame_copy(f);
    if (out.size() != ref.size()) {
      ADD_FAILURE() << "frame " << f << " differs in size";
      return {};
    }
    meter.add_frame(out.data(), ref.data(), *params);
  }
  return meter.rms_ire();
}

const std::vector<orc_videosynth::Band>& bands(orc::VideoSystem system) {
  static std::map<orc::VideoSystem, std::vector<orc_videosynth::Band>> cache;
  auto it = cache.find(system);
  if (it == cache.end()) {
    it = cache.emplace(system, orc_videosynth::composite_bands(system)).first;
  }
  return it->second;
}

// <prefix><band>_rms_ire for each band of |rms|, and, against the error of
// the single capture |capture_rms|, <prefix><band>_gain_db.
void add_band_metrics(std::vector<Metric>& metrics, const std::string& prefix,
                      orc::VideoSystem system, const std::vector<double>& rms,
                      const std::vector<double>& capture_rms) {
  const auto& b = bands(system);
  for (size_t i = 0; i < b.size() && i < rms.size(); ++i) {
    metrics.push_back({prefix + b[i].name + "_rms_ire", rms[i], Better::Lower,
                       orc_videosynth::new_tolerance(rms[i], 0.02, 0.01)});
    if (i < capture_rms.size()) {
      if (const auto gain = orc_videosynth::gain_db(capture_rms[i], rms[i])) {
        metrics.push_back(
            {prefix + b[i].name + "_gain_db", *gain, Better::Higher, 0.2});
      }
    }
  }
}

// A stack of |inputs| with |parameters|. The stacker is returned with its
// output, which reads through it.
struct Stack {
  std::unique_ptr<orc::StackerStage> stacker;
  VfrPtr output;
};

Stack stack(const std::vector<VfrPtr>& inputs, const Parameters& parameters) {
  Stack s{std::make_unique<orc::StackerStage>(), nullptr};
  if (!s.stacker->set_parameters(parameters)) {
    ADD_FAILURE() << "the stacker rejected its parameters";
    return s;
  }
  s.output = s.stacker->process(inputs);
  if (!s.output) ADD_FAILURE() << "the stacker gave no output";
  return s;
}

// ---------------------------------------------------------------------------
// Two pressings of four captures (videosynth's stacking-captures variant)
// ---------------------------------------------------------------------------

// Captures 1-4 of each pressing: whole, starting at picture 5, replaying
// pictures 9-12, whole.
constexpr const char* kCaptureSuffixes[] = {"_cap1", "_cap2", "_cap3", "_cap4"};
constexpr size_t kReplayCapture = 2;
constexpr size_t kLateCapture = 1;
constexpr size_t kLateStartPad = 4;
constexpr size_t kDiscPictures = 16;

// Weighted against Mean on |b_captures| of pressing B's captures with all four
// of pressing A's. With four of each, Weighted must place each pressing's
// captures in a group of their own and be no worse than Mean in any band: the
// pressings are equally noisy, so equal weights are already optimal and
// Weighted can tie Mean but must not lose to it. With one capture of B, that
// capture is the lone disc of its pressing, whose pressing noise cannot be
// told from its own (#347): it is grouped with pressing A and under-weighted,
// and Weighted loses to Mean. Those shortfalls are noted rather than failed,
// so progress shows as an improvement in the metrics.
constexpr double kWeightedTieMargin = 1.01;

void run_pressings_case(const std::string& system_name, orc::VideoSystem system,
                        size_t b_captures) {
  const auto truth =
      open_vfr(case_file(system_name + "_pressing_A_cap1_clean", system));
  ASSERT_NE(truth, nullptr);

  // Each capture, its replay removed, then all aligned by picture number.
  std::vector<orc::ArtifactPtr> captures;
  std::vector<size_t> capture_index;
  std::vector<std::unique_ptr<orc::FrameMapStage>> frame_maps;
  std::vector<Metric> metrics;
  for (const auto& [pressing, count] :
       {std::pair<const char*, size_t>{"A", std::size(kCaptureSuffixes)},
        {"B", b_captures}}) {
    for (size_t c = 0; c < count; ++c) {
      const std::string name =
          system_name + "_pressing_" + pressing + kCaptureSuffixes[c];
      auto artifact = orc_videosynth::open_case(case_file(name, system));
      ASSERT_NE(artifact, nullptr) << name;
      if (c == kReplayCapture) {
        const auto decision =
            orc_videosynth::map_disc(*orc_videosynth::as_vfr(artifact));
        ASSERT_TRUE(decision.success) << name << ": " << decision.rationale;
        EXPECT_EQ(decision.stats.removed_duplicates, 4u)
            << name << ": " << decision.rationale;
        frame_maps.push_back(std::make_unique<orc::FrameMapStage>());
        artifact = orc_videosynth::apply_frame_map(artifact, *frame_maps.back(),
                                                   decision.mapping_spec);
        ASSERT_NE(artifact, nullptr) << name;
      }
      captures.push_back(artifact);
      capture_index.push_back(c);
    }
  }
  const size_t a_captures = std::size(kCaptureSuffixes);
  std::vector<std::vector<orc::PictureNumberReading>> readings;
  for (const auto& capture : captures) {
    readings.push_back(orc_videosynth::readings_of(
        orc_videosynth::read_pictures(*orc_videosynth::as_vfr(capture))));
  }
  const auto alignment = orc::compute_pad_alignment(readings);
  ASSERT_EQ(alignment.first_picture, 1);
  for (size_t i = 0; i < alignment.pads.size(); ++i) {
    const size_t expected =
        capture_index[i] == kLateCapture ? kLateStartPad : 0;
    EXPECT_EQ(alignment.pads[i], expected) << "capture " << i + 1;
  }

  orc::SourceAlignStage align;
  const Parameters align_parameters{
      {"alignmentMode", std::string("pad_for_alignment")},
      {"alignmentMap", orc::format_alignment_map(alignment.pads)}};
  ASSERT_TRUE(align.set_parameters(align_parameters));
  orc::ObservationContext ctx;
  const auto aligned = align.execute(captures, align_parameters, ctx);
  ASSERT_EQ(aligned.size(), captures.size());
  std::vector<VfrPtr> inputs;
  for (const auto& a : aligned) {
    inputs.push_back(orc_videosynth::as_vfr(a));
    ASSERT_NE(inputs.back(), nullptr);
    ASSERT_EQ(inputs.back()->frame_count(), kDiscPictures);
  }

  // Pressing A's first capture alone, for the gain of each stack.
  const auto capture_rms = band_rms(*inputs[0], *truth);
  ASSERT_FALSE(capture_rms.empty());

  std::map<std::string, std::vector<double>> stacked;
  std::vector<size_t> groups;
  for (const char* mode : {"Mean", "Weighted"}) {
    const auto s = stack(inputs, {{"mode", std::string(mode)}});
    ASSERT_NE(s.output, nullptr) << mode;
    ASSERT_EQ(s.output->frame_count(), kDiscPictures) << mode;
    stacked[mode] = band_rms(*s.output, *truth);
    ASSERT_FALSE(stacked[mode].empty()) << mode;
    std::string prefix = mode;
    for (auto& ch : prefix) ch = static_cast<char>(std::tolower(ch));
    add_band_metrics(metrics, prefix + "_", system, stacked[mode], capture_rms);
    if (std::string(mode) == "Weighted") {
      const auto stacked_vfr =
          std::dynamic_pointer_cast<const orc::StackedVideoFrameRepresentation>(
              s.output);
      ASSERT_NE(stacked_vfr, nullptr);
      for (const auto& noise : stacked_vfr->noise_model(kDiscPictures - 1)) {
        groups.push_back(noise.group);
      }
    }
  }
  ASSERT_EQ(groups.size(), inputs.size());

  // The captures share a group exactly when they share a pressing.
  bool grouped_by_pressing = true;
  for (size_t i = 0; i < groups.size(); ++i) {
    for (size_t j = i + 1; j < groups.size(); ++j) {
      grouped_by_pressing &=
          (groups[i] == groups[j]) == ((i < a_captures) == (j < a_captures));
    }
  }
  std::string grouping;
  for (const auto g : groups) grouping += std::to_string(g);
  const auto& b = bands(system);
  std::string lost;
  for (size_t i = 0; i < b.size(); ++i) {
    if (stacked["Weighted"][i] > stacked["Mean"][i] * kWeightedTieMargin) {
      lost += (lost.empty() ? "" : ", ") + b[i].name;
    }
  }
  if (b_captures == a_captures) {
    EXPECT_TRUE(grouped_by_pressing) << "groups " << grouping;
    EXPECT_TRUE(lost.empty()) << "Weighted loses to Mean in: " << lost;
  } else {
    if (!grouped_by_pressing) {
      orc_videosynth::note(
          "The lone capture of pressing B is not grouped on "
          "its own (groups " +
          grouping + ", #347).");
    }
    if (!lost.empty()) {
      orc_videosynth::note("Weighted loses to Mean in: " + lost + " (#347).");
    }
  }
  orc_videosynth::expect_metrics(metrics);
}

// ---------------------------------------------------------------------------
// The Domesday model (projects/domesday/)
// ---------------------------------------------------------------------------

// Four captures of pressing A and two of pressing B, with the capture and
// pressing noise measured on Community South. This is the known-truth stacking
// benchmark: it must rank the modes as the real-capture study ranked them on
// Community South, Weighted level with Mean or ahead of it and Mean ahead of
// Median, in every band, and Weighted must group the captures by pressing.
constexpr const char* kDomesdayCaptures[] = {"A_cap1", "A_cap2", "A_cap3",
                                             "A_cap4", "B_cap1", "B_cap2"};
constexpr size_t kDomesdayACaptures = 4;

void run_domesday_case(const std::string& system_name,
                       orc::VideoSystem system) {
  const auto truth =
      open_vfr(case_file(system_name + "_domesday_A_clean", system));
  ASSERT_NE(truth, nullptr);
  std::vector<VfrPtr> inputs;
  for (const char* capture : kDomesdayCaptures) {
    const std::string name = system_name + "_domesday_" + capture;
    inputs.push_back(open_vfr(case_file(name, system)));
    ASSERT_NE(inputs.back(), nullptr) << name;
    ASSERT_EQ(inputs.back()->frame_count(), kDiscPictures) << name;
  }
  const auto capture_rms = band_rms(*inputs[0], *truth);
  ASSERT_FALSE(capture_rms.empty());

  std::vector<Metric> metrics;
  std::map<std::string, std::vector<double>> stacked;
  std::vector<size_t> groups;
  for (const char* mode : {"Mean", "Median", "Weighted"}) {
    const auto s = stack(inputs, {{"mode", std::string(mode)}});
    ASSERT_NE(s.output, nullptr) << mode;
    stacked[mode] = band_rms(*s.output, *truth);
    ASSERT_FALSE(stacked[mode].empty()) << mode;
    std::string prefix = mode;
    for (auto& ch : prefix) ch = static_cast<char>(std::tolower(ch));
    add_band_metrics(metrics, prefix + "_", system, stacked[mode], capture_rms);
    if (std::string(mode) == "Weighted") {
      const auto stacked_vfr =
          std::dynamic_pointer_cast<const orc::StackedVideoFrameRepresentation>(
              s.output);
      ASSERT_NE(stacked_vfr, nullptr);
      for (const auto& noise : stacked_vfr->noise_model(kDiscPictures - 1)) {
        groups.push_back(noise.group);
      }
    }
  }
  ASSERT_EQ(groups.size(), inputs.size());

  std::string grouping;
  bool grouped_by_pressing = true;
  for (size_t i = 0; i < groups.size(); ++i) {
    grouping += std::to_string(groups[i]);
    for (size_t j = i + 1; j < groups.size(); ++j) {
      grouped_by_pressing &=
          (groups[i] == groups[j]) ==
          ((i < kDomesdayACaptures) == (j < kDomesdayACaptures));
    }
  }
  EXPECT_TRUE(grouped_by_pressing) << "groups " << grouping;
  const auto& b = bands(system);
  for (size_t i = 0; i < b.size(); ++i) {
    EXPECT_LE(stacked["Weighted"][i], stacked["Mean"][i] * kWeightedTieMargin)
        << "Weighted loses to Mean in " << b[i].name;
    EXPECT_LT(stacked["Mean"][i], stacked["Median"][i])
        << "Median beats Mean in " << b[i].name;
  }
  orc_videosynth::expect_metrics(metrics);
}

// ---------------------------------------------------------------------------
// Two captures of one disc on different grids (projects/stacking/)
// ---------------------------------------------------------------------------

// Capture A, capture B (two samples late, its own tone) and A's truth.
struct CapturePair {
  VfrPtr a;
  VfrPtr b;
  VfrPtr truth;
};

std::optional<CapturePair> open_pair(const std::string& system_name,
                                     orc::VideoSystem system) {
  CapturePair p{open_vfr(case_file(system_name + "_capture_A", system)),
                open_vfr(case_file(system_name + "_capture_B", system)),
                open_vfr(case_file(system_name + "_capture_A_clean", system))};
  if (!p.a || !p.b || !p.truth) return std::nullopt;
  return p;
}

double total_rms(const std::vector<double>& band_rms) {
  double energy = 0.0;
  for (const double r : band_rms) energy += r * r;
  return std::sqrt(energy);
}

// sample_align finds capture B's grid, two samples late, and the stack is
// nearer the truth for it.
void run_sample_align_case(const std::string& system_name,
                           orc::VideoSystem system) {
  const auto p = open_pair(system_name, system);
  ASSERT_TRUE(p.has_value());
  const auto capture_rms = band_rms(*p->a, *p->truth);
  std::vector<Metric> metrics;
  std::map<bool, double> total;
  for (const bool align : {false, true}) {
    const auto s = stack(
        {p->a, p->b}, {{"mode", std::string("Mean")}, {"sample_align", align}});
    ASSERT_NE(s.output, nullptr);
    const auto rms = band_rms(*s.output, *p->truth);
    ASSERT_FALSE(rms.empty());
    total[align] = total_rms(rms);
    add_band_metrics(metrics, align ? "aligned_" : "unaligned_", system, rms,
                     capture_rms);
  }
  EXPECT_LT(total[true], total[false])
      << "sample_align did not bring the stack nearer the truth";
  orc_videosynth::expect_metrics(metrics);
}

// chroma_gain_match on three captures of A's disc (projects/variants.json,
// stacking-chroma): capture A, a recapture by another player whose chroma is
// right, and a capture by a player with a chroma gain of 0.85. The match
// brings each input to the inputs' median burst, so it takes two players that
// agree to outvote the third; with two inputs the median is their mean, and a
// Mean stack's chroma is the same either way. The match measures chroma gain
// against the burst. Where the player's error reaches the burst, as a whole
// chroma path's or an FM/MTF loss's does, the match must find it and bring
// the stack's chroma nearer the truth. Where it is in the picture chroma
// only, as a mis-adjusted picture path's is, the burst shows nothing to
// measure: the match leaves the stack as it was, and the metrics record that.
constexpr const char* kPictureOnlyGainUnseen =
    "chroma_gain_match is burst-referenced; a picture-only chroma gain error "
    "is invisible to it.";

void run_chroma_gain_case(const std::string& system_name,
                          orc::VideoSystem system) {
  const auto a = open_vfr(case_file(system_name + "_capture_A", system));
  const auto recapture =
      open_vfr(case_file(system_name + "_capture_A_recapture", system));
  const auto truth =
      open_vfr(case_file(system_name + "_capture_A_clean", system));
  ASSERT_NE(a, nullptr);
  ASSERT_NE(recapture, nullptr);
  ASSERT_NE(truth, nullptr);
  const auto capture_rms = band_rms(*a, *truth);
  const auto chroma_band = bands(system).size() - 1;
  std::vector<Metric> metrics;
  for (const char* error : {"picture", "burst"}) {
    const auto b =
        open_vfr(case_file(system_name + "_capture_A_chroma_" + error, system));
    ASSERT_NE(b, nullptr) << error;
    std::map<bool, std::vector<double>> rms;
    for (const bool match : {false, true}) {
      const auto s = stack({a, recapture, b}, {{"mode", std::string("Mean")},
                                               {"chroma_gain_match", match}});
      ASSERT_NE(s.output, nullptr) << error;
      rms[match] = band_rms(*s.output, *truth);
      ASSERT_FALSE(rms[match].empty()) << error;
      add_band_metrics(
          metrics, std::string(error) + (match ? "_matched_" : "_unmatched_"),
          system, rms[match], capture_rms);
    }
    if (std::string(error) == "burst") {
      EXPECT_LT(rms[true][chroma_band], rms[false][chroma_band])
          << "chroma_gain_match did not correct a chroma gain error that "
             "reaches the burst";
    }
  }
  orc_videosynth::note(kPictureOnlyGainUnseen);
  orc_videosynth::expect_metrics(metrics);
}

// A held still (projects/stacking/*_held_still.yaml) with still-frame
// stacking: each frame is stacked with the copies of the still that share
// its subcarrier phase.
constexpr size_t kHeldStillFrames = 12;
constexpr int32_t kHeldStillSearch = 8;

void run_held_still_case(const std::string& system_name,
                         orc::VideoSystem system) {
  const auto capture = open_vfr(case_file(system_name + "_held_still", system));
  const auto truth =
      open_vfr(case_file(system_name + "_held_still_clean", system));
  ASSERT_NE(capture, nullptr);
  ASSERT_NE(truth, nullptr);
  ASSERT_EQ(capture->frame_count(), kHeldStillFrames);
  const auto s = stack({capture}, {{"still_frame_stacking", true},
                                   {"still_frame_search", kHeldStillSearch}});
  ASSERT_NE(s.output, nullptr);
  const auto stacked =
      std::dynamic_pointer_cast<const orc::StackedVideoFrameRepresentation>(
          s.output);
  ASSERT_NE(stacked, nullptr);

  // Frames between copies in the same subcarrier phase.
  const size_t cadence = system == orc::VideoSystem::NTSC ? 2 : 4;
  for (orc::FrameID f = 0; f < stacked->frame_count(); ++f) {
    EXPECT_GE(stacked->still_copies(f).size(),
              orc::StackedVideoFrameRepresentation::kMinStillCopies)
        << "frame " << f;
    const auto desc = stacked->get_frame_descriptor(f);
    ASSERT_TRUE(desc.has_value());
    EXPECT_TRUE(desc->held_checked) << "frame " << f;
    // The hold reaches the farthest copy in the same subcarrier phase within
    // the search, on each side.
    const auto reach = [&](size_t frames) {
      const size_t within = std::min<size_t>(frames, kHeldStillSearch);
      return static_cast<uint32_t>(within / cadence * cadence);
    };
    EXPECT_EQ(desc->held_frames_before, reach(f)) << "frame " << f;
    EXPECT_EQ(desc->held_frames_after, reach(kHeldStillFrames - 1 - f))
        << "frame " << f;
  }

  const auto capture_rms = band_rms(*capture, *truth);
  const auto rms = band_rms(*s.output, *truth);
  ASSERT_FALSE(rms.empty());
  EXPECT_LT(total_rms(rms), total_rms(capture_rms));
  std::vector<Metric> metrics;
  add_band_metrics(metrics, "still_", system, rms, capture_rms);
  orc_videosynth::expect_metrics(metrics);
}

// Pressing-defect repair: the Domesday model's
// pressing A with stamper defects, unflagged dropouts every capture of it
// carries on the same pictures (projects/pressing_defect/), stacked with
// pressing B. The stack keeps the defects, as A's four captures outvote B's
// two; the repair must bring the stack nearer the truth. On the Domesday
// model without defects, whose test pattern and multiburst carry fine
// detail on every pressing, it must change nothing.
void run_pressing_defect_case(const std::string& system_name,
                              orc::VideoSystem system) {
  const auto truth =
      open_vfr(case_file(system_name + "_domesday_A_clean", system));
  ASSERT_NE(truth, nullptr);
  const auto open_inputs = [&](const char* a_name) {
    std::vector<VfrPtr> inputs;
    for (const char* capture : {"cap1", "cap2", "cap3", "cap4"}) {
      inputs.push_back(open_vfr(case_file(
          system_name + "_domesday_" + a_name + "_" + capture, system)));
    }
    for (const char* capture : {"B_cap1", "B_cap2"}) {
      inputs.push_back(
          open_vfr(case_file(system_name + "_domesday_" + capture, system)));
    }
    return inputs;
  };
  const auto defective = open_inputs("A_defect");
  const auto sound = open_inputs("A");
  for (const auto& input : defective) ASSERT_NE(input, nullptr);
  for (const auto& input : sound) ASSERT_NE(input, nullptr);
  const auto capture_rms = band_rms(*defective[0], *truth);
  ASSERT_FALSE(capture_rms.empty());

  std::vector<Metric> metrics;
  std::map<bool, double> total;
  for (const bool repair : {false, true}) {
    const auto s = stack(defective, {{"mode", std::string("Weighted")},
                                     {"pressing_repair", repair}});
    ASSERT_NE(s.output, nullptr);
    const auto rms = band_rms(*s.output, *truth);
    ASSERT_FALSE(rms.empty());
    total[repair] = total_rms(rms);
    add_band_metrics(metrics, repair ? "repaired_" : "unrepaired_", system, rms,
                     capture_rms);
  }
  EXPECT_LT(total[true], total[false])
      << "pressing repair did not bring the stack nearer the truth";

  // Without defects nothing is repaired.
  const auto off = stack(sound, {{"mode", std::string("Weighted")}});
  const auto on = stack(
      sound, {{"mode", std::string("Weighted")}, {"pressing_repair", true}});
  ASSERT_NE(off.output, nullptr);
  ASSERT_NE(on.output, nullptr);
  size_t changed = 0;
  for (orc::FrameID f = 0; f < off.output->frame_count(); ++f) {
    const auto a = off.output->get_frame_copy(f);
    const auto b = on.output->get_frame_copy(f);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) changed += a[i] != b[i] ? 1 : 0;
  }
  EXPECT_EQ(changed, 0u) << "pressing repair changed a stack without defects";
  metrics.push_back({"sound_samples_changed", static_cast<double>(changed),
                     Better::Lower, 0.0});
  orc_videosynth::expect_metrics(metrics);
}

uint64_t dropout_samples(const orc::VideoFrameRepresentation& vfr,
                         orc::FrameID f) {
  uint64_t samples = 0;
  for (const auto& run : vfr.get_dropout_hints(f)) samples += run.sample_count;
  return samples;
}

// With EFM stacking disabled, each frame's EFM is the source's with the
// fewest dropout samples on that frame, the first on a tie. The captures
// carry different tones, so their EFM differs and the choice shows.
void run_efm_source_case(const std::string& system_name,
                         orc::VideoSystem system) {
  const auto p = open_pair(system_name, system);
  ASSERT_TRUE(p.has_value());
  ASSERT_TRUE(p->a->has_efm());
  ASSERT_TRUE(p->b->has_efm());
  const auto s =
      stack({p->a, p->b}, {{"efm_stacking", std::string("Disabled")}});
  ASSERT_NE(s.output, nullptr);
  size_t differing = 0;
  size_t chose_b = 0;
  for (orc::FrameID f = 0; f < p->a->frame_count(); ++f) {
    const auto efm_a = p->a->get_efm_samples(f);
    const auto efm_b = p->b->get_efm_samples(f);
    if (efm_a == efm_b) continue;
    ++differing;
    const bool b_best = dropout_samples(*p->b, f) < dropout_samples(*p->a, f);
    if (b_best) ++chose_b;
    EXPECT_EQ(s.output->get_efm_samples(f), b_best ? efm_b : efm_a)
        << "frame " << f << ": " << dropout_samples(*p->a, f) << " and "
        << dropout_samples(*p->b, f) << " dropout samples";
  }
  // The case shows the choice only if both sources win somewhere.
  EXPECT_GT(differing, 0u);
  EXPECT_GT(chose_b, 0u);
  EXPECT_LT(chose_b, differing);
}

// ---------------------------------------------------------------------------
// Film with 2:3 pulldown (projects/pulldown/)
// ---------------------------------------------------------------------------

// Captures 1-4 of the film disc: whole; from reel 2, so from picture 9 and
// disc frame 10; replaying reel 3; ending after reel 3. The disc holds 40
// frames and 32 pictures, a frame in five carrying none.
constexpr size_t kFilmFrames = 40;
constexpr size_t kFilmLateStartPad = 10;
constexpr size_t kFilmCaptureFrames[] = {40, 30, 40, 30};

// The chain the GUI runs on a film disc: each capture mapped by the Disc
// Mapper and Frame Map, which must keep its pulldown frames, placed by Source
// Align in frames (a capture starting eight pictures in starts ten frames
// in), checked for content offsets, and stacked against the clean disc.
void run_pulldown_case() {
  constexpr auto kSystem = orc::VideoSystem::NTSC;
  const auto truth = open_vfr(case_file("ntsc_film_disc_clean", kSystem));
  ASSERT_NE(truth, nullptr);
  ASSERT_EQ(truth->frame_count(), kFilmFrames);
  const auto disc_pictures = orc_videosynth::read_pictures(*truth);

  std::vector<orc::ArtifactPtr> captures;
  std::vector<std::unique_ptr<orc::FrameMapStage>> frame_maps;
  size_t pulldown_frames = 0;
  for (size_t c = 0; c < std::size(kCaptureSuffixes); ++c) {
    const std::string name =
        std::string("ntsc_film_disc") + kCaptureSuffixes[c];
    auto artifact = orc_videosynth::open_case(case_file(name, kSystem));
    ASSERT_NE(artifact, nullptr) << name;
    const auto decision =
        orc_videosynth::map_disc(*orc_videosynth::as_vfr(artifact));
    ASSERT_TRUE(decision.success) << name << ": " << decision.rationale;
    EXPECT_EQ(decision.stats.padding_frames, 0u) << name;
    EXPECT_EQ(decision.stats.final_frames, kFilmCaptureFrames[c])
        << name << ": " << decision.rationale;
    pulldown_frames += decision.stats.pulldown_frames;
    frame_maps.push_back(std::make_unique<orc::FrameMapStage>());
    artifact = orc_videosynth::apply_frame_map(artifact, *frame_maps.back(),
                                               decision.mapping_spec);
    ASSERT_NE(artifact, nullptr) << name;
    captures.push_back(artifact);
  }

  std::vector<std::vector<orc::PictureNumberReading>> readings;
  std::vector<const orc::VideoFrameRepresentation*> mapped;
  for (const auto& capture : captures) {
    mapped.push_back(orc_videosynth::as_vfr(capture).get());
    readings.push_back(orc_videosynth::readings_of(
        orc_videosynth::read_pictures(*mapped.back())));
  }
  const auto alignment = orc::compute_pad_alignment(readings);
  ASSERT_EQ(alignment.first_picture, 1);
  std::vector<std::optional<int64_t>> starts;
  for (size_t i = 0; i < alignment.pads.size(); ++i) {
    const size_t expected = i == kLateCapture ? kFilmLateStartPad : 0;
    ASSERT_EQ(alignment.pads[i], expected) << "capture " << i + 1;
    starts.emplace_back(-static_cast<int64_t>(*alignment.pads[i]));
  }

  // Film repeats a picture in one field of a frame in five; the content
  // check must not take that for a capture sitting off its numbers.
  const auto offsets = orc::check_content_offsets(mapped, starts);
  for (size_t i = 0; i < offsets.spans.size(); ++i) {
    EXPECT_TRUE(offsets.spans[i].empty()) << "capture " << i + 1;
  }

  orc::SourceAlignStage align;
  const Parameters align_parameters{
      {"alignmentMode", std::string("pad_for_alignment")},
      {"alignmentMap", orc::format_alignment_map(alignment.pads)}};
  ASSERT_TRUE(align.set_parameters(align_parameters));
  orc::ObservationContext ctx;
  const auto aligned = align.execute(captures, align_parameters, ctx);
  ASSERT_EQ(aligned.size(), captures.size());
  std::vector<VfrPtr> inputs;
  for (const auto& a : aligned) {
    inputs.push_back(orc_videosynth::as_vfr(a));
    ASSERT_NE(inputs.back(), nullptr);
    ASSERT_EQ(inputs.back()->frame_count(), kFilmFrames);
  }

  // Every frame of every capture holds the disc's frame or is padding.
  size_t frames_in_place = 0;
  for (size_t i = 0; i < inputs.size(); ++i) {
    const auto pictures = orc_videosynth::read_pictures(*inputs[i]);
    for (orc::FrameID f = 0; f < kFilmFrames; ++f) {
      if (inputs[i]->get_frame_descriptor(f)->is_padding_frame) continue;
      EXPECT_EQ(pictures[f], disc_pictures[f])
          << "capture " << i + 1 << " frame " << f;
      if (pictures[f] == disc_pictures[f]) ++frames_in_place;
    }
  }

  const auto capture_rms = band_rms(*inputs[0], *truth);
  ASSERT_FALSE(capture_rms.empty());
  std::vector<Metric> metrics{
      {"pulldown_frames_kept", static_cast<double>(pulldown_frames),
       Better::Higher, 0.0},
      {"frames_in_place", static_cast<double>(frames_in_place), Better::Higher,
       0.0}};
  for (const char* mode : {"Mean", "Weighted"}) {
    const auto s = stack(inputs, {{"mode", std::string(mode)}});
    ASSERT_NE(s.output, nullptr) << mode;
    ASSERT_EQ(s.output->frame_count(), kFilmFrames) << mode;
    const auto rms = band_rms(*s.output, *truth);
    ASSERT_FALSE(rms.empty()) << mode;
    std::string prefix = mode;
    for (auto& ch : prefix) ch = static_cast<char>(std::tolower(ch));
    add_band_metrics(metrics, prefix + "_", kSystem, rms, capture_rms);
    // Stacked in step, the captures' noise averages down in every band; a
    // capture one frame out would blend two film pictures instead.
    for (size_t b = 0; b < rms.size(); ++b) {
      EXPECT_LT(rms[b], capture_rms[b])
          << mode << " " << bands(kSystem)[b].name;
    }
  }
  orc_videosynth::expect_metrics(metrics);
}

}  // namespace

TEST(StackerTruth, NTSC_Pulldown) { run_pulldown_case(); }

TEST(StackerTruth, PAL_Pressings) {
  run_pressings_case("pal", orc::VideoSystem::PAL, 4);
}

TEST(StackerTruth, PAL_LonePressing) {
  run_pressings_case("pal", orc::VideoSystem::PAL, 1);
}

TEST(StackerTruth, NTSC_Pressings) {
  run_pressings_case("ntsc", orc::VideoSystem::NTSC, 4);
}

TEST(StackerTruth, NTSC_LonePressing) {
  run_pressings_case("ntsc", orc::VideoSystem::NTSC, 1);
}

TEST(StackerTruth, PALM_Pressings) {
  run_pressings_case("pal_m", orc::VideoSystem::PAL_M, 4);
}

TEST(StackerTruth, PALM_LonePressing) {
  run_pressings_case("pal_m", orc::VideoSystem::PAL_M, 1);
}

TEST(StackerTruth, PAL_DomesdayModel) {
  run_domesday_case("pal", orc::VideoSystem::PAL);
}

TEST(StackerTruth, NTSC_DomesdayModel) {
  run_domesday_case("ntsc", orc::VideoSystem::NTSC);
}

TEST(StackerTruth, PALM_DomesdayModel) {
  run_domesday_case("pal_m", orc::VideoSystem::PAL_M);
}

TEST(StackerTruth, PAL_PressingDefect) {
  run_pressing_defect_case("pal", orc::VideoSystem::PAL);
}

TEST(StackerTruth, NTSC_PressingDefect) {
  run_pressing_defect_case("ntsc", orc::VideoSystem::NTSC);
}

TEST(StackerTruth, PAL_SampleAlign) {
  run_sample_align_case("pal", orc::VideoSystem::PAL);
}

TEST(StackerTruth, NTSC_SampleAlign) {
  run_sample_align_case("ntsc", orc::VideoSystem::NTSC);
}

TEST(StackerTruth, PALM_SampleAlign) {
  run_sample_align_case("pal_m", orc::VideoSystem::PAL_M);
}

TEST(StackerTruth, PAL_ChromaGainMatch) {
  run_chroma_gain_case("pal", orc::VideoSystem::PAL);
}

TEST(StackerTruth, NTSC_ChromaGainMatch) {
  run_chroma_gain_case("ntsc", orc::VideoSystem::NTSC);
}

TEST(StackerTruth, PALM_ChromaGainMatch) {
  run_chroma_gain_case("pal_m", orc::VideoSystem::PAL_M);
}

TEST(StackerTruth, PAL_HeldStill) {
  run_held_still_case("pal", orc::VideoSystem::PAL);
}

TEST(StackerTruth, NTSC_HeldStill) {
  run_held_still_case("ntsc", orc::VideoSystem::NTSC);
}

TEST(StackerTruth, PALM_HeldStill) {
  run_held_still_case("pal_m", orc::VideoSystem::PAL_M);
}

TEST(StackerTruth, PAL_EfmSource) {
  run_efm_source_case("pal", orc::VideoSystem::PAL);
}

TEST(StackerTruth, NTSC_EfmSource) {
  run_efm_source_case("ntsc", orc::VideoSystem::NTSC);
}

TEST(StackerTruth, PALM_EfmSource) {
  run_efm_source_case("pal_m", orc::VideoSystem::PAL_M);
}

}  // namespace orc_videosynth_test
