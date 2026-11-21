/*
 * File:        vbi_source_validation.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Fail-fast configuration checks for raw VBI captures
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_source_validation.h"

namespace orc {

namespace {

// Width of the bt8x8 frame sequence number at the tail of each frame
// (design §3.3).
constexpr uint32_t kFrameCounterBytes = 4;

std::string tv_system_name(VBITVSystem tv_system) {
  switch (tv_system) {
    case VBITVSystem::kPAL:
      return "PAL";
    case VBITVSystem::kNTSC:
      return "NTSC";
    case VBITVSystem::kPALM:
      return "PAL_M";
  }
  return "unknown";
}

std::string tt_system_name(VBITeletextSystem tt_system) {
  return (tt_system == VBITeletextSystem::kWST) ? "WST" : "NABTS";
}

}  // namespace

std::vector<std::string> validate_vbi_source_config(
    const VBISourceFormat& format, std::optional<uint64_t> decoded_stream_bytes,
    const VBITransportHints& transport_hints) {
  std::vector<std::string> errors;

  // ------------------------------------------------------------------
  // Container geometry.  These are checked first and gate the derived
  // checks below, so an unconfigured 'custom' format reports the fields
  // the user has to fill in rather than a cascade of consequences.
  // ------------------------------------------------------------------
  const bool have_line_length = format.line_length > 0;
  const bool have_field_lines = format.field_lines > 0;

  if (!(format.sample_rate_hz > 0.0)) {
    errors.push_back(
        "container.sample_rate must be a positive rate in Hz; no capture "
        "format records it, so it is always configuration.");
  }
  if (!have_line_length) {
    errors.push_back(
        "container.line_length must be a positive number of samples per "
        "stored line record.");
  }
  if (!have_field_lines) {
    errors.push_back(
        "container.field_lines must be a positive number of stored line "
        "records per field.");
  }

  if (have_line_length) {
    if (format.valid_samples == 0) {
      errors.push_back(
          "container.valid_samples must be positive; it is the count of real "
          "samples at the start of each record, the remainder being hardware "
          "padding.");
    } else if (format.valid_samples > format.line_length) {
      errors.push_back(
          "container.valid_samples (" + std::to_string(format.valid_samples) +
          ") exceeds container.line_length (" +
          std::to_string(format.line_length) +
          "); a record cannot hold more real samples than its stride.");
    }
  }

  // ------------------------------------------------------------------
  // Field range: which stored records carry the data service.
  // ------------------------------------------------------------------
  if (format.field_range.end < format.field_range.start) {
    errors.push_back("container.field_range is inverted (" +
                     std::to_string(format.field_range.start) + ".." +
                     std::to_string(format.field_range.end) +
                     "); it is an inclusive, ascending, 0-based record range.");
  } else if (have_field_lines && format.field_range.end >= format.field_lines) {
    errors.push_back("container.field_range ends at record " +
                     std::to_string(format.field_range.end) +
                     ", but the field only stores " +
                     std::to_string(format.field_lines) +
                     " records (container.field_lines), numbered 0.." +
                     std::to_string(format.field_lines - 1) + ".");
  } else if (have_field_lines &&
             format.field_range.end + format.caption_records >=
                 format.field_lines) {
    errors.push_back(
        "the caption records after container.field_range run "
        "past the " +
        std::to_string(format.field_lines) + " records the field stores.");
  }
  // A caption record is placed beside the data lines with their levels, which
  // only a source whose levels are absolute can give it without its own
  // estimate disturbing theirs (VBISourceFormat::caption_records).
  if (format.caption_records > 0 &&
      format.family != VBISourceFamily::kTBCDerived) {
    errors.push_back(
        "caption records are only read from a time-base corrected source, "
        "whose levels are absolute.");
  }

  // ------------------------------------------------------------------
  // System pairing and the standard's teletext line list (design §5.1).
  // Excess records have nowhere to go, so this is an error and never a
  // truncation.
  // ------------------------------------------------------------------
  const uint32_t standard_lines =
      standard_teletext_lines_per_field(format.tv_system, format.tt_system);
  if (standard_lines == 0) {
    errors.push_back("teletext.system " + tt_system_name(format.tt_system) +
                     " has no defined line list on " +
                     tv_system_name(format.tv_system) +
                     "; WST is defined on PAL and on 525-line systems, and "
                     "NABTS on NTSC/PAL_M.");
  } else if (format.field_range.count() > standard_lines) {
    errors.push_back(
        "container.field_range selects " +
        std::to_string(format.field_range.count()) +
        " records per field, but " + tt_system_name(format.tt_system) + " on " +
        tv_system_name(format.tv_system) + " defines only " +
        std::to_string(standard_lines) +
        " teletext lines per field; the excess records have no frame line to "
        "map to.");
  }

  // ------------------------------------------------------------------
  // Frame trailer.  It overlaps the padding of the final record rather
  // than extending the frame, so it must fit inside that padding.
  // ------------------------------------------------------------------
  if (have_line_length && format.valid_samples > 0 &&
      format.valid_samples <= format.line_length) {
    const uint64_t padding_bytes = format.record_padding_bytes();
    if (format.frame_trailer_bytes > padding_bytes) {
      errors.push_back(
          "container.frame_trailer_bytes (" +
          std::to_string(format.frame_trailer_bytes) + ") exceeds the " +
          std::to_string(padding_bytes) +
          " padding bytes of a record; the trailer overlaps the final "
          "record's padding and must not reach into sample data.");
    }
  }
  if (format.frame_trailer_is_counter &&
      format.frame_trailer_bytes < kFrameCounterBytes) {
    errors.push_back(
        "container.frame_trailer_bytes (" +
        std::to_string(format.frame_trailer_bytes) +
        ") is too small to hold the 4-byte frame sequence number the format "
        "declares.");
  }

  // ------------------------------------------------------------------
  // Capture offset.  For TBC-derived sources sample 0 of every record is
  // 0H by construction, so fitting the offset would measure a quantity that
  // is already known — and would do it against a record whose head is the
  // line's sync pulse rather than the back porch a card capture opens in
  // (design §5.3.3).
  //
  // A configured figure is a different matter and is allowed: a crop may
  // start a sample or two either side of 0H, and the user is the only one who
  // can say so.  The preset's own value stays 0.
  // ------------------------------------------------------------------
  if (format.family == VBISourceFamily::kTBCDerived &&
      format.capture_offset_is_auto) {
    errors.push_back(
        "calibration.capture_offset must not be 'auto' for a TBC-derived "
        "source: sample 0 of every record is already 0H, so there is nothing "
        "to calibrate. Configure a figure manually if the crop does not start "
        "exactly there.");
  }

  // ------------------------------------------------------------------
  // Sample format against what the transport declares.  For FLAC this is
  // the one header field that is trustworthy; the declared sample rate is
  // deliberately not consulted anywhere (design §3.3).
  // ------------------------------------------------------------------
  if (transport_hints.bits_per_sample.has_value()) {
    const uint32_t declared_bits = *transport_hints.bits_per_sample;
    const uint32_t configured_bits = format.bytes_per_sample() * 8u;
    if (declared_bits != configured_bits) {
      errors.push_back("container.sample_format '" +
                       to_string(format.sample_format) + "' is " +
                       std::to_string(configured_bits) +
                       " bits per sample, but the transport declares " +
                       std::to_string(declared_bits) + " bits.");
    }
  }

  // ------------------------------------------------------------------
  // Record length against what the transport declares.  A capture carried in
  // a video file stores a line record to a picture row, so its pictures are
  // exactly as wide as its records are long, and a format with records of
  // another length would read rows split and joined at the wrong places.
  // ------------------------------------------------------------------
  if (transport_hints.record_samples.has_value() && have_line_length &&
      *transport_hints.record_samples != format.line_length) {
    errors.push_back("container.line_length is " +
                     std::to_string(format.line_length) +
                     " samples per record, but the capture's pictures are " +
                     std::to_string(*transport_hints.record_samples) +
                     " samples wide, and a picture row is one line record.");
  }

  // ------------------------------------------------------------------
  // Stream length.  The capture has to hold one whole frame; what follows the
  // last whole frame does not have to be anything in particular.
  //
  // A capture stops when it stops, and nothing that writes one of these files
  // rounds it off first.  The circulating VBI-only .tbc crops end on an odd
  // field about as often as not, and a card dump ends wherever the writer was
  // when it was killed — part-way through a record as readily as on a record
  // boundary.  Neither file is misconfigured, so neither is refused: the
  // trailing bytes are short of a frame, so they are not emitted, and the
  // stage says what it dropped rather than passing over it in silence.
  //
  // This does mean a remainder no longer witnesses a wrong geometry.  It never
  // witnessed one reliably — a wrong container leaves an arbitrary remainder
  // and a truncated capture leaves an arbitrary remainder, and the check could
  // not tell them apart — and the fit that follows is what actually catches it:
  // a wrong stride, rate or system scatters the clock run-in far beyond the
  // spread and drift limits and stops the run with a diagnostic naming the
  // cause.
  // ------------------------------------------------------------------
  const uint64_t frame_bytes = format.bytes_per_frame();
  if (decoded_stream_bytes.has_value() && frame_bytes > 0) {
    const uint64_t stream_bytes = *decoded_stream_bytes;
    if (stream_bytes == 0) {
      errors.push_back("The capture is empty (0 bytes after decoding).");
    } else if (stream_bytes < frame_bytes) {
      errors.push_back(
          "The capture is " + std::to_string(stream_bytes) +
          " bytes, which is less than the configured frame size of " +
          std::to_string(frame_bytes) +
          " bytes, so it does not hold one complete frame.");
    }
  }

  return errors;
}

}  // namespace orc
