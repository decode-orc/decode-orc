/*
 * File:        alignment_support.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     Picture numbers, the Disc Mapper and Frame Map for the
 *              videosynth suite's alignment and stacking cases
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "alignment_support.h"

#include <biphase_observer.h>
#include <black_psnr_observer.h>
#include <burst_level_observer.h>
#include <colour_frame_phase_observer.h>
#include <gtest/gtest.h>
#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context.h>
#include <white_snr_observer.h>

#include <map>
#include <variant>

namespace orc_videosynth {

std::vector<std::optional<int32_t>> read_pictures(
    const orc::VideoFrameRepresentation& vfr) {
  orc::BiphaseObserver biphase;
  orc::ObservationContext ctx;
  std::vector<std::optional<int32_t>> pictures;
  for (orc::FrameID f = 0; f < vfr.frame_count(); ++f) {
    biphase.process_frame(vfr, f, ctx);
    // Either field may carry it: film numbers a picture on the field that
    // starts it, field 2 on two frames in five.
    std::optional<int32_t> picture;
    for (uint64_t field = 0; field < 2 && !picture; ++field) {
      const auto value =
          ctx.get(orc::FieldID(f * 2 + field), "vbi", "picture_number");
      if (value && std::holds_alternative<int32_t>(*value)) {
        picture = std::get<int32_t>(*value);
      }
    }
    pictures.push_back(picture);
  }
  return pictures;
}

std::vector<orc::PictureNumberReading> readings_of(
    const std::vector<std::optional<int32_t>>& pictures) {
  std::vector<orc::PictureNumberReading> readings;
  for (size_t i = 0; i < pictures.size(); ++i) {
    if (pictures[i]) readings.push_back({i, *pictures[i]});
  }
  return readings;
}

orc::FieldMappingDecision map_disc(const orc::VideoFrameRepresentation& vfr) {
  orc::BiphaseObserver biphase;
  orc::ColourFramePhaseObserver phase;
  orc::BurstLevelObserver burst;
  orc::WhiteSNRObserver white;
  orc::BlackPSNRObserver black;
  orc::ObservationContext ctx;
  for (orc::FrameID f = 0; f < vfr.frame_count(); ++f) {
    biphase.process_frame(vfr, f, ctx);
    phase.process_frame(vfr, f, ctx);
    burst.process_frame(vfr, f, ctx);
    white.process_frame(vfr, f, ctx);
    black.process_frame(vfr, f, ctx);
  }
  return orc::DiscMapperAnalyzer().analyze(vfr, ctx);
}

orc::ArtifactPtr apply_frame_map(const orc::ArtifactPtr& capture,
                                 orc::FrameMapStage& stage,
                                 const std::string& ranges) {
  const std::map<std::string, orc::ParameterValue> parameters{
      {"ranges", ranges}};
  if (!stage.set_parameters(parameters)) {
    ADD_FAILURE() << "frame_map rejected '" << ranges << "'";
    return nullptr;
  }
  orc::ObservationContext ctx;
  const auto outputs = stage.execute({capture}, parameters, ctx);
  if (outputs.size() != 1) {
    ADD_FAILURE() << "frame_map gave no output";
    return nullptr;
  }
  return outputs[0];
}

}  // namespace orc_videosynth
