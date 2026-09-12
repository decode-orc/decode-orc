/*
 * File:        biphase_observer_test.cpp
 * Module:      orc-tests/core/unit/observers
 * Purpose:     Unit tests for BiphaseObserver line access and VBI extraction
 *
 * Tests: VBI word extraction from both fields including the PAL
 * alternating-line-width geometry; frame-level rather than per-line source
 * access; YC (separate channel) access; scratch-buffer reuse across frames
 * and in the transition-map helper.
 * Frame data is synthesised in memory; no I/O is performed.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <biphase_observer.h>
#include <gtest/gtest.h>
#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/field_id.h>
#include <orc/stage/frame_descriptor.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/vbi_utilities.h>

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace orc {
namespace tests {

namespace {

// ---------------------------------------------------------------------------
// Frame synthesis
// ---------------------------------------------------------------------------

// Total samples in a PAL frame, honouring the lines that carry extra samples
// (kPalExtraSampleLines), so the flat buffer matches real 4FSC geometry.
size_t pal_frame_sample_count() {
  const auto spl = static_cast<size_t>(kPalSamplesPerLineNominal);
  return frame_line_sample_offset(VideoSystem::PAL, spl,
                                  static_cast<size_t>(kPalFrameLines));
}

SourceParameters make_pal_params() {
  SourceParameters p{};
  p.system = VideoSystem::PAL;
  p.frame_width_nominal = kPalSamplesPerLineNominal;
  p.frame_height = kPalFrameLines;
  p.blanking_level = kPalBlanking;
  p.white_level = kPalWhite;
  p.active_video_start = kPalActiveVideoStart;
  return p;
}

// Write a 24-bit biphase (Manchester) word into |line|, matching what the
// decoder expects: a 2 us cell per bit, a 1 -> low half then high half (a 01
// transition), a 0 -> high half then low half (a 10 transition).
// IEC 60857-1986 §10.1: VBI words are 24 bits with the MSB always set.
void write_biphase_word(int16_t* line, size_t width, uint32_t word,
                        size_t active_start) {
  const double cell = kPalSampleRate * 2e-6;  // samples per 2 us bit cell
  const auto low = static_cast<int16_t>(kPalBlanking);
  const auto high = static_cast<int16_t>(kPalWhite);

  for (size_t i = 0; i < width; ++i) line[i] = low;

  for (int bit = 23; bit >= 0; --bit) {
    const size_t index = 23 - static_cast<size_t>(bit);
    const double cell_start =
        static_cast<double>(active_start) + static_cast<double>(index) * cell;
    const double mid = cell_start + cell / 2.0;
    const double cell_end = cell_start + cell;
    const bool is_one = ((word >> bit) & 1u) != 0u;

    // First half.
    for (auto s = static_cast<size_t>(cell_start); s < static_cast<size_t>(mid);
         ++s) {
      if (s < width) line[s] = is_one ? low : high;
    }
    // Second half.
    for (auto s = static_cast<size_t>(mid); s < static_cast<size_t>(cell_end);
         ++s) {
      if (s < width) line[s] = is_one ? high : low;
    }
  }
}

// Build a PAL frame carrying |w16|/|w17|/|w18| on VBI lines 16/17/18 of both
// fields (0-based field lines 15/16/17).
std::vector<int16_t> make_pal_frame_with_vbi(uint32_t w16, uint32_t w17,
                                             uint32_t w18) {
  const auto spl = static_cast<size_t>(kPalSamplesPerLineNominal);
  std::vector<int16_t> samples(pal_frame_sample_count(),
                               static_cast<int16_t>(kPalBlanking));
  const size_t f1 = field1_lines(VideoSystem::PAL);
  const uint32_t words[3] = {w16, w17, w18};

  for (size_t field = 0; field < 2; ++field) {
    const size_t line_offset = (field == 0) ? 0 : f1;
    for (size_t i = 0; i < 3; ++i) {
      const size_t flat_line = line_offset + 15 + i;
      const size_t offset =
          frame_line_sample_offset(VideoSystem::PAL, spl, flat_line);
      const size_t count =
          frame_line_sample_count(VideoSystem::PAL, spl, flat_line);
      write_biphase_word(samples.data() + offset, count, words[i],
                         static_cast<size_t>(kPalActiveVideoStart));
    }
  }
  return samples;
}

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

// Base VFR over a flat PAL frame. get_line_samples() is NOT overridden, so it
// falls through the base default to get_line() -> get_frame(): the whole-frame
// access path the observer used before targeted per-line reads.
class FullFrameVFR : public VideoFrameRepresentation {
 public:
  FullFrameVFR(std::vector<int16_t> samples, SourceParameters params)
      : samples_(std::move(samples)), params_(std::move(params)) {}

  FrameIDRange frame_range() const override { return {0, 0}; }
  size_t frame_count() const override { return 1; }
  bool has_frame(FrameID id) const override { return id == FrameID{0}; }

  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    if (id != FrameID{0}) return std::nullopt;
    FrameDescriptor d;
    d.frame_id = id;
    d.system = params_.system;
    d.height = static_cast<size_t>(params_.frame_height);
    d.samples_total = samples_.size();
    d.samples_per_line_nominal =
        static_cast<size_t>(params_.frame_width_nominal);
    return d;
  }

  const sample_type* get_frame(FrameID id) const override {
    if (id != FrameID{0}) return nullptr;
    ++frame_loads_;
    return samples_.data();
  }

  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    return (id == FrameID{0}) ? samples_ : std::vector<sample_type>{};
  }

  std::optional<SourceParameters> get_video_parameters() const override {
    return params_;
  }

  size_t frame_loads() const { return frame_loads_; }

 protected:
  std::vector<int16_t> samples_;
  SourceParameters params_;
  mutable size_t frame_loads_ = 0;
};

// Composite source offering the targeted per-line override a real CVBS source
// provides, purely so a test can detect whether the observer takes it.
class PerLineProbeVFR : public FullFrameVFR {
 public:
  using FullFrameVFR::FullFrameVFR;

  std::vector<sample_type> get_line_samples(FrameID id,
                                            size_t line) const override {
    if (id != FrameID{0} || line >= static_cast<size_t>(params_.frame_height)) {
      return {};
    }
    requested_lines_.push_back(line);
    const auto spl = static_cast<size_t>(params_.frame_width_nominal);
    const size_t offset = frame_line_sample_offset(params_.system, spl, line);
    const size_t count = frame_line_sample_count(params_.system, spl, line);
    return std::vector<sample_type>(samples_.begin() + offset,
                                    samples_.begin() + offset + count);
  }

  const std::vector<size_t>& requested_lines() const {
    return requested_lines_;
  }

 private:
  mutable std::vector<size_t> requested_lines_;
};

// YC source: luma carries the VBI, so the observer takes the get_line_luma()
// path rather than get_line_samples().
class YcVFR : public FullFrameVFR {
 public:
  using FullFrameVFR::FullFrameVFR;

  bool has_separate_channels() const override { return true; }

  const sample_type* get_frame_luma(FrameID id) const override {
    return get_frame(id);
  }

  const sample_type* get_line_luma(FrameID id, size_t line) const override {
    const sample_type* frame = get_frame_luma(id);
    if (!frame || line >= static_cast<size_t>(params_.frame_height)) {
      return nullptr;
    }
    const auto spl = static_cast<size_t>(params_.frame_width_nominal);
    return frame + frame_line_sample_offset(params_.system, spl, line);
  }
};

// Representative IEC 60857 VBI words: a CLV programme time code of 0h04m on
// lines 17/18 and a CLV picture number (47 s, picture 10) on line 16.
constexpr uint32_t kVbiLine16 = 0x8EE710;
constexpr uint32_t kVbiLine17 = 0xF0DD04;
constexpr uint32_t kVbiLine18 = 0xF0DD04;

// Read an int32 observation, or a sentinel when absent/of another type, so a
// missing value fails the comparison rather than the harness.
int32_t read_int(const ObservationContext& ctx, FieldID field_id,
                 const std::string& ns, const std::string& key) {
  const auto value = ctx.get(field_id, ns, key);
  if (!value || !std::holds_alternative<int32_t>(*value)) return -1;
  return std::get<int32_t>(*value);
}

std::string value_to_string(const ObservationValue& value) {
  return std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return v;
        } else if constexpr (std::is_same_v<T, bool>) {
          return v ? "true" : "false";
        } else {
          return std::to_string(v);
        }
      },
      value);
}

// Collect every observation the observer stored, as a comparable flat list.
// get_all_observations() returns sorted maps, so the ordering is stable.
std::vector<std::string> collect_observations(const ObservationContext& ctx,
                                              size_t field_count) {
  std::vector<std::string> out;
  for (size_t f = 0; f < field_count; ++f) {
    for (const auto& [ns, entries] : ctx.get_all_observations(FieldID(f))) {
      for (const auto& [key, value] : entries) {
        out.push_back(std::to_string(f) + "/" + ns + "/" + key + "=" +
                      value_to_string(value));
      }
    }
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Line access
// ---------------------------------------------------------------------------

// The VBI words must come back exactly as written, on both fields. Field 2's
// flat line index sits beyond PAL line 312, which carries extra samples, so
// this also pins the alternating-line-width geometry.
TEST(BiphaseObserver, DecodesVbiWordsOnBothFields_WhenSourceIsComposite) {
  FullFrameVFR vfr(make_pal_frame_with_vbi(kVbiLine16, kVbiLine17, kVbiLine18),
                   make_pal_params());
  ObservationContext ctx;
  BiphaseObserver observer;

  observer.process_frame(vfr, FrameID{0}, ctx);

  for (size_t field = 0; field < 2; ++field) {
    const FieldID fid(field);
    EXPECT_EQ(read_int(ctx, fid, "biphase", "vbi_line_16"),
              static_cast<int32_t>(kVbiLine16));
    EXPECT_EQ(read_int(ctx, fid, "biphase", "vbi_line_17"),
              static_cast<int32_t>(kVbiLine17));
    EXPECT_EQ(read_int(ctx, fid, "biphase", "vbi_line_18"),
              static_cast<int32_t>(kVbiLine18));
  }
}

// Only 6 of a frame's ~625 lines are read, which makes a targeted per-line
// read look like the cheaper call. It is not: the cost of reading a CVBS
// source is per-read round-trip latency, not bytes moved, so one whole-frame
// read per frame measures roughly twice as fast over NFS as the equivalent
// per-line reads — and it warms the cache that the burst/SNR/PSNR observers'
// own per-line reads then hit for free. This test pins that choice; see the
// rationale on the read in BiphaseObserver::process_frame().
TEST(BiphaseObserver, ReadsThroughFrameAccessor_NotPerLine_ForCompositeSource) {
  PerLineProbeVFR vfr(
      make_pal_frame_with_vbi(kVbiLine16, kVbiLine17, kVbiLine18),
      make_pal_params());
  ObservationContext ctx;
  BiphaseObserver observer;

  observer.process_frame(vfr, FrameID{0}, ctx);

  EXPECT_TRUE(vfr.requested_lines().empty())
      << "per-line reads are slower than one shared frame read";
  EXPECT_GT(vfr.frame_loads(), 0u);
  EXPECT_FALSE(collect_observations(ctx, 2).empty())
      << "the frame path must still decode the VBI";
}

// YC sources take the luma-plane path: the VBI lives in luma. Both paths read
// the same geometry, so they must agree.
TEST(BiphaseObserver, ReadsLumaPlane_WhenSourceHasSeparateChannels) {
  const auto samples =
      make_pal_frame_with_vbi(kVbiLine16, kVbiLine17, kVbiLine18);

  YcVFR yc(samples, make_pal_params());
  ObservationContext yc_ctx;
  BiphaseObserver yc_observer;
  yc_observer.process_frame(yc, FrameID{0}, yc_ctx);

  FullFrameVFR composite(samples, make_pal_params());
  ObservationContext composite_ctx;
  BiphaseObserver composite_observer;
  composite_observer.process_frame(composite, FrameID{0}, composite_ctx);

  EXPECT_EQ(collect_observations(yc_ctx, 2),
            collect_observations(composite_ctx, 2));
}

// A frame whose VBI lines carry no biphase signal must store nothing rather
// than publishing zeroed observations.
TEST(BiphaseObserver, StoresNothing_WhenVbiLinesCarryNoSignal) {
  std::vector<int16_t> blank(pal_frame_sample_count(),
                             static_cast<int16_t>(kPalBlanking));
  FullFrameVFR vfr(std::move(blank), make_pal_params());
  ObservationContext ctx;
  BiphaseObserver observer;

  observer.process_frame(vfr, FrameID{0}, ctx);

  EXPECT_TRUE(collect_observations(ctx, 2).empty());
}

// One observer instance decodes many frames in the analysis passes; the
// scratch buffers it reuses must not leak state between frames.
TEST(BiphaseObserver, ProducesSameResult_WhenInstanceIsReusedAcrossFrames) {
  FullFrameVFR vfr(make_pal_frame_with_vbi(kVbiLine16, kVbiLine17, kVbiLine18),
                   make_pal_params());
  BiphaseObserver observer;

  ObservationContext first_ctx;
  observer.process_frame(vfr, FrameID{0}, first_ctx);

  std::vector<int16_t> blank(pal_frame_sample_count(),
                             static_cast<int16_t>(kPalBlanking));
  FullFrameVFR blank_vfr(std::move(blank), make_pal_params());
  ObservationContext blank_ctx;
  observer.process_frame(blank_vfr, FrameID{0}, blank_ctx);
  EXPECT_TRUE(collect_observations(blank_ctx, 2).empty())
      << "a blank frame must not inherit the previous frame's scratch data";

  ObservationContext second_ctx;
  observer.process_frame(vfr, FrameID{0}, second_ctx);
  EXPECT_EQ(collect_observations(first_ctx, 2),
            collect_observations(second_ctx, 2));
}

// ---------------------------------------------------------------------------
// Transition-map scratch buffer
// ---------------------------------------------------------------------------

TEST(VbiUtilsTransitionMap, MatchesReturningForm_WhenFilledIntoBuffer) {
  std::vector<int16_t> line(400, 100);
  for (size_t i = 120; i < 260; ++i) line[i] = 900;
  for (size_t i = 300; i < 340; ++i) line[i] = 900;

  const auto expected =
      vbi_utils::get_transition_map(line.data(), line.size(), 500);

  std::vector<uint8_t> out;
  vbi_utils::get_transition_map_into(line.data(), line.size(), 500, out);

  EXPECT_EQ(out, expected);
}

// Reuse is the point of the buffer form: a caller decoding line after line
// must not pay an allocation per line.
TEST(VbiUtilsTransitionMap, RetainsCapacity_WhenBufferIsReused) {
  std::vector<int16_t> line(1135, 100);
  for (size_t i = 200; i < 700; ++i) line[i] = 900;

  std::vector<uint8_t> buffer;
  vbi_utils::get_transition_map_into(line.data(), line.size(), 500, buffer);
  const auto* first_data = buffer.data();
  const size_t first_capacity = buffer.capacity();

  vbi_utils::get_transition_map_into(line.data(), line.size(), 500, buffer);

  EXPECT_EQ(buffer.data(), first_data) << "second call must not reallocate";
  EXPECT_EQ(buffer.capacity(), first_capacity);
}

// A shorter line must shrink the map, not leave stale samples beyond its end:
// PAL line widths alternate 1135/1136, so consecutive calls differ in length.
TEST(VbiUtilsTransitionMap, ResizesToSampleCount_WhenLineIsShorterThanBuffer) {
  std::vector<int16_t> long_line(1136, 900);
  std::vector<uint8_t> buffer;
  vbi_utils::get_transition_map_into(long_line.data(), long_line.size(), 500,
                                     buffer);
  ASSERT_EQ(buffer.size(), 1136u);

  std::vector<int16_t> short_line(1135, 100);
  vbi_utils::get_transition_map_into(short_line.data(), short_line.size(), 500,
                                     buffer);

  EXPECT_EQ(buffer.size(), 1135u);
  const auto expected =
      vbi_utils::get_transition_map(short_line.data(), short_line.size(), 500);
  EXPECT_EQ(buffer, expected);
}

}  // namespace tests
}  // namespace orc
