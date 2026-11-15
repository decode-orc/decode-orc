/*
 * File:        cvbs_source_audio_wav.h
 * Module:      orc-core
 * Purpose:     Parse a CVBS audio channel pair sidecar's WAV header (RIFF or
 *              RF64) into the fields the CVBS Source validates
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_CVBS_SOURCE_AUDIO_WAV_H
#define ORC_CORE_CVBS_SOURCE_AUDIO_WAV_H

#include <cstdint>

#include "../common/audio-resample/wav_header.h"
#include "cvbs_source_stage.h"

namespace orc {

// Read the header of a channel pair file of |file_size| bytes through
// |read_at|. Chunks the walk does not need (JUNK among them) are skipped, as
// the CVBS file format specification v1.8.0 onwards requires. The result is
// not judged here: the stage checks the format, the container and the ds64
// sample count. A header that cannot be parsed sets |error|, unless the
// reason is a wrapped RIFF size, which sets |size_wrapped|.
inline CVBSAudioWavInfo parse_cvbs_audio_wav_info(
    const wav::ByteReader& read_at, uint64_t file_size) {
  const wav::WavLayout layout = wav::parse_header(read_at, file_size);
  CVBSAudioWavInfo info;
  info.rf64 = layout.rf64;
  info.bw64 = layout.bw64;
  info.size_wrapped = layout.size_wrapped;
  if (!layout.valid) {
    if (!layout.size_wrapped) info.error = layout.error;
    return info;
  }
  info.format_tag = layout.format_tag;
  info.channels = layout.channels;
  info.sample_rate_hz = layout.sample_rate_hz;
  info.bits_per_sample = layout.bits_per_sample;
  info.data_bytes = layout.data_bytes;
  info.has_ds64 = layout.has_ds64;
  info.ds64_sample_count = layout.ds64_sample_count;
  info.fmt_bytes = layout.fmt_bytes;
  info.data_offset = layout.data_offset;
  return info;
}

}  // namespace orc

#endif  // ORC_CORE_CVBS_SOURCE_AUDIO_WAV_H
