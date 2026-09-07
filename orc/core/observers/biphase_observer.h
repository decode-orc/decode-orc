/*
 * File:        biphase_observer.h
 * Module:      orc-core
 * Purpose:     Biphase VBI data extraction observer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef ORC_CORE_BIPHASE_OBSERVER_H
#define ORC_CORE_BIPHASE_OBSERVER_H

#include <observer.h>

#include <memory>

namespace orc {

// Forward declarations
class ObservationContext;
class VideoFrameRepresentation;

/**
 * @brief Observer that extracts biphase-encoded VBI data
 *
 * This observer reads VBI data from the video frame representation
 * and populates the observation context with raw biphase data.
 * The data can then be decoded by VBIDecoder or other analysis tools.
 */
class BiphaseObserver : public Observer {
 public:
  BiphaseObserver() = default;
  ~BiphaseObserver() override = default;

  std::string observer_name() const override { return "BiphaseObserver"; }
  // 1.1.0: CAV picture numbers are cross-validated across VBI lines 17 and
  // 18 and publish "picture_number_cross_validated". Stored observations from
  // 1.0.0 carry uncross-validated picture numbers and must be recomputed.
  std::string observer_version() const override { return "1.1.0"; }

  void process_frame(const VideoFrameRepresentation& representation,
                     FrameID frame_id, IObservationContext& context) override;

  std::vector<ObservationKey> get_provided_observations() const override {
    return {
        {"biphase", "vbi_line_16", ObservationType::INT32,
         "VBI line 16 raw data"},
        {"biphase", "vbi_line_17", ObservationType::INT32,
         "VBI line 17 raw data"},
        {"biphase", "vbi_line_18", ObservationType::INT32,
         "VBI line 18 raw data"},
        {"vbi", "picture_number", ObservationType::INT32,
         "CAV picture number (if available)"},
        {"vbi", "picture_number_cross_validated", ObservationType::INT32,
         "CAV picture number confirmed by both VBI lines 17 and 18"},
        {"vbi", "chapter_number", ObservationType::INT32,
         "Chapter number (if available)"},
        {"vbi", "clv_timecode_hours", ObservationType::INT32,
         "CLV timecode hours"},
        {"vbi", "clv_timecode_minutes", ObservationType::INT32,
         "CLV timecode minutes"},
        {"vbi", "clv_timecode_seconds", ObservationType::INT32,
         "CLV timecode seconds"},
        {"vbi", "clv_timecode_picture", ObservationType::INT32,
         "CLV timecode picture number"},
        {"vbi", "lead_in", ObservationType::INT32, "Lead-in code present"},
        {"vbi", "lead_out", ObservationType::INT32, "Lead-out code present"},
        {"vbi", "stop_code_present", ObservationType::INT32,
         "Picture stop code present"},
        {"vbi", "user_code", ObservationType::STRING,
         "User code (if available)"},
        {"vbi", "programme_status_cx_enabled", ObservationType::INT32,
         "Programme status: CX enabled"},
        {"vbi", "programme_status_is_12_inch", ObservationType::INT32,
         "Programme status: 12 inch disc"},
        {"vbi", "programme_status_is_side_1", ObservationType::INT32,
         "Programme status: side 1"},
        {"vbi", "programme_status_has_teletext", ObservationType::INT32,
         "Programme status: teletext present"},
        {"vbi", "programme_status_is_digital", ObservationType::INT32,
         "Programme status: digital video"},
        {"vbi", "programme_status_sound_mode", ObservationType::INT32,
         "Programme status: sound mode"},
        {"vbi", "programme_status_is_fm_multiplex", ObservationType::INT32,
         "Programme status: FM multiplex"},
        {"vbi", "programme_status_is_programme_dump", ObservationType::INT32,
         "Programme status: programme dump"},
        {"vbi", "programme_status_parity_valid", ObservationType::INT32,
         "Programme status: parity valid"},
        {"vbi", "amendment2_status_copy_permitted", ObservationType::INT32,
         "Amendment 2: copy permitted"},
        {"vbi", "amendment2_status_is_video_standard", ObservationType::INT32,
         "Amendment 2: video standard"},
        {"vbi", "amendment2_status_sound_mode", ObservationType::INT32,
         "Amendment 2: sound mode"},
    };
  }
};

}  // namespace orc

#endif  // ORC_CORE_BIPHASE_OBSERVER_H
