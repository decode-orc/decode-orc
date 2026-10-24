/*
 * File:        frame_content.cpp
 * Module:      orc-sdk-support
 * Purpose:     Measurements of what a frame shows: block signatures of each
 *              field, same-picture and colour-phase tests between frames,
 *              and the noise they are judged against
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/frame_content.h>
#include <orc/support/frame_line_util.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace orc {

namespace {

// Width of the box that nulls the subcarrier: one cycle at 4FSC.
constexpr size_t kBox = kGridLumaBox;

// Scale from a median absolute deviation to a normal's standard deviation.
constexpr double kMadToSigma = 1.4826;

// Active lines between those the chroma band is taken from.
constexpr size_t kChromaLineStep = 6;

// Lag at which a frame's chroma band repeats: one subcarrier cycle.
constexpr size_t kChromaCycle = 4;

// Within-frame noise: box-filtered luma against itself this many samples
// along, read at a percentile of the magnitude of the difference.  On noise
// the difference is a zero-mean normal, so its magnitude is half-normal and
// the 30th percentile is σ_d × Φ⁻¹(0.65).
constexpr size_t kAnchorLag = 8;
constexpr double kAnchorPercentile = 0.30;
constexpr double kHalfNormalPercentileToScale = 0.38532;
constexpr size_t kAnchorLineStep = 2;
constexpr int32_t kAnchorHistogramBins = 4096;

// The active picture of a field: samples [first_sample, end_sample) of field
// lines [first_line, end_line).
struct ActiveArea {
  size_t first_sample = 0;
  size_t end_sample = 0;
  size_t first_line = 0;
  size_t end_line = 0;
};

// Active picture from the system's nominal geometry, scaled to the line
// length actually stored.  Lines start below kFieldBlankingLines, which also
// keeps NTSC line 21 (closed captions) out of the comparison.
//   EBU Tech. 3280-E (PAL) and SMPTE 170M-2004 (NTSC) active line and
//   sample ranges, as cvbs_signal_constants.h records them.
ActiveArea active_area(VideoSystem system, size_t nominal_width) {
  const bool pal = system == VideoSystem::PAL;
  const double standard_width =
      pal ? kPalSamplesPerLineNominal
          : (system == VideoSystem::PAL_M ? kPalMSamplesPerLine
                                          : kNtscSamplesPerLine);
  const double scale = static_cast<double>(nominal_width) / standard_width;
  const int32_t start = pal ? kPalActiveVideoStart : kNtscActiveVideoStart;
  const int32_t end = pal ? kPalActiveVideoEnd : kNtscActiveVideoEnd;
  const int32_t last_frame_line =
      pal ? kPalLastActiveFrameLine : kNtscLastActiveFrameLine;

  ActiveArea area;
  area.first_sample = static_cast<size_t>(std::lround(start * scale));
  area.end_sample = static_cast<size_t>(std::lround(end * scale));
  area.first_line = kFieldBlankingLines;
  // Frame lines interleave the fields, so a field has half of them.  The
  // last one is dropped: in field 2 of PAL it is line 623, which carries
  // picture in its first half only (ITU-R BT.1700 Annex 1 Part B), and the
  // blanked second half varies from frame to frame.
  area.end_line = static_cast<size_t>(last_frame_line + 1) / 2 - 1;
  return area;
}

// Flat line index of field line |line| of |field|, or nullopt when it lies
// outside the frame.
std::optional<size_t> flat_line(size_t field, size_t line, size_t field1_lines,
                                size_t height) {
  const size_t flat = field == 0 ? line : field1_lines + line;
  if (field == 0 && line >= field1_lines) return std::nullopt;
  if (flat >= height) return std::nullopt;
  return flat;
}

double median_in_place(std::vector<double>& v) {
  const size_t mid = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid),
                   v.end());
  const double upper = v[mid];
  if (v.size() % 2) return upper;
  const double lower = *std::max_element(
      v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
  return 0.5 * (lower + upper);
}

// Robust spread, 1.4826 × MAD, of |v|.
double robust_spread(std::vector<double> v) {
  if (v.empty()) return 0.0;
  const double median = median_in_place(v);
  for (double& x : v) x = std::abs(x - median);
  return kMadToSigma * median_in_place(v);
}

// Blocks a row needs before its smooth change is fitted.
constexpr size_t kMinRowBlocks = 8;

// Spread of block levels in a row, in codes², below which only an offset is
// fitted: a flat row cannot tell a gain from an offset.
constexpr double kMinRowLevelVariance = 25.0;

// Passes of the row fit; each refits without the blocks further than
// kRowFitClip robust standard deviations from the last fit.
constexpr int kRowFitPasses = 3;
constexpr double kRowFitClip = 3.0;

// Blocks below this fraction of the white level above blanking carry too
// little picture to show a fade.
constexpr double kMinFadeLevel = 0.1;

// Lines of a block a short disturbance may cover and still be excused: an
// unflagged dropout spans one to three lines, while a real change of the
// picture (a caption's letters, a cursor) covers most of the block's lines.
constexpr size_t kMaxDisturbedLines = 3;

struct ResidualBlock {
  size_t block = 0;
  // Block difference less the row's fitted smooth change, in codes.
  double residual = 0.0;
  // Noise gain of the difference: both blocks' gains combined.
  double gain = 0.0;
  // The row's fitted smooth change: offset + slope × (level − blanking).
  double offset = 0.0;
  double slope = 0.0;
};

struct Residuals {
  std::vector<ResidualBlock> blocks;
  // Each bright block's difference relative to its level above blanking.
  std::vector<double> relative_change;
};

// Block differences b − a with each row's smooth change removed: a robust
// least-squares fit of offset + gain × (level − blanking) over the row's
// judged blocks.  Rows with fewer than kMinRowBlocks judged blocks are left
// out.  nullopt when the signatures are not complete.
std::optional<Residuals> smooth_change_residuals(const FieldSignature& a,
                                                 const FieldSignature& b) {
  if (a.mean.size() != kSignatureBlocks || b.mean.size() != kSignatureBlocks) {
    return std::nullopt;
  }
  Residuals out;
  const double blanking = 0.5 * (a.blanking + b.blanking);

  std::vector<size_t> row_blocks;
  std::vector<double> level;
  std::vector<double> difference;
  std::vector<double> deviation;
  std::vector<bool> used;
  for (size_t row = 0; row < kSignatureBlocksY; ++row) {
    row_blocks.clear();
    for (size_t col = 0; col < kSignatureBlocksX; ++col) {
      const size_t block = row * kSignatureBlocksX + col;
      if (a.judged(block) && b.judged(block)) row_blocks.push_back(block);
    }
    if (row_blocks.size() < kMinRowBlocks) continue;

    level.clear();
    difference.clear();
    for (size_t block : row_blocks) {
      level.push_back(0.5 * (a.mean[block] + b.mean[block]) - blanking);
      difference.push_back(b.mean[block] - a.mean[block]);
    }

    used.assign(row_blocks.size(), true);
    double offset = 0.0;
    double gain = 0.0;
    for (int pass = 0; pass < kRowFitPasses; ++pass) {
      double n = 0.0;
      double sx = 0.0;
      double sy = 0.0;
      double sxx = 0.0;
      double sxy = 0.0;
      for (size_t i = 0; i < row_blocks.size(); ++i) {
        if (!used[i]) continue;
        n += 1.0;
        sx += level[i];
        sy += difference[i];
        sxx += level[i] * level[i];
        sxy += level[i] * difference[i];
      }
      if (n < 2.0) break;
      const double mean_x = sx / n;
      const double mean_y = sy / n;
      const double variance = sxx / n - mean_x * mean_x;
      gain = variance > kMinRowLevelVariance
                 ? (sxy / n - mean_x * mean_y) / variance
                 : 0.0;
      offset = mean_y - gain * mean_x;

      deviation.clear();
      for (size_t i = 0; i < row_blocks.size(); ++i) {
        deviation.push_back(difference[i] - offset - gain * level[i]);
      }
      const double scale = robust_spread(deviation);
      if (scale <= 0.0) break;
      for (size_t i = 0; i < row_blocks.size(); ++i) {
        used[i] = std::abs(deviation[i]) <= kRowFitClip * scale;
      }
    }

    for (size_t i = 0; i < row_blocks.size(); ++i) {
      const size_t block = row_blocks[i];
      out.blocks.push_back(
          {block, difference[i] - offset - gain * level[i],
           std::hypot(a.noise_gain[block], b.noise_gain[block]), offset, gain});
    }
  }

  const double white_above_blanking = 0.5 * (a.white + b.white) - blanking;
  for (size_t block = 0; block < kSignatureBlocks; ++block) {
    if (!a.judged(block) || !b.judged(block)) continue;
    const double above = 0.5 * (a.mean[block] + b.mean[block]) - blanking;
    if (above < kMinFadeLevel * white_above_blanking) continue;
    out.relative_change.push_back((b.mean[block] - a.mean[block]) / above);
  }
  return out;
}

// True when block |r|'s residual comes from at most kMaxDisturbedLines of its
// lines: with them left out, the rest of the block is within |limit| times
// its noise.
bool short_disturbance(const FieldSignature& a, const FieldSignature& b,
                       const ResidualBlock& r, double noise, double limit) {
  const size_t row = r.block / kSignatureBlocksX;
  const size_t lines = a.row_lines.size() > row ? a.row_lines[row] : 0;
  if (lines < 2 * kMaxDisturbedLines || b.row_lines.size() <= row ||
      b.row_lines[row] != lines) {
    return false;
  }
  const double blanking = 0.5 * (a.blanking + b.blanking);
  std::vector<double> deviation(lines);
  for (size_t l = 0; l < lines; ++l) {
    const float la = a.line_mean[r.block * kMaxBlockLines + l];
    const float lb = b.line_mean[r.block * kMaxBlockLines + l];
    const double level = 0.5 * (la + lb) - blanking;
    deviation[l] = lb - la - r.offset - r.slope * level;
  }
  std::sort(deviation.begin(), deviation.end(),
            [](double x, double y) { return std::abs(x) < std::abs(y); });
  const size_t kept = lines - kMaxDisturbedLines;
  double sum = 0.0;
  for (size_t l = 0; l < kept; ++l) sum += deviation[l];
  const double residual = sum / static_cast<double>(kept);
  const double gain = r.gain * std::sqrt(static_cast<double>(lines) /
                                         static_cast<double>(kept));
  return std::abs(residual) <= limit * noise * gain;
}

}  // namespace

// ── Geometry ────────────────────────────────────────────────────────────────

size_t colour_frame_cadence(VideoSystem system) {
  return system == VideoSystem::NTSC ? 2 : 4;
}

PictureLevels standard_picture_levels(VideoSystem system) {
  if (system == VideoSystem::PAL) return {kPalBlanking, kPalWhite};
  return {kNtscBlanking, kNtscWhite};
}

// ── Field signature ─────────────────────────────────────────────────────────

std::array<FieldSignature, 2> build_field_signatures(
    const std::vector<StackSample>& frame, VideoSystem system,
    size_t nominal_width, size_t height,
    const std::vector<DropoutRun>& dropouts, PictureLevels levels) {
  const ActiveArea area = active_area(system, nominal_width);
  const size_t field1_lines = std::min(field1_line_count(system), height);
  const std::vector<uint8_t> flagged = dropout_mask(dropouts, frame.size());

  std::array<size_t, kSignatureBlocksX + 1> column_edge{};
  const size_t active_width = area.end_sample - area.first_sample;
  for (size_t i = 0; i <= kSignatureBlocksX; ++i) {
    column_edge[i] = area.first_sample + active_width * i / kSignatureBlocksX;
  }
  std::array<size_t, kSignatureBlocksY + 1> row_edge{};
  const size_t active_lines = area.end_line - area.first_line;
  for (size_t j = 0; j <= kSignatureBlocksY; ++j) {
    row_edge[j] = area.first_line + active_lines * j / kSignatureBlocksY;
  }

  std::array<FieldSignature, 2> fields;
  for (size_t field = 0; field < 2; ++field) {
    std::vector<double> sum(kSignatureBlocks, 0.0);
    std::vector<size_t> positions(kSignatureBlocks, 0);
    std::vector<size_t> flagged_samples(kSignatureBlocks, 0);
    std::vector<size_t> samples(kSignatureBlocks, 0);
    std::vector<size_t> lines_used(kSignatureBlocks, 0);
    FieldSignature& sig = fields[field];
    sig.line_mean.assign(kSignatureBlocks * kMaxBlockLines, 0.0F);
    sig.row_lines.assign(kSignatureBlocksY, 0);

    for (size_t row = 0; row < kSignatureBlocksY; ++row) {
      sig.row_lines[row] = static_cast<uint8_t>(
          std::min(row_edge[row + 1] - row_edge[row], kMaxBlockLines));
      for (size_t line = row_edge[row]; line < row_edge[row + 1]; ++line) {
        const size_t line_in_row = line - row_edge[row];
        const auto flat = flat_line(field, line, field1_lines, height);
        if (!flat) continue;
        const size_t start =
            frame_line_sample_offset(system, nominal_width, *flat);
        const size_t length =
            frame_line_sample_count(system, nominal_width, *flat);
        if (start + length > frame.size() || area.end_sample + kBox > length) {
          continue;
        }
        const StackSample* x = frame.data() + start;
        const uint8_t* bad = flagged.data() + start;

        for (size_t col = 0; col < kSignatureBlocksX; ++col) {
          const size_t block = row * kSignatureBlocksX + col;
          ++lines_used[block];
          double line_sum = 0.0;
          size_t line_positions = 0;
          for (size_t n = column_edge[col]; n < column_edge[col + 1]; ++n) {
            // A box position reads 3 samples past the block's own, so it
            // counts as flagged when any of them is.
            ++samples[block];
            if (bad[n] || bad[n + 1] || bad[n + 2] || bad[n + 3]) {
              ++flagged_samples[block];
              continue;
            }
            const int32_t box = x[n] + x[n + 1] + x[n + 2] + x[n + 3];
            sum[block] += box;
            ++positions[block];
            line_sum += box;
            ++line_positions;
          }
          if (line_in_row < kMaxBlockLines && line_positions > 0) {
            sig.line_mean[block * kMaxBlockLines + line_in_row] =
                static_cast<float>(line_sum / static_cast<double>(kBox) /
                                   static_cast<double>(line_positions));
          }
        }
      }
    }

    sig.blanking = static_cast<float>(levels.blanking);
    sig.white = static_cast<float>(levels.white);
    sig.mean.assign(kSignatureBlocks, 0.0F);
    sig.noise_gain.assign(kSignatureBlocks, 0.0F);
    sig.dropout_fraction.assign(kSignatureBlocks, 1.0F);
    for (size_t block = 0; block < kSignatureBlocks; ++block) {
      if (samples[block] > 0) {
        sig.dropout_fraction[block] =
            static_cast<float>(static_cast<double>(flagged_samples[block]) /
                               static_cast<double>(samples[block]));
      }
      if (positions[block] == 0) continue;
      const double mean = sum[block] / static_cast<double>(kBox) /
                          static_cast<double>(positions[block]);
      sig.mean[block] = static_cast<float>(mean);

      // A box position averages 4 raw samples and neighbours overlap, so for
      // white noise of σ = 1 a block of W positions a line over its lines
      // has variance (16W − 20) / (16 W² lines) — (1 − 1.25 / W) / positions.
      const double width =
          static_cast<double>(samples[block]) /
          static_cast<double>(std::max<size_t>(1, lines_used[block]));
      const double overlap = width > 1.25 ? 1.0 - 1.25 / width : 1.0;
      sig.noise_gain[block] = static_cast<float>(
          std::sqrt(overlap / static_cast<double>(positions[block])));
    }
  }
  return fields;
}

// ── Same picture ────────────────────────────────────────────────────────────

FieldComparison same_picture(const FieldSignature& a, const FieldSignature& b,
                             double sigma, const SamePictureLimits& limits) {
  FieldComparison result;
  const std::optional<Residuals> residuals = smooth_change_residuals(a, b);
  if (!residuals) return result;
  result.judged_blocks = residuals->blocks.size();
  if (static_cast<double>(result.judged_blocks) <
      kMinJudgedBlockFraction * static_cast<double>(kSignatureBlocks)) {
    return result;
  }

  std::vector<double> scaled;
  scaled.reserve(residuals->blocks.size());
  for (const auto& r : residuals->blocks) {
    scaled.push_back(r.residual / r.gain);
  }
  result.pair_sigma = robust_spread(scaled);

  const double reference = std::max(sigma, kMinPressingNoiseSigma);
  const double noise = std::max(result.pair_sigma, reference);
  for (const auto& r : residuals->blocks) {
    const double multiple = std::abs(r.residual) / (noise * r.gain);
    result.worst_block = std::max(result.worst_block, multiple);
    if (multiple <= limits.block) continue;
    if (short_disturbance(a, b, r, noise, limits.block)) {
      ++result.disturbed_blocks;
    } else {
      ++result.failed_blocks;
    }
  }

  std::vector<double> relative = residuals->relative_change;
  if (!relative.empty()) result.level_change = median_in_place(relative);

  const bool same = result.pair_sigma <= limits.noise_ratio * reference &&
                    result.failed_blocks == 0 &&
                    std::abs(result.level_change) <= limits.level_change;
  result.match = same ? PictureMatch::Same : PictureMatch::Different;
  return result;
}

// ── Colour phase ────────────────────────────────────────────────────────────

ChromaSignature build_chroma_signature(
    const std::vector<StackSample>& frame, VideoSystem system,
    size_t nominal_width, size_t height,
    const std::vector<DropoutRun>& dropouts) {
  ChromaSignature sig;
  const ActiveArea area = active_area(system, nominal_width);
  const size_t field1_lines = std::min(field1_line_count(system), height);
  const std::vector<uint8_t> flagged = dropout_mask(dropouts, frame.size());

  // Lines on which the subcarrier is inverted against the line being read.
  //   PAL: 283.75 cycles a line, so two lines on is 567.5 cycles, and the V
  //   switch has turned twice.  NTSC: 227.5 cycles, so the next line.
  const size_t comb = system == VideoSystem::NTSC ? 1 : 2;
  // The band-pass reads two samples either side.
  const size_t first = std::max<size_t>(area.first_sample, 2);
  if (area.end_sample <= first + kChromaCycle) return sig;
  sig.positions_per_line = area.end_sample - first;

  for (size_t field = 0; field < 2; ++field) {
    for (size_t line = area.first_line; line + comb < area.end_line;
         line += kChromaLineStep) {
      const auto upper = flat_line(field, line, field1_lines, height);
      const auto lower = flat_line(field, line + comb, field1_lines, height);
      if (!upper || !lower) continue;
      const size_t start_a =
          frame_line_sample_offset(system, nominal_width, *upper);
      const size_t start_b =
          frame_line_sample_offset(system, nominal_width, *lower);
      const size_t length_b =
          frame_line_sample_count(system, nominal_width, *lower);
      if (start_b + length_b > frame.size() || area.end_sample + 2 > length_b) {
        continue;
      }
      const StackSample* xa = frame.data() + start_a;
      const StackSample* xb = frame.data() + start_b;
      const uint8_t* bad_a = flagged.data() + start_a;
      const uint8_t* bad_b = flagged.data() + start_b;

      for (size_t n = first; n < area.end_sample; ++n) {
        const double ha = (2.0 * xa[n] - xa[n - 2] - xa[n + 2]) / 4.0;
        const double hb = (2.0 * xb[n] - xb[n - 2] - xb[n + 2]) / 4.0;
        bool ok = true;
        for (size_t k = n - 2; k <= n + 2; ++k) {
          ok = ok && !bad_a[k] && !bad_b[k];
        }
        sig.band.push_back(static_cast<float>(0.5 * (ha - hb)));
        sig.valid.push_back(ok ? 1 : 0);
      }
    }
  }

  // Self-correlation one subcarrier cycle along each line.
  double xy = 0.0;
  double xx = 0.0;
  double yy = 0.0;
  const size_t per_line = sig.positions_per_line;
  for (size_t base = 0; base + per_line <= sig.band.size(); base += per_line) {
    for (size_t n = base; n + kChromaCycle < base + per_line; ++n) {
      if (!sig.valid[n] || !sig.valid[n + kChromaCycle]) continue;
      const double p = sig.band[n];
      const double q = sig.band[n + kChromaCycle];
      xy += p * q;
      xx += p * p;
      yy += q * q;
    }
  }
  if (xx > 0.0 && yy > 0.0) sig.self_correlation = xy / std::sqrt(xx * yy);
  return sig;
}

ColourPhaseMeasure colour_phase_relation(const ChromaSignature& a,
                                         const ChromaSignature& b) {
  ColourPhaseMeasure result;
  if (a.band.size() != b.band.size() || a.band.empty()) return result;

  double xy = 0.0;
  double xx = 0.0;
  double yy = 0.0;
  for (size_t n = 0; n < a.band.size(); ++n) {
    if (!a.valid[n] || !b.valid[n]) continue;
    xy += static_cast<double>(a.band[n]) * b.band[n];
    xx += static_cast<double>(a.band[n]) * a.band[n];
    yy += static_cast<double>(b.band[n]) * b.band[n];
  }
  if (xx <= 0.0 || yy <= 0.0) return result;
  result.correlation = xy / std::sqrt(xx * yy);
  result.expected = std::sqrt(std::max(0.0, a.self_correlation) *
                              std::max(0.0, b.self_correlation));
  if (result.expected < kMinChromaCorrelation) return result;

  const double fraction = result.correlation / result.expected;
  if (fraction >= kPhaseAgreementFraction) {
    result.relation = ColourPhaseRelation::Same;
  } else if (fraction <= -kPhaseAgreementFraction) {
    result.relation = ColourPhaseRelation::Opposite;
  } else if (std::abs(fraction) <= kPhaseQuadratureFraction) {
    result.relation = ColourPhaseRelation::Quadrature;
  }
  return result;
}

// ── Duplicate reads ─────────────────────────────────────────────────────────

bool is_duplicate_read(const std::vector<StackSample>& a,
                       const std::vector<StackSample>& b) {
  return !a.empty() && a == b;
}

uint64_t frame_sample_hash(const std::vector<StackSample>& frame) {
  constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
  constexpr uint64_t kFnvPrime = 1099511628211ULL;
  constexpr size_t kLanes = 4;
  constexpr size_t kWordSamples = sizeof(uint64_t) / sizeof(StackSample);
  // FNV-1a over 64-bit words, in independent lanes so that the multiplies
  // do not wait on each other.  Each step is a bijection of the lane, so a
  // difference anywhere stays to the end.
  std::array<uint64_t, kLanes> lane{kFnvOffset, kFnvOffset + 1, kFnvOffset + 2,
                                    kFnvOffset + 3};
  const size_t block = kLanes * kWordSamples;
  const size_t whole = frame.size() / block * block;
  for (size_t i = 0; i < whole; i += block) {
    for (size_t l = 0; l < kLanes; ++l) {
      uint64_t word = 0;
      std::memcpy(&word, &frame[i + l * kWordSamples], sizeof(word));
      lane[l] = (lane[l] ^ word) * kFnvPrime;
    }
  }
  for (size_t i = whole; i < frame.size(); ++i) {
    lane[0] = (lane[0] ^ static_cast<uint16_t>(frame[i])) * kFnvPrime;
  }
  uint64_t hash = kFnvOffset;
  for (uint64_t value : lane) hash = (hash ^ value) * kFnvPrime;
  return (hash ^ frame.size()) * kFnvPrime;
}

// ── Noise ───────────────────────────────────────────────────────────────────

void BlockNoiseMeter::add(const FieldSignature& a, const FieldSignature& b) {
  const std::optional<Residuals> residuals = smooth_change_residuals(a, b);
  if (!residuals) return;
  for (const auto& r : residuals->blocks) {
    scaled_.push_back(r.residual / r.gain);
  }
}

std::optional<double> BlockNoiseMeter::sigma() const {
  if (scaled_.size() < kMinNoiseBlocks) return std::nullopt;
  return robust_spread(scaled_);
}

std::optional<double> within_frame_noise(
    const std::vector<StackSample>& frame, VideoSystem system,
    size_t nominal_width, size_t height,
    const std::vector<DropoutRun>& dropouts) {
  const ActiveArea area = active_area(system, nominal_width);
  const size_t field1_lines = std::min(field1_line_count(system), height);
  const std::vector<uint8_t> flagged = dropout_mask(dropouts, frame.size());

  // |d| for d the difference of two 4-sample sums, which is an integer.
  std::vector<uint64_t> histogram(kAnchorHistogramBins, 0);
  uint64_t count = 0;
  for (size_t field = 0; field < 2; ++field) {
    for (size_t line = area.first_line; line < area.end_line;
         line += kAnchorLineStep) {
      const auto flat = flat_line(field, line, field1_lines, height);
      if (!flat) continue;
      const size_t start =
          frame_line_sample_offset(system, nominal_width, *flat);
      const size_t length =
          frame_line_sample_count(system, nominal_width, *flat);
      if (start + length > frame.size() ||
          area.end_sample + kAnchorLag + kBox > length) {
        continue;
      }
      const StackSample* x = frame.data() + start;
      const uint8_t* bad = flagged.data() + start;
      for (size_t n = area.first_sample; n < area.end_sample; ++n) {
        bool ok = true;
        int32_t d = 0;
        for (size_t k = 0; k < kBox; ++k) {
          ok = ok && !bad[n + k] && !bad[n + kAnchorLag + k];
          d += x[n + k] - x[n + kAnchorLag + k];
        }
        if (!ok) continue;
        ++histogram[static_cast<size_t>(
            std::min(std::abs(d), kAnchorHistogramBins - 1))];
        ++count;
      }
    }
  }
  if (count == 0) return std::nullopt;

  // Percentile of |d|, with each integer bin k spread over [k − ½, k + ½)
  // (bin 0 over [0, ½)) so that the integer steps do not quantise it.
  const double target = kAnchorPercentile * static_cast<double>(count);
  double seen = 0.0;
  double percentile = 0.0;
  for (int32_t k = 0; k < kAnchorHistogramBins; ++k) {
    const auto in_bin = static_cast<double>(histogram[static_cast<size_t>(k)]);
    if (seen + in_bin >= target && in_bin > 0.0) {
      const double low = k == 0 ? 0.0 : k - 0.5;
      const double width = k == 0 ? 0.5 : 1.0;
      percentile = low + width * (target - seen) / in_bin;
      break;
    }
    seen += in_bin;
  }

  // d sums 8 samples' independent noise: σ_d = √8 σ.
  return percentile / kHalfNormalPercentileToScale / std::sqrt(8.0);
}

}  // namespace orc
