/*
 * File:        truth_comparison.h
 * Module:      orc-videosynth-support
 * Purpose:     Error measurements of a stage's output against a videosynth
 *              truth case: per band, and inside and outside sample runs
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/common_types.h>
#include <orc/stage/dropout/dropout_run.h>
#include <orc/stage/orc_source_parameters.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace orc_videosynth {

// Flat frame samples in the CVBS_U10_4FSC 10-bit domain.
using Sample = int16_t;

// ── Bands ───────────────────────────────────────────────────────────────────

// A frequency band of the composite signal, [low_hz, high_hz).
struct Band {
  std::string name;
  double low_hz = 0.0;
  double high_hz = 0.0;
};

// The composite bands errors are reported in: low luma (0-2 MHz), high luma
// (2-3.5 MHz) and chroma (3.5-5.5 MHz) for PAL. NTSC and PAL-M scale the PAL
// edges by the ratio of their subcarrier to PAL's, so each band sits in the
// same place relative to the subcarrier.
std::vector<Band> composite_bands(orc::VideoSystem system);

// ── Levels ──────────────────────────────────────────────────────────────────

// IRE per 10-bit code: 100 IRE spans blanking to white.
double ire_per_code(const orc::SourceParameters& params);

// The gain of a stage in dB: how far it reduced the error the unprocessed
// capture had. Positive when the output is nearer the truth. nullopt when
// either error is zero, where the ratio means nothing.
std::optional<double> gain_db(double capture_rms, double output_rms);

// ── Per-band error ──────────────────────────────────────────────────────────

// Accumulates the error of an output against the truth, split into bands
// over active picture lines. Each line's error is transformed whole, and by
// Parseval its energy divides exactly among the frequencies, so the bands'
// mean squares add up to the line's (less what lies above the top band).
class BandErrorMeter {
 public:
  BandErrorMeter(orc::VideoSystem system, double ire_per_code);
  ~BandErrorMeter();
  BandErrorMeter(const BandErrorMeter&) = delete;
  BandErrorMeter& operator=(const BandErrorMeter&) = delete;

  // One line's samples from each, the same |count| from both.
  void add_line(const Sample* output, const Sample* truth, size_t count);

  // Every active line of a flat frame, over the active samples |params|
  // states.
  void add_frame(const Sample* output, const Sample* truth,
                 const orc::SourceParameters& params);

  const std::vector<Band>& bands() const { return bands_; }

  // RMS error per band, in IRE, over every sample added; 0 before any.
  std::vector<double> rms_ire() const;

  // RMS error over every sample added, in IRE: all frequencies.
  double total_rms_ire() const;

 private:
  struct Transform;

  orc::VideoSystem system_;
  double ire_per_code_;
  std::vector<Band> bands_;
  std::vector<double> band_energy_;
  double total_energy_ = 0.0;
  uint64_t samples_ = 0;
  std::unique_ptr<Transform> transform_;
};

// ── Error inside and outside sample runs ───────────────────────────────────

struct SplitError {
  double inside_rms_ire = 0.0;
  uint64_t inside_samples = 0;
  double outside_rms_ire = 0.0;
  uint64_t outside_samples = 0;
};

// Accumulates the error of an output against the truth separately inside
// sample runs (dropouts, typically) and outside them, so a correction is
// judged where it acted and where it should have left the picture alone.
class RunErrorMeter {
 public:
  explicit RunErrorMeter(double ire_per_code);

  // One line's samples; |inside| is one byte per sample, non-zero inside a
  // run.
  void add_line(const Sample* output, const Sample* truth,
                const uint8_t* inside, size_t count);

  // The active picture of a flat frame of |total| samples, |runs| marking
  // the inside.
  void add_frame(const Sample* output, const Sample* truth, size_t total,
                 const std::vector<orc::DropoutRun>& runs,
                 const orc::SourceParameters& params);

  SplitError result() const;

 private:
  double ire_per_code_;
  double inside_energy_ = 0.0;
  double outside_energy_ = 0.0;
  uint64_t inside_samples_ = 0;
  uint64_t outside_samples_ = 0;
};

// ── Damage the runs do not flag ────────────────────────────────────────────

// The runs where |capture| differs from |truth|: every sample a videosynth
// impairment changed, flagged or not, for a case without noise. Runs are
// split at line ends, as a capture's flagged runs are.
std::vector<orc::DropoutRun> damage_runs(const Sample* capture,
                                         const Sample* truth,
                                         const orc::SourceParameters& params,
                                         size_t total, orc::FrameID frame_id);

struct Coverage {
  uint64_t damaged_samples = 0;
  uint64_t flagged_samples = 0;  // of the damaged ones

  // The fraction of damaged samples a run flags; 1 when nothing is damaged.
  double flagged_fraction() const;
};

// Counts, in the active picture, the samples of |damage| that |flagged|
// covers: how much of what an impairment did the capture's dropout runs
// report.
class CoverageMeter {
 public:
  void add_frame(const std::vector<orc::DropoutRun>& damage,
                 const std::vector<orc::DropoutRun>& flagged, size_t total,
                 const orc::SourceParameters& params);

  const Coverage& result() const { return coverage_; }

 private:
  Coverage coverage_;
};

}  // namespace orc_videosynth
