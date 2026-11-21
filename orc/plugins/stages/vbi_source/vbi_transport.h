/*
 * File:        vbi_transport.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Raw-file, FLAC-wrapped and video-file byte transports for VBI
 *              captures
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_TRANSPORT_H
#define ORC_VBI_TRANSPORT_H

#include <cstdint>
#include <memory>
#include <string>

#include "vbi_byte_source.h"

namespace orc {

// The four magic bytes at offset 0 of a FLAC stream.
extern const char kFlacStreamMarker[5];

// The four bytes at offset 0 of a Matroska (or WebM) file: the element ID of
// its EBML header.
extern const uint8_t kMatroskaStreamMarker[4];

// Does the buffer start with the FLAC stream marker?  This is step 1 of the
// format-detection ladder (design §8.1); it and the Matroska marker below are
// what decide whether a capture needs unwrapping.
bool has_flac_stream_marker(const uint8_t* buffer, size_t length);

// Does the buffer start with the Matroska marker?  A capture stored as the
// pictures of a video file is unwrapped by the video transport.  Neither
// family's raw captures can open with these bytes: a card dump opens on
// blanking and a .tbc crop on the line's sync tip.
bool has_matroska_stream_marker(const uint8_t* buffer, size_t length);

// How a video stream codes its luma samples.
struct VBIVideoLumaCoding {
  // Bits per luma sample, 8 to 16.
  uint32_t bit_depth = 8;

  // True for a full-range ("pc") stream, whose samples span the whole word;
  // false for a limited-range ("tv") one.
  // ITU-R BT.601-5 Section 3.5.3: limited-range luma is 219 E + 16, black at
  // level 16 and white at level 235, scaled up with the bit depth.
  bool full_range = false;
};

// The 16-bit capture word a luma sample of a video-carried capture stands
// for.
//
// The crop of a time-base corrected capture is 16-bit grey, and FFmpeg
// treats grey as full range: a limited-range picture made from it maps word
// w to luma 16 + 219 w / 65 280 at 8 bits, and a full-range one replicates
// the word's top bits.  This undoes either, rounding to the nearest word and
// clamping to the word's range, so a capture comes back on the scale it was
// written on and the formats for it read it unchanged.
uint16_t vbi_sample_word_from_luma(uint32_t luma,
                                   const VBIVideoLumaCoding& coding);

// Open a capture for reading, transparently unwrapping FLAC and video files.
//
// FLAC is used on these captures as a generic lossless byte compressor, so
// the decoded byte sequence is identical to the raw file and every consumer
// above IVBIByteSource sees the same thing either way (design §3.3).  A video
// file's pictures are unwrapped into the 16-bit words they were made from, a
// picture row to a line record.  Returns nullptr with an error message when
// the file cannot be opened or its transport is not supported.
std::unique_ptr<IVBIByteSource> open_vbi_byte_source(
    const std::string& path, std::string& error_message);

}  // namespace orc

#endif  // ORC_VBI_TRANSPORT_H
