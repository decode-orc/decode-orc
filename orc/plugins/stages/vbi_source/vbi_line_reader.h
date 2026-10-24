/*
 * File:        vbi_line_reader.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Turns a raw VBI byte stream into indexed line records
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_LINE_READER_H
#define ORC_VBI_LINE_READER_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vbi_byte_source.h"
#include "vbi_source_format.h"

namespace orc {

// One stored line record that carries the data service, with the index tuple
// and source byte range it came from.
//
// The byte range is what closes the provenance loop (design §1.1): because
// the record index is preserved exactly as stored and records are never
// dropped or reordered, a packet recovered downstream resolves back to these
// bytes as a pure index relation.
struct VBILineRecord {
  uint64_t frame_index = 0;   // 0-based stored frame in the capture
  uint32_t field_index = 0;   // 0 = first stored field, 1 = second
  uint32_t record_index = 0;  // 0-based record within the stored field

  uint64_t source_byte_offset = 0;  // offset of sample 0 in the raw stream
  uint64_t source_byte_length = 0;  // valid samples only, padding excluded

  // Raw sample values, padding excluded.  Held as doubles because every
  // consumer above the reader (level estimation, correlation, resampling)
  // works in the continuous domain; no scaling or conditioning is applied
  // here.
  std::vector<double> samples;

  // Whether every sample is a byte's own value, a whole number from 0 to 255,
  // as an 8-bit container's are: the level estimate then counts them without
  // checking each first (estimate_vbi_data_levels()).
  bool byte_samples = false;
};

// Every data-service record of one stored frame, in stored order: the first
// stored field's records followed by the second's.
struct VBIFrameRecords {
  uint64_t frame_index = 0;

  // The bt8x8 driver's per-frame sequence number, when the format declares a
  // frame-counter trailer.  Interpreted little-endian; the kernel writes
  // machine endianness and every platform these captures come from is
  // little-endian (design §3.3).
  std::optional<uint32_t> frame_counter;

  std::vector<VBILineRecord> lines;
};

// Reads fixed-stride line records out of a raw VBI byte stream.
//
// The reader is generic over the container descriptor and holds no format
// knowledge of its own: record stride, padding exclusion, field selection and
// the frame trailer all come from VBISourceFormat.  It performs no signal
// processing whatsoever.
//
// Thread safety: everything that reads through the byte source is not
// thread-safe, because the byte source holds a stream position; callers
// serialise it.  decode_frame() reads nothing and holds no state, so it may be
// called from any number of threads at once, including while another thread
// reads — which is what lets a caller hold its lock across the byte read and
// nothing else.
class VBILineReader {
 public:
  // The referenced byte source must outlive the reader.
  VBILineReader(VBISourceFormat format, IVBIByteSource& byte_source);

  const VBISourceFormat& format() const { return format_; }

  uint64_t bytes_per_frame() const { return format_.bytes_per_frame(); }

  // Whole stored frames available, or nullopt when the transport cannot
  // report its size without decoding everything.
  std::optional<uint64_t> frame_count() const;

  // True when the stream ends part-way through a frame.  Not an error: a
  // capture stops when it stops.  The trailing bytes are short of a frame and
  // so are never emitted, which the stage reports rather than hiding.
  bool has_partial_trailing_frame() const;

  // Bytes following the last whole frame, or nullopt when the transport cannot
  // report its size without decoding everything.  Zero when the capture
  // factorises exactly.
  //
  // What they are is worth telling apart. A whole field is the ordinary way a
  // capture ends; anything else means the writer stopped part-way through a
  // record, which is ordinary for a card dump killed at the keyboard and is
  // also what a wrong container geometry would leave behind.
  std::optional<uint64_t> trailing_bytes() const;

  // Read every data-service record of one stored frame.  Returns false with
  // an error message on a short read (a truncated final frame) or a sample
  // format whose decode path is not implemented.
  //
  // Exactly read_frame_bytes() followed by decode_frame().
  bool read_frame(uint64_t frame_index, VBIFrameRecords& out_records,
                  std::string& error_message) const;

  // Read the raw bytes of one stored frame, bytes_per_frame() of them, and
  // nothing else: the only part of read_frame() that touches the byte source.
  // Returns false with an error message on a short read, which means the
  // configured frame size does not divide the capture.
  bool read_frame_bytes(uint64_t frame_index, std::vector<uint8_t>& out_bytes,
                        std::string& error_message) const;

  // Decode one stored frame's raw bytes, as read_frame_bytes() returns them,
  // into its records.  Reads nothing, so it is safe to call concurrently with
  // anything, reads through the byte source included.  Returns false with an
  // error message when the buffer is not one whole frame or the sample format
  // has no decode path.
  bool decode_frame(uint64_t frame_index,
                    const std::vector<uint8_t>& frame_bytes,
                    VBIFrameRecords& out_records,
                    std::string& error_message) const;

  // Read one stored frame's counter without decoding its records.
  //
  // Frame-drop detection needs the counter of a frame and nothing else, and
  // asking for the samples as well would decode 64 KiB to read four bytes.
  // out_counter is empty when the format declares no counter trailer, which
  // is not an error: it is the answer that the source cannot report drops
  // (design §6.3).
  bool read_frame_counter(uint64_t frame_index,
                          std::optional<uint32_t>& out_counter,
                          std::string& error_message) const;

 private:
  VBISourceFormat format_;
  IVBIByteSource* byte_source_;
};

// Decode one record's worth of sample words into raw sample values.
// record_bytes must hold at least sample_count words of the given format.
// Returns false with an error message for sample formats that have no decode
// path yet.
bool decode_vbi_samples(VBISampleFormat sample_format,
                        const uint8_t* record_bytes, uint32_t sample_count,
                        std::vector<double>& out_samples,
                        std::string& error_message);

}  // namespace orc

#endif  // ORC_VBI_LINE_READER_H
