/*
 * File:        vbi_line_mapping.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Maps stored VBI line records onto CVBS frame lines
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_LINE_MAPPING_H
#define ORC_VBI_LINE_MAPPING_H

#include <cstdint>
#include <string>
#include <vector>

#include "vbi_source_format.h"

namespace orc {

// The frame lines a data service occupies, one list per television field.
//
// Held as an explicit table rather than an offset and a stride, because the
// two fields' line ranges are not related by a constant offset in either
// system, and because a source may carry fewer lines than the standard
// allows (design §5.1).
//
// Entries are stored frame lines: 0-based, with stored frame line 0 being
// broadcast frame line 1, the first line of field 1.
struct VBITeletextLineMap {
  std::vector<uint32_t> field1;
  std::vector<uint32_t> field2;
  // How many entries of each list are the standard's data lines; the rest are
  // for a source's caption records only (VBISourceFormat::caption_records).
  uint32_t data_lines = 0;

  // Line list of television field 1 or 2.  An out-of-range field number
  // yields an empty list.
  const std::vector<uint32_t>& for_tv_field(uint32_t tv_field) const;
};

// Build the frame-line table for a system pairing.  Returns false with an
// error message for pairings the stage does not yet place.
bool make_vbi_teletext_line_map(VBITVSystem tv_system,
                                VBITeletextSystem tt_system,
                                VBITeletextLineMap& out_line_map,
                                std::string& error_message);

// Which television field a stored field carries.
//
// A capture records two sequential fields per frame but nothing about which
// television field came first; that is a driver convention, so it is
// configuration (design §6.1).  Returns 1 or 2.
uint32_t vbi_tv_field_for_stored_field(const VBISourceFormat& format,
                                       uint32_t stored_field_index);

// Resolve one stored record to the stored frame line it belongs on.
//
// Sources store their useful records contiguously, so a source carrying fewer
// lines than the standard maps from the start of the field's line list.
// Returns false with an error message when the record falls outside the
// configured field range, or when the standard has no line for it — the
// latter meaning the configuration is wrong, and never something to truncate
// silently (design §5.1).
bool map_vbi_record_to_frame_line(const VBISourceFormat& format,
                                  const VBITeletextLineMap& line_map,
                                  uint32_t stored_field_index,
                                  uint32_t record_index,
                                  uint32_t& out_frame_line,
                                  std::string& error_message);

}  // namespace orc

#endif  // ORC_VBI_LINE_MAPPING_H
