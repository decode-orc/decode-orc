/*
 * File:        sink_decode.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     Decoding a videosynth case through the video sink into raw
 *              YUV444P16, for the chroma decoder tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "sink_decode.h"

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <filesystem>

#include "video_sink_stage.h"
#include "videosynth_harness.h"

namespace orc_videosynth_test {

namespace fs = std::filesystem;

std::optional<orc_videosynth::DecodedFile> decode_case(
    const orc_videosynth::CaseFile& file, const std::string& decoder,
    const Parameters& extra, const std::string& label, size_t frames) {
  const auto artifact = orc_videosynth::open_case(file);
  if (!artifact) return std::nullopt;
  const auto root = orc_videosynth::case_dir("decoded");
  if (!root) return std::nullopt;
  fs::create_directories(*root);
  const fs::path out = *root / (label + ".yuv");
  Parameters parameters = extra;
  parameters["output_path"] = out.string();
  parameters["output_mode"] = std::string("raw");
  parameters["raw_format"] = std::string("yuv");
  parameters["decoder_type"] = decoder;
  orc::VideoSinkStage sink;
  orc::ObservationContext ctx;
  if (!sink.trigger({artifact}, parameters, ctx)) {
    ADD_FAILURE() << "the video sink failed on " << file.case_name << " with "
                  << decoder << ": " << sink.get_trigger_status();
    return std::nullopt;
  }
  std::string error;
  auto decoded = orc_videosynth::DecodedFile::read(out, frames, &error);
  if (!decoded) ADD_FAILURE() << error;
  fs::remove(out);
  return decoded;
}

}  // namespace orc_videosynth_test
