/*
 * File:        still_frame_stacking_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the Stacker's still-frame stacking: finding the
 *              repeats of a held still within a source and stacking them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/support/frame_line_util.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <vector>

#include "../../../../orc/plugins/stages/stacker/stacker_stage.h"

namespace orc_unit_test {
namespace {

using orc::DropoutRun;
using orc::FrameID;
using orc::StackedVideoFrameRepresentation;
using orc::VideoSystem;
using sample_type = orc::VideoFrameRepresentation::sample_type;
using StillCopy = StackedVideoFrameRepresentation::StillCopy;

constexpr double kPi = 3.14159265358979323846;

struct Format {
  VideoSystem system;
  size_t width;
  size_t height;
  size_t frame_samples;
  int32_t black;
};

const Format kPal{VideoSystem::PAL, 1135, 625, 709379, 256};
const Format kNtsc{VideoSystem::NTSC, 910, 525, 477750, 282};

// Picture shown by each field of each frame.  Different ids are different
// pictures.
using PictureOf = std::function<int(size_t frame, size_t field)>;

PictureOf still_then_motion(size_t still_frames) {
  return [still_frames](size_t frame, size_t) {
    return frame < still_frames ? 0 : static_cast<int>(100 + frame);
  };
}

PictureOf one_still() {
  return [](size_t, size_t) { return 0; };
}

// Not separable into a row factor and a column factor: same_picture fits
// each row of blocks with a gain, which would absorb a different picture
// that only rescaled its rows.
double picture_value(int picture, size_t line, size_t n) {
  const double p = picture;
  const auto x = static_cast<double>(n);
  const auto y = static_cast<double>(line);
  return 500.0 +
         100.0 * std::sin(x / 37.0 + 1.3 * p) * std::cos(y / 23.0 + 0.7 * p) +
         60.0 * std::sin((x + 3.0 * y) / 29.0 + 2.1 * p);
}

// A source of synthetic frames in a continuous 4FSC recording, so the
// subcarrier phase follows the real colour cadence: textured luma, a
// uniform colour (PAL with its V switch), and white noise.  Frames are made
// once, up front.
class FakeStillSource : public orc::VideoFrameRepresentation,
                        public orc::Artifact {
 public:
  struct Options {
    Format fmt = kPal;
    size_t frames = 40;
    PictureOf picture = one_still();
    double sigma = 5.0;
    uint32_t seed = 1;
    bool chroma = true;
    bool yc = false;
    // Frames whose samples sit this many samples late on every line.
    std::map<size_t, int> slips;
    // The sample, per frame, of a 1-sample outline 150 codes bright on field
    // lines 100-159 of both fields; none where it gives nullopt.
    std::function<std::optional<size_t>(size_t frame)> outline;
    std::map<size_t, std::vector<DropoutRun>> dropouts;
  };

  explicit FakeStillSource(Options options)
      : orc::Artifact(orc::ArtifactID("fake_still_source"), orc::Provenance{}),
        options_(std::move(options)) {
    for (size_t f = 0; f < options_.frames; ++f) {
      std::vector<sample_type> luma;
      std::vector<sample_type> chroma;
      make(f, options_.sigma, luma, chroma);
      luma_.push_back(std::move(luma));
      chroma_.push_back(std::move(chroma));
    }
  }

  std::string type_name() const override { return "fake_still_source"; }

  // Frame |f| without noise: composite, or luma for a YC source.
  std::vector<sample_type> clean(size_t f) const {
    std::vector<sample_type> luma;
    std::vector<sample_type> chroma;
    make(f, 0.0, luma, chroma);
    return luma;
  }

  orc::FrameIDRange frame_range() const override {
    return {FrameID{0}, FrameID{options_.frames - 1}};
  }
  size_t frame_count() const override { return options_.frames; }
  bool has_frame(FrameID id) const override { return id < options_.frames; }

  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    if (!has_frame(id)) return std::nullopt;
    orc::FrameDescriptor desc;
    desc.frame_id = id;
    desc.system = options_.fmt.system;
    desc.height = options_.fmt.height;
    desc.samples_total = options_.fmt.frame_samples;
    desc.samples_per_line_nominal = options_.fmt.width;
    return desc;
  }

  const sample_type* get_frame(FrameID id) const override {
    return has_frame(id) && !options_.yc ? luma_[id].data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    return has_frame(id) && !options_.yc ? luma_[id]
                                         : std::vector<sample_type>{};
  }

  bool has_separate_channels() const override { return options_.yc; }
  const sample_type* get_frame_luma(FrameID id) const override {
    return has_frame(id) && options_.yc ? luma_[id].data() : nullptr;
  }
  const sample_type* get_frame_chroma(FrameID id) const override {
    return has_frame(id) && options_.yc ? chroma_[id].data() : nullptr;
  }

  std::vector<DropoutRun> get_dropout_hints(FrameID id) const override {
    const auto it = options_.dropouts.find(id);
    return it == options_.dropouts.end() ? std::vector<DropoutRun>{}
                                         : it->second;
  }

  std::optional<orc::SourceParameters> get_video_parameters() const override {
    orc::SourceParameters params;
    params.system = options_.fmt.system;
    params.frame_width_nominal = static_cast<int32_t>(options_.fmt.width);
    params.frame_height = static_cast<int32_t>(options_.fmt.height);
    params.black_level = options_.fmt.black;
    params.number_of_sequential_frames = static_cast<int32_t>(options_.frames);
    return params;
  }

  size_t audio_channel_pair_count() const override { return 1; }
  std::vector<int32_t> get_audio_samples(size_t pair,
                                         FrameID id) const override {
    return {static_cast<int32_t>(id * 10 + pair), -7, 1000};
  }
  bool has_efm() const override { return true; }
  uint32_t get_efm_sample_count(FrameID) const override { return 3; }
  // Packed t-values with a doubt nibble, which the Mean mode would strip.
  std::vector<uint8_t> get_efm_samples(FrameID id) const override {
    return {static_cast<uint8_t>(0x30 | (3 + id % 8)), 0x24, 0x1B};
  }

 private:
  void make(size_t f, double sigma, std::vector<sample_type>& luma,
            std::vector<sample_type>& chroma) const {
    const Format& fmt = options_.fmt;
    luma.assign(fmt.frame_samples, 0);
    if (options_.yc) chroma.assign(fmt.frame_samples, 0);
    std::mt19937 rng(options_.seed * 100003U + static_cast<uint32_t>(f));
    std::normal_distribution<double> noise(0.0, sigma);
    const auto slip_it = options_.slips.find(f);
    const int slip = slip_it == options_.slips.end() ? 0 : slip_it->second;
    const size_t field1 = orc::field1_line_count(fmt.system);
    const double u = options_.chroma ? 60.0 : 0.0;
    const double v = options_.chroma ? 40.0 : 0.0;
    for (size_t line = 0; line < fmt.height; ++line) {
      const size_t start =
          orc::frame_line_sample_offset(fmt.system, fmt.width, line);
      const size_t length =
          orc::frame_line_sample_count(fmt.system, fmt.width, line);
      const double v_sign =
          fmt.system == VideoSystem::PAL && (f * fmt.height + line) % 2 ? -1.0
                                                                        : 1.0;
      const size_t field = line < field1 ? 0 : 1;
      const size_t field_line = field == 0 ? line : line - field1;
      const int picture = options_.picture(f, field);
      const std::optional<size_t> outline =
          options_.outline && field_line >= 100 && field_line < 160
              ? options_.outline(f)
              : std::nullopt;
      for (size_t n = 0; n < length; ++n) {
        const auto m = static_cast<int64_t>(n) - slip;
        const auto s = static_cast<int64_t>(f * fmt.frame_samples + start) + m;
        const double theta = 2.0 * kPi * static_cast<double>(s % 4) / 4.0;
        const double colour =
            u * std::sin(theta) + v_sign * v * std::cos(theta);
        const auto position = static_cast<size_t>(std::max<int64_t>(m, 0));
        const double picture_level =
            picture_value(picture, field_line, position) +
            (outline && position == *outline ? 150.0 : 0.0);
        if (options_.yc) {
          luma[start + n] = static_cast<sample_type>(
              std::lround(picture_level + (sigma > 0 ? noise(rng) : 0.0)));
          chroma[start + n] = static_cast<sample_type>(
              std::lround(512.0 + colour + (sigma > 0 ? noise(rng) : 0.0)));
        } else {
          luma[start + n] = static_cast<sample_type>(std::lround(
              picture_level + colour + (sigma > 0 ? noise(rng) : 0.0)));
        }
      }
    }
  }

  Options options_;
  std::vector<std::vector<sample_type>> luma_;
  std::vector<std::vector<sample_type>> chroma_;
};

using Sources =
    std::vector<std::shared_ptr<const orc::VideoFrameRepresentation>>;

// A source of |frames| frames of the default still, changed by |configure|.
std::shared_ptr<FakeStillSource> make_source(
    size_t frames,
    const std::function<void(FakeStillSource::Options&)>& configure = {}) {
  FakeStillSource::Options options;
  options.frames = frames;
  if (configure) configure(options);
  return std::make_shared<FakeStillSource>(std::move(options));
}

std::shared_ptr<const StackedVideoFrameRepresentation> stack(
    orc::StackerStage& stage, const Sources& sources) {
  return std::dynamic_pointer_cast<const StackedVideoFrameRepresentation>(
      stage.process(sources));
}

void stack_stills(orc::StackerStage& stage, int32_t search = 8) {
  EXPECT_TRUE(stage.set_parameters({{"still_frame_stacking", true},
                                    {"still_frame_search", search},
                                    {"sample_align", false}}));
}

// Flat line index of field line |line| of |field|.
size_t flat_line(const Format& fmt, size_t field, size_t line) {
  return field == 0 ? line : orc::field1_line_count(fmt.system) + line;
}

// Standard deviation of |out| − |clean| over the middle of the active
// picture of |field|.
double residual_noise(const Format& fmt, const std::vector<sample_type>& out,
                      const std::vector<sample_type>& clean, size_t field) {
  double sum = 0.0;
  double sum2 = 0.0;
  double count = 0.0;
  for (size_t line = 40; line < 250; ++line) {
    const size_t start = orc::frame_line_sample_offset(
        fmt.system, fmt.width, flat_line(fmt, field, line));
    for (size_t n = 200; n < 800; ++n) {
      const double d = static_cast<double>(out[start + n]) - clean[start + n];
      sum += d;
      sum2 += d * d;
      count += 1.0;
    }
  }
  const double mean = sum / count;
  return std::sqrt(sum2 / count - mean * mean);
}

bool lines_equal(const Format& fmt, const std::vector<sample_type>& a,
                 const std::vector<sample_type>& b, size_t flat) {
  const size_t start =
      orc::frame_line_sample_offset(fmt.system, fmt.width, flat);
  const size_t length =
      orc::frame_line_sample_count(fmt.system, fmt.width, flat);
  return std::equal(a.begin() + static_cast<std::ptrdiff_t>(start),
                    a.begin() + static_cast<std::ptrdiff_t>(start + length),
                    b.begin() + static_cast<std::ptrdiff_t>(start));
}

std::vector<FrameID> copy_ids(const std::vector<StillCopy>& copies) {
  std::vector<FrameID> ids;
  for (const auto& copy : copies) ids.push_back(copy.id);
  std::sort(ids.begin(), ids.end());
  return ids;
}

}  // namespace

// ── Parameters ──────────────────────────────────────────────────────────────

TEST(StillFrameStackingTest, Parameters_DefaultsMatchDescriptors) {
  orc::StackerStage stage;
  const auto params = stage.get_parameters();
  EXPECT_FALSE(std::get<bool>(params.at("still_frame_stacking")));
  EXPECT_EQ(std::get<int32_t>(params.at("still_frame_search")), 8);

  for (const auto& descriptor : stage.get_parameter_descriptors()) {
    if (descriptor.name != "still_frame_stacking" &&
        descriptor.name != "still_frame_search") {
      continue;
    }
    ASSERT_TRUE(descriptor.constraints.default_value.has_value());
    EXPECT_EQ(*descriptor.constraints.default_value, params.at(descriptor.name))
        << descriptor.name;
  }
}

TEST(StillFrameStackingTest, Parameters_RoundTripAndAreBounded) {
  orc::StackerStage stage;
  ASSERT_TRUE(stage.set_parameters(
      {{"still_frame_stacking", true}, {"still_frame_search", int32_t{12}}}));
  const auto params = stage.get_parameters();
  EXPECT_TRUE(std::get<bool>(params.at("still_frame_stacking")));
  EXPECT_EQ(std::get<int32_t>(params.at("still_frame_search")), 12);

  EXPECT_FALSE(stage.set_parameters({{"still_frame_search", int32_t{1}}}));
  EXPECT_FALSE(stage.set_parameters({{"still_frame_search", int32_t{51}}}));
  EXPECT_FALSE(stage.set_parameters({{"still_frame_stacking", int32_t{1}}}));
}

// ── Single input ────────────────────────────────────────────────────────────

TEST(StillFrameStackingTest, SingleInput_PassesThroughWhenOff) {
  orc::StackerStage stage;
  auto source = make_source(4);
  EXPECT_EQ(stage.process({source}).get(), source.get());
}

TEST(StillFrameStackingTest, SingleInput_ChangesOnlyFramesWithCopies) {
  orc::StackerStage stage;
  stack_stills(stage);
  auto source = make_source(28, [](FakeStillSource::Options& o) {
    o.picture = still_then_motion(16);
  });
  const auto stacked = stack(stage, {source});
  ASSERT_NE(stacked, nullptr);

  for (FrameID f = 0; f < 28; ++f) {
    const auto copies = stacked->still_copies(f);
    const auto out = stacked->get_frame_copy(f);
    if (f < 16) {
      EXPECT_GE(copies.size(), 2u) << "frame " << f;
      EXPECT_NE(out, source->get_frame_copy(f)) << "frame " << f;
    } else {
      EXPECT_TRUE(copies.empty()) << "frame " << f;
      EXPECT_EQ(out, source->get_frame_copy(f)) << "frame " << f;
    }
  }
}

TEST(StillFrameStackingTest, Execute_ReturnsTheStackForOneInputWhenOn) {
  orc::StackerStage stage;
  auto source = make_source(4);
  orc::ObservationContext ctx;
  const std::vector<orc::ArtifactPtr> inputs = {source};

  const auto off =
      stage.execute(inputs, {{"still_frame_stacking", false}}, ctx);
  ASSERT_EQ(off.size(), 1u);
  EXPECT_EQ(off[0].get(), static_cast<const orc::Artifact*>(source.get()));

  const auto on = stage.execute(inputs, {{"still_frame_stacking", true}}, ctx);
  ASSERT_EQ(on.size(), 1u);
  EXPECT_NE(
      std::dynamic_pointer_cast<const StackedVideoFrameRepresentation>(on[0]),
      nullptr);
}

// ── Finding copies ──────────────────────────────────────────────────────────

TEST(StillFrameStackingTest, Copies_DoNotDependOnVisitingOrder) {
  auto source = make_source(40, [](FakeStillSource::Options& o) {
    o.picture = still_then_motion(24);
  });
  std::vector<FrameID> order(40);
  for (FrameID f = 0; f < 40; ++f) order[f] = f;

  auto copies_in = [&](const std::vector<FrameID>& visits) {
    orc::StackerStage stage;
    stack_stills(stage);
    const auto stacked = stack(stage, {source});
    std::map<FrameID, std::vector<FrameID>> found;
    for (FrameID f : visits) found[f] = copy_ids(stacked->still_copies(f));
    return found;
  };

  const auto forwards = copies_in(order);
  std::reverse(order.begin(), order.end());
  const auto backwards = copies_in(order);
  std::shuffle(order.begin(), order.end(), std::mt19937(7));
  const auto random = copies_in(order);

  EXPECT_EQ(forwards, backwards);
  EXPECT_EQ(forwards, random);
  EXPECT_EQ(forwards.at(12), (std::vector<FrameID>{4, 8, 16, 20}));
}

TEST(StillFrameStackingTest, Copies_OneMatchingCopyTakesNone) {
  // Frames 0 and 4 show one picture, which nothing else repeats; 20-39 hold
  // another still, which sets the reference noise.
  auto picture = [](size_t frame, size_t) {
    if (frame == 0 || frame == 4) return 1;
    if (frame >= 20) return 2;
    return static_cast<int>(100 + frame);
  };
  orc::StackerStage stage;
  stack_stills(stage);
  auto source = make_source(
      40, [&](FakeStillSource::Options& o) { o.picture = picture; });
  const auto stacked = stack(stage, {source});

  EXPECT_TRUE(stacked->still_copies(0).empty());
  EXPECT_TRUE(stacked->still_copies(4).empty());
  EXPECT_EQ(copy_ids(stacked->still_copies(28)),
            (std::vector<FrameID>{20, 24, 32, 36}));
  EXPECT_EQ(stacked->get_frame_copy(0), source->get_frame_copy(0));
}

// An outline 2 samples along, as a character drawn over a held background
// moves: the block signatures do not see it, the sample check does.
std::function<std::optional<size_t>(size_t)> outline_moved_in(
    std::vector<size_t> frames) {
  return [frames](size_t frame) -> std::optional<size_t> {
    const bool moved =
        std::find(frames.begin(), frames.end(), frame) != frames.end();
    return moved ? 622 : 620;
  };
}

TEST(StillFrameStackingTest, Copies_AMovedOutlineIsNotACopy) {
  orc::StackerStage stage;
  stack_stills(stage);
  auto source = make_source(40, [](FakeStillSource::Options& o) {
    o.outline = outline_moved_in({16});
  });
  const auto stacked = stack(stage, {source});

  EXPECT_EQ(copy_ids(stacked->still_copies(20)),
            (std::vector<FrameID>{12, 24, 28}));
  EXPECT_TRUE(stacked->still_copies(16).empty());
  EXPECT_EQ(stacked->get_frame_copy(16), source->get_frame_copy(16));
}

TEST(StillFrameStackingTest, Copies_AFieldLeftWithOneCopyTakesNone) {
  // Searching 4 frames either side, frame 20 has two candidates, 16 and 24,
  // and the outline moved in 24.
  orc::StackerStage stage;
  stack_stills(stage, 4);
  auto source = make_source(40, [](FakeStillSource::Options& o) {
    o.outline = outline_moved_in({24});
  });
  const auto stacked = stack(stage, {source});

  EXPECT_TRUE(stacked->still_copies(20).empty());
  EXPECT_EQ(copy_ids(stacked->still_copies(12)), (std::vector<FrameID>{8, 16}));
}

TEST(StillFrameStackingTest, Copies_SampleCheckDoesNotDependOnVisitingOrder) {
  auto source = make_source(40, [](FakeStillSource::Options& o) {
    o.outline = outline_moved_in({9, 16, 17, 30});
  });
  std::vector<FrameID> order(40);
  for (FrameID f = 0; f < 40; ++f) order[f] = f;

  auto copies_in = [&](const std::vector<FrameID>& visits) {
    orc::StackerStage stage;
    stack_stills(stage);
    const auto stacked = stack(stage, {source});
    std::map<FrameID, std::vector<FrameID>> found;
    for (FrameID f : visits) found[f] = copy_ids(stacked->still_copies(f));
    return found;
  };

  const auto forwards = copies_in(order);
  std::reverse(order.begin(), order.end());
  const auto backwards = copies_in(order);
  std::shuffle(order.begin(), order.end(), std::mt19937(11));
  const auto random = copies_in(order);

  EXPECT_EQ(forwards, backwards);
  EXPECT_EQ(forwards, random);
  EXPECT_EQ(forwards.at(12), (std::vector<FrameID>{4, 8, 20}));
}

// Without chroma every frame of a still reads as the same phase, so only the
// cadence keeps the frames between out.
TEST(StillFrameStackingTest, Copies_AreOnlyTriedAtTheColourCadence) {
  for (const Format& fmt : {kPal, kNtsc}) {
    orc::StackerStage stage;
    stack_stills(stage, 8);
    auto source = make_source(30, [&](FakeStillSource::Options& o) {
      o.fmt = fmt;
      o.chroma = false;
    });
    const auto stacked = stack(stage, {source});
    const FrameID cadence = fmt.system == VideoSystem::PAL ? 4 : 2;
    for (FrameID f = 10; f < 20; ++f) {
      const auto ids = copy_ids(stacked->still_copies(f));
      EXPECT_EQ(ids.size(), 2 * (8 / cadence)) << "frame " << f;
      for (FrameID id : ids) {
        EXPECT_EQ((id > f ? id - f : f - id) % cadence, 0u)
            << "frame " << f << " copy " << id;
      }
    }
  }
}

TEST(StillFrameStackingTest, Copies_AFieldThatDiffersIsNotCopied) {
  // Field 2 of frame 16 shows a different picture.
  auto picture = [](size_t frame, size_t field) {
    return frame == 16 && field == 1 ? 5 : 0;
  };
  orc::StackerStage stage;
  stack_stills(stage);
  auto source = make_source(
      40, [&](FakeStillSource::Options& o) { o.picture = picture; });
  const auto stacked = stack(stage, {source});

  const auto copies = stacked->still_copies(20);
  ASSERT_EQ(copy_ids(copies), (std::vector<FrameID>{12, 16, 24, 28}));
  for (const auto& copy : copies) {
    EXPECT_TRUE(copy.field[0]) << "copy " << copy.id;
    EXPECT_EQ(copy.field[1], copy.id != 16) << "copy " << copy.id;
  }

  // Field 2 is stacked from the other copies and does not take on frame
  // 16's picture.
  const auto out = stacked->get_frame_copy(20);
  const auto clean = source->clean(20);
  EXPECT_LT(residual_noise(kPal, out, clean, 1), 3.5);
}

TEST(StillFrameStackingTest, Copies_OneSampleOutIsNotUsed) {
  orc::StackerStage stage;
  stack_stills(stage);
  auto source =
      make_source(40, [](FakeStillSource::Options& o) { o.slips = {{24, 1}}; });
  const auto stacked = stack(stage, {source});
  EXPECT_EQ(copy_ids(stacked->still_copies(20)),
            (std::vector<FrameID>{12, 16, 28}));
}

TEST(StillFrameStackingTest, Copies_ADuplicateReadIsNotACopy) {
  // Frame 24 reads back frame 20's samples, as a Frame Map duplicate does.
  struct DuplicatingSource : FakeStillSource {
    using FakeStillSource::FakeStillSource;
    std::vector<sample_type> get_frame_copy(FrameID id) const override {
      return FakeStillSource::get_frame_copy(id == 24 ? 20 : id);
    }
  };
  FakeStillSource::Options options;
  options.frames = 40;
  auto duplicating = std::make_shared<DuplicatingSource>(options);
  orc::StackerStage stage;
  stack_stills(stage);
  const auto stacked = stack(stage, {duplicating});
  EXPECT_EQ(copy_ids(stacked->still_copies(20)),
            (std::vector<FrameID>{12, 16, 28}));
}

// ── Stacking copies ─────────────────────────────────────────────────────────

TEST(StillFrameStackingTest, Stack_NoiseFallsByRootN) {
  orc::StackerStage stage;
  stack_stills(stage);
  auto source = make_source(40);
  const auto stacked = stack(stage, {source});

  ASSERT_EQ(stacked->still_copies(20).size(), 4u);
  const auto clean = source->clean(20);
  const double before =
      residual_noise(kPal, source->get_frame_copy(20), clean, 0);
  const double after =
      residual_noise(kPal, stacked->get_frame_copy(20), clean, 0);
  // Five values: 1/√5 = 0.447 of the noise, plus rounding.
  EXPECT_NEAR(after / before, 1.0 / std::sqrt(5.0), 0.05);
}

TEST(StillFrameStackingTest, Stack_YCNoiseFallsByRootN) {
  orc::StackerStage stage;
  stack_stills(stage);
  auto source =
      make_source(40, [](FakeStillSource::Options& o) { o.yc = true; });
  const auto stacked = stack(stage, {source});

  ASSERT_EQ(stacked->still_copies(20).size(), 4u);
  const auto clean = source->clean(20);
  const sample_type* in = source->get_frame_luma(20);
  const sample_type* out = stacked->get_frame_luma(20);
  ASSERT_NE(out, nullptr);
  const std::vector<sample_type> input(in, in + kPal.frame_samples);
  const std::vector<sample_type> output(out, out + kPal.frame_samples);
  EXPECT_NEAR(residual_noise(kPal, output, clean, 0) /
                  residual_noise(kPal, input, clean, 0),
              1.0 / std::sqrt(5.0), 0.05);
}

TEST(StillFrameStackingTest, Stack_FillsADropoutFromTheCopies) {
  // Frame 20 is flagged over 40 samples of an active line of field 1.
  DropoutRun run;
  run.sample_start =
      orc::frame_line_sample_offset(kPal.system, kPal.width, 100) + 400;
  run.sample_count = 40;
  run.severity = 50;
  // Garbage under the flag.
  struct DamagedSource : FakeStillSource {
    using FakeStillSource::FakeStillSource;
    uint64_t at = 0;
    std::vector<sample_type> get_frame_copy(FrameID id) const override {
      auto frame = FakeStillSource::get_frame_copy(id);
      if (id == 20) {
        std::fill_n(frame.begin() + static_cast<std::ptrdiff_t>(at), 40, 1000);
      }
      return frame;
    }
  };
  FakeStillSource::Options options;
  options.frames = 40;
  options.dropouts[20] = {run};
  auto source = std::make_shared<DamagedSource>(options);
  source->at = run.sample_start;

  orc::StackerStage stage;
  stack_stills(stage);
  const auto stacked = stack(stage, {source});
  ASSERT_EQ(stacked->still_copies(20).size(), 4u);

  const auto out = stacked->get_frame_copy(20);
  const auto clean = source->clean(20);
  for (uint64_t s = run.sample_start; s < run.sample_start + 40; ++s) {
    EXPECT_NEAR(out[s], clean[s], 15) << "sample " << s;
  }
  for (const auto& hint : stacked->get_dropout_hints(20)) {
    EXPECT_FALSE(hint.sample_start < run.sample_start + 40 &&
                 hint.sample_start + hint.sample_count > run.sample_start)
        << "the dropout is still flagged";
  }
}

TEST(StillFrameStackingTest, Stack_VbiLinesComeFromTheFrameAlone) {
  // A single input: VBI lines are the input's own.
  {
    orc::StackerStage stage;
    stack_stills(stage);
    auto source = make_source(40);
    const auto stacked = stack(stage, {source});
    ASSERT_FALSE(stacked->still_copies(20).empty());
    const auto in = source->get_frame_copy(20);
    const auto out = stacked->get_frame_copy(20);
    for (size_t field = 0; field < 2; ++field) {
      for (size_t line = 0; line < 22; ++line) {
        EXPECT_TRUE(lines_equal(kPal, in, out, flat_line(kPal, field, line)))
            << "field " << field + 1 << " line " << line;
      }
      EXPECT_FALSE(lines_equal(kPal, in, out, flat_line(kPal, field, 22)));
    }
  }

  // Two inputs: VBI lines are the stack of the inputs without copies.
  auto first = make_source(40, [](FakeStillSource::Options& o) { o.seed = 1; });
  auto second =
      make_source(40, [](FakeStillSource::Options& o) { o.seed = 2; });
  orc::StackerStage plain;
  ASSERT_TRUE(plain.set_parameters({{"sample_align", false}}));
  orc::StackerStage stage;
  stack_stills(stage);
  const auto without = plain.process({first, second})->get_frame_copy(20);
  const auto stacked = stack(stage, {first, second});
  ASSERT_EQ(stacked->still_copies(20).size(), 8u);
  const auto with = stacked->get_frame_copy(20);
  for (size_t field = 0; field < 2; ++field) {
    for (size_t line = 0; line < 22; ++line) {
      EXPECT_TRUE(
          lines_equal(kPal, without, with, flat_line(kPal, field, line)))
          << "field " << field + 1 << " line " << line;
    }
  }
}

TEST(StillFrameStackingTest, Stack_NtscCaptionLinesTakeNoCopies) {
  orc::StackerStage stage;
  stack_stills(stage);
  auto source =
      make_source(30, [](FakeStillSource::Options& o) { o.fmt = kNtsc; });
  const auto stacked = stack(stage, {source});
  ASSERT_FALSE(stacked->still_copies(15).empty());

  const auto in = source->get_frame_copy(15);
  const auto out = stacked->get_frame_copy(15);
  // Line 21 is field line 20 of field 1, and line 284 is field line 20 of
  // field 2.
  for (size_t field = 0; field < 2; ++field) {
    EXPECT_TRUE(lines_equal(kNtsc, in, out, flat_line(kNtsc, field, 20)))
        << "field " << field + 1;
    EXPECT_FALSE(lines_equal(kNtsc, in, out, flat_line(kNtsc, field, 21)))
        << "field " << field + 1;
  }
}

TEST(StillFrameStackingTest, Stack_AudioAndEfmAreUnchanged) {
  orc::StackerStage stage;
  stack_stills(stage);
  ASSERT_TRUE(stage.set_parameters({{"efm_stacking", std::string("Mean")}}));
  auto source = make_source(40);
  const auto stacked = stack(stage, {source});
  ASSERT_FALSE(stacked->still_copies(20).empty());

  EXPECT_EQ(stacked->get_audio_samples(0, 20),
            source->get_audio_samples(0, 20));
  EXPECT_EQ(stacked->get_efm_samples(20), source->get_efm_samples(20));

  // With two inputs, audio and EFM are the stack of the inputs alone.
  auto second =
      make_source(40, [](FakeStillSource::Options& o) { o.seed = 2; });
  orc::StackerStage plain;
  ASSERT_TRUE(plain.set_parameters(
      {{"sample_align", false}, {"efm_stacking", std::string("Mean")}}));
  const auto without = plain.process({source, second});
  const auto with = stack(stage, {source, second});
  EXPECT_EQ(with->get_audio_samples(0, 20), without->get_audio_samples(0, 20));
  EXPECT_EQ(with->get_efm_samples(20), without->get_efm_samples(20));
}

}  // namespace orc_unit_test
