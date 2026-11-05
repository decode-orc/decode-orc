/*
 * File:        sink_decode.h
 * Module:      orc-videosynth-tests
 * Purpose:     Decoding a videosynth case through the video sink into raw
 *              YUV444P16, for the chroma decoder tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <decoded_comparison.h>
#include <orc/stage/params/parameter_types.h>

#include <cstddef>
#include <map>
#include <optional>
#include <string>

#include "videosynth_cases.h"

namespace orc_videosynth_test {

using Parameters = std::map<std::string, orc::ParameterValue>;

// |file| decoded by the video sink with |decoder| and |extra| parameters into
// raw YUV444P16 of |frames| frames, written as <label>.yuv under the
// "decoded" case directory and removed once read; nullopt, with the test
// failed, when it cannot be.
std::optional<orc_videosynth::DecodedFile> decode_case(
    const orc_videosynth::CaseFile& file, const std::string& decoder,
    const Parameters& extra, const std::string& label, size_t frames);

}  // namespace orc_videosynth_test
