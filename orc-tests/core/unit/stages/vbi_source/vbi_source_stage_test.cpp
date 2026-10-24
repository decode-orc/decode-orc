/*
 * File:        vbi_source_stage_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the raw VBI capture source stage
 *
 * Every capture is rendered in memory and reaches the stage through the
 * injected dependency interface, so nothing here touches the filesystem.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_source_stage.h"

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/error_types.h>
#include <orc/stage/node_type.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/params/parameter_types.h>
#include <orc/stage/video_frame_representation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "vbi-services/teletext_slicer.h"
#include "vbi_output_frame.h"
#include "vbi_source_format.h"
#include "vbi_synthetic_line.h"
#include "vbi_teletext_service.h"

namespace orc {
namespace {

using orc::testing::render_synthetic_vbi_line;
using orc::testing::SyntheticVBILine;

// The capture offset every synthetic capture below is built at: the figure
// measured on the reference bt8x8 sample, so calibration has something
// realistic to find.
constexpr double kTruthOffsetSamples = 261.6;

constexpr const char* kCapturePath = "/captures/synthetic.vbi";

VBISourceFormat bt8x8_pal_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", format, error))
      << error;
  return format;
}

VBITeletextService wst_service() {
  VBITeletextService service;
  std::string error;
  EXPECT_TRUE(vbi_teletext_service(VBITVSystem::kPAL, VBITeletextSystem::kWST,
                                   service, error))
      << error;
  return service;
}

// ---------------------------------------------------------------------------
// A synthetic capture
// ---------------------------------------------------------------------------

// Build a bt8x8 PAL capture whose teletext lines all sit at one true capture
// offset, with the driver's frame counter written into each frame's trailer.
std::vector<uint8_t> make_synthetic_capture(
    const VBISourceFormat& format, uint64_t frame_count,
    double capture_offset_samples = kTruthOffsetSamples,
    uint32_t first_counter = 399598u, uint64_t dropped_after_frame = UINT64_MAX,
    uint32_t dropped_frames = 0) {
  const VBITeletextService service = wst_service();
  std::vector<uint8_t> bytes(
      static_cast<size_t>(format.bytes_per_frame() * frame_count), 0);

  const double position =
      service.cri_start_samples(format.sample_rate_hz, capture_offset_samples);

  uint32_t counter = first_counter;
  for (uint64_t frame = 0; frame < frame_count; ++frame) {
    for (uint32_t field = 0; field < 2u; ++field) {
      for (uint32_t index = 0; index < format.field_lines; ++index) {
        SyntheticVBILine line;
        line.sample_rate_hz = format.sample_rate_hz;
        line.valid_samples = format.valid_samples;
        line.anchor_position_samples = position;
        line.seed =
            static_cast<uint32_t>(frame * 64u + field * 32u + index + 1u);
        line.noise_amplitude = 0.5;

        const std::vector<double> samples = render_synthetic_vbi_line(line);
        const uint64_t offset = frame * format.bytes_per_frame() +
                                field * format.bytes_per_field() +
                                index * format.bytes_per_record();
        for (uint32_t sample = 0; sample < format.valid_samples; ++sample) {
          const double value =
              std::clamp(std::round(samples[sample]), 0.0, 255.0);
          bytes[static_cast<size_t>(offset + sample)] =
              static_cast<uint8_t>(value);
        }
      }
    }

    // The driver's frame sequence number occupies the last four bytes of the
    // frame, which on PAL are the padding of its final record.
    const size_t trailer =
        static_cast<size_t>((frame + 1u) * format.bytes_per_frame() - 4u);
    for (uint32_t byte = 0; byte < 4u; ++byte) {
      bytes[trailer + byte] =
          static_cast<uint8_t>((counter >> (8u * byte)) & 0xFFu);
    }

    ++counter;
    if (frame == dropped_after_frame) counter += dropped_frames;
  }
  return bytes;
}

// In-memory capture: unit tests never touch the filesystem, and the transport
// presents identical bytes whatever the container.
class FakeByteSource : public IVBIByteSource {
 public:
  explicit FakeByteSource(std::vector<uint8_t> bytes)
      : bytes_(std::move(bytes)) {}

  std::optional<uint64_t> size_bytes() const override {
    return static_cast<uint64_t>(bytes_.size());
  }

  size_t read_at(uint64_t byte_offset, size_t count, uint8_t* out_buffer,
                 std::string& /*error_message*/) override {
    if (byte_offset >= bytes_.size()) return 0;
    const size_t available = bytes_.size() - static_cast<size_t>(byte_offset);
    const size_t produced = std::min(count, available);
    std::memcpy(out_buffer, bytes_.data() + byte_offset, produced);
    return produced;
  }

  std::optional<uint32_t> declared_bits_per_sample() const override {
    return declared_bits;
  }

  std::optional<uint32_t> declared_bits = 8u;

 private:
  std::vector<uint8_t> bytes_;
};

class FakeDeps final : public IVBISourceStageDeps {
 public:
  explicit FakeDeps(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}

  bool validate_input_file(const std::string& /*input_path*/,
                           std::string& error_message) const override {
    if (!file_valid) {
      error_message = "VBI capture is not accessible.";
      return false;
    }
    return true;
  }

  std::unique_ptr<IVBIByteSource> open_byte_source(
      const std::string& /*input_path*/,
      std::string& error_message) const override {
    if (!open_succeeds) {
      error_message = "VBI capture could not be opened.";
      return nullptr;
    }
    ++opens;
    auto source = std::make_unique<FakeByteSource>(bytes_);
    source->declared_bits = declared_bits;
    return source;
  }

  bool file_valid = true;
  bool open_succeeds = true;
  // What the transport declares: the one FLAC header field that is meaningful.
  uint32_t declared_bits = 8u;
  // Atomic because the read-ahead's decoders open the capture from threads
  // of their own.
  mutable std::atomic<uint32_t> opens{0};

 private:
  std::vector<uint8_t> bytes_;
};

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

std::map<std::string, ParameterValue> default_parameters() {
  return {{"input_path", std::string(kCapturePath)}};
}

// Run the stage over a synthetic capture and return its output.
ArtifactPtr run_stage(VBISourceStage& stage,
                      const std::map<std::string, ParameterValue>& parameters) {
  ObservationContext observations;
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, parameters, observations);
  return outputs.empty() ? nullptr : outputs.front();
}

std::shared_ptr<VideoFrameRepresentation> representation_of(
    const ArtifactPtr& artifact) {
  return std::dynamic_pointer_cast<VideoFrameRepresentation>(artifact);
}

// ---------------------------------------------------------------------------
// Identity and contract
// ---------------------------------------------------------------------------

TEST(VBISourceStageIdentity, IsASourceForEitherSystemWithNoInputsAndOneOutput) {
  VBISourceStage stage;
  const NodeTypeInfo info = stage.get_node_type_info();

  EXPECT_EQ(info.stage_name, "vbi_source");
  EXPECT_EQ(info.type, NodeType::SOURCE);
  EXPECT_EQ(info.compatible_formats, VideoFormatCompatibility::ALL);
  EXPECT_EQ(stage.required_input_count(), 0u);
  EXPECT_EQ(stage.output_count(), 1u);
}

TEST(VBISourceStageIdentity, RejectsInputArtifacts) {
  VBISourceStage stage;
  ObservationContext observations;
  const std::vector<ArtifactPtr> inputs = {nullptr};
  EXPECT_THROW(stage.execute(inputs, default_parameters(), observations),
               std::runtime_error);
}

TEST(VBISourceStageIdentity, ShipsItsOwnInstructions) {
  VBISourceStage stage;
  // The instructions are read from alongside the plugin at runtime, which a
  // test binary linking the stage directly cannot do; the accessor must still
  // answer rather than crash.
  EXPECT_NO_THROW((void)stage.get_instructions());
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

TEST(VBISourceStageParameters, EveryRuntimeParameterHasADescriptor) {
  VBISourceStage stage;
  const auto descriptors = stage.get_parameter_descriptors();
  const auto parameters = stage.get_parameters();

  for (const auto& entry : parameters) {
    const std::string& name = entry.first;
    const auto it = std::find_if(
        descriptors.begin(), descriptors.end(),
        [&name](const ParameterDescriptor& d) { return d.name == name; });
    EXPECT_NE(it, descriptors.end()) << "no descriptor for '" << name << "'";
  }
}

TEST(VBISourceStageParameters, DefaultsMatchTheDescriptorDefaults) {
  VBISourceStage stage;
  for (const ParameterDescriptor& descriptor :
       stage.get_parameter_descriptors()) {
    ASSERT_TRUE(descriptor.constraints.default_value.has_value())
        << descriptor.name << " has no default";
    VBISourceStage fresh;
    ASSERT_TRUE(fresh.set_parameters(
        {{descriptor.name, *descriptor.constraints.default_value}}))
        << "rejected its own default for '" << descriptor.name << "'";
    const auto applied = fresh.get_parameters();
    const auto it = applied.find(descriptor.name);
    ASSERT_NE(it, applied.end()) << descriptor.name;
    EXPECT_EQ(it->second, *descriptor.constraints.default_value)
        << descriptor.name;
  }
}

TEST(VBISourceStageParameters, OffersEveryKnownContainerPreset) {
  VBISourceStage stage;
  const auto descriptors = stage.get_parameter_descriptors();
  const auto it = std::find_if(
      descriptors.begin(), descriptors.end(),
      [](const ParameterDescriptor& d) { return d.name == "format"; });
  ASSERT_NE(it, descriptors.end());
  EXPECT_EQ(it->constraints.allowed_strings, vbi_source_preset_names());
}

// A capture's television system fixes the geometry of the frames it is placed
// on, so a project is offered the formats of its own system and nothing else.
// That is the whole of the choice: a PAL project is offered the card container
// in its two source flavours, an NTSC project the two services its crops carry.
TEST(VBISourceStageParameters, TheFormatsOfferedFollowTheProjectSystem) {
  VBISourceStage stage;

  const auto format_descriptor = [&stage](VideoSystem system) {
    const auto descriptors =
        stage.get_parameter_descriptors(system, SourceType::Unknown);
    const auto it = std::find_if(
        descriptors.begin(), descriptors.end(),
        [](const ParameterDescriptor& d) { return d.name == "format"; });
    EXPECT_NE(it, descriptors.end());
    return *it;
  };

  const ParameterDescriptor pal = format_descriptor(VideoSystem::PAL);
  EXPECT_EQ(pal.constraints.allowed_strings,
            vbi_source_preset_names(VBITVSystem::kPAL));
  EXPECT_EQ(pal.constraints.default_value,
            ParameterValue(std::string("bt8x8 card dump, 8-bit (WST)")));

  const ParameterDescriptor ntsc = format_descriptor(VideoSystem::NTSC);
  EXPECT_EQ(ntsc.constraints.allowed_strings,
            vbi_source_preset_names(VBITVSystem::kNTSC));
  EXPECT_EQ(ntsc.constraints.default_value,
            ParameterValue(std::string(".tbc VBI crop, 16-bit (WST)")));
}

// The preset carries the whole configuration, so there is nothing else for the
// surface to offer: the file, the format and one policy about dropped frames.
TEST(VBISourceStageParameters, TheSurfaceIsThreeParameters) {
  VBISourceStage stage;
  const auto descriptors = stage.get_parameter_descriptors();

  std::vector<std::string> names;
  for (const ParameterDescriptor& descriptor : descriptors) {
    names.push_back(descriptor.name);
    // Nothing is conditional on anything else any more.
    EXPECT_FALSE(descriptor.constraints.depends_on.has_value())
        << descriptor.name;
  }
  EXPECT_EQ(names, (std::vector<std::string>{"input_path", "format", "drops"}));
}

// The browse dialog filters on this hint, and these captures arrive under
// several extensions: a card dump is .vbi raw or .flac wrapped, a decoder's
// VBI-only export is named for its word size, a crop keeps .tbc, and a crop
// kept as a video is .mkv. Offering only one of them hides the rest behind the
// All Files entry.
TEST(VBISourceStageParameters, TheCapturePathHintCoversEveryContainerInUse) {
  VBISourceStage stage;
  const auto descriptors = stage.get_parameter_descriptors();

  const auto path = std::find_if(descriptors.begin(), descriptors.end(),
                                 [](const ParameterDescriptor& descriptor) {
                                   return descriptor.name == "input_path";
                                 });
  ASSERT_NE(path, descriptors.end());

  for (const char* extension :
       {".vbi", ".flac", ".u8", ".u16", ".tbc", ".mkv"}) {
    EXPECT_NE(path->file_extension_hint.find(extension), std::string::npos)
        << extension << " missing from '" << path->file_extension_hint << "'";
  }

  // Nothing about a capture is read from its name, so the hint must stay a
  // dialog filter and never become a requirement.
  EXPECT_FALSE(path->constraints.required);
}

TEST(VBISourceStageParameters, RejectsAnUnknownParameter) {
  VBISourceStage stage;
  EXPECT_FALSE(stage.set_parameters({{"nonsense", std::string("value")}}));
}

TEST(VBISourceStageParameters, RoundTripsTheParametersItIsGiven) {
  VBISourceStage stage;
  ASSERT_TRUE(stage.set_parameters({
      {"input_path", std::string(kCapturePath)},
      {"format", std::string(".tbc VBI crop, 16-bit (NABTS)")},
      {"drops", std::string("pad")},
  }));

  const auto parameters = stage.get_parameters();
  EXPECT_EQ(std::get<std::string>(parameters.at("input_path")), kCapturePath);
  EXPECT_EQ(std::get<std::string>(parameters.at("format")),
            ".tbc VBI crop, 16-bit (NABTS)");
  EXPECT_EQ(std::get<std::string>(parameters.at("drops")), "pad");
}

// Every parameter is a string now, so a caller handing over a number is
// handing over the wrong thing and is told rather than quietly ignored.
TEST(VBISourceStageParameters, RejectsAParameterOfTheWrongType) {
  VBISourceStage stage;
  EXPECT_FALSE(stage.set_parameters({{"format", uint32_t{2}}}));
}

// ---------------------------------------------------------------------------
// Configuration status
// ---------------------------------------------------------------------------

TEST(VBISourceStageStatus, IsRedUntilACaptureIsConfigured) {
  VBISourceStage stage;
  EXPECT_EQ(stage.get_configuration_status(), ConfigurationStatus::Red);
}

TEST(VBISourceStageStatus, IsGreenForAnAccessibleCaptureAndAKnownPreset) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 2));
  VBISourceStage stage(deps);

  ASSERT_TRUE(stage.set_parameters(default_parameters()));
  EXPECT_EQ(stage.get_configuration_status(), ConfigurationStatus::Green);
}

TEST(VBISourceStageStatus, IsRedWhenTheCaptureIsNotAccessible) {
  auto deps = std::make_shared<FakeDeps>(std::vector<uint8_t>{});
  deps->file_valid = false;
  VBISourceStage stage(deps);

  ASSERT_TRUE(stage.set_parameters(default_parameters()));
  EXPECT_EQ(stage.get_configuration_status(), ConfigurationStatus::Red);
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

TEST(VBISourceStageExecution, ProducesNoOutputUntilACaptureIsConfigured) {
  VBISourceStage stage(std::make_shared<FakeDeps>(std::vector<uint8_t>{}));
  ObservationContext observations;
  EXPECT_TRUE(stage.execute({}, {}, observations).empty());
}

TEST(VBISourceStageExecution, EmitsOneOutputFramePerStoredFrame) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 3));
  VBISourceStage stage(deps);

  const auto representation =
      representation_of(run_stage(stage, default_parameters()));
  ASSERT_NE(representation, nullptr);

  EXPECT_EQ(representation->frame_count(), 3u);
  EXPECT_TRUE(representation->has_frame(2));
  EXPECT_FALSE(representation->has_frame(3));
}

TEST(VBISourceStageExecution, FramesAreNormativelySizedPALCVBS) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 1));
  VBISourceStage stage(deps);

  const auto representation =
      representation_of(run_stage(stage, default_parameters()));
  ASSERT_NE(representation, nullptr);

  const auto parameters = representation->get_video_parameters();
  ASSERT_TRUE(parameters.has_value());
  EXPECT_EQ(parameters->system, VideoSystem::PAL);
  EXPECT_EQ(parameters->frame_height, kPalFrameLines);
  EXPECT_EQ(parameters->frame_width_nominal, kPalSamplesPerLineNominal);
  EXPECT_EQ(parameters->blanking_level, kPalBlanking);

  const auto descriptor = representation->get_frame_descriptor(0);
  ASSERT_TRUE(descriptor.has_value());
  EXPECT_EQ(descriptor->samples_total, static_cast<size_t>(kPalFrameSamples));

  EXPECT_EQ(representation->get_frame_copy(0).size(),
            static_cast<size_t>(kPalFrameSamples));
}

// The sample encoding reserves the extremes of the 10-bit word; nothing the
// stage writes may land in either protected range (design §2.2).
TEST(VBISourceStageExecution, EverySampleIsInsideTheLegalRange) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 1));
  VBISourceStage stage(deps);

  const auto representation =
      representation_of(run_stage(stage, default_parameters()));
  ASSERT_NE(representation, nullptr);

  const std::vector<int16_t> frame = representation->get_frame_copy(0);
  ASSERT_FALSE(frame.empty());
  const auto bounds = std::minmax_element(frame.begin(), frame.end());
  EXPECT_GE(*bounds.first, kVBIOutputSampleMin);
  EXPECT_LE(*bounds.second, kVBIOutputSampleMax);
}

// The teletext lines of a 625-line WST capture are broadcast frame lines 7-22
// and 320-335, which are stored frame lines 6-21 and 319-334 (design §5.1).
TEST(VBISourceStageExecution, DataLandsOnTheTeletextLinesAndNowhereElse) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 1));
  VBISourceStage stage(deps);

  const auto representation =
      representation_of(run_stage(stage, default_parameters()));
  ASSERT_NE(representation, nullptr);

  VBIOutputFrame output;
  std::string error;
  ASSERT_TRUE(make_vbi_output_frame(VBITVSystem::kPAL, VBITeletextSystem::kWST,
                                    output, error))
      << error;
  const VBIOutputLevels& levels = output.levels;

  // How far above blanking a line's data region rises: a teletext line swings
  // to the logic 1 level, a blank one stays at blanking.
  const auto line_peak = [&](size_t line) {
    const std::vector<int16_t> samples =
        representation->get_line_samples(0, line);
    EXPECT_FALSE(samples.empty()) << "line " << line;
    // Start inside the data region, past the back porch.
    const size_t data_begin = std::min<size_t>(300, samples.size());
    return *std::max_element(samples.begin() + data_begin, samples.end());
  };

  const int16_t threshold =
      static_cast<int16_t>((levels.blanking + levels.logic1) / 2);

  for (const size_t line : {size_t{6}, size_t{21}, size_t{319}, size_t{334}}) {
    EXPECT_GT(line_peak(line), threshold) << "teletext line " << line;
  }
  for (const size_t line : {size_t{5}, size_t{22}, size_t{318}, size_t{335},
                            size_t{100}, size_t{400}}) {
    EXPECT_LT(line_peak(line), threshold) << "non-teletext line " << line;
  }
}

// The stage places data and manufactures nothing: every sample outside the
// data region of a teletext line is exactly the blanking level, with no sync,
// no vertical interval and no colour burst anywhere in the frame.
TEST(VBISourceStageExecution, EverythingOutsideTheDataRegionIsBlanking) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 1));
  VBISourceStage stage(deps);

  const auto representation =
      representation_of(run_stage(stage, default_parameters()));
  ASSERT_NE(representation, nullptr);

  VBIOutputFrame output;
  std::string error;
  ASSERT_TRUE(make_vbi_output_frame(VBITVSystem::kPAL, VBITeletextSystem::kWST,
                                    output, error))
      << error;
  const auto blanking = static_cast<int16_t>(output.levels.blanking);

  const std::vector<int16_t> frame = representation->get_frame_copy(0);
  ASSERT_EQ(frame.size(), static_cast<size_t>(kPalFrameSamples));

  // Every line that carries no data service, sample for sample.
  for (uint32_t line = 0; line < output.lines_per_frame; ++line) {
    const bool data_line =
        (line >= 6u && line <= 21u) || (line >= 319u && line <= 334u);
    if (data_line) continue;
    const size_t begin = output.line_offset(line);
    const size_t length = output.line_length(line);
    for (size_t sample = 0; sample < length; ++sample) {
      ASSERT_EQ(frame[begin + sample], blanking)
          << "line " << line << " sample " << sample;
    }
  }

  // And the head of a data line, ahead of everything the record covers.
  const size_t data_line_begin = output.line_offset(6);
  for (size_t sample = 0; sample < 120u; ++sample) {
    EXPECT_EQ(frame[data_line_begin + sample], blanking) << "sample " << sample;
  }
}

TEST(VBISourceStageExecution, RepeatedExecutionReusesTheOpenedCapture) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 2));
  VBISourceStage stage(deps);

  const ArtifactPtr first = run_stage(stage, default_parameters());
  const ArtifactPtr second = run_stage(stage, default_parameters());
  EXPECT_EQ(first, second);
  EXPECT_EQ(deps->opens.load(), 1u);

  // A changed configuration is a different capture and is re-read.
  auto parameters = default_parameters();
  parameters["drops"] = std::string("pad");
  const ArtifactPtr third = run_stage(stage, parameters);
  EXPECT_NE(first, third);
  EXPECT_EQ(deps->opens.load(), 2u);
}

// ---------------------------------------------------------------------------
// The whole point: what the teletext decoders read back
// ---------------------------------------------------------------------------

// A recognisable T42 packet: magazine 1 row 0, then 40 display bytes carrying
// odd parity, as ETSI EN 300 706 §7.1.2 and §8.1 define them.
std::array<uint8_t, kTeletextPacketBytes> reference_packet() {
  std::array<uint8_t, kTeletextPacketBytes> bytes{};
  bytes[0] = teletext_hamming84_encode(1);  // magazine 1, row 0 low bit
  bytes[1] = teletext_hamming84_encode(0);
  for (size_t index = 2; index < bytes.size(); ++index) {
    auto value = static_cast<uint8_t>(0x20u + ((index * 7u) % 0x5Fu));
    int ones = 0;
    for (int bit = 0; bit < 7; ++bit) {
      if (((value >> bit) & 1u) != 0u) ++ones;
    }
    if ((ones % 2) == 0) value |= 0x80u;  // odd parity over the whole byte
    bytes[index] = value;
  }
  return bytes;
}

// The packet's bits in transmission order: each byte least significant bit
// first (ETSI EN 300 706 §7.1).
std::vector<bool> packet_bits(
    const std::array<uint8_t, kTeletextPacketBytes>& bytes) {
  std::vector<bool> bits;
  bits.reserve(bytes.size() * 8u);
  for (const uint8_t byte : bytes) {
    for (int bit = 0; bit < 8; ++bit) {
      bits.push_back(((byte >> bit) & 1u) != 0u);
    }
  }
  return bits;
}

// A capture every one of whose lines carries |bytes|.
std::vector<uint8_t> make_packet_capture(
    const VBISourceFormat& format,
    const std::array<uint8_t, kTeletextPacketBytes>& bytes) {
  const VBITeletextService service = wst_service();
  std::vector<uint8_t> capture(static_cast<size_t>(format.bytes_per_frame()),
                               0);

  SyntheticVBILine line;
  line.sample_rate_hz = format.sample_rate_hz;
  line.valid_samples = format.valid_samples;
  line.anchor_position_samples =
      service.cri_start_samples(format.sample_rate_hz, kTruthOffsetSamples);
  line.payload = packet_bits(bytes);
  line.noise_amplitude = 0.5;
  const std::vector<double> samples = render_synthetic_vbi_line(line);

  for (uint32_t field = 0; field < 2u; ++field) {
    for (uint32_t index = 0; index < format.field_lines; ++index) {
      const uint64_t offset =
          field * format.bytes_per_field() + index * format.bytes_per_record();
      for (uint32_t sample = 0; sample < format.valid_samples; ++sample) {
        capture[static_cast<size_t>(offset + sample)] = static_cast<uint8_t>(
            std::clamp(std::round(samples[sample]), 0.0, 255.0));
      }
    }
  }
  return capture;
}

// The stage exists so that the teletext decoders see what they would see on a
// native decode.  This is that claim, end to end: a packet put into a raw VBI
// record comes back out of the slicer byte for byte, off the frame the stage
// built, with no sync, no burst and no vertical interval anywhere in it.
TEST(VBISourceStageDecoding, TheTeletextSlicerRecoversThePacketsThatWentIn) {
  const VBISourceFormat format = bt8x8_pal_format();
  const std::array<uint8_t, kTeletextPacketBytes> expected = reference_packet();

  VBISourceStage stage(
      std::make_shared<FakeDeps>(make_packet_capture(format, expected)));
  const auto representation =
      representation_of(run_stage(stage, default_parameters()));
  ASSERT_NE(representation, nullptr);

  // EBU Tech. 3280-E §1.1.1: 4FSC PAL; the bit rate is fixed by ETSI EN
  // 300 706 §5.3.  These are the observer's own slicer settings.
  const TeletextSlicer slicer(kPalSampleRate, kTeletextBitRate);

  for (const size_t line : {size_t{6}, size_t{21}, size_t{319}, size_t{334}}) {
    const std::vector<int16_t> samples =
        representation->get_line_samples(0, line);
    ASSERT_FALSE(samples.empty()) << "line " << line;

    const TeletextLineResult result =
        slicer.slice(samples.data(), samples.size(), kPalBlack, kPalWhite);
    ASSERT_TRUE(result.valid)
        << "line " << line
        << " rejected: " << teletext_reject_reason_name(result.reject_reason);
    EXPECT_EQ(result.bytes, expected) << "line " << line;
  }

  // And a line the capture never carried data on yields nothing, rather than
  // the slicer finding a packet in manufactured signal.
  const std::vector<int16_t> blank = representation->get_line_samples(0, 100);
  ASSERT_FALSE(blank.empty());
  EXPECT_FALSE(
      slicer.slice(blank.data(), blank.size(), kPalBlack, kPalWhite).valid);
}

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------

TEST(VBISourceStageCalibration, FitsTheCapturesOwnOffsetAndReportsIt) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 4));
  VBISourceStage stage(deps);

  ObservationContext observations;
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, default_parameters(), observations);
  ASSERT_FALSE(outputs.empty());

  const auto offset =
      observations.get(FieldID(0), "vbi_source", "capture_offset");
  ASSERT_TRUE(offset.has_value());
  EXPECT_NEAR(std::get<double>(*offset), kTruthOffsetSamples, 0.5);
}

// A capture whose teletext cannot be located is not decoded with a guessed
// offset: it is refused (design §5.3.4).
TEST(VBISourceStageCalibration, RefusesACaptureItCannotLockTo) {
  const VBISourceFormat format = bt8x8_pal_format();
  auto deps = std::make_shared<FakeDeps>(std::vector<uint8_t>(
      static_cast<size_t>(format.bytes_per_frame() * 2u), 0u));
  VBISourceStage stage(deps);

  ObservationContext observations;
  EXPECT_THROW(stage.execute({}, default_parameters(), observations),
               UserDataError);
}

// A 525-line bt8x8 dump: NABTS on |nabts_records| of each field, through the
// VHS channel of design §5.3.6, and, when |captions| is set, a caption line of
// null pairs on line 21 of both fields (record 11) — at one true capture
// offset, with the driver's frame counter in each frame's trailer.
std::vector<uint8_t> make_synthetic_bt8x8_ntsc_capture(
    const VBISourceFormat& format, uint64_t frame_count,
    double capture_offset_samples, const std::vector<uint32_t>& nabts_records,
    bool captions) {
  VBITeletextService nabts;
  VBITeletextService caption;
  std::string error;
  EXPECT_TRUE(vbi_teletext_service(VBITVSystem::kNTSC,
                                   VBITeletextSystem::kNABTS, nabts, error))
      << error;
  EXPECT_TRUE(vbi_caption_service(VBITVSystem::kNTSC, caption, error)) << error;

  // A null pair, 0x80 0x80: seven 0 data bits and a 1 parity bit a byte,
  // sent least significant bit first and counted here in half-bit symbols.
  std::vector<bool> null_pair;
  for (uint32_t byte = 0; byte < 2u; ++byte) {
    for (uint32_t bit = 0; bit < 8u; ++bit) {
      null_pair.push_back(bit == 7u);
      null_pair.push_back(bit == 7u);
    }
  }

  std::vector<uint8_t> bytes(
      static_cast<size_t>(format.bytes_per_frame() * frame_count), 0);
  for (uint64_t frame = 0; frame < frame_count; ++frame) {
    for (uint32_t field = 0; field < 2u; ++field) {
      for (uint32_t index = 0; index < format.field_lines; ++index) {
        SyntheticVBILine line;
        line.sample_rate_hz = format.sample_rate_hz;
        line.valid_samples = format.valid_samples;
        line.logic0 = 60.0;
        line.noise_amplitude = 0.5;
        line.seed =
            static_cast<uint32_t>(frame * 64u + field * 32u + index + 1u);
        line.carries_data = false;
        if (std::find(nabts_records.begin(), nabts_records.end(), index) !=
            nabts_records.end()) {
          line.carries_data = true;
          line.bit_rate_hz = nabts.bit_rate_hz;
          line.pattern = nabts.cri_frc_pattern;
          line.payload_bits = nabts.payload_bytes * 8u;
          line.logic1 = 180.0;
          line.blur_bit_periods = 0.8;
          line.noise_amplitude = 2.0;
          line.anchor_position_samples = nabts.cri_start_samples(
              format.sample_rate_hz, capture_offset_samples);
        } else if (captions && index == 11u) {
          // CTA-608-E §4: the run-in and the data swing from blanking to
          // 50 IRE.
          line.carries_data = true;
          line.bit_rate_hz = caption.bit_rate_hz;
          line.pattern = caption.cri_frc_pattern;
          line.pattern_bits = caption.cri_bits + caption.frc_bits;
          line.run_in_bits = caption.cri_bits;
          line.payload = null_pair;
          line.logic1 = 140.0;
          line.anchor_position_samples = caption.cri_start_samples(
              format.sample_rate_hz, capture_offset_samples);
        }

        const std::vector<double> samples = render_synthetic_vbi_line(line);
        const uint64_t offset = frame * format.bytes_per_frame() +
                                field * format.bytes_per_field() +
                                index * format.bytes_per_record();
        for (uint32_t sample = 0; sample < format.valid_samples; ++sample) {
          bytes[static_cast<size_t>(offset + sample)] = static_cast<uint8_t>(
              std::clamp(std::round(samples[sample]), 0.0, 255.0));
        }
      }
    }

    const size_t trailer =
        static_cast<size_t>((frame + 1u) * format.bytes_per_frame() - 4u);
    const uint32_t counter = 1000u + static_cast<uint32_t>(frame);
    for (uint32_t byte = 0; byte < 4u; ++byte) {
      bytes[trailer + byte] =
          static_cast<uint8_t>((counter >> (8u * byte)) & 0xFFu);
    }
  }
  return bytes;
}

std::map<std::string, ParameterValue> bt8x8_ntsc_parameters(
    const std::string& service) {
  auto parameters = default_parameters();
  parameters["format"] =
      std::string("bt8x8 card dump, 8-bit (" + service + ", NTSC source)");
  return parameters;
}

VBISourceFormat bt8x8_nabts_ntsc_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(expand_vbi_source_preset(
      "bt8x8 card dump, 8-bit (NABTS, NTSC source)", format, error))
      << error;
  return format;
}

// A 525-line card dump that carries no teletext at all is no wrong
// configuration, and nearly every US recording carries line-21 captions: the
// capture offset is fitted on their run-in instead, and line 21 is written to
// the end of the caption line — further than a teletext packet runs — so a
// caption decoder reads it whole.
TEST(VBISourceStageCalibration, ACaptionsOnlyCaptureOpensOnItsCaptionRunIn) {
  constexpr double kTruthOffset = 258.0;
  const VBISourceFormat format = bt8x8_nabts_ntsc_format();
  // Eight frames: each field's run-in is fitted on its own records, and a fit
  // needs eight of them.
  auto deps = std::make_shared<FakeDeps>(make_synthetic_bt8x8_ntsc_capture(
      format, 8, kTruthOffset, {}, /*captions=*/true));
  VBISourceStage stage(deps);

  ObservationContext observations;
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, bt8x8_ntsc_parameters("NABTS"), observations);
  ASSERT_FALSE(outputs.empty());

  const auto offset =
      observations.get(FieldID(0), "vbi_source", "capture_offset");
  ASSERT_TRUE(offset.has_value());
  EXPECT_NEAR(std::get<double>(*offset), kTruthOffset, 0.5);

  const auto representation = representation_of(outputs.front());
  ASSERT_NE(representation, nullptr);
  const int16_t* frame = representation->get_frame(0);
  ASSERT_NE(frame, nullptr);

  VBIOutputFrame output;
  std::string error;
  ASSERT_TRUE(make_vbi_output_frame(VBITVSystem::kNTSC,
                                    VBITeletextSystem::kNABTS, output, error))
      << error;

  // The last bit of the null pair is its second byte's 1 parity bit, which
  // ends 51 half-bit symbols after the run-in's first rise at 10,5 us: 61,1
  // us, past the 60,8 us (output sample 870) a NABTS packet's written region
  // ends at.  Its last quarter is written, and written high.
  VBITeletextService caption;
  ASSERT_TRUE(vbi_caption_service(VBITVSystem::kNTSC, caption, error));
  const double last_bit_us =
      caption.t_offset_ns * 1e-3 + 50.75 / caption.bit_rate_hz * 1e6;
  const auto last_bit =
      static_cast<size_t>(std::lround(last_bit_us * 1e-6 * kNtscSampleRate));
  ASSERT_GT(last_bit, 870u);
  const int16_t* line_21 = frame + output.line_offset(20);
  EXPECT_GT(line_21[last_bit],
            (output.levels.logic0 + output.levels.logic1) / 2);
}

// The wrong choice of a 525-line service stays loud with captions aboard: a
// NABTS capture opened as WST is refused, naming the service it does carry,
// rather than opened on its captions with nothing for the teletext decoder.
TEST(VBISourceStageCalibration, TheOtherTeletextServiceIsNamedNotOpenedOn) {
  const VBISourceFormat format = bt8x8_nabts_ntsc_format();
  auto deps = std::make_shared<FakeDeps>(make_synthetic_bt8x8_ntsc_capture(
      format, 4, 258.0, {4u, 5u, 6u, 7u}, /*captions=*/true));
  VBISourceStage stage(deps);

  ObservationContext observations;
  try {
    (void)stage.execute({}, bt8x8_ntsc_parameters("WST"), observations);
    ADD_FAILURE() << "a NABTS capture opened as WST was not refused";
  } catch (const UserDataError& error) {
    EXPECT_NE(std::string(error.what()).find("NABTS"), std::string::npos)
        << error.what();
  }
}

// With neither teletext nor captions there is nothing to fit, and the capture
// is refused as before.
TEST(VBISourceStageCalibration, A525LineCaptureWithNothingToFitIsRefused) {
  const VBISourceFormat format = bt8x8_nabts_ntsc_format();
  auto deps = std::make_shared<FakeDeps>(make_synthetic_bt8x8_ntsc_capture(
      format, 4, 258.0, {}, /*captions=*/false));
  VBISourceStage stage(deps);

  ObservationContext observations;
  EXPECT_THROW(
      (void)stage.execute({}, bt8x8_ntsc_parameters("NABTS"), observations),
      UserDataError);
}

// ---------------------------------------------------------------------------
// Frame sequence
// ---------------------------------------------------------------------------

TEST(VBISourceStageSequence, AContinuousCounterIsReported) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 4));
  VBISourceStage stage(deps);

  ObservationContext observations;
  ASSERT_FALSE(stage.execute({}, default_parameters(), observations).empty());

  const auto summary =
      observations.get(FieldID(0), "vbi_source", "frame_sequence");
  ASSERT_TRUE(summary.has_value());
  EXPECT_NE(std::get<std::string>(*summary).find("continuous"),
            std::string::npos)
      << std::get<std::string>(*summary);
}

TEST(VBISourceStageSequence, PreservingDroppedFramesEmitsOnlyWhatIsPresent) {
  auto deps = std::make_shared<FakeDeps>(make_synthetic_capture(
      bt8x8_pal_format(), 4, kTruthOffsetSamples, 399598u, 1u, 2u));
  VBISourceStage stage(deps);

  ObservationContext observations;
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, default_parameters(), observations);
  ASSERT_FALSE(outputs.empty());

  EXPECT_EQ(representation_of(outputs.front())->frame_count(), 4u);

  const auto summary =
      observations.get(FieldID(0), "vbi_source", "frame_sequence");
  ASSERT_TRUE(summary.has_value());
  EXPECT_NE(std::get<std::string>(*summary).find("2 frame(s) missing"),
            std::string::npos)
      << std::get<std::string>(*summary);
}

TEST(VBISourceStageSequence, PaddingKeepsTheOutputAlignedWithTheSource) {
  auto deps = std::make_shared<FakeDeps>(make_synthetic_capture(
      bt8x8_pal_format(), 4, kTruthOffsetSamples, 399598u, 1u, 2u));
  VBISourceStage stage(deps);

  auto parameters = default_parameters();
  parameters["drops"] = std::string("pad");

  ObservationContext observations;
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, parameters, observations);
  ASSERT_FALSE(outputs.empty());

  const auto representation = representation_of(outputs.front());
  ASSERT_NE(representation, nullptr);
  EXPECT_EQ(representation->frame_count(), 6u);

  // A padded frame carries no data at all: it is blanking throughout.
  const std::vector<int16_t> padded = representation->get_frame_copy(2);
  ASSERT_EQ(padded.size(), static_cast<size_t>(kPalFrameSamples));
  const auto bounds = std::minmax_element(padded.begin(), padded.end());
  EXPECT_EQ(*bounds.first, kPalBlanking);
  EXPECT_EQ(*bounds.second, kPalBlanking);
}

// ---------------------------------------------------------------------------
// Rejected configurations
// ---------------------------------------------------------------------------

// A capture holding less than one whole frame has nothing to emit, so it is
// refused; a remainder on top of at least one whole frame is not, because that
// is only a capture that stopped where it stopped.
TEST(VBISourceStageValidation, RefusesACaptureShorterThanOneWholeFrame) {
  const VBISourceFormat format = bt8x8_pal_format();
  std::vector<uint8_t> capture = make_synthetic_capture(format, 1);
  capture.resize(capture.size() - 16u);

  VBISourceStage stage(std::make_shared<FakeDeps>(capture));
  ObservationContext observations;
  EXPECT_THROW(stage.execute({}, default_parameters(), observations),
               UserDataError);
}

TEST(VBISourceStageValidation, LoadsACaptureWithARaggedTrailingFrame) {
  const VBISourceFormat format = bt8x8_pal_format();
  std::vector<uint8_t> capture = make_synthetic_capture(format, 2);
  capture.resize(capture.size() - 16u);

  VBISourceStage stage(std::make_shared<FakeDeps>(capture));
  const auto representation =
      representation_of(run_stage(stage, default_parameters()));
  ASSERT_NE(representation, nullptr);
  EXPECT_EQ(representation->frame_count(), 1u);
}

TEST(VBISourceStageValidation, RefusesACaptureItCannotOpen) {
  auto deps = std::make_shared<FakeDeps>(std::vector<uint8_t>{});
  deps->open_succeeds = false;
  VBISourceStage stage(deps);

  ObservationContext observations;
  EXPECT_THROW(stage.execute({}, default_parameters(), observations),
               UserDataError);
}

TEST(VBISourceStageValidation, RefusesAnUnknownFormatPreset) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 1));
  VBISourceStage stage(deps);

  auto parameters = default_parameters();
  parameters["format"] = std::string("bt8x8-secam");

  ObservationContext observations;
  EXPECT_THROW(stage.execute({}, parameters, observations), UserDataError);
}

// ---------------------------------------------------------------------------
// 525-line captures
// ---------------------------------------------------------------------------

VBISourceFormat tbc_vbi_ntsc_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(
      expand_vbi_source_preset(".tbc VBI crop, 16-bit (WST)", format, error))
      << error;
  return format;
}

// A capture in the shape of the circulating NTSC VBI-only crops: 16-bit
// little-endian words in the decoder's amplitude domain, sixteen whole .tbc
// lines per field, records starting at 0H so the offset is zero.
// anchor_shift_samples moves the run-in away from the service table's nominal
// time, which is what a real transmission does: the tabulated 525-line figures
// were measured on particular captures and broadcasters disagree with them by
// up to a microsecond.  A crop of such a transmission is the case the stage has
// to measure rather than assume.
std::vector<uint8_t> make_synthetic_ntsc_capture(
    const VBISourceFormat& format, uint64_t frame_count,
    double anchor_shift_samples = 0.0) {
  VBITeletextService service;
  std::string error;
  EXPECT_TRUE(vbi_teletext_service(VBITVSystem::kNTSC, VBITeletextSystem::kWST,
                                   service, error))
      << error;

  std::vector<uint8_t> bytes(
      static_cast<size_t>(format.bytes_per_frame() * frame_count), 0);
  const double position =
      service.cri_start_samples(format.sample_rate_hz, 0.0) +
      anchor_shift_samples;

  for (uint64_t frame = 0; frame < frame_count; ++frame) {
    for (uint32_t field = 0; field < 2u; ++field) {
      for (uint32_t index = 0; index < format.field_lines; ++index) {
        SyntheticVBILine line;
        line.sample_rate_hz = format.sample_rate_hz;
        line.valid_samples = format.valid_samples;
        line.bit_rate_hz = service.bit_rate_hz;
        line.payload_bits = service.payload_bytes * 8u;
        line.anchor_position_samples = position;
        line.logic0 = static_cast<double>(kNtscBlack) * 64.0;
        line.logic1 = 624.0 * 64.0;
        line.noise_amplitude = 32.0;
        line.seed =
            static_cast<uint32_t>(frame * 64u + field * 32u + index + 1u);
        // Records outside the data range are the equalising line and the
        // active picture, neither of which carries a data service.
        line.carries_data = index >= format.field_range.start &&
                            index <= format.field_range.end;

        const std::vector<double> samples = render_synthetic_vbi_line(line);
        const uint64_t offset = frame * format.bytes_per_frame() +
                                field * format.bytes_per_field() +
                                index * format.bytes_per_record();
        for (uint32_t sample = 0; sample < format.valid_samples; ++sample) {
          const auto value = static_cast<uint32_t>(
              std::clamp(std::round(samples[sample]), 0.0, 65535.0));
          const size_t at = static_cast<size_t>(offset) + sample * 2u;
          bytes[at] = static_cast<uint8_t>(value & 0xFFu);
          bytes[at + 1u] = static_cast<uint8_t>((value >> 8u) & 0xFFu);
        }
      }
    }
  }
  return bytes;
}

std::map<std::string, ParameterValue> ntsc_parameters() {
  auto parameters = default_parameters();
  parameters["format"] = std::string(".tbc VBI crop, 16-bit (WST)");
  return parameters;
}

TEST(VBISourceStageNTSC, FramesAreNormativelySizedNTSCCVBS) {
  const VBISourceFormat format = tbc_vbi_ntsc_format();
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_ntsc_capture(format, 3));
  deps->declared_bits = 16u;
  VBISourceStage stage(deps);

  const auto representation =
      representation_of(run_stage(stage, ntsc_parameters()));
  ASSERT_NE(representation, nullptr);
  EXPECT_EQ(representation->frame_count(), 3u);

  const auto params = representation->get_video_parameters();
  ASSERT_TRUE(params.has_value());
  EXPECT_EQ(params->system, VideoSystem::NTSC);
  EXPECT_EQ(params->frame_width_nominal, kNtscSamplesPerLine);
  EXPECT_EQ(params->frame_height, kNtscFrameLines);
  EXPECT_EQ(params->blanking_level, kNtscBlanking);

  const auto descriptor = representation->get_frame_descriptor(0);
  ASSERT_TRUE(descriptor.has_value());
  EXPECT_EQ(descriptor->system, VideoSystem::NTSC);
  EXPECT_EQ(descriptor->samples_total, static_cast<size_t>(kNtscFrameSamples));
  EXPECT_EQ(descriptor->samples_per_line_nominal, 910u);
}

// The whole 525-line path: data on broadcast lines 10-21 and 273-284 and
// blanking everywhere else, with every sample inside the legal range.
TEST(VBISourceStageNTSC, DataLandsOnTheStandardLinesAndNowhereElse) {
  const VBISourceFormat format = tbc_vbi_ntsc_format();
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_ntsc_capture(format, 2));
  deps->declared_bits = 16u;
  VBISourceStage stage(deps);

  const auto representation =
      representation_of(run_stage(stage, ntsc_parameters()));
  ASSERT_NE(representation, nullptr);

  VBIOutputFrame output;
  std::string error;
  ASSERT_TRUE(make_vbi_output_frame(VBITVSystem::kNTSC, VBITeletextSystem::kWST,
                                    output, error))
      << error;

  const int16_t* frame = representation->get_frame(0);
  ASSERT_NE(frame, nullptr);

  for (uint32_t line = 0; line < output.lines_per_frame; ++line) {
    // The twelve data lines, and line 22 (frame lines 21 and 284), which
    // carries the crop's record 13 for the caption observer.
    const bool data =
        (line >= 9u && line <= 21u) || (line >= 272u && line <= 284u);
    const int16_t* samples = frame + output.line_offset(line);
    bool anything_written = false;
    for (size_t sample = 0; sample < output.line_length(line); ++sample) {
      EXPECT_GE(samples[sample], kVBIOutputSampleMin);
      EXPECT_LE(samples[sample], kVBIOutputSampleMax);
      if (samples[sample] != static_cast<int16_t>(output.levels.blanking)) {
        anything_written = true;
      }
    }
    EXPECT_EQ(anything_written, data) << "line " << line;
  }
}

// Nothing in a capture records which of the two 525-line services it carries,
// so the service is configuration — and choosing it changes the framing code
// that is placed, the packet length, and the amplitude the data sits at.
TEST(VBISourceStageNTSC, NABTSIsPlacedAsItsOwnService) {
  const VBISourceFormat format = tbc_vbi_ntsc_format();
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_ntsc_capture(format, 2));
  deps->declared_bits = 16u;
  VBISourceStage stage(deps);

  auto parameters = ntsc_parameters();
  parameters["format"] = std::string(".tbc VBI crop, 16-bit (NABTS)");

  ObservationContext observations;
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, parameters, observations);
  ASSERT_FALSE(outputs.empty());
  const auto representation = representation_of(outputs.front());
  ASSERT_NE(representation, nullptr);
  EXPECT_EQ(representation->frame_count(), 2u);

  // The configured service reaches the output, so a downstream decoder sees
  // which one it is being handed rather than assuming.
  const auto system =
      observations.get(FieldID(0), "vbi_source", "teletext_system");
  ASSERT_TRUE(system.has_value());
  EXPECT_EQ(std::get<std::string>(*system), "NABTS");

  // NABTS puts logic 0 at blanking where WST puts it at black, so the two
  // place the same records at measurably different amplitudes.
  VBIOutputFrame nabts_frame;
  VBIOutputFrame wst_frame;
  std::string error;
  ASSERT_TRUE(make_vbi_output_frame(
      VBITVSystem::kNTSC, VBITeletextSystem::kNABTS, nabts_frame, error))
      << error;
  ASSERT_TRUE(make_vbi_output_frame(VBITVSystem::kNTSC, VBITeletextSystem::kWST,
                                    wst_frame, error))
      << error;
  EXPECT_NE(nabts_frame.levels.logic0, wst_frame.levels.logic0);

  // And the data still lands on the twelve lines the standard gives it.
  const int16_t* frame = representation->get_frame(0);
  ASSERT_NE(frame, nullptr);
  for (uint32_t line = 0; line < nabts_frame.lines_per_frame; ++line) {
    // The twelve data lines, and line 22, the crop's caption record.
    const bool data =
        (line >= 9u && line <= 21u) || (line >= 272u && line <= 284u);
    const int16_t* samples = frame + nabts_frame.line_offset(line);
    const auto bounds =
        std::minmax_element(samples, samples + nabts_frame.line_length(line));
    const bool written = *bounds.second != nabts_frame.levels.blanking ||
                         *bounds.first != nabts_frame.levels.blanking;
    EXPECT_EQ(written, data) << "line " << line;
  }
}

// A capture ending on an odd field is ordinary; the trailing field is one
// short of a frame and is simply not emitted.
TEST(VBISourceStageNTSC, ACaptureEndingOnAnOddFieldLoadsAndDropsThatField) {
  const VBISourceFormat format = tbc_vbi_ntsc_format();
  std::vector<uint8_t> capture = make_synthetic_ntsc_capture(format, 3);
  capture.resize(capture.size() -
                 static_cast<size_t>(format.bytes_per_field()));

  auto deps = std::make_shared<FakeDeps>(capture);
  deps->declared_bits = 16u;
  VBISourceStage stage(deps);
  const auto representation =
      representation_of(run_stage(stage, ntsc_parameters()));
  ASSERT_NE(representation, nullptr);
  EXPECT_EQ(representation->frame_count(), 2u);
}

// A crop of a transmission that put its run-in earlier than the service table
// says must have the data window moved to follow it. The window is cut one bit
// period ahead of the anchor, so a transmission four bits early loses the head
// of every run-in it places if the tabulated figure is used as transmitted —
// which is the whole of what the measurement prevents.
TEST(VBISourceStageNTSC, TheDataWindowFollowsTheTransmissionsOwnRunIn) {
  const VBISourceFormat format = tbc_vbi_ntsc_format();

  // Stored frame line 14 is broadcast field line 15, a data line of field 1.
  constexpr size_t kDataLine = 14;

  // Where the written region of that line begins and ends. Outside it the line
  // is exactly blanking; inside it every sample sits at logic 0 or above, so
  // the first and last samples that differ from blanking are the region.
  auto written_region = [&](double shift) {
    auto deps = std::make_shared<FakeDeps>(
        make_synthetic_ntsc_capture(format, 3, shift));
    deps->declared_bits = 16u;
    VBISourceStage stage(deps);
    const auto representation =
        representation_of(run_stage(stage, ntsc_parameters()));
    EXPECT_NE(representation, nullptr);

    const std::vector<int16_t> frame = representation->get_frame_copy(1);
    const size_t begin_of_line = kDataLine * kNtscSamplesPerLine;
    size_t first = kNtscSamplesPerLine;
    size_t last = 0;
    for (size_t sample = 0; sample < kNtscSamplesPerLine; ++sample) {
      if (frame[begin_of_line + sample] == kNtscBlanking) continue;
      first = std::min(first, sample);
      last = std::max(last, sample);
    }
    return std::pair<size_t, size_t>{first, last};
  };

  const auto nominal = written_region(0.0);
  ASSERT_LT(nominal.first, nominal.second);

  // A transmission eleven samples — some four bits — ahead of the table, which
  // is what the reference NABTS crop measures against its own service entry.
  constexpr double kShiftSamples = -11.0;
  const auto early = written_region(kShiftSamples);

  // The whole region moves with the run-in rather than the run-in moving out
  // of a fixed region.
  EXPECT_NEAR(
      static_cast<double>(early.first) - static_cast<double>(nominal.first),
      kShiftSamples, 1.5);
  EXPECT_NEAR(
      static_cast<double>(early.second) - static_cast<double>(nominal.second),
      kShiftSamples, 1.5);

  // And it keeps its width: nothing is clipped off either end.
  EXPECT_NEAR(static_cast<double>(early.second - early.first),
              static_cast<double>(nominal.second - nominal.first), 2.0);
}

// A capture cut part-way through a record is just as ordinary — it is how a
// card dump stopped at the keyboard ends — and is loaded the same way, with
// the ragged tail dropped rather than the file refused.
TEST(VBISourceStageNTSC, ACaptureCutMidRecordLoadsAndDropsTheRaggedTail) {
  const VBISourceFormat format = tbc_vbi_ntsc_format();
  std::vector<uint8_t> capture = make_synthetic_ntsc_capture(format, 3);

  // One whole field, one whole record and 17 bytes past the second frame:
  // a remainder that is none of the container's own quantities.
  capture.resize(capture.size() -
                 static_cast<size_t>(format.bytes_per_frame()) +
                 static_cast<size_t>(format.bytes_per_field()) +
                 static_cast<size_t>(format.bytes_per_record()) + 17u);

  auto deps = std::make_shared<FakeDeps>(capture);
  deps->declared_bits = 16u;
  VBISourceStage stage(deps);
  const auto representation =
      representation_of(run_stage(stage, ntsc_parameters()));
  ASSERT_NE(representation, nullptr);
  EXPECT_EQ(representation->frame_count(), 2u);
}

// ---------------------------------------------------------------------------
// Preview
// ---------------------------------------------------------------------------

TEST(VBISourceStagePreview, AdvertisesNoPreviewUntilACaptureIsLoaded) {
  VBISourceStage stage;
  EXPECT_FALSE(stage.get_preview_capability().is_valid());
}

TEST(VBISourceStagePreview, AdvertisesTheBuiltFramesOnceLoaded) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 3));
  VBISourceStage stage(deps);
  ASSERT_NE(run_stage(stage, default_parameters()), nullptr);

  const StagePreviewCapability capability = stage.get_preview_capability();
  ASSERT_TRUE(capability.is_valid());
  EXPECT_GT(capability.navigation_extent.item_count, 0u);
}

// ---------------------------------------------------------------------------
// Line access
// ---------------------------------------------------------------------------

// Every line of every frame, read through both line accessors, against the
// same line of the built frame: a line read never builds a frame, and must
// never differ from one either.
void expect_lines_match_frames(const VideoFrameRepresentation& representation) {
  const auto params = representation.get_video_parameters();
  ASSERT_TRUE(params.has_value());
  const size_t width = static_cast<size_t>(params->frame_width_nominal);
  const size_t height = static_cast<size_t>(params->frame_height);

  for (FrameID id = 0; id < static_cast<FrameID>(representation.frame_count());
       ++id) {
    const std::vector<int16_t> frame = representation.get_frame_copy(id);
    ASSERT_FALSE(frame.empty()) << "frame " << id;
    for (size_t line = 0; line < height; ++line) {
      const size_t offset =
          frame_line_sample_offset(params->system, width, line);
      const size_t length =
          frame_line_sample_count(params->system, width, line);

      const std::vector<int16_t> samples =
          representation.get_line_samples(id, line);
      ASSERT_EQ(samples.size(), width) << "frame " << id << " line " << line;
      ASSERT_TRUE(
          std::equal(samples.begin(), samples.end(), frame.begin() + offset))
          << "frame " << id << " line " << line;

      const int16_t* pointer = representation.get_line(id, line);
      ASSERT_NE(pointer, nullptr) << "frame " << id << " line " << line;
      ASSERT_TRUE(std::equal(pointer, pointer + length, frame.begin() + offset))
          << "frame " << id << " line " << line;
    }
  }
}

TEST(VBISourceStageLines, EveryPALLineReadsAsTheBuiltFrameHoldsIt) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 2));
  VBISourceStage stage(deps);
  const auto representation =
      representation_of(run_stage(stage, default_parameters()));
  ASSERT_NE(representation, nullptr);

  expect_lines_match_frames(*representation);
}

TEST(VBISourceStageLines, EveryNTSCLineReadsAsTheBuiltFrameHoldsIt) {
  const VBISourceFormat format = tbc_vbi_ntsc_format();
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_ntsc_capture(format, 3));
  deps->declared_bits = 16u;
  VBISourceStage stage(deps);
  const auto representation =
      representation_of(run_stage(stage, ntsc_parameters()));
  ASSERT_NE(representation, nullptr);

  expect_lines_match_frames(*representation);
}

// A reader of a few lines of every frame has only those resampled — line 21's
// decoders read two of a frame's twenty-four — and every line still reads as
// the built frame holds it: the lines read first, the whole frame built from a
// placement that held only those, and every line read after them.
TEST(VBISourceStageLines, LinesReadAloneReadAsTheBuiltFrameHoldsThem) {
  const std::vector<uint8_t> capture =
      make_synthetic_capture(bt8x8_pal_format(), 3);
  VBISourceStage reference_stage(std::make_shared<FakeDeps>(capture));
  const auto reference =
      representation_of(run_stage(reference_stage, default_parameters()));
  VBISourceStage stage(std::make_shared<FakeDeps>(capture));
  const auto representation =
      representation_of(run_stage(stage, default_parameters()));
  ASSERT_NE(reference, nullptr);
  ASSERT_NE(representation, nullptr);

  const auto params = reference->get_video_parameters();
  ASSERT_TRUE(params.has_value());
  const size_t width = static_cast<size_t>(params->frame_width_nominal);
  const size_t height = static_cast<size_t>(params->frame_height);
  const auto frames = static_cast<FrameID>(reference->frame_count());

  const auto expect_line = [&](FrameID id, size_t line) {
    const std::vector<int16_t> frame = reference->get_frame_copy(id);
    const size_t offset = frame_line_sample_offset(params->system, width, line);
    const size_t length = frame_line_sample_count(params->system, width, line);
    const int16_t* pointer = representation->get_line(id, line);
    ASSERT_NE(pointer, nullptr) << "frame " << id << " line " << line;
    EXPECT_TRUE(std::equal(pointer, pointer + length, frame.begin() + offset))
        << "frame " << id << " line " << line;
    const std::vector<int16_t> samples =
        representation->get_line_samples(id, line);
    EXPECT_TRUE(
        std::equal(samples.begin(), samples.end(), frame.begin() + offset))
        << "frame " << id << " line " << line;
  };

  for (FrameID id = 0; id < frames; ++id) {
    expect_line(id, 21);
    expect_line(id, 334);
  }
  EXPECT_EQ(representation->get_frame_copy(0), reference->get_frame_copy(0));
  for (FrameID id = 0; id < frames; ++id) {
    for (size_t line = 0; line < height; ++line) {
      expect_line(id, line);
    }
    EXPECT_EQ(representation->get_frame_copy(id), reference->get_frame_copy(id))
        << "frame " << id;
  }
}

// A padded frame has no source: every line of it is blanking, read either way.
TEST(VBISourceStageLines, APaddedFrameReadsAsBlankingOnEveryLine) {
  auto deps = std::make_shared<FakeDeps>(make_synthetic_capture(
      bt8x8_pal_format(), 4, kTruthOffsetSamples, 399598u, 1u, 2u));
  VBISourceStage stage(deps);
  auto parameters = default_parameters();
  parameters["drops"] = std::string("pad");
  const auto representation = representation_of(run_stage(stage, parameters));
  ASSERT_NE(representation, nullptr);
  ASSERT_EQ(representation->frame_count(), 6u);

  for (const size_t line : {size_t{0}, size_t{6}, size_t{21}, size_t{319}}) {
    const std::vector<int16_t> samples =
        representation->get_line_samples(2, line);
    ASSERT_EQ(samples.size(), static_cast<size_t>(kPalSamplesPerLineNominal));
    EXPECT_TRUE(std::all_of(samples.begin(), samples.end(),
                            [](int16_t s) { return s == kPalBlanking; }))
        << "line " << line;
  }
  expect_lines_match_frames(*representation);
}

TEST(VBISourceStageLines, LinesOutsideTheFrameOrTheCaptureAreRefused) {
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_capture(bt8x8_pal_format(), 2));
  VBISourceStage stage(deps);
  const auto representation =
      representation_of(run_stage(stage, default_parameters()));
  ASSERT_NE(representation, nullptr);

  EXPECT_TRUE(representation->get_line_samples(2, 6).empty());
  EXPECT_EQ(representation->get_line(2, 6), nullptr);
  EXPECT_TRUE(
      representation->get_line_samples(0, static_cast<size_t>(kPalFrameLines))
          .empty());
  EXPECT_EQ(representation->get_line(0, static_cast<size_t>(kPalFrameLines)),
            nullptr);
}

// The lines of the data window a decoder reads, frame by frame.
std::vector<std::vector<int16_t>> read_data_lines(
    const VideoFrameRepresentation& representation, FrameID id) {
  std::vector<std::vector<int16_t>> lines;
  for (size_t line = 9; line <= 20; ++line) {
    lines.push_back(representation.get_line_samples(id, line));
    lines.push_back(representation.get_line_samples(id, line + 263u));
  }
  return lines;
}

// Readers working through a capture together, as a sink's slicing threads
// do, are served by the read-ahead; what each of them reads is exactly what a
// single reader going frame by frame reads.
TEST(VBISourceStageLines, ReadersWorkingTogetherReadWhatOneReaderReads) {
  constexpr uint64_t kFrames = 160;
  constexpr int kReaders = 8;
  const VBISourceFormat format = tbc_vbi_ntsc_format();
  const std::vector<uint8_t> capture =
      make_synthetic_ntsc_capture(format, kFrames);

  auto reference_deps = std::make_shared<FakeDeps>(capture);
  reference_deps->declared_bits = 16u;
  VBISourceStage reference_stage(reference_deps);
  const auto reference =
      representation_of(run_stage(reference_stage, ntsc_parameters()));
  ASSERT_NE(reference, nullptr);

  auto deps = std::make_shared<FakeDeps>(capture);
  deps->declared_bits = 16u;
  VBISourceStage stage(deps);
  const auto representation =
      representation_of(run_stage(stage, ntsc_parameters()));
  ASSERT_NE(representation, nullptr);
  ASSERT_EQ(representation->frame_count(), kFrames);

  std::vector<std::vector<std::vector<int16_t>>> read(kFrames);
  std::atomic<uint64_t> next{0};
  std::vector<std::thread> readers;
  for (int reader = 0; reader < kReaders; ++reader) {
    readers.emplace_back([&] {
      for (uint64_t frame = next.fetch_add(1); frame < kFrames;
           frame = next.fetch_add(1)) {
        read[frame] =
            read_data_lines(*representation, static_cast<FrameID>(frame));
      }
    });
  }
  for (std::thread& reader : readers) {
    reader.join();
  }

  for (uint64_t frame = 0; frame < kFrames; ++frame) {
    ASSERT_EQ(read[frame],
              read_data_lines(*reference, static_cast<FrameID>(frame)))
        << "frame " << frame;
  }

  // The pass was recognised and decoded ahead, on byte sources of the
  // read-ahead's own.
  EXPECT_GE(deps->opens.load(), 2u);
}

// A thread keeps the placement it last read a line of in front of the shared
// cache. That placement is its own representation's: one thread reading the
// same frame of two captures in turn is handed each capture's own lines.
TEST(VBISourceStageLines, AThreadReadingTwoCapturesInTurnGetsEachItsOwnLines) {
  constexpr uint64_t kFrames = 4;
  const VBISourceFormat format = tbc_vbi_ntsc_format();
  const std::vector<uint8_t> first_capture =
      make_synthetic_ntsc_capture(format, kFrames);
  // The second capture carries the first's frame 3 as its frame 2.
  std::vector<uint8_t> second_capture = first_capture;
  const auto frame_bytes =
      static_cast<std::ptrdiff_t>(format.bytes_per_frame());
  std::copy(first_capture.begin() + 3 * frame_bytes,
            first_capture.begin() + 4 * frame_bytes,
            second_capture.begin() + 2 * frame_bytes);

  auto first_deps = std::make_shared<FakeDeps>(first_capture);
  first_deps->declared_bits = 16u;
  VBISourceStage first_stage(first_deps);
  const auto first =
      representation_of(run_stage(first_stage, ntsc_parameters()));
  auto second_deps = std::make_shared<FakeDeps>(second_capture);
  second_deps->declared_bits = 16u;
  VBISourceStage second_stage(second_deps);
  const auto second =
      representation_of(run_stage(second_stage, ntsc_parameters()));
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  // Each read on a thread of its own, so that nothing read before stands in
  // front of it.
  const auto read_alone = [](const VideoFrameRepresentation& representation,
                             FrameID id, size_t line) {
    std::vector<int16_t> samples;
    std::thread([&] {
      samples = representation.get_line_samples(id, line);
    }).join();
    return samples;
  };
  constexpr size_t kLine = 12;
  const std::vector<int16_t> first_line = read_alone(*first, 2, kLine);
  const std::vector<int16_t> second_line = read_alone(*second, 2, kLine);
  ASSERT_FALSE(first_line.empty());
  ASSERT_NE(first_line, second_line);

  for (int round = 0; round < 3; ++round) {
    EXPECT_EQ(first->get_line_samples(2, kLine), first_line);
    EXPECT_EQ(second->get_line_samples(2, kLine), second_line);
  }
}

// A single frame read out of the middle of a capture — a preview jumping to
// it — is read through the capture the stage opened, and starts nothing.
TEST(VBISourceStageLines, ASingleFrameReadOpensNothingFurther) {
  const VBISourceFormat format = tbc_vbi_ntsc_format();
  auto deps =
      std::make_shared<FakeDeps>(make_synthetic_ntsc_capture(format, 20));
  deps->declared_bits = 16u;
  VBISourceStage stage(deps);
  const auto representation =
      representation_of(run_stage(stage, ntsc_parameters()));
  ASSERT_NE(representation, nullptr);

  EXPECT_EQ(read_data_lines(*representation, 12).size(), 24u);
  EXPECT_EQ(deps->opens.load(), 1u);
}

}  // namespace
}  // namespace orc
