/*
 * File:        videosynth_cases.h
 * Module:      orc-videosynth-tests
 * Purpose:     Opening a generated case through cvbs_source, and reading its
 *              dropout sidecar, for the videosynth suite's tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/artifact.h>
#include <orc/stage/common_types.h>
#include <orc/stage/video_frame_representation.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace orc_videosynth {

using VfrPtr = std::shared_ptr<const orc::VideoFrameRepresentation>;

// One output of a videosynth case: the case's directory name (an
// orc_videosynth_case() in CMakeLists.txt) and its output.video_path stem.
struct CaseFile {
  std::string case_name;
  std::string stem;
  orc::VideoSystem system = orc::VideoSystem::PAL;
  bool yc = false;  // .cvbsy/.cvbsc rather than .cvbs
};

// <case dir>/<stem>, or nullopt (with the test failed) outside ctest.
std::optional<std::filesystem::path> case_base(const CaseFile& file);

// The case opened through cvbs_source; nullptr, with the test failed, when
// it cannot be. Stages take the artifact; as_vfr() gives the video.
orc::ArtifactPtr open_case(const CaseFile& file);

VfrPtr as_vfr(const orc::ArtifactPtr& artifact);

// A dropout run as the sidecar records it: frame, start, count, severity.
using RunKey = std::tuple<orc::FrameID, uint64_t, uint32_t, uint8_t>;

// Every dropout_run row of a dropout sidecar, read directly rather than
// through cvbs_source so the two can be compared. Fails the test when the
// file cannot be read.
std::vector<RunKey> read_sidecar_runs(const std::filesystem::path& path);

}  // namespace orc_videosynth
