/*
 * File:        vbi_level_mapper.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Maps card-capture sample levels into the CVBS amplitude domain
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_LEVEL_MAPPER_H
#define ORC_VBI_LEVEL_MAPPER_H

#include <cstdint>
#include <optional>
#include <vector>

#include "vbi_cri_correlator.h"
#include "vbi_cri_template.h"
#include "vbi_line_reader.h"
#include "vbi_output_frame.h"
#include "vbi_source_format.h"
#include "vbi_teletext_service.h"

namespace orc {

// How the mapper decides which levels to apply to a line.
enum class VBILevelMode {
  // Each line is normalised by its own estimate.  Follows fast gain changes
  // but carries per-line estimation noise into the output.
  kPerLine,

  // Levels are the median over the lines of a stored frame, with a line's own
  // estimate used only where it deviates significantly from that median.
  kRolling,

  // Configured levels applied to every line; nothing is estimated.
  kFixed,
};

// The levels applied to one line record, in source-domain counts.
struct VBILineLevels {
  double logic0 = 0.0;
  double logic1 = 0.0;

  // False when no level reference could be established, in which case the line
  // carries no data service and is emitted as blanking.
  bool usable = false;

  double amplitude() const { return logic1 - logic0; }
};

// Level estimation and mapping policy.
struct VBILevelMapperConfig {
  VBILevelMode mode = VBILevelMode::kPerLine;

  // Quantiles of a record's payload samples read as its logic 0 and logic 1.
  //
  // The payload is where the data levels are.  Its bits are between a third
  // and two thirds ones on 98 % of the packets recovered from the NABTS corpus
  // and at least a sixth ones on 99,8 %, so both levels are held by runs long
  // enough to settle on every line, and the tenth of the samples at either end
  // sits on those runs while staying clear of a noisy tape's extremes and the
  // overshoot a band-limited channel adds to its edges.  Read at the twentieth
  // instead, the levels follow that noise: on a bt8x8 dump and two card dumps
  // checked against other transfers of the same broadcast, the tenth leaves
  // fewer record bytes wrong.  Nothing outside the
  // payload is a reference: a card's window can open on whatever its clamp and
  // the tape left ahead of the run-in — on one bt8x8 dump that is 57-119
  // counts against a data logic 0 of about 33 — and on a tape the run-in itself
  // has collapsed to the data midpoint.
  double data_low_quantile = 0.10;
  double data_high_quantile = 0.90;

  // Smallest data amplitude, in source counts, that can be a measurement
  // rather than noise.  A line below it carries no data service, so its own
  // estimate is discarded.
  double minimum_amplitude_counts = 8.0;

  // A line whose amplitude falls below this fraction of the frame's data
  // amplitude carries no data service, so it is mapped through the frame's
  // levels rather than stretched to fill the data amplitude.  Half, because a
  // payload quantile reads a blank line's noise as amplitude: on the noisiest
  // card dump in circulation a blank line spans 0,3 of the data lines' range,
  // and mapping it by its own estimate would amplify that noise into a
  // full-scale burst for the slicer to chase.
  double minimum_amplitude_fraction = 0.5;

  // Relative departure from the frame's median amplitude at which kRolling
  // stops holding a line at the median and applies the line's own estimate.
  double rolling_deviation_fraction = 0.25;

  // Source-domain levels used by kFixed.
  double fixed_logic0 = 0.0;
  double fixed_logic1 = 0.0;

  // The levels the capture's data lines were measured at when it was opened
  // (VBIOffsetCalibration::data_levels), applied to a frame in which no line
  // carries the data service.  Empty leaves such a frame without a level
  // reference, and so as blanking.
  std::optional<VBILineLevels> capture_levels;
};

// The factor between the 16-bit domain ld-decode and vhs-decode write and the
// 10-bit CVBS_U10_4FSC domain the stage emits.  Both are anchored to the same
// physical levels — PAL blanking is 256 in one and 16 384 in the other, NTSC
// white 800 and 51 200 — so the whole mapping is this one scale.
constexpr double kTBCSampleScale = 64.0;

// Levels for a source whose samples are already in the output's amplitude
// domain, scaled by the factor above.
//
// A time-base corrected capture has absolute levels: the decoder that produced
// it has already normalised blanking and white to fixed values, so estimating
// them again from the record's own structure would replace a known quantity
// with a measured one.
//
// Expressed as fixed levels rather than as a mode of its own, because that is
// exactly what it is: the two logic levels are known, so nothing is measured.
// The pair chosen does not matter as long as both come from the same domain —
// any pair gives the same affine map, which is a division by the scale.
VBILevelMapperConfig vbi_absolute_level_config(
    const VBIOutputLevels& output_levels);

// The windows of a line record its levels are read from.  Ranges are
// half-open, in samples from the start of the record.
struct VBIDataWindows {
  // The clock run-in: whole samples strictly inside its alternating bits.
  uint32_t run_in_begin = 0;
  uint32_t run_in_end = 0;

  // The payload, from the end of the framing code to the end of the packet.
  uint32_t payload_begin = 0;
  uint32_t payload_end = 0;
};

// The level-reading windows of a record whose clock run-in starts at
// |cri_start_samples|, clamped to the record's |valid_samples|.
VBIDataWindows vbi_data_windows(const VBITeletextService& service,
                                double sample_rate_hz, double cri_start_samples,
                                uint32_t valid_samples);

// Sample windows within a stored line record, at the fitted run-in position.
struct VBIRecordWindows {
  // Where a record's clock run-in is looked for to tell whether the line
  // carries the data service: the fitted run-in position, widened by the
  // scatter the source format allows about it.
  VBICRISearchWindow presence{};

  // Where its levels are read from.
  VBIDataWindows data{};
};

// The affine map from a record's own amplitude domain into the output's.
//
// Applied after resampling rather than before it.  The two are equivalent for a
// filter whose response at DC is exactly one — which is what the resampler's
// per-sample weight normalisation guarantees — and mapping the far smaller
// output saves a whole pass over every record.
struct VBISampleMap {
  double source_logic0 = 0.0;
  double output_logic0 = 0.0;
  double gain = 0.0;

  double apply(double sample) const {
    return output_logic0 + (sample - source_logic0) * gain;
  }
};

// Build the map from a line's measured levels onto the output's.  An unusable
// or degenerate measurement yields a map that produces blanking, so a line with
// no honest scale is never amplified by a fabricated one.
VBISampleMap make_vbi_sample_map(const VBILineLevels& levels,
                                 const VBIOutputLevels& output_levels);

// Derive the level-reading windows of a source format, whose capture offset is
// the fitted one.
VBIRecordWindows vbi_record_windows(const VBISourceFormat& format,
                                    const VBITeletextService& service);

// Read the levels of one record: logic 0 is the low quantile of its payload,
// and logic 1 the higher of the high quantiles of its payload and its run-in.
//
// The run-in is half ones whatever the payload carries, so it holds logic 1 on
// a line whose payload hardly does — line-21 caption padding is an eighth ones,
// each of them a lone bit a band-limited channel never lets settle — while on
// a tape, where the run-in has collapsed to the data midpoint, the payload's
// is the higher and is the one taken.  Logic 0 is never read from the run-in:
// a caption run-in dips below blanking on a tape, and a collapsed teletext one
// never reaches logic 0 at all.
//
// |byte_samples| vouches that every sample is a whole number from 0 to 255
// (VBILineRecord::byte_samples), and lets the quantiles be counted without
// each sample being checked first.  The levels are the same to the bit either
// way: unvouched samples are checked, and counted if they turn out to be bytes
// all the same.
VBILineLevels estimate_vbi_data_levels(const std::vector<double>& samples,
                                       const VBIDataWindows& windows,
                                       const VBILevelMapperConfig& config,
                                       bool byte_samples = false);

// The source levels at which a 525-line capture's service logic levels sit,
// from the two levels of its line-21 caption data: blanking and 50 IRE
// (CTA-608-E §4).  Those two fix the capture's gain on the IRE scale, and the
// service's logic levels are read off it at the IRE the output puts them —
// NABTS logic 0 at blanking and logic 1 at 70 IRE (EIA-516).  An unusable
// measurement gives an unusable result.
VBILineLevels vbi_levels_from_caption_levels(const VBILineLevels& caption,
                                             const VBIOutputLevels& output);

// Establishes the amplitude domain of u8 card captures — the sources whose
// levels are relative and affected by automatic gain control.
//
// Levels are read from the samples that carry the data, and from nothing else:
// a line's logic 0 and logic 1 are low and high quantiles of its own payload,
// logic 1 from its run-in where that reaches higher (estimate_vbi_data_levels).
// What precedes the clock run-in in a card's record is not a level reference.
// The window opens wherever the hardware opens it, on whatever the card's clamp
// and the tape's playback electronics left there: one bt8x8 dump carries
// 57-119 counts there against a data logic 0 of about 33, which read as
// logic 0 put the data's lower third below the output's floor.  The run-in is
// no reference either, having collapsed to the data midpoint on any tape
// (design §5.3.6).
//
// A payload quantile reads any line, blank or not, so a frame's levels are
// taken from the lines that carry the service: those whose run-in and framing
// code correlate where the capture-offset fit put them, to the correlation the
// fit accepted them at.  A line that swings less than half as far as they do
// — a blank line, whose payload quantiles span only its noise — is mapped
// through the frame's levels rather than stretched to fill the data amplitude;
// a line that swings as far keeps its own, whether it is a data line, a test
// signal or a caption line.  A frame in which no line carries the service is
// mapped at the levels its data lines were measured at when the capture was
// opened.
//
// The mapping the levels imply is linear and nothing else is done to the
// samples.  A deconvolving slicer downstream recovers data by matching the
// blurred waveform it is given, so any sharpening, slicing or re-quantisation
// here would destroy the information it depends on (design §5.3.6).
//
// The mapper works on a whole stored frame at a time.  That is what makes
// kRolling's median deterministic: frames are built lazily and in whatever
// order they are asked for, so a window carried across frames would make a
// frame's output depend on which frames were read before it.
//
// Thread-compatible: instances hold configuration only.
class VBILevelMapper {
 public:
  VBILevelMapper() = default;

  VBILevelMapper(const VBISourceFormat& format,
                 const VBITeletextService& service,
                 VBILevelMapperConfig config);

  const VBIRecordWindows& windows() const { return windows_; }

  const VBILevelMapperConfig& config() const { return config_; }

  // Whether a record carries the data service: its run-in and framing code
  // are found inside the presence window.
  bool carries_service(const std::vector<double>& samples) const;

  // Levels to apply to every record of one stored frame.  out_levels holds one
  // entry per input record, in the same order.
  void map_frame(const std::vector<VBILineRecord>& records,
                 std::vector<VBILineLevels>& out_levels) const;

 private:
  // Levels held in common by a frame, from which lines are mapped when their
  // own measurement is not used: the median of the lines that carry the
  // service, else the capture's measured levels.
  VBILineLevels frame_levels(const std::vector<VBILineLevels>& measured,
                             const std::vector<uint8_t>& carries) const;

  VBILevelMapperConfig config_{};
  VBIRecordWindows windows_{};

  // The run-in and framing code template the presence test correlates, and
  // the correlation it must reach.
  VBICRITemplate cri_template_{};
  double presence_correlation_ = 0.0;
};

}  // namespace orc

#endif  // ORC_VBI_LEVEL_MAPPER_H
