/*
 * File:        video_sink_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit test(s) for VideoSinkStage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/sinks/common/video_sink_stage.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace orc_unit_test {
namespace {
const orc::ParameterDescriptor* find_parameter(
    const std::vector<orc::ParameterDescriptor>& descriptors,
    const std::string& name) {
  auto it = std::find_if(descriptors.begin(), descriptors.end(),
                         [&name](const orc::ParameterDescriptor& descriptor) {
                           return descriptor.name == name;
                         });

  return (it == descriptors.end()) ? nullptr : &(*it);
}

bool has_string(const std::vector<std::string>& values,
                const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

std::string string_param(
    const std::map<std::string, orc::ParameterValue>& params,
    const std::string& name) {
  auto it = params.find(name);
  if (it == params.end() || !std::holds_alternative<std::string>(it->second)) {
    return "";
  }
  return std::get<std::string>(it->second);
}
}  // namespace

TEST(VideoSinkStageTest, NodeTypeInfo_ReportsVideoSinkIdentity) {
  orc::VideoSinkStage stage;
  const auto info = stage.get_node_type_info();

  EXPECT_EQ(info.stage_name, "video_sink");
  EXPECT_EQ(info.display_name, "Video Sink");
}

TEST(VideoSinkStageTest, ParameterDescriptors_OfferRawAndFfmpegModes) {
  orc::VideoSinkStage stage;
  auto descriptors = stage.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Composite);

  const auto* output_mode = find_parameter(descriptors, "output_mode");
  ASSERT_NE(output_mode, nullptr);
  const auto& modes = output_mode->constraints.allowed_strings;
  ASSERT_EQ(modes.size(), 2U);
  EXPECT_TRUE(has_string(modes, "raw"));
  EXPECT_TRUE(has_string(modes, "ffmpeg"));

  const auto* raw_format = find_parameter(descriptors, "raw_format");
  ASSERT_NE(raw_format, nullptr);
  const auto& raw_allowed = raw_format->constraints.allowed_strings;
  ASSERT_EQ(raw_allowed.size(), 3U);
  EXPECT_TRUE(has_string(raw_allowed, "rgb"));
  EXPECT_TRUE(has_string(raw_allowed, "yuv"));
  EXPECT_TRUE(has_string(raw_allowed, "y4m"));
  ASSERT_TRUE(raw_format->constraints.depends_on.has_value());
  EXPECT_EQ(raw_format->constraints.depends_on->parameter_name, "output_mode");

  const auto* ffmpeg_format = find_parameter(descriptors, "ffmpeg_format");
  ASSERT_NE(ffmpeg_format, nullptr);
  const auto& ffmpeg_allowed = ffmpeg_format->constraints.allowed_strings;
  EXPECT_FALSE(ffmpeg_allowed.empty());
  EXPECT_FALSE(has_string(ffmpeg_allowed, "rgb"));
  EXPECT_FALSE(has_string(ffmpeg_allowed, "yuv"));
  EXPECT_FALSE(has_string(ffmpeg_allowed, "y4m"));
  EXPECT_TRUE(has_string(ffmpeg_allowed, "mp4-h264"));
  EXPECT_TRUE(has_string(ffmpeg_allowed, "nut-rawvideo"));
  EXPECT_TRUE(has_string(ffmpeg_allowed, "nut-ffv1"));
  ASSERT_TRUE(ffmpeg_format->constraints.depends_on.has_value());
  EXPECT_EQ(ffmpeg_format->constraints.depends_on->parameter_name,
            "output_mode");

  // The legacy single output_format parameter must not be exposed.
  EXPECT_EQ(find_parameter(descriptors, "output_format"), nullptr);
}

TEST(VideoSinkStageTest, ParameterDescriptors_KeyEncoderControlsOffFormat) {
  orc::VideoSinkStage stage;
  auto descriptors = stage.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Composite);

  for (const auto* name :
       {"encoder_preset", "encoder_crf", "encoder_bitrate", "hardware_encoder",
        "prores_profile", "use_lossless_mode", "apply_deinterlace",
        "embed_audio", "embed_closed_captions", "embed_chapter_metadata"}) {
    const auto* descriptor = find_parameter(descriptors, name);
    ASSERT_NE(descriptor, nullptr) << name;
    ASSERT_TRUE(descriptor->constraints.depends_on.has_value()) << name;
    EXPECT_EQ(descriptor->constraints.depends_on->parameter_name,
              "ffmpeg_format")
        << name;
  }
}

TEST(VideoSinkStageTest, ParameterDescriptors_Av1EnablesRateControls) {
  orc::VideoSinkStage stage;
  const auto descriptors = stage.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Composite);

  for (const auto* name : {"encoder_crf", "encoder_bitrate"}) {
    const auto* descriptor = find_parameter(descriptors, name);
    ASSERT_NE(descriptor, nullptr) << name;
    ASSERT_TRUE(descriptor->constraints.depends_on.has_value()) << name;
    EXPECT_TRUE(has_string(descriptor->constraints.depends_on->required_values,
                           "mp4-av1"))
        << name;
  }
}

TEST(VideoSinkStageTest, Decoder_OptionsIncludeNtscPathsForCompositeAndYc) {
  orc::VideoSinkStage stage;

  for (const auto source_type :
       {orc::SourceType::Composite, orc::SourceType::YC}) {
    auto descriptors =
        stage.get_parameter_descriptors(orc::VideoSystem::NTSC, source_type);
    const auto* decoder_type = find_parameter(descriptors, "decoder_type");

    ASSERT_NE(decoder_type, nullptr);
    const auto& allowed = decoder_type->constraints.allowed_strings;

    if (!decoder_type->constraints.default_value.has_value()) {
      FAIL() << "Expected decoder_type default_value to have a value";
      return;
    }
    ASSERT_TRUE(std::holds_alternative<std::string>(
        *decoder_type->constraints.default_value));
    EXPECT_EQ(std::get<std::string>(*decoder_type->constraints.default_value),
              "ntsc2d");

    EXPECT_FALSE(has_string(allowed, "auto"));
    EXPECT_TRUE(has_string(allowed, "mono"));
    EXPECT_TRUE(has_string(allowed, "ntsc1d"));
    EXPECT_TRUE(has_string(allowed, "ntsc2d"));
    EXPECT_TRUE(has_string(allowed, "ntsc3d"));
    EXPECT_TRUE(has_string(allowed, "ntsc3dnoadapt"));
    EXPECT_FALSE(has_string(allowed, "pal2d"));
    EXPECT_FALSE(has_string(allowed, "transform2d"));
    EXPECT_FALSE(has_string(allowed, "transform3d"));

    if (source_type == orc::SourceType::YC) {
      EXPECT_NE(decoder_type->description.find("YC sources"),
                std::string::npos);
    } else {
      EXPECT_EQ(decoder_type->description.find("YC sources"),
                std::string::npos);
    }
  }
}

TEST(VideoSinkStageTest, Decoder_OptionsIncludePalPathsForCompositeAndYc) {
  orc::VideoSinkStage stage;

  for (const auto video_system :
       {orc::VideoSystem::PAL, orc::VideoSystem::PAL_M}) {
    for (const auto source_type :
         {orc::SourceType::Composite, orc::SourceType::YC}) {
      auto descriptors =
          stage.get_parameter_descriptors(video_system, source_type);
      const auto* decoder_type = find_parameter(descriptors, "decoder_type");

      ASSERT_NE(decoder_type, nullptr);
      const auto& allowed = decoder_type->constraints.allowed_strings;

      if (!decoder_type->constraints.default_value.has_value()) {
        FAIL() << "Expected decoder_type default_value to have a value";
        return;
      }
      ASSERT_TRUE(std::holds_alternative<std::string>(
          *decoder_type->constraints.default_value));
      EXPECT_EQ(std::get<std::string>(*decoder_type->constraints.default_value),
                "pal2d");

      EXPECT_FALSE(has_string(allowed, "auto"));
      EXPECT_TRUE(has_string(allowed, "mono"));
      EXPECT_TRUE(has_string(allowed, "pal2d"));
      EXPECT_TRUE(has_string(allowed, "transform2d"));
      EXPECT_TRUE(has_string(allowed, "transform3d"));
      EXPECT_FALSE(has_string(allowed, "ntsc1d"));
      EXPECT_FALSE(has_string(allowed, "ntsc2d"));
      EXPECT_FALSE(has_string(allowed, "ntsc3d"));
      EXPECT_FALSE(has_string(allowed, "ntsc3dnoadapt"));

      if (source_type == orc::SourceType::YC) {
        EXPECT_NE(decoder_type->description.find("YC sources"),
                  std::string::npos);
      } else {
        EXPECT_EQ(decoder_type->description.find("YC sources"),
                  std::string::npos);
      }
    }
  }
}

TEST(VideoSinkStageTest, SetParameters_RejectsInvalidOutputMode) {
  orc::VideoSinkStage stage;

  EXPECT_FALSE(stage.set_parameters({{"output_mode", std::string("dvd")}}));
}

TEST(VideoSinkStageTest, SetParameters_RejectsEncodedValueAsRawFormat) {
  orc::VideoSinkStage stage;

  EXPECT_FALSE(stage.set_parameters({{"raw_format", std::string("mp4-h264")}}));
}

TEST(VideoSinkStageTest, SetParameters_RejectsRawValueAsFfmpegFormat) {
  orc::VideoSinkStage stage;

  EXPECT_FALSE(stage.set_parameters({{"ffmpeg_format", std::string("rgb")}}));
}

TEST(VideoSinkStageTest, SetParameters_SelectsRawOutput) {
  orc::VideoSinkStage stage;

  const bool ok = stage.set_parameters({{"output_mode", std::string("raw")},
                                        {"raw_format", std::string("y4m")}});

  EXPECT_TRUE(ok);
  const auto params = stage.get_parameters();
  EXPECT_EQ(string_param(params, "output_mode"), "raw");
  EXPECT_EQ(string_param(params, "raw_format"), "y4m");
}

TEST(VideoSinkStageTest, SetParameters_SelectsFfmpegOutput) {
  orc::VideoSinkStage stage;

  const bool ok =
      stage.set_parameters({{"output_mode", std::string("ffmpeg")},
                            {"ffmpeg_format", std::string("mkv-ffv1")}});

#ifdef HAVE_FFMPEG
  EXPECT_TRUE(ok);
  const auto params = stage.get_parameters();
  EXPECT_EQ(string_param(params, "output_mode"), "ffmpeg");
  EXPECT_EQ(string_param(params, "ffmpeg_format"), "mkv-ffv1");
#else
  EXPECT_FALSE(ok);
#endif
}

TEST(VideoSinkStageTest, SetParameters_MapsLegacyRawOutputFormat) {
  // Projects saved before the raw and FFmpeg sinks were merged store a
  // single output_format value; it must select the matching mode.
  orc::VideoSinkStage stage;

  const bool ok = stage.set_parameters({{"output_format", std::string("y4m")}});

  EXPECT_TRUE(ok);
  const auto params = stage.get_parameters();
  EXPECT_EQ(string_param(params, "output_mode"), "raw");
  EXPECT_EQ(string_param(params, "raw_format"), "y4m");
  EXPECT_TRUE(params.find("output_format") == params.end());
}

TEST(VideoSinkStageTest, SetParameters_MapsLegacyFfmpegOutputFormat) {
  orc::VideoSinkStage stage;

  const bool ok =
      stage.set_parameters({{"output_format", std::string("mkv-ffv1")}});

#ifdef HAVE_FFMPEG
  EXPECT_TRUE(ok);
  const auto params = stage.get_parameters();
  EXPECT_EQ(string_param(params, "output_mode"), "ffmpeg");
  EXPECT_EQ(string_param(params, "ffmpeg_format"), "mkv-ffv1");
#else
  EXPECT_FALSE(ok);
#endif
}

TEST(VideoSinkStageTest, SetParameters_RejectsUnknownLegacyOutputFormat) {
  orc::VideoSinkStage stage;

  EXPECT_FALSE(
      stage.set_parameters({{"output_format", std::string("avi-divx")}}));
}

TEST(VideoSinkStageTest, GetParameters_KeepsFfmpegSpecificControls) {
  orc::VideoSinkStage stage;
  const auto params = stage.get_parameters();

  EXPECT_TRUE(params.find("encoder_preset") != params.end());
  EXPECT_TRUE(params.find("encoder_crf") != params.end());
  EXPECT_TRUE(params.find("encoder_bitrate") != params.end());
  EXPECT_TRUE(params.find("embed_audio") != params.end());
  EXPECT_TRUE(params.find("embed_closed_captions") != params.end());
}

TEST(VideoSinkStageTest, StageTools_IncludeFfmpegPresetConfig) {
  orc::VideoSinkStage stage;
  const auto tools = stage.get_stage_tools();

  ASSERT_EQ(tools.size(), 1U);
  EXPECT_EQ(tools.front().tool_id, "ffmpeg_preset_config");
}

TEST(VideoSinkStageTest, ParameterDescriptors_OfferDisplayAspectRatio) {
  orc::VideoSinkStage stage;
  auto descriptors = stage.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Composite);

  const auto* aspect = find_parameter(descriptors, "display_aspect_ratio");
  ASSERT_NE(aspect, nullptr);
  const auto& allowed = aspect->constraints.allowed_strings;
  ASSERT_EQ(allowed.size(), 3U);
  EXPECT_TRUE(has_string(allowed, "auto"));
  EXPECT_TRUE(has_string(allowed, "4:3"));
  EXPECT_TRUE(has_string(allowed, "16:9"));

  ASSERT_TRUE(aspect->constraints.default_value.has_value());
  ASSERT_TRUE(
      std::holds_alternative<std::string>(*aspect->constraints.default_value));
  EXPECT_EQ(std::get<std::string>(*aspect->constraints.default_value), "auto");

  ASSERT_TRUE(aspect->constraints.depends_on.has_value());
  EXPECT_EQ(aspect->constraints.depends_on->parameter_name, "output_mode");
}

TEST(VideoSinkStageTest, ParameterDescriptors_Ffv1SlicesCoversNutFfv1) {
  orc::VideoSinkStage stage;
  auto descriptors = stage.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Composite);

  const auto* ffv1_slices = find_parameter(descriptors, "ffv1_slices");
  ASSERT_NE(ffv1_slices, nullptr);
  ASSERT_TRUE(ffv1_slices->constraints.depends_on.has_value());
  EXPECT_TRUE(has_string(ffv1_slices->constraints.depends_on->required_values,
                         "nut-ffv1"));
}

TEST(VideoSinkStageTest, ParameterDescriptors_OfferRawvideoFormat) {
  orc::VideoSinkStage stage;
  auto descriptors = stage.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Composite);

  const auto* rawvideo_format = find_parameter(descriptors, "rawvideo_format");
  ASSERT_NE(rawvideo_format, nullptr);
  const auto& allowed = rawvideo_format->constraints.allowed_strings;
  ASSERT_EQ(allowed.size(), 2U);
  EXPECT_TRUE(has_string(allowed, "rgb"));
  EXPECT_TRUE(has_string(allowed, "yuv"));

  ASSERT_TRUE(rawvideo_format->constraints.default_value.has_value());
  ASSERT_TRUE(std::holds_alternative<std::string>(
      *rawvideo_format->constraints.default_value));
  EXPECT_EQ(std::get<std::string>(*rawvideo_format->constraints.default_value),
            "rgb");

  ASSERT_TRUE(rawvideo_format->constraints.depends_on.has_value());
  EXPECT_EQ(rawvideo_format->constraints.depends_on->parameter_name,
            "ffmpeg_format");
  ASSERT_EQ(rawvideo_format->constraints.depends_on->required_values.size(),
            1U);
  EXPECT_EQ(rawvideo_format->constraints.depends_on->required_values.front(),
            "nut-rawvideo");
}

TEST(VideoSinkStageTest, SetParameters_RoundTripsRawvideoFormat) {
  orc::VideoSinkStage stage;

  ASSERT_TRUE(stage.set_parameters({{"rawvideo_format", std::string("yuv")}}));
  auto params = stage.get_parameters();
  EXPECT_EQ(string_param(params, "rawvideo_format"), "yuv");

  ASSERT_TRUE(stage.set_parameters({{"rawvideo_format", std::string("rgb")}}));
  params = stage.get_parameters();
  EXPECT_EQ(string_param(params, "rawvideo_format"), "rgb");

  // An unrecognised value clamps to the "rgb" default rather than being
  // rejected outright — matching bt601_bit_depth's own clamp-not-reject
  // handling for this kind of minor cosmetic option.
  ASSERT_TRUE(
      stage.set_parameters({{"rawvideo_format", std::string("bogus")}}));
  params = stage.get_parameters();
  EXPECT_EQ(string_param(params, "rawvideo_format"), "rgb");
}

TEST(VideoSinkStageTest, GetParameters_DefaultsRawvideoFormatToRgb) {
  orc::VideoSinkStage stage;
  const auto params = stage.get_parameters();

  EXPECT_EQ(string_param(params, "rawvideo_format"), "rgb");
}

TEST(VideoSinkStageTest, ParameterDescriptors_OfferFreeTextVideoFilter) {
  orc::VideoSinkStage stage;
  auto descriptors = stage.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Composite);

  const auto* filter = find_parameter(descriptors, "video_filter");
  ASSERT_NE(filter, nullptr);
  EXPECT_EQ(filter->type, orc::ParameterType::STRING);
  EXPECT_TRUE(filter->constraints.allowed_strings.empty());

  ASSERT_TRUE(filter->constraints.default_value.has_value());
  ASSERT_TRUE(
      std::holds_alternative<std::string>(*filter->constraints.default_value));
  EXPECT_EQ(std::get<std::string>(*filter->constraints.default_value), "");

  ASSERT_TRUE(filter->constraints.depends_on.has_value());
  EXPECT_EQ(filter->constraints.depends_on->parameter_name, "output_mode");
}

TEST(VideoSinkStageTest, SetParameters_RejectsInvalidDisplayAspectRatio) {
  orc::VideoSinkStage stage;

  EXPECT_FALSE(
      stage.set_parameters({{"display_aspect_ratio", std::string("2.35:1")}}));

  // A rejected value must leave the stored parameter untouched.
  const auto params = stage.get_parameters();
  EXPECT_EQ(string_param(params, "display_aspect_ratio"), "auto");
}

TEST(VideoSinkStageTest, SetParameters_RoundTripsDisplayAspectRatio) {
  orc::VideoSinkStage stage;

  for (const auto* value : {"4:3", "16:9", "auto"}) {
    ASSERT_TRUE(
        stage.set_parameters({{"display_aspect_ratio", std::string(value)}}))
        << value;
    const auto params = stage.get_parameters();
    EXPECT_EQ(string_param(params, "display_aspect_ratio"), value);
  }
}

TEST(VideoSinkStageTest, SetParameters_RoundTripsVideoFilter) {
  orc::VideoSinkStage stage;

  ASSERT_TRUE(stage.set_parameters(
      {{"video_filter", std::string("fieldmatch,decimate")}}));
  auto params = stage.get_parameters();
  EXPECT_EQ(string_param(params, "video_filter"), "fieldmatch,decimate");

  // Clearing the filter must round-trip too.
  ASSERT_TRUE(stage.set_parameters({{"video_filter", std::string("")}}));
  params = stage.get_parameters();
  EXPECT_EQ(string_param(params, "video_filter"), "");
}

TEST(VideoSinkStageTest, GetParameters_DefaultsForCustomFfmpegOptions) {
  orc::VideoSinkStage stage;
  const auto params = stage.get_parameters();

  EXPECT_EQ(string_param(params, "display_aspect_ratio"), "auto");
  EXPECT_EQ(string_param(params, "video_filter"), "");
}

TEST(VideoSinkStageTest, ParameterDescriptors_AudioGainRequiresEmbedAudio) {
  orc::VideoSinkStage stage;
  auto descriptors = stage.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Composite);

  const auto* gain = find_parameter(descriptors, "audio_gain_db");
  ASSERT_NE(gain, nullptr);
  EXPECT_EQ(gain->type, orc::ParameterType::DOUBLE);

  ASSERT_TRUE(gain->constraints.default_value.has_value());
  ASSERT_TRUE(std::holds_alternative<double>(*gain->constraints.default_value));
  EXPECT_EQ(std::get<double>(*gain->constraints.default_value), 0.0);

  ASSERT_TRUE(gain->constraints.min_value.has_value());
  EXPECT_EQ(std::get<double>(*gain->constraints.min_value), -24.0);
  ASSERT_TRUE(gain->constraints.max_value.has_value());
  EXPECT_EQ(std::get<double>(*gain->constraints.max_value), 24.0);

  // Only meaningful when audio is embedded in the FFmpeg output.
  ASSERT_TRUE(gain->constraints.depends_on.has_value());
  EXPECT_EQ(gain->constraints.depends_on->parameter_name, "embed_audio");
  ASSERT_EQ(gain->constraints.depends_on->required_values.size(), 1U);
  EXPECT_EQ(gain->constraints.depends_on->required_values.front(), "true");
}

TEST(VideoSinkStageTest, SetParameters_RoundTripsAudioGain) {
  orc::VideoSinkStage stage;

  ASSERT_TRUE(stage.set_parameters({{"audio_gain_db", 6.0}}));
  auto params = stage.get_parameters();
  auto it = params.find("audio_gain_db");
  ASSERT_NE(it, params.end());
  ASSERT_TRUE(std::holds_alternative<double>(it->second));
  EXPECT_EQ(std::get<double>(it->second), 6.0);

  // Default is unity gain (0 dB).
  orc::VideoSinkStage fresh;
  params = fresh.get_parameters();
  it = params.find("audio_gain_db");
  ASSERT_NE(it, params.end());
  ASSERT_TRUE(std::holds_alternative<double>(it->second));
  EXPECT_EQ(std::get<double>(it->second), 0.0);
}

TEST(VideoSinkStageTest,
     ParameterDescriptors_AudioChannelPairsRequiresEmbedAudio) {
  orc::VideoSinkStage stage;
  auto descriptors = stage.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Composite);

  const auto* pairs = find_parameter(descriptors, "audio_channel_pairs");
  ASSERT_NE(pairs, nullptr);
  EXPECT_EQ(pairs->type, orc::ParameterType::STRING);

  ASSERT_TRUE(pairs->constraints.default_value.has_value());
  ASSERT_TRUE(
      std::holds_alternative<std::string>(*pairs->constraints.default_value));
  EXPECT_EQ(std::get<std::string>(*pairs->constraints.default_value), "all");

  // Only meaningful when audio is embedded in the FFmpeg output.
  ASSERT_TRUE(pairs->constraints.depends_on.has_value());
  EXPECT_EQ(pairs->constraints.depends_on->parameter_name, "embed_audio");
  ASSERT_EQ(pairs->constraints.depends_on->required_values.size(), 1U);
  EXPECT_EQ(pairs->constraints.depends_on->required_values.front(), "true");
}

TEST(VideoSinkStageTest, SetParameters_RoundTripsAudioChannelPairs) {
  orc::VideoSinkStage stage;

  ASSERT_TRUE(
      stage.set_parameters({{"audio_channel_pairs", std::string("0,2")}}));
  auto params = stage.get_parameters();
  EXPECT_EQ(string_param(params, "audio_channel_pairs"), "0,2");

  // An empty selection falls back to the default of all channel pairs.
  ASSERT_TRUE(stage.set_parameters({{"audio_channel_pairs", std::string("")}}));
  params = stage.get_parameters();
  EXPECT_EQ(string_param(params, "audio_channel_pairs"), "all");

  orc::VideoSinkStage fresh;
  params = fresh.get_parameters();
  EXPECT_EQ(string_param(params, "audio_channel_pairs"), "all");
}

// Teletext subtitle embedding was removed: its only data source was the host
// teletext observer, and recovery now lives in the teletext sink.
TEST(VideoSinkStageTest, ParameterDescriptors_OfferNoTeletextEmbedding) {
  orc::VideoSinkStage stage;
  auto descriptors = stage.get_parameter_descriptors(
      orc::VideoSystem::PAL, orc::SourceType::Composite);

  EXPECT_EQ(find_parameter(descriptors, "embed_teletext_subtitles"), nullptr);
  EXPECT_EQ(find_parameter(descriptors, "teletext_subtitle_page"), nullptr);

  // Closed caption embedding, which shares the mov_text stream, is untouched.
  const auto* captions = find_parameter(descriptors, "embed_closed_captions");
  ASSERT_NE(captions, nullptr);
  EXPECT_EQ(captions->type, orc::ParameterType::BOOL);
}

TEST(VideoSinkStageTest, GetParameters_CarryNoTeletextSettings) {
  orc::VideoSinkStage stage;
  const auto params = stage.get_parameters();

  EXPECT_EQ(params.find("embed_teletext_subtitles"), params.end());
  EXPECT_EQ(params.find("teletext_subtitle_page"), params.end());
  EXPECT_NE(params.find("embed_closed_captions"), params.end());
}

}  // namespace orc_unit_test
