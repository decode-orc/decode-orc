/*
 * File:        colour_frame_phase_query.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     Measure a frame's colour-sequence phase via the host observer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/abi/orc_plugin_services.h>
#include <orc/stage/field_id.h>
#include <orc/stage/frame_id.h>
#include <orc/stage/observation/observation_context_interface.h>
#include <orc/stage/observation/observation_service_interface.h>
#include <orc/stage/video_frame_representation.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

// Colour-sequence phase (which frame/field a sample sits at within the colour
// subcarrier cycle) is a property of the burst signal, not of source-side
// metadata: it must be measured the same way for TBC and CVBS sources, and it
// is the ColourFramePhaseObserver that measures it.  These helpers let any
// stage pull that measurement on demand via the host observation service,
// rather than reading a value the source stage baked into FrameDescriptor.
//
// Measurement reads the frame's samples (to demodulate the burst); callers in
// hot loops should cache the result per frame id.  A blank/padding frame (no
// burst) yields all -1.
//
// IMPORTANT — these values are comparable only WITHIN a single source.  The
// burst is demodulated against the frame buffer's own sample grid, and PAL's
// TBC layout does not pin subcarrier phase to sample index the way NTSC's 910
// samples/line = 227.5 subcarrier cycles does.  Two decodes of the same disc
// whose line starts land one 4FSC sample apart — 90° of subcarrier, well
// inside what sync-edge fitting varies by between runs — report colour frame
// indices two apart for the very same picture.  Comparing them across sources
// to align frames therefore reports a difference where the content has none;
// align sources by VBI frame number (frame_map / source_align) instead, and
// correct sample-grid differences by shifting samples.
namespace orc::observation {

namespace detail {

// Minimal in-memory IObservationContext used to capture a single observer
// pass so the values can be read back.  Stage plugins do not link liborc-core,
// where the full ObservationContext lives; the colour_frame_phase observer only
// needs set()/get(), so the remaining interface methods are stubs.
class CaptureContext : public IObservationContext {
 public:
  void set(FieldID field_id, const std::string& namespace_,
           const std::string& key, const ObservationValue& value) override {
    values_[{field_id.value(), namespace_, key}] = value;
  }
  std::optional<ObservationValue> get(FieldID field_id,
                                      const std::string& namespace_,
                                      const std::string& key) const override {
    auto it = values_.find({field_id.value(), namespace_, key});
    if (it == values_.end()) return std::nullopt;
    return it->second;
  }
  bool has(FieldID field_id, const std::string& namespace_,
           const std::string& key) const override {
    return values_.count({field_id.value(), namespace_, key}) > 0;
  }
  std::vector<std::string> get_keys(FieldID,
                                    const std::string&) const override {
    return {};
  }
  std::vector<std::string> get_namespaces(FieldID) const override { return {}; }
  std::map<std::string, std::map<std::string, ObservationValue>>
  get_all_observations(FieldID) const override {
    return {};
  }
  void clear() override { values_.clear(); }
  void clear_field(FieldID) override {}
  void register_schema(const std::vector<ObservationKey>&) override {}
  void clear_schema() override {}

 private:
  std::map<std::tuple<uint64_t, std::string, std::string>, ObservationValue>
      values_;
};

inline int32_t read_phase_int(const CaptureContext& ctx, FieldID field_id,
                              const std::string& key) {
  auto value = ctx.get(field_id, "colour_frame_phase", key);
  if (value && std::holds_alternative<int32_t>(*value)) {
    return std::get<int32_t>(*value);
  }
  return -1;
}

}  // namespace detail

// Colour-sequence phase for one frame, measured from the burst signal.
//   colour_frame_index: 0/1 (NTSC A/B) or 1-4 (PAL / PAL_M); -1 unknown.
//   fieldN_phase_id:    per-field 1-4 (NTSC) or 1-8 (PAL / PAL_M); -1 unknown.
struct FramePhase {
  int32_t colour_frame_index = -1;
  int32_t field1_phase_id = -1;
  int32_t field2_phase_id = -1;
};

// Measure the colour-sequence phase of |frame_id| by running the host
// "colour_frame_phase" observer over the frame's burst.  Returns all -1 when
// the observation service is unavailable, the observer is not registered, or
// the burst is not measurable (e.g. a blank padding frame).  Stateless and
// thread-safe: each call creates its own observer handle and context.
inline FramePhase measure_frame_phase(const VideoFrameRepresentation& vfr,
                                      FrameID frame_id) {
  FramePhase out;

  IObservationService* service = orc::plugin::get_observation_service();
  if (!service) return out;

  std::unique_ptr<IObserverHandle> observer =
      service->create_observer("colour_frame_phase");
  if (!observer) return out;

  detail::CaptureContext ctx;
  observer->process_frame(vfr, frame_id, ctx);

  const FieldID field1(static_cast<FieldID::value_type>(frame_id) * 2);
  const FieldID field2(static_cast<FieldID::value_type>(frame_id) * 2 + 1);
  out.colour_frame_index =
      detail::read_phase_int(ctx, field1, "colour_frame_index");
  out.field1_phase_id = detail::read_phase_int(ctx, field1, "field_phase_id");
  out.field2_phase_id = detail::read_phase_int(ctx, field2, "field_phase_id");
  return out;
}

// Convenience wrapper returning only the frame-level colour_frame_index.
inline int32_t measure_colour_frame_index(const VideoFrameRepresentation& vfr,
                                          FrameID frame_id) {
  return measure_frame_phase(vfr, frame_id).colour_frame_index;
}

}  // namespace orc::observation
