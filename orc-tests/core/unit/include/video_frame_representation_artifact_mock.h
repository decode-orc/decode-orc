/*
 * File:        video_frame_representation_artifact_mock.h
 * Module:      orc-core-tests
 * Purpose:     Mock combining VideoFrameRepresentation and Artifact for tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef DECODE_ORC_ROOT_VIDEO_FRAME_REPRESENTATION_ARTIFACT_MOCK_H
#define DECODE_ORC_ROOT_VIDEO_FRAME_REPRESENTATION_ARTIFACT_MOCK_H

#include <gmock/gmock.h>
#include <orc/stage/artifact.h>
#include <orc/stage/video_frame_representation.h>

// Combines VideoFrameRepresentation with Artifact so the mock can be passed as
// ArtifactPtr while the VideoSinkStage dynamic_pointer_cast succeeds.
namespace orc_unit_test {

class MockVideoFrameRepresentationArtifact
    : public orc::VideoFrameRepresentation,
      public orc::Artifact {
 public:
  MockVideoFrameRepresentationArtifact()
      : orc::Artifact(orc::ArtifactID("test_vfr_artifact"), orc::Provenance{}) {
  }

  using sample_type = orc::VideoFrameRepresentation::sample_type;

  // Artifact
  MOCK_METHOD(std::string, type_name, (), (const, override));

  // Navigation
  MOCK_METHOD(orc::FrameIDRange, frame_range, (), (const, override));
  MOCK_METHOD(size_t, frame_count, (), (const, override));
  MOCK_METHOD(bool, has_frame, (orc::FrameID), (const, override));
  MOCK_METHOD(std::optional<orc::FrameDescriptor>, get_frame_descriptor,
              (orc::FrameID), (const, override));

  // Flat access
  MOCK_METHOD(const sample_type*, get_frame, (orc::FrameID), (const, override));
  MOCK_METHOD(const sample_type*, get_line, (orc::FrameID, size_t),
              (const, override));
  MOCK_METHOD((std::vector<sample_type>), get_frame_copy, (orc::FrameID),
              (const, override));

  // Hints
  MOCK_METHOD((std::vector<orc::DropoutRun>), get_dropout_hints, (orc::FrameID),
              (const, override));
  MOCK_METHOD((std::optional<orc::SourceParameters>), get_video_parameters, (),
              (const, override));

  // Audio channel pairs
  MOCK_METHOD(size_t, audio_channel_pair_count, (), (const, override));
  MOCK_METHOD((std::optional<orc::AudioChannelPairDescriptor>),
              get_audio_channel_pair_descriptor, (size_t), (const, override));
  MOCK_METHOD((std::vector<int32_t>), get_audio_samples, (size_t, orc::FrameID),
              (const, override));

  // EFM
  MOCK_METHOD(uint32_t, get_efm_sample_count, (orc::FrameID),
              (const, override));
  MOCK_METHOD((std::vector<uint8_t>), get_efm_samples, (orc::FrameID),
              (const, override));
};

}  // namespace orc_unit_test

#endif  // DECODE_ORC_ROOT_VIDEO_FRAME_REPRESENTATION_ARTIFACT_MOCK_H
