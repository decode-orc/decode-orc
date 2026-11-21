/*
 * File:        vbi_frame_builder.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Places VBI line records on an otherwise blank CVBS frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_FRAME_BUILDER_H
#define ORC_VBI_FRAME_BUILDER_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vbi_level_mapper.h"
#include "vbi_line_placement.h"
#include "vbi_line_reader.h"
#include "vbi_output_frame.h"
#include "vbi_resampler.h"
#include "vbi_source_format.h"
#include "vbi_teletext_service.h"

namespace orc {

// The data lines one stored frame puts on its output frame, and nothing else.
//
// A frame is blanking everywhere a record did not land, so the records' lines
// are the whole of what distinguishes one frame from another: 24 lines of the
// 525 on NTSC, 32 of the 625 on PAL.  Holding those and not the rest is what
// lets a consumer that reads a few lines of every frame — which is every
// teletext and NABTS decoder — be served without a whole frame being built for
// it (a 955 kB NTSC frame against the 44 kB of lines it carries).
//
// Every record slot holds a whole output line of |line_stride| samples: the
// placed samples over the data window and blanking around them, exactly as
// the line reads in the built frame.  A slot whose record received no data
// holds blanking throughout, which is also exactly how the frame reads there.
struct VBIPlacedFrame {
  // Samples each slot's line holds: the longest frame line any slot lands on,
  // so a caller reading a line to its full length never runs into the next.
  size_t line_stride = 0;

  // Per record slot: whether its record was written.  Slots are numbered in
  // the order the reader yields records — stored field major, record minor —
  // and VBIFrameBuilder::frame_line_of_slot() gives the line each lands on.
  std::vector<uint8_t> slot_placed;

  // Per record slot: whether it was worked out at all.  A placement made for
  // the slots a reader asks for (VBIFrameBuilder::place_frame() with a wanted
  // set) resamples only those and leaves the rest unresolved, holding
  // blanking that is not what the frame reads there.  A resolved slot reads
  // exactly as it would in a placement of every slot.
  std::vector<uint8_t> slot_resolved;

  // slot_placed.size() lines of line_stride samples.
  std::vector<int16_t> samples;

  // Frame lines that received source data, of the slots resolved.
  uint32_t data_lines = 0;

  // The line of slot |slot|, line_stride samples.
  const int16_t* line(size_t slot) const {
    return samples.data() + slot * line_stride;
  }

  // Whether every slot is resolved, as the whole frame needs them.
  bool complete() const {
    return std::find(slot_resolved.begin(), slot_resolved.end(), 0) ==
           slot_resolved.end();
  }
};

// Builds the CVBS frames the stage emits.
//
// Everything a capture holds is the teletext lines, and everything downstream
// wants of it is those lines at the right place on the right frame line at the
// right amplitude.  So that is all the builder writes: the frame is blanking
// throughout, and each record is band-limited onto the output lattice, mapped
// into the amplitude domain and laid over the data region of its frame line.
//
// Nothing else is manufactured.  There is no sync, no vertical interval and no
// colour burst, because there is nothing in a VBI capture to reconstruct them
// from and no consumer of this stage that reads them: the teletext slicer locks
// to the clock run-in inside the line it is handed, and every other observer of
// a VBI capture works the same way.  Synthesising the rest of a television
// signal would cost roughly forty times what placing the data costs, all of it
// spent on samples nothing reads.
//
// Which frame line a record lands on is resolved once, at construction, into a
// table per stored field; building a frame is then a fill, a lookup and a
// filter per data line.
//
// Thread-compatible: instances hold configuration only, and build_frame() is
// const and writes only to its output.
class VBIFrameBuilder {
 public:
  // An unusable builder; make_vbi_frame_builder() produces the usable
  // instances.  Building through this one is a reported error rather than a
  // crash.
  VBIFrameBuilder() = default;

  VBIFrameBuilder(VBISourceFormat format, VBIOutputFrame output_frame,
                  VBIDataPlacement placement, VBIRecordResampler resampler,
                  VBILevelMapper level_mapper,
                  std::array<std::vector<uint32_t>, 2> frame_lines);

  const VBIOutputFrame& output_frame() const { return output_frame_; }

  const VBIDataPlacement& placement() const { return placement_; }

  const VBILevelMapper& level_mapper() const { return level_mapper_; }

  // Frame lines the records of a stored field are written to, in record order.
  const std::vector<uint32_t>& frame_lines(uint32_t stored_field_index) const;

  // A frame of blanking and nothing else.  This is what a frame synthesised to
  // fill a gap in the source's frame counter is made of (design §6.3), and the
  // starting point of every other frame.
  void build_blank_frame(std::vector<int16_t>& out_samples) const;

  // Build the frame carrying one stored frame's records.
  //
  // Records whose levels could not be established are left as blanking rather
  // than written as arbitrarily scaled noise.  out_data_lines counts the frame
  // lines that did receive source data.
  //
  // Exactly place_frame() followed by render_frame().
  bool build_frame(const std::vector<VBILineRecord>& records,
                   std::vector<int16_t>& out_samples, uint32_t& out_data_lines,
                   std::string& error_message) const;

  // Record slots a frame has: every record of the format's field range, in
  // both stored fields.
  size_t slot_count() const { return slot_lines_.size(); }

  // Frame line record slot |slot| lands on.
  uint32_t frame_line_of_slot(size_t slot) const { return slot_lines_[slot]; }

  // Record slot that lands on frame line |frame_line|, or kNoSlot for a line
  // no record is placed on — which the whole of it is blanking on every frame.
  static constexpr int32_t kNoSlot = -1;
  int32_t slot_of_frame_line(size_t frame_line) const {
    return frame_line < line_slots_.size() ? line_slots_[frame_line] : kNoSlot;
  }

  // Place one stored frame's records: every sample build_frame() writes, and
  // only those, into a VBIPlacedFrame.  Level mapping, resampling and the
  // final clamp are build_frame()'s own, so a line read out of the placement
  // is the line the built frame holds, bit for bit.
  bool place_frame(const std::vector<VBILineRecord>& records,
                   VBIPlacedFrame& out_placed,
                   std::string& error_message) const;

  // The same, resampling only the slots |wanted| marks (an entry a slot,
  // nonzero to place it) and leaving the others unresolved.  The frame's
  // levels are still measured on every record, since a line's own levels
  // give way to the frame's where it swings too little, so a slot reads the
  // same whichever others are placed with it.  What a slot left out saves is
  // its resampling, the bulk of the work: a reader of line 21 alone has two
  // of a frame's twenty-four records resampled.
  bool place_frame(const std::vector<VBILineRecord>& records,
                   const std::vector<uint8_t>& wanted,
                   VBIPlacedFrame& out_placed,
                   std::string& error_message) const;

  // A placement with no data on it: every slot blanking.  What a frame
  // synthesised to fill a gap in the source's frame counter is placed as.
  void place_blank_frame(VBIPlacedFrame& out_placed) const;

  // The whole frame a placement describes: blanking, with each placed slot's
  // line laid over its frame line.
  void render_frame(const VBIPlacedFrame& placed,
                    std::vector<int16_t>& out_samples) const;

 private:
  VBISourceFormat format_{};
  VBIOutputFrame output_frame_{};
  VBIDataPlacement placement_{};
  VBIRecordResampler resampler_{};
  VBILevelMapper level_mapper_{};

  // Frame line per record, indexed by stored field then by record index
  // relative to the format's field range.
  std::array<std::vector<uint32_t>, 2> frame_lines_{};

  // The same table flattened into slot order, and its inverse over every line
  // of the output frame; both resolved at construction.
  std::vector<uint32_t> slot_lines_;
  std::vector<int32_t> line_slots_;

  // Samples per slot line in a placement (see VBIPlacedFrame::line_stride).
  size_t slot_line_stride_ = 0;

  // place_frame() of the slots |wanted| marks, or of every slot when it is
  // null.
  bool place_slots(const std::vector<VBILineRecord>& records,
                   const std::vector<uint8_t>* wanted,
                   VBIPlacedFrame& out_placed,
                   std::string& error_message) const;
};

// Build the assembler a source format calls for: its output lattice, the
// placement of its records on that lattice, the resampler that lands them
// there, and the frame line each record belongs to.
//
// The two timing figures settled when a capture is opened.
//
// They are passed explicitly rather than read from the format and the service
// table so that what was measured overrides what was tabulated, without either
// of those having to be rewritten (design §5.3.4).  Which of the two is
// measured depends on the source family, and it is never both: a capture has
// one unknown here, and fitting two would be fitting it twice.
struct VBIResolvedTiming {
  // Time from 0H to sample 0 of every record, in source samples.  Fitted from
  // the run-in for a card capture, whose hardware does not say; exactly zero
  // for a TBC-derived one, whose records start at 0H by construction.
  double capture_offset_samples = 0.0;

  // The service's 0H-to-run-in anchor in nanoseconds, when it was measured
  // from the capture rather than taken from the service table.  Empty leaves
  // the tabulated figure standing.
  //
  // Only a TBC-derived capture ever sets it.  A card capture's fitted offset
  // has already put its run-in at the tabulated anchor, so there is nothing
  // left for a measurement here to say.
  std::optional<double> service_anchor_ns;

  // The service whose data region is written, when it is not the format's:
  // a card capture that carries no teletext, whose offset was fitted on its
  // line-21 captions instead, is written over the caption line, which runs
  // a microsecond and a half past the end of a teletext packet.  Empty
  // writes the format's own service's region.
  std::optional<VBITeletextService> written_service;
};

// Returns false with an error message for any part the stage does not yet
// support, and for a configuration that maps two records of a frame onto the
// same frame line — which is a wrong field range rather than something to
// resolve silently.
bool make_vbi_frame_builder(const VBISourceFormat& format,
                            const VBILevelMapperConfig& level_config,
                            const VBIResolvedTiming& timing,
                            VBIFrameBuilder& out_builder,
                            std::string& error_message);

}  // namespace orc

#endif  // ORC_VBI_FRAME_BUILDER_H
