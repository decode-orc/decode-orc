/*
 * File:        still_truth_test.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     The PAL chroma decoders scored against the source picture:
 *              each still of projects/still_truth/pal_stills.yaml decoded
 *              and compared with videosynth's picture truth
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <decoded_comparison.h>
#include <gtest/gtest.h>
#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <picture_truth.h>
#include <still_metrics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sink_decode.h"
#include "videosynth_cases.h"
#include "videosynth_harness.h"

namespace orc_videosynth_test {

namespace {

using orc_videosynth::Better;
using orc_videosynth::CaseFile;
using orc_videosynth::ComponentPicture;
using orc_videosynth::DecodedFile;
using orc_videosynth::Metric;
using orc_videosynth::TruthFile;

constexpr const char* kStem = "videosynth_pal_stills";
constexpr size_t kFrames = 14;

// The measured stills of pal_stills.yaml, by frame; frames 0 and 13 are the
// different pictures either side.
struct Still {
  size_t frame;
  const char* name;
};
constexpr std::array<Still, 12> kStills = {{{1, "stress"},
                                            {2, "luma_zone_plate"},
                                            {3, "chroma_zone_plate"},
                                            {4, "test_card_f"},
                                            {5, "bars"},
                                            {6, "tartan"},
                                            {7, "osm_town"},
                                            {8, "osm_rural"},
                                            {9, "text_card"},
                                            {10, "green_magenta_edges"},
                                            {11, "photo_butterfly"},
                                            {12, "photo_galtstroms"}}};
constexpr size_t kStressFrame = 1;
constexpr size_t kLumaZonePlateFrame = 2;
constexpr size_t kTestCardFrame = 4;
constexpr size_t kBarsFrame = 5;

const CaseFile kNoisy{"pal_stills", kStem, orc::VideoSystem::PAL};
const CaseFile kClean{"pal_stills_clean", "videosynth_pal_stills_clean",
                      orc::VideoSystem::PAL};
const CaseFile kYc{"pal_stills_yc", "videosynth_pal_stills_clean",
                   orc::VideoSystem::PAL, true};

std::optional<TruthFile> read_truth(const CaseFile& file) {
  const auto base = orc_videosynth::case_base(file);
  if (!base) return std::nullopt;
  std::string error;
  auto truth = TruthFile::read(*base, &error);
  if (!truth) {
    ADD_FAILURE() << error;
    return std::nullopt;
  }
  if (truth->geometry().frame_count != kFrames) {
    ADD_FAILURE() << "the truth holds " << truth->geometry().frame_count
                  << " frames, not " << kFrames;
    return std::nullopt;
  }
  return truth;
}

// The decode's width: the sink writes the cvbs_source active window
// unpadded (output_padding 1).
constexpr size_t kDecodedWidth =
    orc::kPalActiveVideoEnd - orc::kPalActiveVideoStart;

// The PAL truth's samples per line.
constexpr size_t kPalTruthWidth = 922;

const Parameters kUnpadded{{"output_padding", 1}};

struct Decode {
  DecodedFile decoded;
  TruthFile truth;
};

std::optional<Decode> decode_stills(const CaseFile& file,
                                    const std::string& decoder) {
  auto truth = read_truth(file);
  if (!truth) return std::nullopt;
  auto decoded = decode_case(file, decoder, kUnpadded,
                             file.case_name + "_" + decoder, kFrames);
  if (!decoded) return std::nullopt;
  return Decode{std::move(*decoded), std::move(*truth)};
}

// Frame |frame| of a decode on the truth grid, and the samples to measure:
// the decode covers them and they carry picture.
std::pair<ComponentPicture, std::vector<uint8_t>> on_truth_grid(
    const Decode& d, size_t frame,
    orc_videosynth::DecodeOrigin origin =
        orc_videosynth::decode_origin(orc::VideoSystem::PAL)) {
  std::vector<uint8_t> covered;
  auto picture = orc_videosynth::decoded_picture(
      d.decoded, frame, kDecodedWidth, origin, d.truth.geometry(), &covered);
  return {std::move(picture),
          orc_videosynth::mask_and(d.truth.picture_mask(), covered)};
}

orc_videosynth::StillMetricOptions options(const TruthFile& truth) {
  orc_videosynth::StillMetricOptions o;
  o.system = truth.geometry().system;
  o.sample_rate_hz = truth.geometry().sample_rate_hz;
  return o;
}

// The RMS of |picture|'s chroma vector over |region|, in percent.
double chroma_rms_pct(const ComponentPicture& picture,
                      const orc_videosynth::Region& region) {
  double energy = 0.0;
  size_t samples = 0;
  for (size_t row = region.y; row < region.y + region.height; ++row) {
    for (size_t x = region.x; x < region.x + region.width; ++x) {
      const size_t n = picture.index(x, row);
      energy += (picture.u[n] * picture.u[n]) + (picture.v[n] * picture.v[n]);
      ++samples;
    }
  }
  return samples == 0
             ? 0.0
             : std::sqrt(energy / static_cast<double>(samples)) * 100.0;
}

// The 0-based line of a flat (transmission-order) frame that carries truth
// row |row|: the upper field supplies the even rows.
size_t flat_line(const orc_videosynth::TruthGeometry& g, size_t row) {
  const bool upper = row % 2 == 0;
  const int field = upper ? g.upper_field : 3 - g.upper_field;
  const int first = field == 1 ? g.first_line_field1 : g.first_line_field2;
  return static_cast<size_t>(first - 1) + (row / 2);
}

void add(std::vector<Metric>& metrics, const std::string& name, double value,
         Better better, double relative, double absolute) {
  metrics.push_back({name, value, better,
                     orc_videosynth::new_tolerance(value, relative, absolute)});
}

// The stress card's chroma_line_sign regions (pal_stills.yaml), in source
// pixels: the line-alternating strips and the burst-gate castellation. No
// single-frame decoder can recover a line-alternating colour (it decodes
// grey: PAL_StressAlternatingBars), so they are left out of the card's
// scores, with a margin for each decoder's filter spill.
struct PixelRegion {
  size_t x;
  size_t y;
  size_t width;
  size_t height;
};
constexpr std::array<PixelRegion, 3> kLineSignRegions = {
    {{26, 16, 38, 544}, {656, 16, 38, 544}, {0, 100, 24, 40}}};
constexpr size_t kLineSignMargin = 8;  // pixels and rows

std::vector<uint8_t> without_line_sign_regions(std::vector<uint8_t> mask) {
  const size_t width = kPalTruthWidth;
  const size_t height = mask.size() / width;
  for (const PixelRegion& r : kLineSignRegions) {
    const size_t x = r.x > kLineSignMargin ? r.x - kLineSignMargin : 0;
    const size_t y = r.y > kLineSignMargin ? r.y - kLineSignMargin : 0;
    const auto region = orc_videosynth::source_region(
        x, y, r.x + r.width + kLineSignMargin - x,
        r.y + r.height + kLineSignMargin - y, width);
    const auto inside = orc_videosynth::region_mask(width, height, region);
    for (size_t n = 0; n < mask.size(); ++n) {
      if (inside[n] != 0) mask[n] = 0;
    }
  }
  return mask;
}

// The truth samples near which the clean composite of |frame| sits at a
// coding rail (sync tip or peak), so no decoder can recover the truth
// there: 100 % yellow and cyan peak above the 10-bit ceiling, and so does
// the ringing of the band-limited mapping on hard-edged glyphs. A clipped
// sample spoils every decode whose filter reaches it, so the mask spans
// the 16-sample edge exclusion across and two field lines down.
constexpr size_t kClipMarginSamples = 16;
constexpr size_t kClipMarginRows = 4;

struct ClipMask {
  std::vector<uint8_t> mask;
  size_t clipped = 0;  // composite samples at a rail, under the picture
};

std::optional<ClipMask> clip_mask(const CaseFile& clean, const TruthFile& truth,
                                  size_t frame) {
  const auto artifact = orc_videosynth::open_case(clean);
  if (artifact == nullptr) return std::nullopt;
  const auto vfr = orc_videosynth::as_vfr(artifact);
  if (!vfr) return std::nullopt;
  const auto& g = truth.geometry();
  const auto picture = truth.picture_mask();
  ClipMask result{std::vector<uint8_t>(picture.size(), 1), 0};
  const orc::FrameID id = vfr->frame_range().first + frame;
  for (size_t row = 0; row < g.rows; ++row) {
    const auto* line = vfr->get_line(id, flat_line(g, row));
    if (line == nullptr) return std::nullopt;
    for (size_t x = 0; x < g.samples_per_line; ++x) {
      const auto s = line[static_cast<size_t>(g.first_sample) + x];
      if (s > orc::kPalSyncTip && s < orc::kPalPeak) continue;
      if (picture[(row * g.samples_per_line) + x] == 0) continue;
      ++result.clipped;
      const size_t r0 = row > kClipMarginRows ? row - kClipMarginRows : 0;
      const size_t r1 = std::min(g.rows, row + kClipMarginRows + 1);
      const size_t x0 = x > kClipMarginSamples ? x - kClipMarginSamples : 0;
      const size_t x1 =
          std::min(g.samples_per_line, x + kClipMarginSamples + 1);
      for (size_t r = r0; r < r1; ++r) {
        for (size_t c = x0; c < x1; ++c) {
          result.mask[(r * g.samples_per_line) + c] = 0;
        }
      }
    }
  }
  return result;
}

// A black-to-white step of the card's top castellations (rows 2 to 11, inside
// the border's first 14 rows), the one nearest the centre: 16 samples
// either side of where the truth crosses half way.
std::optional<orc_videosynth::Region> castellation_edge(
    const ComponentPicture& truth) {
  constexpr size_t kFirstRow = 2;
  constexpr size_t kRows = 10;
  constexpr size_t kHalf = 16;
  std::vector<double> profile(truth.width, 0.0);
  for (size_t row = kFirstRow; row < kFirstRow + kRows; ++row) {
    for (size_t x = 0; x < truth.width; ++x) {
      profile[x] += truth.y[truth.index(x, row)] / kRows;
    }
  }
  std::optional<orc_videosynth::Region> best;
  size_t best_distance = truth.width;
  for (size_t x = kHalf; x + kHalf < truth.width; ++x) {
    if (profile[x - 1] < 0.5 && profile[x] >= 0.5 &&
        profile[x - kHalf] < 0.05 && profile[x + kHalf - 1] > 0.95) {
      const size_t distance =
          x > truth.width / 2 ? x - (truth.width / 2) : (truth.width / 2) - x;
      if (distance < best_distance) {
        best_distance = distance;
        best = orc_videosynth::Region{x - kHalf, kFirstRow, 2 * kHalf, kRows};
      }
    }
  }
  return best;
}

// An edge's 10-90 % rise and overshoot, each as the decode's excess over
// the truth's own: the truth's edge is already band-limited by the source
// mapping, and only what the decoder adds is its.
void add_edge(std::vector<Metric>& metrics, const std::string& name,
              const Decode& d, size_t frame,
              const orc_videosynth::Region& region, double sample_rate_hz) {
  const auto [decoded, mask] = on_truth_grid(d, frame);
  (void)mask;
  const auto truth = d.truth.frame(frame);
  const auto measured = orc_videosynth::edge_response(
      decoded, orc_videosynth::Plane::Y, region, sample_rate_hz);
  const auto reference = orc_videosynth::edge_response(
      truth, orc_videosynth::Plane::Y, region, sample_rate_hz);
  ASSERT_TRUE(measured.has_value() && reference.has_value())
      << "no luma step at the " << name;
  add(metrics, name + "_rise_excess_ns", measured->rise_ns - reference->rise_ns,
      Better::Lower, 0.0, 2.0);
  add(metrics, name + "_overshoot_excess_pct",
      measured->overshoot_pct - reference->overshoot_pct, Better::Lower, 0.0,
      0.2);
}

// The bench at the other places in the PAL sequence (rotate_stills.py):
// at rotation r (0-3) each still sits r frames later, the last ones
// wrapping to the front. Each still has only ever been decoded at one
// carrier phase and V-switch alignment otherwise, and a decoder tuned on
// it could fit a still and its phase together (Domesday2D investigation, Phase
// 7).
constexpr int kRotations = 4;

CaseFile rotated(const CaseFile& file, int rotation) {
  if (rotation == 0) return file;
  const std::string tag = "_rot" + std::to_string(rotation);
  if (file.case_name == kClean.case_name) {
    return {"pal_stills" + tag + "_clean",
            "videosynth_pal_stills" + tag + "_clean", orc::VideoSystem::PAL};
  }
  return {"pal_stills" + tag, "videosynth_pal_stills" + tag,
          orc::VideoSystem::PAL};
}

size_t rotated_frame(size_t frame, int rotation) {
  return ((frame - 1 + static_cast<size_t>(rotation)) % kStills.size()) + 1;
}

// Every still of |file| at |rotation| decoded with |decoder| and scored
// against the truth, into |metrics|; |reference| is rotation 0's truth, which
// each still's must match wherever it moved.
void score_rotation(const CaseFile& file, const std::string& decoder,
                    int rotation, const TruthFile* reference,
                    std::vector<Metric>& metrics) {
  const auto d = decode_stills(rotated(file, rotation), decoder);
  ASSERT_TRUE(d.has_value());
  const CaseFile clean = rotated(kClean, rotation);
  const auto o = options(d->truth);
  // Where a still is, by its rotation-0 frame.
  const auto at = [rotation](size_t frame) {
    return rotated_frame(frame, rotation);
  };
  for (const Still& still : kStills) {
    const ComponentPicture truth = d->truth.frame(at(still.frame));
    if (reference != nullptr) {
      ASSERT_EQ(truth.y, reference->frame(still.frame).y)
          << still.name << " is not on frame " << at(still.frame)
          << " at rotation " << rotation;
    }
    auto [decoded, mask] = on_truth_grid(*d, at(still.frame));
    if (still.frame == kStressFrame) mask = without_line_sign_regions(mask);
    const auto clipped = clip_mask(clean, d->truth, at(still.frame));
    ASSERT_TRUE(clipped.has_value()) << still.name;
    mask = orc_videosynth::mask_and(mask, clipped->mask);
    const auto m = orc_videosynth::measure_still(decoded, truth, mask, o);
    ASSERT_GT(m.samples, 0U) << still.name;
    const std::string p = std::string(still.name) + "_";
    if (clipped->clipped > 0) {
      metrics.push_back({p + "clipped_samples",
                         static_cast<double>(clipped->clipped), Better::Lower,
                         0.0});
    }
    add(metrics, p + "y_rms_pct", m.y_rms_pct, Better::Lower, 0.05, 0.01);
    add(metrics, p + "u_rms_pct", m.u_rms_pct, Better::Lower, 0.05, 0.01);
    add(metrics, p + "v_rms_pct", m.v_rms_pct, Better::Lower, 0.05, 0.01);
    add(metrics, p + "rgb_psnr_db", m.rgb_psnr_db, Better::Higher, 0.0, 0.1);
    add(metrics, p + "delta_e_mean", m.delta_e_mean, Better::Lower, 0.05, 0.02);
    add(metrics, p + "delta_e_p95", m.delta_e_p95, Better::Lower, 0.05, 0.05);
    if (m.chroma_samples > 0) {
      add(metrics, p + "chroma_magnitude_rms_pct", m.chroma_magnitude_rms_pct,
          Better::Lower, 0.05, 0.01);
      add(metrics, p + "chroma_phase_rms_deg", m.chroma_phase_rms_deg,
          Better::Lower, 0.05, 0.1);
      add(metrics, p + "chroma_gain_error_pct",
          std::abs(m.chroma_gain - 1.0) * 100.0, Better::Lower, 0.05, 0.05);
    }
    if (m.cross_colour_samples > 0) {
      add(metrics, p + "cross_colour_rms_pct", m.cross_colour_rms_pct,
          Better::Lower, 0.05, 0.01);
    }
    if (m.cross_luma_samples > 0) {
      add(metrics, p + "cross_luma_rms_pct", m.cross_luma_rms_pct,
          Better::Lower, 0.05, 0.01);
    }
  }

  // Edges: the green-to-magenta boundary of the 75 % bars (source pixel
  // 360), a luma step with a chroma step on it, and a black-to-white
  // castellation of the stress card's top border, luma alone.
  const size_t width = d->truth.geometry().samples_per_line;
  add_edge(metrics, "bars_edge", *d, at(kBarsFrame),
           orc_videosynth::source_region(348, 100, 24, 376, width),
           o.sample_rate_hz);
  const auto castellation = castellation_edge(d->truth.frame(at(kStressFrame)));
  ASSERT_TRUE(castellation.has_value()) << "no castellation edge on the card";
  add_edge(metrics, "castellation_edge", *d, at(kStressFrame), *castellation,
           o.sample_rate_hz);
}

// Each metric's mean over |runs|. A still's clipped-sample count is only
// recorded where it has some, so a rotation without one counts as 0.
std::vector<Metric> mean_over(const std::vector<std::vector<Metric>>& runs) {
  std::vector<Metric> mean;
  std::vector<size_t> seen;
  for (const auto& run : runs) {
    for (const Metric& m : run) {
      const auto it =
          std::find_if(mean.begin(), mean.end(),
                       [&m](const Metric& x) { return x.name == m.name; });
      if (it == mean.end()) {
        mean.push_back(m);
        seen.push_back(1);
      } else {
        it->value += m.value;
        it->new_tolerance += m.new_tolerance;
        ++seen[static_cast<size_t>(it - mean.begin())];
      }
    }
  }
  for (size_t i = 0; i < mean.size(); ++i) {
    const bool clipped =
        mean[i].name.find("clipped_samples") != std::string::npos;
    const double n = static_cast<double>(clipped ? runs.size() : seen[i]);
    mean[i].value /= n;
    mean[i].new_tolerance /= static_cast<double>(seen[i]);
    if (!clipped && seen[i] != runs.size()) {
      ADD_FAILURE() << mean[i].name << " was measured at " << seen[i] << " of "
                    << runs.size() << " rotations";
    }
  }
  return mean;
}

// Every still of |file| decoded with |decoder| at each place in the PAL
// sequence and scored against the truth; each metric is the mean over the
// four.
void run_still_case(const CaseFile& file, const std::string& decoder) {
  const auto reference = read_truth(file);
  ASSERT_TRUE(reference.has_value());
  std::vector<std::vector<Metric>> runs;
  for (int rotation = 0; rotation < kRotations; ++rotation) {
    std::vector<Metric> metrics;
    score_rotation(file, decoder, rotation,
                   rotation == 0 ? nullptr : &*reference, metrics);
    if (::testing::Test::HasFatalFailure()) return;
    runs.push_back(std::move(metrics));
  }
  orc_videosynth::expect_metrics(mean_over(runs));
}

}  // namespace

// Phase 1 acceptance (truth): re-encoded with videosynth's own carrier, the
// truth is the Y/C rendering to within 1 LSB. Luma directly; chroma as
// U sin(phi) + s V cos(phi) on the 4fsc grid, each line's carrier phase and
// V-switch sign s fitted, since the truth does not state them. The Y/C
// rendering is read through cvbs_source, whose frames are flat: lines in
// transmission order, each starting at 0H.
//
// Where the band-limited mapping rings past the 10-bit range (the text
// card's hard-edged glyphs), the truth holds the unclipped value and the
// coding clips it: such a sample must sit exactly at its rail. Chroma is
// allowed 1 LSB plus 0.25 % of the line's chroma peak: the most saturated
// lines (336 codes, on the town map) differ by about 0.3 % within a line.
// The chroma gain is checked on lines whose chroma peaks at 0.05 or more;
// on fainter lines the fitted gain is dominated by rounding.
TEST(DecoderStillTruth, PAL_TruthMatchesYc) {
  const auto truth = read_truth(kYc);
  ASSERT_TRUE(truth.has_value());
  const auto artifact = orc_videosynth::open_case(kYc);
  ASSERT_TRUE(artifact != nullptr);
  const auto vfr = orc_videosynth::as_vfr(artifact);
  ASSERT_TRUE(vfr && vfr->has_separate_channels());
  const auto params = vfr->get_video_parameters();
  ASSERT_TRUE(params.has_value());
  const auto& g = truth->geometry();
  const double luma_codes = params->white_level - params->black_level;
  const double codes_per_mv =
      (params->white_level - params->blanking_level) / g.white_mv;
  const double chroma_codes = (g.white_mv - g.black_mv) * codes_per_mv;
  const auto mask = truth->picture_mask();

  double worst_luma = 0.0;
  double worst_chroma = 0.0;
  double worst_chroma_excess = 0.0;
  double worst_gain = 0.0;
  size_t luma_clipped = 0;
  const auto first_frame = vfr->frame_range().first;
  for (size_t frame = 0; frame < kFrames; ++frame) {
    const auto picture = truth->frame(frame);
    const orc::FrameID id = first_frame + frame;
    for (size_t row = 0; row < g.rows; ++row) {
      const size_t line = flat_line(g, row);
      const auto* luma = vfr->get_line_luma(id, line);
      const auto* chroma = vfr->get_line_chroma(id, line);
      ASSERT_TRUE(luma != nullptr && chroma != nullptr);
      std::vector<size_t> xs;
      for (size_t x = 0; x < g.samples_per_line; ++x) {
        if (mask[picture.index(x, row)] != 0) xs.push_back(x);
      }
      double chroma_peak = 0.0;
      for (const size_t x : xs) {
        const size_t n = picture.index(x, row);
        const double expected =
            params->black_level + (picture.y[n] * luma_codes);
        const double coded =
            std::clamp(expected, static_cast<double>(orc::kPalSyncTip),
                       static_cast<double>(orc::kPalPeak));
        if (coded != expected) ++luma_clipped;
        worst_luma = std::max(
            worst_luma,
            std::abs(luma[static_cast<size_t>(g.first_sample) + x] - coded));
        chroma_peak =
            std::max(chroma_peak, std::hypot(picture.u[n], picture.v[n]));
      }
      // C = A b1 + B b2 + c0 with b1 = U cos(n pi/2) - s V sin(n pi/2) and
      // b2 = U sin(n pi/2) + s V cos(n pi/2): phi = phi0 + n pi/2, A and B
      // the carrier's sin and cos phi0 times the gain.
      double best = 1e30;
      double best_gain = 0.0;
      for (const double s : {1.0, -1.0}) {
        std::array<std::array<double, 4>, 3> normal{};
        std::vector<std::array<double, 3>> basis;
        basis.reserve(xs.size());
        for (const size_t x : xs) {
          const size_t n = static_cast<size_t>(g.first_sample) + x;
          static constexpr std::array<int, 4> kCos = {1, 0, -1, 0};
          static constexpr std::array<int, 4> kSin = {0, 1, 0, -1};
          const double c = kCos[n % 4];
          const double sn = kSin[n % 4];
          const double u = picture.u[picture.index(x, row)];
          const double v = s * picture.v[picture.index(x, row)];
          const std::array<double, 3> b = {(u * c) - (v * sn),
                                           (u * sn) + (v * c), 1.0};
          basis.push_back(b);
          for (size_t i = 0; i < 3; ++i) {
            for (size_t j = 0; j < 3; ++j) normal[i][j] += b[i] * b[j];
            normal[i][3] += b[i] * chroma[n];
          }
        }
        // Gauss-Jordan on the 3x3 normal equations; a line without chroma
        // leaves only c0.
        std::array<double, 3> k = {0.0, 0.0, 0.0};
        if (normal[0][0] + normal[1][1] < 1e-9) {
          k[2] = normal[2][3] / normal[2][2];
        } else {
          for (size_t i = 0; i < 3; ++i) {
            size_t pivot = i;
            for (size_t r = i + 1; r < 3; ++r) {
              if (std::abs(normal[r][i]) > std::abs(normal[pivot][i])) {
                pivot = r;
              }
            }
            std::swap(normal[i], normal[pivot]);
            for (size_t r = 0; r < 3; ++r) {
              if (r == i) continue;
              const double f = normal[r][i] / normal[i][i];
              for (size_t col = i; col < 4; ++col) {
                normal[r][col] -= f * normal[i][col];
              }
            }
          }
          for (size_t i = 0; i < 3; ++i) k[i] = normal[i][3] / normal[i][i];
        }
        double worst = 0.0;
        for (size_t i = 0; i < xs.size(); ++i) {
          const size_t n = static_cast<size_t>(g.first_sample) + xs[i];
          const double fit = (k[0] * basis[i][0]) + (k[1] * basis[i][1]) + k[2];
          worst = std::max(worst, std::abs(chroma[n] - fit));
        }
        if (worst < best) {
          best = worst;
          best_gain = std::hypot(k[0], k[1]);
        }
      }
      worst_chroma = std::max(worst_chroma, best);
      worst_chroma_excess = std::max(
          worst_chroma_excess, best - (0.0025 * chroma_peak * chroma_codes));
      if (chroma_peak >= 0.05) {
        worst_gain =
            std::max(worst_gain, std::abs((best_gain / chroma_codes) - 1.0));
      }
    }
  }
  EXPECT_LE(worst_luma, 1.0) << "the truth's luma is not the Y/C luma";
  EXPECT_LE(worst_chroma_excess, 1.0)
      << "the truth's chroma is not the Y/C chroma";
  EXPECT_LT(worst_gain, 0.01) << "the fitted chroma gain is not the truth's";
  orc_videosynth::expect_metrics({
      {"luma_max_lsb", worst_luma, Better::Lower, 1.0},
      {"chroma_max_lsb", worst_chroma, Better::Lower, 1.0},
      {"luma_clipped_samples", static_cast<double>(luma_clipped), Better::Lower,
       0.0},
  });
}

// The decode lands on the truth grid where decode_origin() says: on the Y/C
// rendering (no separation), Test Card F's luma error is smallest there of
// every offset within 3 samples and 2 rows.
TEST(DecoderStillTruth, PAL_Registration) {
  const auto d = decode_stills(kYc, "pal2d");
  ASSERT_TRUE(d.has_value());
  const auto truth = d->truth.frame(kTestCardFrame);
  const auto nominal = orc_videosynth::decode_origin(orc::VideoSystem::PAL);
  const auto o = options(d->truth);
  double best = 1e30;
  int best_dx = 0;
  int best_dy = 0;
  double at_nominal = 0.0;
  for (int dy = -2; dy <= 2; ++dy) {
    for (int dx = -3; dx <= 3; ++dx) {
      const orc_videosynth::DecodeOrigin origin{nominal.first_sample + dx,
                                                nominal.first_row + dy};
      const auto [decoded, mask] = on_truth_grid(*d, kTestCardFrame, origin);
      const double y =
          orc_videosynth::measure_still(decoded, truth, mask, o).y_rms_pct;
      if (dx == 0 && dy == 0) at_nominal = y;
      if (y < best) {
        best = y;
        best_dx = dx;
        best_dy = dy;
      }
    }
  }
  EXPECT_EQ(best_dx, 0);
  EXPECT_EQ(best_dy, 0);
  orc_videosynth::expect_metrics(
      {{"test_card_f_yc_y_rms_pct", at_nominal, Better::Lower,
        orc_videosynth::new_tolerance(at_nominal, 0.05, 0.01)}});
}

// Phase 1 acceptance (resampling): with the band-limited mapping, a
// luma-only still decoded with mono leaves no energy above 5.5 MHz in its
// difference from the truth.
TEST(DecoderStillTruth, PAL_MonoBandLimit) {
  const auto d = decode_stills(kClean, "mono");
  ASSERT_TRUE(d.has_value());
  const auto truth = d->truth.frame(kLumaZonePlateFrame);
  const auto [decoded, mask] = on_truth_grid(*d, kLumaZonePlateFrame);
  std::vector<double> difference(truth.y.size());
  for (size_t n = 0; n < difference.size(); ++n) {
    difference[n] = decoded.y[n] - truth.y[n];
  }
  const auto& g = d->truth.geometry();
  const double above = orc_videosynth::band_rms_pct(
      difference, g.samples_per_line, g.rows, mask, 16, 5.5e6,
      g.sample_rate_hz / 2.0, g.sample_rate_hz);
  // 10-bit quantisation alone leaves about 0.03 % above 5.5 MHz.
  EXPECT_LT(above, 0.1);
  orc_videosynth::expect_metrics(
      {{"luma_zone_plate_above_5m5_rms_pct", above, Better::Lower,
        orc_videosynth::new_tolerance(above, 0.05, 0.01)}});
}

// Phase 1 acceptance (decoder stress): pal2d decodes the PM5544-style
// line-alternating strips grey, as F. Hendil (1970) predicts of a PAL
// decoder that averages V over the switch; transform2d is recorded beside
// it. Measured over the strips' interior, 4 pixels in from each side, as
// the decoded chroma's RMS over the truth's. Both decoders leave about
// 0.10: a residual of +/-0.02 against the strips' 0.23, alternating in sign
// line to line on some rows; a decoder that kept the alternation would
// read about 1.
constexpr double kGreyRatio = 0.15;

TEST(DecoderStillTruth, PAL_StressAlternatingBars) {
  std::vector<Metric> metrics;
  for (const char* decoder : {"pal2d", "transform2d"}) {
    const auto d = decode_stills(kClean, decoder);
    ASSERT_TRUE(d.has_value());
    const auto truth = d->truth.frame(kStressFrame);
    const auto [decoded, mask] = on_truth_grid(*d, kStressFrame);
    (void)mask;
    const size_t width = d->truth.geometry().samples_per_line;
    for (const auto& [name, x] :
         {std::pair<const char*, size_t>{"v_alternating", 26},
          {"u_alternating", 656}}) {
      const auto region =
          orc_videosynth::source_region(x + 4, 24, 30, 528, width);
      const double ratio =
          chroma_rms_pct(decoded, region) / chroma_rms_pct(truth, region);
      if (std::string(decoder) == "pal2d") {
        EXPECT_LT(ratio, kGreyRatio)
            << "pal2d does not decode the " << name << " strip grey";
      }
      metrics.push_back({std::string(decoder) + "_" + name + "_chroma_ratio",
                         ratio, Better::Lower,
                         orc_videosynth::new_tolerance(ratio, 0.05, 0.005)});
    }
  }
  orc_videosynth::expect_metrics(metrics);
}

#define ORC_STILL_TEST(DECODER)                       \
  TEST(DecoderStillTruth, PAL_##DECODER##_Clean) {    \
    run_still_case(kClean, #DECODER);                 \
  }                                                   \
  TEST(DecoderStillTruth, PAL_##DECODER##_Domesday) { \
    run_still_case(kNoisy, #DECODER);                 \
  }

// pal2d and transform2d are the baselines PAL Still must beat;
// transform3d reads neighbouring frames, so it is recorded for information.
ORC_STILL_TEST(pal2d)
ORC_STILL_TEST(transform2d)
ORC_STILL_TEST(transform3d)
ORC_STILL_TEST(palstill)

#undef ORC_STILL_TEST

}  // namespace orc_videosynth_test
