/*
 * File:        still_truth_test.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     The PAL and SECAM chroma decoders scored against the source
 *              picture: each still of projects/still_truth/pal_stills.yaml
 *              and secam_stills.yaml decoded and compared with videosynth's
 *              picture truth, and the SECAM colour-frame observer against
 *              the truth's line parity
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <decoded_comparison.h>
#include <gtest/gtest.h>
#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/support/observers/colour_frame_phase_observer.h>
#include <orc/support/pal_line_locked_resample.h>
#include <picture_truth.h>
#include <still_metrics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <variant>
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

// |tag| keeps the decode's file apart from another test's decode of the
// same case with the same decoder, so the two can run at once.
std::optional<Decode> decode_stills(const CaseFile& file,
                                    const std::string& decoder,
                                    const std::string& tag = "") {
  auto truth = read_truth(file);
  if (!truth) return std::nullopt;
  auto decoded = decode_case(file, decoder, kUnpadded,
                             file.case_name + "_" + decoder + tag, kFrames);
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

// The 0H offset of flat PAL frame line |line| (0-based) on the 4fsc lattice:
// the stored line carries truth sample j at first_sample + j + d samples
// from its first sample (videosynth docs/reference/output.md, ".truth").
double lattice_delay(size_t line) {
  const auto field1 = static_cast<size_t>(orc::kPalField1Lines);
  const int field = line < field1 ? 1 : 2;
  const auto row = static_cast<int32_t>(line < field1 ? line : line - field1);
  return orc::pal_line_locked::row_0h_offset(field, row);
}

// Moving the truth onto the lattice: a Kaiser-windowed sinc over
// 2 × kDelayHalfTaps truth samples. The truth is float and band-limited like
// the signal (6 MHz flat on PAL against an 8.87 MHz Nyquist), so the kernel
// adds well under 1 LSB and the stored samples' rounding is the only other
// error. A sample is scored only where every tap it reads carries picture.
constexpr int kDelayHalfTaps = 24;
constexpr double kDelayKaiserBeta = 9.0;

double bessel_i0(double x) {
  double term = 1.0;
  double sum = 1.0;
  for (int k = 1; k < 64; ++k) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
    if (term < sum * 1e-17) break;
  }
  return sum;
}

struct DelayedRow {
  std::vector<double> y, u, v;
  std::vector<uint8_t> valid;
};

// Truth row |row| of |picture| evaluated |delay| samples earlier, so that
// entry x is the picture at stored sample first_sample + x of a line whose
// 0H is |delay| samples late. |mask| is the truth's picture mask.
DelayedRow delayed_row(const ComponentPicture& picture,
                       const std::vector<uint8_t>& mask, size_t row,
                       double delay) {
  const size_t width = picture.width;
  DelayedRow out{
      std::vector<double>(width, 0.0), std::vector<double>(width, 0.0),
      std::vector<double>(width, 0.0), std::vector<uint8_t>(width, 0)};
  const double whole = std::floor(delay);
  const double frac = delay - whole;
  if (frac == 0.0) {
    for (size_t x = 0; x < width; ++x) {
      const long from = static_cast<long>(x) - static_cast<long>(whole);
      if (from < 0 || static_cast<size_t>(from) >= width) continue;
      const size_t n = picture.index(static_cast<size_t>(from), row);
      out.y[x] = picture.y[n];
      out.u[x] = picture.u[n];
      out.v[x] = picture.v[n];
      out.valid[x] = mask[n];
    }
    return out;
  }
  // Position x - delay = (x - whole - 1) + (1 - frac): taps k = -H+1..H
  // around base i = x - whole - 1, at distance k - (1 - frac).
  constexpr int kHalf = kDelayHalfTaps;
  std::array<double, 2 * kHalf> h{};
  double sum = 0.0;
  const double i0_beta = bessel_i0(kDelayKaiserBeta);
  for (int k = -kHalf + 1; k <= kHalf; ++k) {
    const double t = static_cast<double>(k) - (1.0 - frac);
    const double r = t / static_cast<double>(kHalf);
    const double window =
        std::abs(r) >= 1.0
            ? 0.0
            : bessel_i0(kDelayKaiserBeta * std::sqrt(1.0 - (r * r))) / i0_beta;
    constexpr double kPi = 3.14159265358979323846;
    const double sinc = std::sin(kPi * t) / (kPi * t);
    h[static_cast<size_t>(k + kHalf - 1)] = sinc * window;
    sum += sinc * window;
  }
  for (double& tap : h) tap /= sum;
  for (size_t x = 0; x < width; ++x) {
    const long base = static_cast<long>(x) - static_cast<long>(whole) - 1;
    if (base - kHalf + 1 < 0 || base + kHalf >= static_cast<long>(width)) {
      continue;
    }
    bool valid = true;
    double y = 0.0;
    double u = 0.0;
    double v = 0.0;
    for (int k = -kHalf + 1; k <= kHalf; ++k) {
      const size_t n = picture.index(static_cast<size_t>(base + k), row);
      valid = valid && mask[n] != 0;
      const double w = h[static_cast<size_t>(k + kHalf - 1)];
      y += w * picture.y[n];
      u += w * picture.u[n];
      v += w * picture.v[n];
    }
    out.y[x] = y;
    out.u[x] = u;
    out.v[x] = v;
    out.valid[x] = valid ? 1 : 0;
  }
  return out;
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
    const size_t flat = flat_line(g, row);
    const auto* line = vfr->get_line(id, flat);
    if (line == nullptr) return std::nullopt;
    // Stored sample first_sample + x + d carries truth sample x; d is under
    // 2 samples, well inside the margin, so the whole-sample shift will do.
    const auto shift = static_cast<size_t>(std::lround(lattice_delay(flat)));
    for (size_t x = 0; x < g.samples_per_line; ++x) {
      const auto s = line[static_cast<size_t>(g.first_sample) + x + shift];
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

// Every still of |file| decoded with |decoder| and scored against the
// truth, into |metrics|.
void score_stills(const CaseFile& file, const std::string& decoder,
                  std::vector<Metric>& metrics) {
  const auto d = decode_stills(file, decoder);
  ASSERT_TRUE(d.has_value());
  const auto o = options(d->truth);
  for (const Still& still : kStills) {
    const ComponentPicture truth = d->truth.frame(still.frame);
    auto [decoded, mask] = on_truth_grid(*d, still.frame);
    if (still.frame == kStressFrame) mask = without_line_sign_regions(mask);
    const auto clipped = clip_mask(kClean, d->truth, still.frame);
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
  add_edge(metrics, "bars_edge", *d, kBarsFrame,
           orc_videosynth::source_region(348, 100, 24, 376, width),
           o.sample_rate_hz);
  const auto castellation = castellation_edge(d->truth.frame(kStressFrame));
  ASSERT_TRUE(castellation.has_value()) << "no castellation edge on the card";
  add_edge(metrics, "castellation_edge", *d, kStressFrame, *castellation,
           o.sample_rate_hz);
}

// Every still of |file| decoded with |decoder| and scored against the
// truth.
void run_still_case(const CaseFile& file, const std::string& decoder) {
  std::vector<Metric> metrics;
  score_stills(file, decoder, metrics);
  if (::testing::Test::HasFatalFailure()) return;
  orc_videosynth::expect_metrics(metrics);
}

}  // namespace

// The truth: re-encoded with videosynth's own carrier, the
// truth is the Y/C rendering to within 1 LSB. Luma directly; chroma as
// U sin(phi) + s V cos(phi) on the 4fsc grid, each line's carrier phase and
// V-switch sign s fitted, since the truth does not state them. The Y/C
// rendering is read through cvbs_source, whose frames are flat: lines in
// transmission order on the 4fsc lattice, each line's 0H d samples later
// than field 1 row 0's. The truth is 0H-relative, so each row is moved onto
// its line's lattice positions first (delayed_row), and only the samples
// whose interpolation stays inside the picture are scored.
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
      const DelayedRow at =
          delayed_row(picture, mask, row, lattice_delay(line));
      std::vector<size_t> xs;
      for (size_t x = 0; x < g.samples_per_line; ++x) {
        if (at.valid[x] != 0) xs.push_back(x);
      }
      double chroma_peak = 0.0;
      for (const size_t x : xs) {
        const double expected = params->black_level + (at.y[x] * luma_codes);
        const double coded =
            std::clamp(expected, static_cast<double>(orc::kPalSyncTip),
                       static_cast<double>(orc::kPalPeak));
        if (coded != expected) ++luma_clipped;
        worst_luma = std::max(
            worst_luma,
            std::abs(luma[static_cast<size_t>(g.first_sample) + x] - coded));
        chroma_peak = std::max(chroma_peak, std::hypot(at.u[x], at.v[x]));
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
          const double u = at.u[x];
          const double v = s * at.v[x];
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

// Where |plane|, averaged over one subcarrier cycle (4 samples), crosses half
// way between its levels either side of |region| on truth row |row|, in
// truth samples; empty when it does not cross. The average takes out
// subcarrier a decode leaves in its luma, which sits on the lattice, not on
// the picture; the truth and the decode take the same average, so its
// 1.5-sample delay cancels.
constexpr size_t kCarrierCycle = 4;

std::optional<double> plane_crossing(const ComponentPicture& picture,
                                     orc_videosynth::Plane plane,
                                     const orc_videosynth::Region& region,
                                     size_t row) {
  if (region.width <= kCarrierCycle + 1) return std::nullopt;
  const std::vector<double>& p = plane == orc_videosynth::Plane::Y ? picture.y
                                 : plane == orc_videosynth::Plane::U
                                     ? picture.u
                                     : picture.v;
  std::vector<double> smooth(region.width - kCarrierCycle + 1, 0.0);
  for (size_t i = 0; i < smooth.size(); ++i) {
    for (size_t k = 0; k < kCarrierCycle; ++k) {
      smooth[i] += p[picture.index(region.x + i + k, row)] /
                   static_cast<double>(kCarrierCycle);
    }
  }
  const double mid = (smooth.front() + smooth.back()) / 2.0;
  for (size_t i = 0; i + 1 < smooth.size(); ++i) {
    const double a = smooth[i] - mid;
    const double b = smooth[i + 1] - mid;
    if ((a < 0.0) != (b < 0.0) && a != b) {
      return static_cast<double>(region.x + i) + (a / (a - b));
    }
  }
  return std::nullopt;
}

// The drift of |plane|'s edge in |region| of frame |frame|: the mean edge
// position over the last kDriftRows rows less that over the first, each
// against the truth's own edge.
constexpr size_t kDriftRows = 64;

std::optional<double> edge_drift(const Decode& d, size_t frame,
                                 orc_videosynth::Plane plane,
                                 const orc_videosynth::Region& region) {
  const auto truth = d.truth.frame(frame);
  const auto [decoded, mask] = on_truth_grid(d, frame);
  (void)mask;
  std::vector<double> offsets;
  for (size_t row = region.y; row < region.y + region.height; ++row) {
    const auto at = plane_crossing(decoded, plane, region, row);
    const auto expected = plane_crossing(truth, plane, region, row);
    if (!at || !expected) return std::nullopt;
    offsets.push_back(*at - *expected);
  }
  if (offsets.size() < 2 * kDriftRows) return std::nullopt;
  double top = 0.0;
  double bottom = 0.0;
  for (size_t i = 0; i < kDriftRows; ++i) {
    top += offsets[i] / kDriftRows;
    bottom += offsets[offsets.size() - 1 - i] / kDriftRows;
  }
  return bottom - top;
}

// The lattice shear: the green-to-magenta edge of the 75 % bars, a vertical
// edge in the source, sits in one column of the decode from the top of the
// picture to the bottom. PAL on the 4fsc lattice moves 0H 4/625 sample a row,
// up to 2 samples through a field; Video Sink takes that out of the decoded
// picture (PalOutputRaster). The edge is measured in V, which no decoder
// leaves subcarrier in, and in Y. mono passes the composite through as its
// luma, subcarrier and all, so it is measured on the Y/C rendering.
//
// pal2d's luma is recorded but not held to the bound: at this saturated
// colour edge its luma separation leaves a step whose position depends on
// the subcarrier phase under the edge, which changes along the edge (0.31
// sample top to bottom; its V edge, like every other decoder's, does not
// move).
constexpr double kMaxEdgeDrift = 0.3;  // samples

TEST(DecoderStillTruth, PAL_EdgeDrift) {
  std::vector<Metric> metrics;
  for (const char* decoder :
       {"pal2d", "transform2d", "transform3d", "palstill", "mono"}) {
    const bool mono = std::string(decoder) == "mono";
    const auto d = decode_stills(mono ? kYc : kClean, decoder, "_drift");
    ASSERT_TRUE(d.has_value()) << decoder;
    const auto region = orc_videosynth::source_region(
        348, 100, 24, 376, d->truth.geometry().samples_per_line);
    for (const auto& [plane, name] :
         {std::pair{orc_videosynth::Plane::Y, "y"},
          std::pair{orc_videosynth::Plane::V, "v"}}) {
      if (mono && plane != orc_videosynth::Plane::Y) continue;
      const std::string label = std::string(decoder) + "_" + name;
      const auto drift = edge_drift(*d, kBarsFrame, plane, region);
      ASSERT_TRUE(drift.has_value()) << label << ": no edge on every row";
      const bool gated = !(std::string(decoder) == "pal2d" &&
                           plane == orc_videosynth::Plane::Y);
      if (gated) {
        EXPECT_LE(std::abs(*drift), kMaxEdgeDrift)
            << label << ": the bars edge leans " << *drift
            << " samples top to bottom";
      }
      metrics.push_back(
          {label + "_bars_edge_drift_samples", std::abs(*drift), Better::Lower,
           orc_videosynth::new_tolerance(std::abs(*drift), 0.0, 0.05)});
    }
  }
  orc_videosynth::expect_metrics(metrics);
}

// The resampling: with the band-limited mapping, a
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

// Decoder stress: pal2d decodes the PM5544-style
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

// ── SECAM ───────────────────────────────────────────────────────────────────

namespace {

// projects/still_truth/secam_stills.yaml: six pictures of two frames each,
// one whole sequence, after a different picture; frames 0 and 13 are the
// pictures either side. Same 14 frames as the PAL stills.
struct SecamStill {
  size_t first_frame;
  const char* name;
};
constexpr std::array<SecamStill, 6> kSecamStills = {
    {{1, "bars"},
     {3, "chroma_zone_plate"},
     {5, "test_card_f"},
     {7, "tartan"},
     {9, "photo_butterfly"},
     {11, "green_magenta_edges"}}};

const CaseFile kSecamNoisy{"secam_stills", "videosynth_secam_stills",
                           orc::VideoSystem::SECAM};
const CaseFile kSecamClean{"secam_stills_clean",
                           "videosynth_secam_stills_clean",
                           orc::VideoSystem::SECAM};

// The error of one colour-difference plane, in percent, over the rows that
// transmit it and over the rows the decoder must rebuild it on.
struct SecamPlaneError {
  double transmitted_rms_pct = 0.0;
  double reconstructed_rms_pct = 0.0;
};

struct SecamRowErrors {
  SecamPlaneError u;  // D'B
  SecamPlaneError v;  // D'R
};

// Each row transmits D'R or D'B by the truth's secam_frame line parity
// (videosynth docs/reference/output.md, "Line rule"); the other component
// comes from the decoder's neighbours, vertical or temporal. Measured over
// the samples |mask| keeps, less the active window's edges.
SecamRowErrors secam_row_errors(const ComponentPicture& decoded,
                                const ComponentPicture& truth,
                                const std::vector<uint8_t>& mask,
                                const TruthFile& truth_file, size_t frame,
                                size_t edge_exclusion) {
  // Energy and count: U transmitted, U rebuilt, V transmitted, V rebuilt.
  std::array<double, 4> energy{};
  std::array<size_t, 4> count{};
  for (size_t row = 0; row < truth.height; ++row) {
    const auto dr = truth_file.row_transmits_dr(frame, row);
    if (!dr) continue;
    const size_t u_slot = *dr ? 1 : 0;
    const size_t v_slot = *dr ? 2 : 3;
    for (size_t x = edge_exclusion; x + edge_exclusion < truth.width; ++x) {
      const size_t n = truth.index(x, row);
      if (mask[n] == 0) continue;
      const double du = decoded.u[n] - truth.u[n];
      const double dv = decoded.v[n] - truth.v[n];
      energy[u_slot] += du * du;
      energy[v_slot] += dv * dv;
      ++count[u_slot];
      ++count[v_slot];
    }
  }
  auto rms = [&](size_t slot) {
    return count[slot] == 0
               ? 0.0
               : std::sqrt(energy[slot] / static_cast<double>(count[slot])) *
                     100.0;
  };
  return {{rms(0), rms(1)}, {rms(2), rms(3)}};
}

// Every SECAM still of |file| decoded by the secam decoder with |extra|,
// scored against the truth on both of its frames: "first", after a
// different picture, so the missing component can only come from the
// field's own neighbouring lines, and "held", after the same picture, where
// the temporal reconstruction may take it from the frame before.
void score_secam_stills(const CaseFile& file, const Parameters& extra,
                        const std::string& tag, std::vector<Metric>& metrics) {
  auto truth = read_truth(file);
  ASSERT_TRUE(truth.has_value());
  ASSERT_EQ(truth->geometry().system, orc::VideoSystem::SECAM);
  Parameters parameters = extra;
  parameters["output_padding"] = 1;
  auto decoded = decode_case(file, "secam", parameters,
                             file.case_name + "_secam" + tag, kFrames);
  ASSERT_TRUE(decoded.has_value());
  const Decode d{std::move(*decoded), std::move(*truth)};
  const auto o = options(d.truth);
  for (const SecamStill& still : kSecamStills) {
    for (const auto& [which, frame] :
         {std::pair<const char*, size_t>{"first", still.first_frame},
          {"held", still.first_frame + 1}}) {
      const ComponentPicture truth_frame = d.truth.frame(frame);
      const auto [picture, mask] = on_truth_grid(d, frame);
      const auto m =
          orc_videosynth::measure_still(picture, truth_frame, mask, o);
      ASSERT_GT(m.samples, 0U) << still.name;
      const auto rows = secam_row_errors(picture, truth_frame, mask, d.truth,
                                         frame, o.edge_exclusion);
      const std::string p = std::string(still.name) + "_" + which + "_";
      add(metrics, p + "y_rms_pct", m.y_rms_pct, Better::Lower, 0.05, 0.01);
      add(metrics, p + "u_transmitted_rms_pct", rows.u.transmitted_rms_pct,
          Better::Lower, 0.05, 0.01);
      add(metrics, p + "u_reconstructed_rms_pct", rows.u.reconstructed_rms_pct,
          Better::Lower, 0.05, 0.01);
      add(metrics, p + "v_transmitted_rms_pct", rows.v.transmitted_rms_pct,
          Better::Lower, 0.05, 0.01);
      add(metrics, p + "v_reconstructed_rms_pct", rows.v.reconstructed_rms_pct,
          Better::Lower, 0.05, 0.01);
      add(metrics, p + "rgb_psnr_db", m.rgb_psnr_db, Better::Higher, 0.0, 0.1);
      add(metrics, p + "delta_e_mean", m.delta_e_mean, Better::Lower, 0.05,
          0.02);
      add(metrics, p + "delta_e_p95", m.delta_e_p95, Better::Lower, 0.05, 0.05);
      if (m.chroma_samples > 0) {
        add(metrics, p + "chroma_gain_error_pct",
            std::abs(m.chroma_gain - 1.0) * 100.0, Better::Lower, 0.05, 0.05);
      }
      if (m.cross_colour_samples > 0) {
        add(metrics, p + "cross_colour_rms_pct", m.cross_colour_rms_pct,
            Better::Lower, 0.05, 0.01);
      }
    }
  }
}

// Known deviations of the secam decoder, held in the baselines:
//  - On the 75 % bars V comes out 5 % low on cyan and 3.5 % low on
//    magenta, on transmitted and rebuilt rows alike, and is right on the
//    other bars: an error in the decoder's frequency-to-colour difference
//    mapping at those deviations.
constexpr const char* kSecamDecoderNote =
    "Known deviation: V is 3-5 % low on the cyan and magenta bars.";

void run_secam_still_case(const CaseFile& file, const Parameters& extra,
                          const std::string& tag) {
  orc_videosynth::note(kSecamDecoderNote);
  std::vector<Metric> metrics;
  score_secam_stills(file, extra, tag, metrics);
  if (::testing::Test::HasFatalFailure()) return;
  orc_videosynth::expect_metrics(metrics);
}

const Parameters kNoTemporal{{"secam_temporal_chroma", false}};
const Parameters kCrossColour{{"secam_cross_colour_suppression", true}};

}  // namespace

// The secam decoder with its defaults: temporal reconstruction on,
// cross-colour suppression off.
TEST(DecoderStillTruth, SECAM_secam_Clean) {
  run_secam_still_case(kSecamClean, {}, "");
}

TEST(DecoderStillTruth, SECAM_secam_Noisy) {
  run_secam_still_case(kSecamNoisy, {}, "");
}

TEST(DecoderStillTruth, SECAM_secam_NoTemporal_Clean) {
  run_secam_still_case(kSecamClean, kNoTemporal, "_no_temporal");
}

TEST(DecoderStillTruth, SECAM_secam_CrossColour_Clean) {
  run_secam_still_case(kSecamClean, kCrossColour, "_cross_colour");
}

// The colour-frame observer's D'R/D'B identification, frame by frame,
// against the truth's line parity: sequence frame 1 is the frame whose
// odd-numbered lines carry D'R. Both fields of a frame carry its index.
TEST(DecoderStillTruth, SECAM_ObserverMatchesTruth) {
  for (const CaseFile& file : {kSecamClean, kSecamNoisy}) {
    const auto truth = read_truth(file);
    ASSERT_TRUE(truth.has_value());
    const auto vfr = orc_videosynth::as_vfr(orc_videosynth::open_case(file));
    ASSERT_NE(vfr, nullptr);
    ASSERT_EQ(vfr->frame_count(), kFrames);
    orc::ColourFramePhaseObserver observer;
    orc::ObservationContext ctx;
    for (orc::FrameID f = 0; f < kFrames; ++f) {
      observer.process_frame(*vfr, f, ctx);
      const auto expected = truth->secam_sequence_frame(f);
      ASSERT_TRUE(expected.has_value());
      for (const orc::FrameID field : {f * 2, (f * 2) + 1}) {
        const auto value = ctx.get(orc::FieldID(field), "colour_frame_phase",
                                   "colour_frame_index");
        ASSERT_TRUE(value.has_value() &&
                    std::holds_alternative<int32_t>(*value))
            << file.case_name << " frame " << f;
        EXPECT_EQ(std::get<int32_t>(*value), *expected)
            << file.case_name << " frame " << f << " field " << field % 2 + 1;
      }
    }
  }
}

}  // namespace orc_videosynth_test
