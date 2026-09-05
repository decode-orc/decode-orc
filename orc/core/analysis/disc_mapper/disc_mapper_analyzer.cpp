/*
 * File:        disc_mapper_analyzer.cpp
 * Module:      orc-core/analysis
 * Purpose:     Frame mapping analyzer implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "disc_mapper_analyzer.h"

#include <orc/stage/observation/observation_context.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

#include "../analysis_progress.h"
#include "frame_quality_score.h"

namespace orc {

// ============================================================================
// Internal data structures for the disc mapping pipeline
// ============================================================================

/**
 * @brief Which end of the disc a field's lead code marks, if any
 */
enum class LeadType { None, LeadIn, LeadOut };

/**
 * @brief Normalized field metadata extracted from VBI
 */
struct NormalizedField {
  FieldID field_id;
  bool is_first_field = false;  // true = field 0 of frame, false = field 1
  VideoFormat format;

  // VBI-derived picture number (source of truth)
  std::optional<int32_t> picture_number;  // From CAV or CLV
  bool is_cav = false;    // True if PN from CAV, false if from CLV
  int pn_confidence = 0;  // 0-100

  // Phase information (supporting evidence only)
  std::optional<int32_t> phase;  // PAL: 1-8, NTSC: 1-4

  // Quality metrics for tie-breaking. Both fields of a frame carry the same
  // values: the burst/SNR observers publish one reading per frame.
  FrameQualityMetrics quality_metrics;
  double quality_score = kNeutralFrameQualityScore;

  // Flags
  LeadType lead_type = LeadType::None;  // Lead-in/out code, or CAV PN == 0
  bool has_user_code = false;           // Line 16 carries a user's code
  bool is_invalid = false;              // Corrupt or unusable
};

/**
 * @brief Candidate frame pairing two fields
 */
struct CandidateFrame {
  FieldID first_field;
  FieldID second_field;

  std::optional<int32_t> picture_number;
  bool is_cav = false;
  int pn_confidence = 0;

  bool phase_valid = true;
  bool parity_valid = true;

  FrameQualityMetrics quality_metrics;
  double quality_score = kNeutralFrameQualityScore;

  LeadType lead_type = LeadType::None;
  bool has_user_code = false;
  bool pn_disagreement = false;  // Fields have different PNs
};

/**
 * @brief Final mapped frame in the output sequence
 */
struct MappedFrame {
  std::optional<int32_t> picture_number;
  std::optional<FieldID> first_field;
  std::optional<FieldID> second_field;
  bool is_pad = false;  // Missing frame placeholder
};

// ============================================================================
// Helper functions
// ============================================================================

/**
 * @brief Decode BCD (Binary Coded Decimal) - same as BiphaseObserver
 */
static bool decode_bcd(uint32_t bcd, int32_t& output) {
  output = 0;
  int32_t multiplier = 1;

  while (bcd > 0) {
    uint32_t digit = bcd & 0x0F;
    if (digit > 9) {
      return false;  // Invalid BCD digit
    }
    output += static_cast<int32_t>(digit) * multiplier;
    multiplier *= 10;
    bcd >>= 4;
  }

  return true;
}

/**
 * @brief Decode CAV picture number from VBI data
 */
static std::optional<int32_t> decode_cav_picture_number(int32_t vbi17,
                                                        int32_t vbi18) {
  // IEC 60857-1986 - 10.1.3 Picture numbers (CAV discs)
  // Lines 17 and 18 carry redundant copies of the picture number.
  // Cross-validate: if both decode successfully but disagree, one is corrupted
  // and we cannot trust either — return nullopt so the other field of the frame
  // can supply the picture number instead.

  std::optional<int32_t> pn17, pn18;

  if ((vbi17 & 0xF00000) == 0xF00000) {
    int32_t pic_no;
    if (decode_bcd(vbi17 & 0x07FFFF, pic_no)) {
      pn17 = pic_no;
    }
  }

  if ((vbi18 & 0xF00000) == 0xF00000) {
    int32_t pic_no;
    if (decode_bcd(vbi18 & 0x07FFFF, pic_no)) {
      pn18 = pic_no;
    }
  }

  if (pn17 && pn18) {
    return (*pn17 == *pn18) ? pn17 : std::nullopt;
  }

  return pn17 ? pn17 : pn18;
}

/**
 * @brief Decode CLV timecode from VBI data and convert to picture number
 */
static std::optional<int32_t> decode_clv_picture_number(int32_t vbi16,
                                                        int32_t vbi17,
                                                        int32_t vbi18,
                                                        VideoFormat format) {
  // IEC 60857-1986 - 10.1.6 Programme time code (CLV hours and minutes)
  int32_t hours = -1, minutes = -1;

  // Decode hours/minutes from line 17 or 18
  if ((vbi17 & 0xF0FF00) == 0xF0DD00) {
    int32_t h, m;
    if (decode_bcd((vbi17 & 0x0F0000) >> 16, h) &&
        decode_bcd(vbi17 & 0x0000FF, m)) {
      if (h >= 0 && h <= 23 && m >= 0 && m <= 59) {
        hours = h;
        minutes = m;
      }
    }
  }

  if (hours < 0 && (vbi18 & 0xF0FF00) == 0xF0DD00) {
    int32_t h, m;
    if (decode_bcd((vbi18 & 0x0F0000) >> 16, h) &&
        decode_bcd(vbi18 & 0x0000FF, m)) {
      if (h >= 0 && h <= 23 && m >= 0 && m <= 59) {
        hours = h;
        minutes = m;
      }
    }
  }

  // IEC 60857-1986 - 10.1.10 CLV picture number (seconds and frame within
  // second)
  int32_t seconds = -1, picture = -1;

  if ((vbi16 & 0xF0F000) == 0x80E000) {
    uint32_t tens = (vbi16 & 0x0F0000) >> 16;
    int32_t sec_digit, pic_no;

    if (tens >= 0xA && tens <= 0xF &&
        decode_bcd((vbi16 & 0x000F00) >> 8, sec_digit) &&
        decode_bcd(vbi16 & 0x0000FF, pic_no)) {
      int32_t sec = (10 * static_cast<int32_t>(tens - 0xA)) + sec_digit;

      if (sec >= 0 && sec <= 59 && pic_no >= 0 && pic_no <= 29) {
        seconds = sec;
        picture = pic_no;
      }
    }
  }

  // Only compute picture number if all components are valid
  if (hours >= 0 && minutes >= 0 && seconds >= 0 && picture >= 0) {
    int fps = (format == VideoFormat::PAL) ? 25 : 30;
    int32_t total_seconds = hours * 3600 + minutes * 60 + seconds;
    int32_t pn = total_seconds * fps + picture;
    return pn > 0 ? std::optional<int32_t>(pn) : std::nullopt;
  }

  return std::nullopt;
}

/**
 * @brief Classify a field's lead-in/lead-out code, if it carries one
 *
 * Lead-in takes precedence: a field cannot legitimately carry both codes, so
 * if a dropout has produced one of each the disc-start reading is the safer
 * one to trust (lead-out frames are only ever re-attached at the tail).
 */
static LeadType classify_lead_type(int32_t vbi17, int32_t vbi18) {
  // IEC 60857-1986 - 10.1.1 Lead-in, 10.1.2 Lead-out
  if (vbi17 == 0x88FFFF || vbi18 == 0x88FFFF) {
    return LeadType::LeadIn;
  }
  if (vbi17 == 0x80EEEE || vbi18 == 0x80EEEE) {
    return LeadType::LeadOut;
  }
  return LeadType::None;
}

/**
 * @brief Check whether line 16 carries a user's code
 *
 * The user's code is the payload that makes a retained lead-in frame worth
 * having: it is what a downstream sink reads out of the lead-in. Only the
 * presence test is needed here — decoding it is the biphase observer's job.
 */
static bool has_user_code(int32_t vbi16) {
  // IEC 60857-1986 - 10.1.9 Users code: 0x8_D___ with X1 in 0..7
  if ((vbi16 & 0xF0F000) != 0x80D000) {
    return false;
  }
  return ((vbi16 & 0x0F0000) >> 16) <= 7;
}

/**
 * @brief Normalize a single field's metadata using VBI bytes from
 * ObservationContext. Parity and format are derived from the parent frame.
 * Phase hints are not available from VideoFrameRepresentation; the VFR pipeline
 * already guarantees correct field pairing so phase validation is skipped.
 */
static NormalizedField normalize_field(const ObservationContext& obs_context,
                                       FieldID field_id, bool is_first_field,
                                       VideoFormat format) {
  NormalizedField nf;
  nf.field_id = field_id;
  nf.is_first_field = is_first_field;
  nf.format = format;

  // Get VBI bytes from ObservationContext (populated by observers)
  auto vbi16_opt = obs_context.get(field_id, "biphase", "vbi_line_16");
  auto vbi17_opt = obs_context.get(field_id, "biphase", "vbi_line_17");
  auto vbi18_opt = obs_context.get(field_id, "biphase", "vbi_line_18");

  if (vbi16_opt && vbi17_opt && vbi18_opt) {
    int32_t vbi16 = std::get<int32_t>(*vbi16_opt);
    int32_t vbi17 = std::get<int32_t>(*vbi17_opt);
    int32_t vbi18 = std::get<int32_t>(*vbi18_opt);

    ORC_LOG_DEBUG("Field {}: VBI data: {:08x} {:08x} {:08x}", field_id.value(),
                  vbi16, vbi17, vbi18);

    // Check for lead-in/out first
    nf.lead_type = classify_lead_type(vbi17, vbi18);
    if (nf.lead_type != LeadType::None) {
      ORC_LOG_DEBUG("Field {}: Detected {}", field_id.value(),
                    nf.lead_type == LeadType::LeadIn ? "lead-in" : "lead-out");
    }
    nf.has_user_code = has_user_code(vbi16);

    // Try CAV picture number (priority 1)
    auto cav_pn = decode_cav_picture_number(vbi17, vbi18);
    if (cav_pn) {
      nf.picture_number = cav_pn;
      nf.is_cav = true;
      nf.pn_confidence = 95;

      ORC_LOG_DEBUG("Field {}: CAV picture number = {}", field_id.value(),
                    *cav_pn);

      // Picture numbering starts at 1, so a decoded zero is a lead-in field
      // whose lead-in code was lost to a dropout.
      if (*cav_pn == 0 && nf.lead_type == LeadType::None) {
        nf.lead_type = LeadType::LeadIn;
      }
    }
    // Try CLV timecode (priority 2)
    else {
      auto clv_pn = decode_clv_picture_number(vbi16, vbi17, vbi18, nf.format);
      if (clv_pn) {
        nf.picture_number = clv_pn;
        nf.is_cav = false;
        nf.pn_confidence = 85;

        ORC_LOG_DEBUG("Field {}: CLV picture number = {}", field_id.value(),
                      *clv_pn);
      } else {
        ORC_LOG_DEBUG("Field {}: Failed to decode CLV timecode",
                      field_id.value());
      }
    }
  } else {
    ORC_LOG_DEBUG("Field {}: No VBI data in ObservationContext",
                  field_id.value());
  }

  return nf;
}

/**
 * @brief Convert picture number to CLV timecode string
 */
static std::string picture_number_to_timecode(int32_t pn, VideoFormat format) {
  int32_t fps = (format == VideoFormat::PAL) ? 25 : 30;

  int32_t f = pn % fps;
  int32_t total_seconds = pn / fps;
  int32_t s = total_seconds % 60;
  int32_t total_minutes = total_seconds / 60;
  int32_t m = total_minutes % 60;
  int32_t h = total_minutes / 60;

  std::ostringstream tc;
  tc << h << ":" << std::setfill('0') << std::setw(2) << m << ":"
     << std::setfill('0') << std::setw(2) << s << "." << std::setfill('0')
     << std::setw(2) << f;

  return tc.str();
}

/**
 * @brief Generate a compact visual representation of picture numbers in frame
 * list
 */
static std::string generate_frame_map(const std::vector<CandidateFrame>& frames,
                                      bool is_clv, VideoFormat format) {
  if (frames.empty()) return "(empty)";

  // Extract picture numbers that exist
  std::vector<int32_t> pns;
  for (const auto& frame : frames) {
    if (frame.picture_number) {
      pns.push_back(*frame.picture_number);
    }
  }

  if (pns.empty()) return "(no picture numbers)";

  // Sort
  std::sort(pns.begin(), pns.end());

  // Build range notation
  std::ostringstream result;
  size_t i = 0;
  while (i < pns.size()) {
    int32_t start = pns[i];
    int32_t end = start;
    size_t j = i + 1;

    // Look for consecutive numbers
    while (j < pns.size() && pns[j] == end + 1) {
      end = pns[j];
      j++;
    }

    if (i > 0) result << ",";

    if (is_clv) {
      // Use timecode format for CLV
      if (j - i >= 3) {
        result << picture_number_to_timecode(start, format) << "-"
               << picture_number_to_timecode(end, format);
      } else {
        for (size_t k = i; k < j; k++) {
          if (k > i) result << ",";
          result << picture_number_to_timecode(pns[k], format);
        }
      }
    } else {
      // Use picture numbers for CAV
      if (j - i >= 3) {
        result << start << "-" << end;
      } else {
        for (size_t k = i; k < j; k++) {
          if (k > i) result << ",";
          result << pns[k];
        }
      }
    }

    i = j;
  }

  return result.str();
}

/**
 * @brief Generate frame map for MappedFrame vector (includes PAD markers)
 */
static std::string generate_frame_map(const std::vector<MappedFrame>& frames,
                                      bool is_clv, VideoFormat format) {
  if (frames.empty()) return "(empty)";

  // Build a list with picture numbers (or PAD markers)
  struct Entry {
    bool is_pad;
    std::optional<int32_t> pn;
  };

  std::vector<Entry> entries;
  for (const auto& frame : frames) {
    if (frame.is_pad) {
      entries.push_back({true, std::nullopt});
    } else if (frame.picture_number) {
      entries.push_back({false, frame.picture_number});
    }
  }

  if (entries.empty()) return "(no entries)";

  // Build range notation with PAD support
  std::ostringstream result;
  size_t i = 0;
  while (i < entries.size()) {
    // Handle PAD sequences
    if (entries[i].is_pad) {
      size_t pad_count = 1;
      while (i + pad_count < entries.size() && entries[i + pad_count].is_pad) {
        pad_count++;
      }

      if (i > 0) result << ",";
      if (pad_count == 1) {
        result << "PAD";
      } else {
        result << "PAD(" << pad_count << ")";
      }

      i += pad_count;
      continue;
    }

    // Handle consecutive picture number sequences
    const auto& ipn = entries[i].pn;
    if (!ipn.has_value()) {
      ++i;
      continue;
    }
    int32_t start_pn = *ipn;
    int32_t end_pn = start_pn;
    size_t j = i + 1;

    // Look for consecutive picture numbers
    while (j < entries.size() && !entries[j].is_pad) {
      const auto& jpn = entries[j].pn;
      if (!jpn.has_value()) break;
      if (*jpn == end_pn + 1) {
        end_pn = *jpn;
        j++;
      } else {
        break;
      }
    }

    if (i > 0) result << ",";

    if (is_clv) {
      // Use timecode format for CLV
      if (j - i >= 3) {
        result << picture_number_to_timecode(start_pn, format) << "-"
               << picture_number_to_timecode(end_pn, format);
      } else {
        for (size_t k = i; k < j; k++) {
          if (k > i) result << ",";
          const auto& kpn = entries[k].pn;
          if (kpn.has_value()) {
            result << picture_number_to_timecode(*kpn, format);
          }
        }
      }
    } else {
      // Use picture numbers for CAV
      if (j - i >= 3) {
        result << start_pn << "-" << end_pn;
      } else {
        for (size_t k = i; k < j; k++) {
          if (k > i) result << ",";
          const auto& kpn = entries[k].pn;
          if (kpn.has_value()) result << *kpn;
        }
      }
    }

    i = j;
  }

  return result.str();
}

/**
 * @brief Check if phase sequence is valid for a frame pair
 */
static bool is_phase_valid(const std::optional<int32_t>& phase1,
                           const std::optional<int32_t>& phase2,
                           VideoFormat format) {
  if (!phase1 || !phase2) return true;  // Can't validate without phase

  if (format == VideoFormat::PAL) {
    // PAL: 8-field sequence, expect increment by 2
    int expected_phase2 = (*phase1 % 8) + 2;
    if (expected_phase2 > 8) expected_phase2 -= 8;
    return *phase2 == expected_phase2 || *phase2 == expected_phase2 - 1 ||
           *phase2 == expected_phase2 + 1;
  } else if (format == VideoFormat::NTSC) {
    // NTSC: 4-field sequence, expect increment by 2
    int expected_phase2 = (*phase1 % 4) + 2;
    if (expected_phase2 > 4) expected_phase2 -= 4;
    return *phase2 == expected_phase2 || *phase2 == expected_phase2 - 1 ||
           *phase2 == expected_phase2 + 1;
  }

  return true;
}

/**
 * @brief Create candidate frame from two normalized fields
 */
static std::optional<CandidateFrame> pair_fields(const NormalizedField& f1,
                                                 const NormalizedField& f2) {
  CandidateFrame frame;
  frame.first_field = f1.field_id;
  frame.second_field = f2.field_id;

  // Select picture number using priority:
  // 1. First field CAV
  // 2. Second field CAV
  // 3. First field CLV
  // 4. Second field CLV
  if (f1.picture_number && f1.is_cav) {
    frame.picture_number = f1.picture_number;
    frame.is_cav = true;
    frame.pn_confidence = f1.pn_confidence;
  } else if (f2.picture_number && f2.is_cav) {
    frame.picture_number = f2.picture_number;
    frame.is_cav = true;
    frame.pn_confidence = f2.pn_confidence;
  } else if (f1.picture_number && !f1.is_cav) {
    frame.picture_number = f1.picture_number;
    frame.is_cav = false;
    frame.pn_confidence = f1.pn_confidence;
  } else if (f2.picture_number && !f2.is_cav) {
    frame.picture_number = f2.picture_number;
    frame.is_cav = false;
    frame.pn_confidence = f2.pn_confidence;
  }

  // Check for PN disagreement
  if (f1.picture_number && f2.picture_number &&
      *f1.picture_number != *f2.picture_number) {
    frame.pn_disagreement = true;
  }

  // Check phase validity
  frame.phase_valid = is_phase_valid(f1.phase, f2.phase, f1.format);

  // Check parity: valid frame pair has one first_field and one second_field
  frame.parity_valid = (f1.is_first_field != f2.is_first_field);

  // Combine quality scores. Both fields carry the frame's readings, so the
  // mean is that same value; averaging keeps the result correct if a future
  // change ever pairs fields from different frames.
  frame.quality_metrics = f1.quality_metrics;
  frame.quality_score = (f1.quality_score + f2.quality_score) / 2.0;

  // Check for lead-in/out. Either field carrying a code marks the frame; a
  // lead-in reading wins over a lead-out one for the reason given in
  // classify_lead_type().
  if (f1.lead_type == LeadType::LeadIn || f2.lead_type == LeadType::LeadIn) {
    frame.lead_type = LeadType::LeadIn;
  } else if (f1.lead_type == LeadType::LeadOut ||
             f2.lead_type == LeadType::LeadOut) {
    frame.lead_type = LeadType::LeadOut;
  }
  frame.has_user_code = f1.has_user_code || f2.has_user_code;

  return frame;
}

// ============================================================================
// Main analysis implementation
// ============================================================================

FieldMappingDecision DiscMapperAnalyzer::analyze(
    const VideoFrameRepresentation& source,
    const ObservationContext& observation_context, const Options& options,
    AnalysisProgress* progress) {
  FieldMappingDecision decision;
  std::ostringstream rationale;

  // Derive field range from frame range (each frame contributes 2 fields)
  auto frame_range = source.frame_range();
  size_t total_frames = frame_range.count();
  size_t total_fields = total_frames * 2;

  decision.stats.total_fields = total_fields;

  if (total_fields == 0) {
    decision.success = false;
    decision.rationale = "No fields found in source";
    return decision;
  }

  rationale << "=== Disc Mapping Analysis ===\n\n";
  rationale << "Input: " << total_fields << " fields (" << total_frames
            << " frames)\n\n";

  // ========================================================================
  // Stage 1: Per-field VBI normalization
  // ========================================================================

  if (progress) {
    progress->setStatus("Normalizing field metadata...");
    progress->setProgress(0);
  }

  std::vector<NormalizedField> normalized_fields;
  normalized_fields.reserve(total_fields);

  size_t fields_with_pn = 0;
  size_t cav_fields = 0;
  size_t clv_fields = 0;
  size_t frames_with_quality = 0;

  // Reference burst amplitude used to score the measured burst level. Derived
  // once from the source's level domain; nullopt when the source reports no
  // video parameters, in which case burst readings are left unscored.
  std::optional<double> nominal_burst_10bit;
  if (auto vp = source.get_video_parameters()) {
    nominal_burst_10bit = nominal_burst_peak_10bit(
        vp->system, vp->blanking_level, vp->white_level);
  }

  {
    size_t norm_idx = 0;
    size_t norm_interval = std::max(static_cast<size_t>(1), total_fields / 100);
    for (FrameID frame_id = frame_range.first; frame_id <= frame_range.last;
         ++frame_id) {
      VideoFormat format = VideoFormat::Unknown;
      auto frame_desc = source.get_frame_descriptor(frame_id);
      if (frame_desc) {
        format = video_format_from_system(frame_desc->system);
      }

      // Burst/SNR readings are frame-scoped; both fields of the frame inherit
      // them so that the pairing stage can rank duplicate pictures.
      const FrameQualityMetrics quality =
          read_frame_quality_metrics(observation_context, frame_id);
      const double quality_score =
          compute_frame_quality_score(quality, nominal_burst_10bit);
      if (!quality.empty()) {
        ++frames_with_quality;
      }

      for (size_t field_idx = 0; field_idx < 2; ++field_idx) {
        FieldID fid(frame_id * 2 + field_idx);
        bool is_first = (field_idx == 0);
        auto nf = normalize_field(observation_context, fid, is_first, format);
        nf.quality_metrics = quality;
        nf.quality_score = quality_score;

        if (nf.picture_number) {
          fields_with_pn++;
          if (nf.is_cav) {
            cav_fields++;
          } else {
            clv_fields++;
          }
        }

        normalized_fields.push_back(nf);
        ++norm_idx;
        if (progress && norm_idx % norm_interval == 0) {
          progress->setProgress(
              static_cast<int>(norm_idx * 100 / total_fields));
        }
      }
    }
    if (progress) progress->setProgress(100);
  }

  // Determine disc type
  decision.is_cav = (cav_fields > clv_fields);

  // Determine video format from first frame
  auto first_frame_desc = source.get_frame_descriptor(frame_range.first);
  if (first_frame_desc) {
    decision.is_pal = (first_frame_desc->system == VideoSystem::PAL);
  }

  // ========================================================================
  // Stage 1b: Sequence-based resolution of CAV VBI line disagreements
  // ========================================================================
  // When lines 17 and 18 of the same field decode to different picture
  // numbers, Stage 1 leaves picture_number unset (cannot trust either value
  // in isolation).  Here we re-decode both lines for each such field and
  // check whether exactly one candidate is consistent with the sequence of
  // confirmed picture numbers from neighbouring fields.  If so, we promote
  // that candidate with reduced confidence.
  //
  // Expected PN at field index i, given confirmed neighbour at index j:
  //   expected = pn_j + (i/2) - (j/2)
  // Each PN increments once per frame (every 2 field indices).

  size_t pn_resolved_count = 0;

  if (decision.is_cav) {
    for (size_t i = 0; i < normalized_fields.size(); ++i) {
      auto& nf = normalized_fields[i];
      if (nf.picture_number) continue;

      auto vbi17_opt =
          observation_context.get(nf.field_id, "biphase", "vbi_line_17");
      auto vbi18_opt =
          observation_context.get(nf.field_id, "biphase", "vbi_line_18");
      if (!vbi17_opt || !vbi18_opt) continue;

      int32_t vbi17 = std::get<int32_t>(*vbi17_opt);
      int32_t vbi18 = std::get<int32_t>(*vbi18_opt);

      std::optional<int32_t> pn_a, pn_b;
      if ((vbi17 & 0xF00000) == 0xF00000) {
        int32_t n;
        if (decode_bcd(vbi17 & 0x07FFFF, n)) pn_a = n;
      }
      if ((vbi18 & 0xF00000) == 0xF00000) {
        int32_t n;
        if (decode_bcd(vbi18 & 0x07FFFF, n)) pn_b = n;
      }

      // Only handle the disagreement case: both decoded to different values
      if (!pn_a || !pn_b || *pn_a == *pn_b) continue;

      // Find the nearest confirmed PN before index i
      std::optional<int32_t> prev_pn;
      size_t prev_idx = 0;
      for (size_t j = i; j-- > 0;) {
        if (normalized_fields[j].picture_number) {
          prev_pn = normalized_fields[j].picture_number;
          prev_idx = j;
          break;
        }
      }

      // Find the nearest confirmed PN after index i
      std::optional<int32_t> next_pn;
      size_t next_idx = 0;
      for (size_t j = i + 1; j < normalized_fields.size(); ++j) {
        if (normalized_fields[j].picture_number) {
          next_pn = normalized_fields[j].picture_number;
          next_idx = j;
          break;
        }
      }

      // Returns the expected PN at field index field_i given a confirmed
      // neighbour at field index field_j with picture number pn_j.
      auto expected_pn = [](size_t field_i, size_t field_j,
                            int32_t pn_j) -> int32_t {
        return pn_j + static_cast<int32_t>(field_i / 2) -
               static_cast<int32_t>(field_j / 2);
      };

      auto fits_sequence = [&](int32_t candidate) -> bool {
        if (prev_pn && expected_pn(i, prev_idx, *prev_pn) == candidate) {
          return true;
        }
        if (next_pn && expected_pn(i, next_idx, *next_pn) == candidate) {
          return true;
        }
        return false;
      };

      const bool a_fits = fits_sequence(*pn_a);
      const bool b_fits = fits_sequence(*pn_b);

      if (a_fits == b_fits) continue;  // Both or neither fit — leave unresolved

      const int32_t resolved = a_fits ? *pn_a : *pn_b;
      ++pn_resolved_count;

      // Do NOT promote nf.picture_number here. A field whose two VBI lines
      // disagree indicates a dropout that likely also corrupted the picture
      // data. Leaving picture_number unset causes the frame to fall into
      // frames_without_pn in Stage 4, and Stage 5 will insert a PAD frame at
      // the correct disc-picture position — which is the right outcome for
      // multi-source stacking (other sources will provide the picture instead).
      ORC_LOG_DEBUG(
          "Field {}: VBI disagreement ({} vs {}), likely PN {} — treating "
          "frame as PAD (other sources will supply this disc picture)",
          nf.field_id.value(), *pn_a, *pn_b, resolved);
    }
  }

  rationale << "Stage 1: VBI Normalization\n";
  rationale << "  Fields with picture numbers: " << fields_with_pn << " / "
            << total_fields << "\n";
  rationale << "  CAV fields: " << cav_fields << "\n";
  rationale << "  CLV fields: " << clv_fields << "\n";
  if (pn_resolved_count > 0) {
    rationale << "  VBI disagreements (will be padded): " << pn_resolved_count
              << "\n";
  }
  rationale << "  Frames with quality readings: " << frames_with_quality
            << " / " << total_frames << "\n";
  rationale << "  Detected format: " << (decision.is_pal ? "PAL" : "NTSC")
            << "\n";
  rationale << "  Detected disc type: " << (decision.is_cav ? "CAV" : "CLV")
            << "\n\n";

  if (progress && progress->isCancelled()) {
    decision.success = false;
    decision.rationale = "Analysis cancelled by user";
    return decision;
  }

  // ========================================================================
  // Stage 2: Field pairing (candidate frame generation)
  // ========================================================================

  if (progress) {
    progress->setStatus("Pairing fields into frames...");
    progress->setProgress(0);
  }

  std::vector<CandidateFrame> candidate_frames;

  // Simple sequential pairing
  for (size_t i = 0; i + 1 < normalized_fields.size(); i += 2) {
    auto frame_opt =
        pair_fields(normalized_fields[i], normalized_fields[i + 1]);
    if (frame_opt) {
      candidate_frames.push_back(*frame_opt);
    }
  }

  if (progress) progress->setProgress(100);
  rationale << "Stage 2: Field Pairing\n";
  rationale << "  Candidate frames created: " << candidate_frames.size()
            << "\n";
  VideoFormat fmt = decision.is_pal ? VideoFormat::PAL : VideoFormat::NTSC;
  rationale << "  Frame map: "
            << generate_frame_map(candidate_frames, !decision.is_cav, fmt)
            << "\n\n";

  if (progress && progress->isCancelled()) {
    decision.success = false;
    decision.rationale = "Analysis cancelled by user";
    return decision;
  }

  // ========================================================================
  // Stage 3: Frame validation and filtering
  // ========================================================================

  if (progress) {
    progress->setStatus("Validating frames...");
    progress->setProgress(0);
  }

  std::vector<CandidateFrame> valid_frames;
  size_t removed_lead_in_out = 0;
  size_t removed_invalid_phase = 0;

  // Lead frames held back for re-attachment in stage 5. They never enter
  // valid_frames: a lead-in frame carries no picture number (or a zero one),
  // so letting it through would corrupt the deduplication and gap detection
  // that the rest of the pipeline performs on picture numbers.
  //
  // Lead-in choice: prefer a frame whose line 16 carries the user's code,
  // since collecting that code is the point of keeping the frame; among
  // equally useful candidates take the last one, which is the frame closest
  // to the start of the programme. Lead-out choice: the first lead-out frame,
  // for the mirrored reason.
  std::optional<CandidateFrame> lead_in_frame;
  std::optional<CandidateFrame> lead_out_frame;

  for (const auto& frame : candidate_frames) {
    // Drop lead-in/out frames
    if (frame.lead_type != LeadType::None) {
      if (options.include_lead_in_out) {
        if (frame.lead_type == LeadType::LeadIn) {
          const bool is_better = !lead_in_frame || frame.has_user_code ||
                                 !lead_in_frame->has_user_code;
          if (is_better) {
            // The candidate this one displaces is discarded like any other.
            if (lead_in_frame) removed_lead_in_out++;
            lead_in_frame = frame;
            continue;
          }
        } else if (!lead_out_frame) {
          lead_out_frame = frame;
          continue;
        }
      }
      removed_lead_in_out++;
      continue;
    }

    // Drop phase-invalid frames only if no PN or low confidence
    if (!frame.phase_valid &&
        (!frame.picture_number || frame.pn_confidence < 50)) {
      removed_invalid_phase++;
      continue;
    }

    valid_frames.push_back(frame);
  }

  decision.stats.removed_lead_in_out = removed_lead_in_out;
  decision.stats.removed_invalid_phase = removed_invalid_phase;
  decision.stats.lead_in_included = lead_in_frame.has_value();
  decision.stats.lead_out_included = lead_out_frame.has_value();

  if (progress) progress->setProgress(100);
  rationale << "Stage 3: Frame Validation\n";
  rationale << "  Frames after filtering: " << valid_frames.size() << "\n";
  rationale << "  Removed (lead-in/out): " << removed_lead_in_out << "\n";
  if (options.include_lead_in_out) {
    rationale << "  Lead-in frame kept: ";
    if (lead_in_frame) {
      rationale << "source frame " << (lead_in_frame->first_field.value() / 2)
                << (lead_in_frame->has_user_code ? " (carries user's code)"
                                                 : " (no user's code found)")
                << "\n";
    } else {
      rationale << "none found\n";
    }
    rationale << "  Lead-out frame kept: ";
    if (lead_out_frame) {
      rationale << "source frame " << (lead_out_frame->first_field.value() / 2)
                << "\n";
    } else {
      rationale << "none found\n";
    }
  }
  rationale << "  Removed (invalid phase): " << removed_invalid_phase << "\n";
  rationale << "  Frame map: "
            << generate_frame_map(valid_frames, !decision.is_cav, fmt)
            << "\n\n";

  if (progress && progress->isCancelled()) {
    decision.success = false;
    decision.rationale = "Analysis cancelled by user";
    return decision;
  }

  // ========================================================================
  // Stage 4: Deduplication by picture number
  // ========================================================================

  if (progress) {
    progress->setStatus("Deduplicating frames...");
    progress->setProgress(0);
  }

  // Group frames by PN
  std::map<int32_t, std::vector<CandidateFrame>> frames_by_pn;
  std::vector<CandidateFrame> frames_without_pn;

  for (const auto& frame : valid_frames) {
    if (frame.picture_number) {
      frames_by_pn[*frame.picture_number].push_back(frame);
    } else {
      frames_without_pn.push_back(frame);
    }
  }

  // Select best frame for each PN
  std::vector<CandidateFrame> selected_frames;
  size_t removed_duplicates = 0;
  size_t duplicate_groups = 0;
  size_t duplicates_decided_by_quality = 0;

  // The per-candidate report line: score plus the readings behind it.
  auto describe_quality = [](const CandidateFrame& frame) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << "score " << frame.quality_score;
    const auto& metrics = frame.quality_metrics;
    if (metrics.median_burst_10bit) {
      os << ", burst " << *metrics.median_burst_10bit;
    }
    if (metrics.white_snr_db) {
      os << ", white SNR " << *metrics.white_snr_db << " dB";
    }
    if (metrics.black_psnr_db) {
      os << ", black PSNR " << *metrics.black_psnr_db << " dB";
    }
    if (metrics.empty()) {
      os << " (no quality readings)";
    }
    return os.str();
  };

  // Cap on how many duplicate groups are itemised in the report; a badly
  // tracking capture can produce thousands and the detail stops being useful.
  constexpr size_t kMaxReportedDuplicateGroups = 20;
  std::ostringstream duplicate_report;

  for (const auto& [pn, frames] : frames_by_pn) {
    if (frames.size() == 1) {
      selected_frames.push_back(frames[0]);
      continue;
    }

    // Multiple frames with same PN - select best
    removed_duplicates += frames.size() - 1;
    ++duplicate_groups;

    auto best =
        std::max_element(frames.begin(), frames.end(),
                         [](const CandidateFrame& a, const CandidateFrame& b) {
                           // Priority: confidence > phase_valid > quality.
                           // Quality is the burst/SNR score, so
                           // equally-confident copies of the same disc picture
                           // are separated by measured signal condition.
                           if (a.pn_confidence != b.pn_confidence) {
                             return a.pn_confidence < b.pn_confidence;
                           }
                           if (a.phase_valid != b.phase_valid) {
                             return !a.phase_valid;
                           }
                           return a.quality_score < b.quality_score;
                         });

    // Quality made the decision when nothing earlier in the priority order
    // separated the candidates and the scores were not all equal.
    const bool confidence_and_phase_tied = std::all_of(
        frames.begin(), frames.end(), [&frames](const CandidateFrame& f) {
          return f.pn_confidence == frames.front().pn_confidence &&
                 f.phase_valid == frames.front().phase_valid;
        });
    const bool scores_differ = std::any_of(
        frames.begin(), frames.end(), [&frames](const CandidateFrame& f) {
          return f.quality_score != frames.front().quality_score;
        });
    if (confidence_and_phase_tied && scores_differ) {
      ++duplicates_decided_by_quality;
    }

    if (duplicate_groups <= kMaxReportedDuplicateGroups) {
      duplicate_report << "    Picture " << pn << ": " << frames.size()
                       << " copies\n";
      for (const auto& frame : frames) {
        const uint64_t src_frame_id = frame.first_field.value() / 2;
        duplicate_report << "      "
                         << (&frame == &*best ? "kept   " : "dropped")
                         << " source frame " << src_frame_id << " — "
                         << describe_quality(frame) << "\n";
      }
    }

    selected_frames.push_back(*best);
  }

  decision.stats.removed_duplicates = removed_duplicates;
  decision.stats.frames_with_quality = frames_with_quality;
  decision.stats.duplicate_groups = duplicate_groups;
  decision.stats.duplicates_decided_by_quality = duplicates_decided_by_quality;

  if (progress) progress->setProgress(100);
  rationale << "Stage 4: Deduplication\n";
  rationale << "  Unique picture numbers: " << frames_by_pn.size() << "\n";
  rationale << "  Duplicates removed: " << removed_duplicates << "\n";
  rationale << "  Duplicate groups decided by signal quality: "
            << duplicates_decided_by_quality << " / " << duplicate_groups
            << "\n";
  rationale << "  Frames without PN: " << frames_without_pn.size() << "\n";
  if (!duplicate_report.str().empty()) {
    rationale << "  Duplicate selection:\n" << duplicate_report.str();
    if (duplicate_groups > kMaxReportedDuplicateGroups) {
      rationale << "    ... "
                << (duplicate_groups - kMaxReportedDuplicateGroups)
                << " further duplicate groups not itemised\n";
    }
  }
  rationale << "  Frame map: "
            << generate_frame_map(selected_frames, !decision.is_cav, fmt)
            << "\n\n";

  if (progress && progress->isCancelled()) {
    decision.success = false;
    decision.rationale = "Analysis cancelled by user";
    return decision;
  }

  // ========================================================================
  // Stage 5: Sort by PN and detect gaps
  // ========================================================================

  if (progress) {
    progress->setStatus("Detecting gaps and building timeline...");
    progress->setProgress(0);
  }

  // Sort selected frames by PN
  std::sort(selected_frames.begin(), selected_frames.end(),
            [](const CandidateFrame& a, const CandidateFrame& b) {
              if (!a.picture_number) return true;
              if (!b.picture_number) return false;
              return *a.picture_number < *b.picture_number;
            });

  // Build final mapped sequence with PAD insertion
  std::vector<MappedFrame> final_frames;
  size_t gaps_padded = 0;
  size_t padding_frames = 0;

  std::optional<int32_t> prev_pn;
  size_t frames_without_pn_included = 0;

  for (const auto& frame : selected_frames) {
    // Handle frames WITH picture numbers
    if (frame.picture_number) {
      int32_t current_pn = *frame.picture_number;

      // Check for gap
      if (prev_pn && current_pn > *prev_pn + 1) {
        if (options.pad_gaps) {
          // Insert PAD frames
          for (int32_t missing_pn = *prev_pn + 1; missing_pn < current_pn;
               ++missing_pn) {
            MappedFrame pad;
            pad.picture_number = missing_pn;
            pad.is_pad = true;
            final_frames.push_back(pad);
            padding_frames++;
            ORC_LOG_DEBUG("Inserted PAD frame for missing picture number {}",
                          missing_pn);
          }
          gaps_padded++;
        }
      }

      // Add current frame
      MappedFrame mapped;
      mapped.picture_number = frame.picture_number;
      mapped.first_field = frame.first_field;
      mapped.second_field = frame.second_field;
      final_frames.push_back(mapped);

      prev_pn = current_pn;
    } else {
      // Frames without PN - include them but don't use for gap detection
      // (Section 11: "Do not invent PN" but still include for continuity)
      MappedFrame mapped;
      mapped.first_field = frame.first_field;
      mapped.second_field = frame.second_field;
      final_frames.push_back(mapped);
      frames_without_pn_included++;
    }
  }

  // Re-attach the retained lead frames to the ends of the timeline. They are
  // added after gap detection so that their absent (or zero) picture numbers
  // cannot be mistaken for a gap in the programme numbering.
  if (lead_in_frame) {
    MappedFrame mapped;
    mapped.first_field = lead_in_frame->first_field;
    mapped.second_field = lead_in_frame->second_field;
    final_frames.insert(final_frames.begin(), mapped);
  }
  if (lead_out_frame) {
    MappedFrame mapped;
    mapped.first_field = lead_out_frame->first_field;
    mapped.second_field = lead_out_frame->second_field;
    final_frames.push_back(mapped);
  }

  decision.stats.gaps_padded = gaps_padded;
  decision.stats.padding_frames = padding_frames;

  if (progress) progress->setProgress(100);
  rationale << "Stage 5: Gap Detection and Timeline Construction\n";
  rationale << "  Gaps detected: " << gaps_padded << "\n";
  rationale << "  PAD frames inserted: " << padding_frames << "\n";
  rationale << "  Frames without PN (included): " << frames_without_pn_included
            << "\n";
  if (lead_in_frame || lead_out_frame) {
    rationale << "  Lead frames re-attached: "
              << (lead_in_frame ? "lead-in" : "")
              << (lead_in_frame && lead_out_frame ? ", " : "")
              << (lead_out_frame ? "lead-out" : "") << "\n";
  }
  rationale << "  Final frame count: " << final_frames.size() << "\n";
  rationale << "  Frame map: "
            << generate_frame_map(final_frames, !decision.is_cav, fmt)
            << "\n\n";

  // ========================================================================
  // Stage 6: Generate mapping specification with range notation
  // ========================================================================

  if (progress) {
    progress->setStatus("Generating mapping specification...");
    progress->setProgress(0);
  }

  // Build a list of source FrameIDs or PAD markers — one entry per output
  // frame. frame_map operates at frame granularity (not field granularity):
  // the FieldID convention is FieldID = FrameID * 2 + field_index, so the
  // source FrameID is recovered as first_field / 2.
  std::vector<std::string> frame_list;
  for (const auto& frame : final_frames) {
    if (frame.is_pad) {
      frame_list.push_back("PAD");
    } else if (frame.first_field) {
      uint64_t src_frame_id = frame.first_field->value() / 2;
      frame_list.push_back(std::to_string(src_frame_id));
    }
  }

  ORC_LOG_DEBUG("Frame list size: {}, final_frames size: {}", frame_list.size(),
                final_frames.size());

  // Collapse consecutive sequences into ranges
  std::ostringstream spec;
  size_t i = 0;
  while (i < frame_list.size()) {
    const auto& current = frame_list[i];

    // Handle PAD sequences — always emit PAD_N (including N=1) because the
    // frame_map parser only recognises the PAD_<count> form.
    if (current == "PAD") {
      size_t pad_count = 1;
      while (i + pad_count < frame_list.size() &&
             frame_list[i + pad_count] == "PAD") {
        pad_count++;
      }

      if (i > 0) spec << ",";
      spec << "PAD_" << pad_count;

      i += pad_count;
      continue;
    }

    // Try to parse as number
    int start_num = std::stoi(current);
    int end_num = start_num;
    size_t j = i + 1;

    // Look ahead for consecutive numbers
    while (j < frame_list.size() && frame_list[j] != "PAD") {
      int next_num = std::stoi(frame_list[j]);
      if (next_num == end_num + 1) {
        end_num = next_num;
        j++;
      } else {
        break;
      }
    }

    // Output range or single number
    if (i > 0) spec << ",";
    if (j - i >= 3) {
      // Use range notation for 3+ consecutive numbers
      spec << start_num << "-" << end_num;
    } else {
      // Output individual numbers
      for (size_t k = i; k < j; k++) {
        if (k > i) spec << ",";
        spec << frame_list[k];
      }
    }

    i = j;
  }

  decision.mapping_spec = spec.str();

  decision.rationale = rationale.str();
  decision.success = true;

  if (progress) {
    progress->setStatus("Analysis complete");
    progress->setProgress(100);
  }

  ORC_LOG_INFO("Disc mapping analysis complete: {} frames ({} PAD)",
               final_frames.size(), padding_frames);

  return decision;
}

}  // namespace orc
