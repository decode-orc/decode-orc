/*
 * File:        disc_metadata_document.h
 * Module:      orc-stage-plugin-video-sink
 * Purpose:     LaserDisc VBI metadata document for player emulation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_PLUGIN_VIDEO_SINK_DISC_METADATA_DOCUMENT_H
#define ORC_STAGE_PLUGIN_VIDEO_SINK_DISC_METADATA_DOCUMENT_H

#include <orc/stage/common_types.h>
#include <orc/support/vbi_types.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// A LaserDisc decode carries far more than pictures: the VBI biphase data on
// lines 16-18 of every field encodes picture numbers, programme time codes,
// chapter markers, stop codes, lead-in/lead-out markers and the programme
// status word - the same information a real player exposes over its serial
// interface. This header builds a self-describing document from those
// observations so an emulator can seek by disc address.
//
// See docs/technical/disc-metadata-format.md for the format reference: the
// schema, the address rule, the version-compatibility contract and what a
// consumer must do.
//
// Everything here is pure: no FFmpeg types, no observation context, no I/O.
// The observation-reading adapter lives in disc_metadata_collect.h so that the
// run-collapsing and emitting logic can be unit tested over plain data.
//
// The YAML is emitted by hand rather than through yaml-cpp. The document is
// flat, and it has to be byte-deterministic so the format's header can be
// sniffed and a redeploy of the same decode produces the same bytes; against
// that, adding yaml-cpp to a plugin that otherwise links only
// orc_chroma_decoders and FFmpeg is not worth the dependency. The same
// function is what a standalone sidecar sink would call, so the two routes
// cannot drift (issue #300).

namespace orc {

// ---------------------------------------------------------------------------
// Identity and versioning
// ---------------------------------------------------------------------------

// The attachment's filename and MIME type are a contract: a consumer locates
// the document by either and must not have to guess. Neither is ever
// versioned - they are how the document is *found*; format_version below says
// whether it can be *read*.
inline constexpr const char* kDiscMetadataFilename = "orc-disc-metadata.yaml";
inline constexpr const char* kDiscMetadataMimeType =
    "application/vnd.decode-orc.disc-metadata+yaml";

// Document type key, constant for the life of the format. Identifies the
// document independently of filename and MIME type, so it survives being
// extracted, renamed, or carried in some future container.
inline constexpr const char* kDiscMetadataFormat = "orc-disc-metadata";

// MAJOR.MINOR. A MINOR bump is additive only and a reader must accept it,
// ignoring what it does not recognise. A MAJOR bump changes a rule a reader
// relies on, and a reader must refuse the document rather than guess.
inline constexpr int kDiscMetadataFormatMajor = 1;
inline constexpr int kDiscMetadataFormatMinor = 0;
inline constexpr const char* kDiscMetadataFormatVersion = "1.0";

// ---------------------------------------------------------------------------
// Input: one file frame's worth of recovered VBI
// ---------------------------------------------------------------------------

/**
 * @brief Recovered VBI for a single file frame, both fields merged
 *
 * LaserDisc VBI is routinely split across the two fields of a frame (CLV
 * hours/minutes on lines 17/18 against seconds/picture on line 16), so the
 * frame-level members below are merged. The programme status word appears on
 * line 16 of every field, so those stay per-field: voting over both fields
 * doubles the sample count.
 */
struct DiscMetadataFrame {
  // Frame-level, merged from both fields
  std::optional<int32_t> picture_number;    // CAV
  std::optional<CLVTimecode> clv_timecode;  // CLV
  std::optional<int32_t> chapter_number;
  bool lead_in = false;
  bool lead_out = false;
  bool stop_code = false;
  std::optional<std::string> user_code;

  // Per-field; index 0 is the first field of the frame
  std::array<std::optional<ProgrammeStatus>, 2> programme_status;
  std::array<std::optional<Amendment2Status>, 2> amendment2_status;

  // Raw biphase words: f1 line 16/17/18 then f2 line 16/17/18.
  // -1 means the line was not decoded. Always collected, even when the detail
  // level will not emit them: the hole classifier needs them.
  std::array<int32_t, 6> raw_vbi = {-1, -1, -1, -1, -1, -1};
};

// ---------------------------------------------------------------------------
// Output model
// ---------------------------------------------------------------------------

/// How much of the document to emit.
enum class DiscMetadataDetail {
  Map,  ///< Address map, events and disc status (the default)
  Full  ///< ...plus the raw biphase words for every field
};

/// Which address space the map is expressed in.
enum class DiscAddressKind {
  None,         ///< No address information was recovered
  CavPicture,   ///< CAV picture numbers
  ClvTimecode,  ///< CLV programme time codes
};

/// A half-open set of file frames, stored as sorted disjoint ranges.
struct DiscFrameSet {
  std::vector<std::pair<uint64_t, uint64_t>> ranges;  // {file_frame, count}

  void add(uint64_t file_frame);  ///< Append; must be non-decreasing
  uint64_t count() const;         ///< Total frames in the set
  bool empty() const { return ranges.empty(); }
  bool contains(uint64_t file_frame) const;
  /// Members in [0, file_frame); the correction term of the address rule.
  uint64_t count_before(uint64_t file_frame) const;
};

/**
 * @brief A span of file frames over which the disc address is arithmetic
 *
 * The address advances once per file frame, except across frames listed in
 * the map's `unnumbered` set, which advance it by nothing:
 *
 *   address(f) = address0 + (f - file_frame) - |unnumbered n [file_frame, f)|
 *
 * Frames in `undecoded` advance the address normally - their address exists,
 * it is merely unknown - so the same expression yields it correctly.
 */
struct DiscAddressRun {
  uint64_t file_frame = 0;  ///< First file frame of the run
  uint64_t count = 0;       ///< Frames spanned, holes included
  int64_t address = 0;      ///< Address of `file_frame` (see kind)
};

struct DiscAddressMap {
  DiscAddressKind kind = DiscAddressKind::None;
  std::string source = "biphase";  ///< "biphase"; "fm40" reserved (NTSC)
  std::vector<DiscAddressRun> runs;
  DiscFrameSet unnumbered;  ///< No number on the disc (NTSC pulldown frames)
  DiscFrameSet undecoded;   ///< Number exists but was not recovered
  DiscFrameSet unmapped;    ///< Covered by no run at all
};

/// Disc-level status, voted across every field that carried a status word.
struct DiscStatusSummary {
  std::optional<bool> is_12_inch;
  std::optional<bool> is_side_1;
  std::optional<bool> cx_enabled;
  std::optional<bool> has_teletext;
  std::optional<bool> is_digital;
  std::optional<VbiSoundMode> sound_mode;
  std::optional<bool> fm_multiplex;
  std::optional<bool> programme_dump;
  std::optional<std::string> user_code;

  bool amendment2_present = false;
  std::optional<bool> a2_copy_permitted;
  std::optional<bool> a2_video_standard;
  std::optional<VbiSoundMode> a2_sound_mode;

  // Confidence: how much evidence stands behind the values above
  uint64_t fields_total = 0;
  uint64_t fields_with_status = 0;
  uint64_t fields_parity_valid = 0;
  double agreement = 0.0;  ///< Fraction of status fields agreeing with the vote
};

struct DiscEvents {
  std::vector<std::pair<uint64_t, uint64_t>> lead_in;   // {file_frame, count}
  std::vector<std::pair<uint64_t, uint64_t>> lead_out;  // {file_frame, count}
  std::vector<uint64_t> stop_codes;                     // file frames
  std::vector<std::pair<uint64_t, int32_t>> chapters;   // {file_frame, chapter}
};

/// Assembled document, ready to emit.
struct DiscMetadataDocument {
  // generator
  std::string generator_version;  ///< decode-orc version string
  std::string created;            ///< ISO 8601 UTC, empty to omit

  // video
  VideoSystem system = VideoSystem::PAL;
  int frame_rate_nominal = 25;  ///< 25 (PAL) or 30 (NTSC, PAL-M)
  std::pair<int, int> frame_rate_exact = {25, 1};
  bool is_tff = true;
  uint64_t frame_count = 0;

  // source provenance
  uint64_t source_first_frame = 0;
  uint64_t source_last_frame = 0;

  // disc
  bool disc_format_known = false;
  bool disc_is_cav = false;
  DiscStatusSummary status;

  DiscAddressMap address_map;
  DiscEvents events;

  // Raw biphase words, one entry per file frame; only emitted at Full detail.
  std::vector<std::array<int32_t, 6>> raw_vbi;
};

// ---------------------------------------------------------------------------
// Builder and emitters (pure)
// ---------------------------------------------------------------------------

/// Nominal frames per second for a video system: the rate CLV timecode counts
/// at, which for NTSC is 30 against an actual 30000/1001.
int disc_nominal_frame_rate(VideoSystem system);

/// Exact frame rate as {numerator, denominator}.
std::pair<int, int> disc_exact_frame_rate(VideoSystem system);

/**
 * @brief Assemble the document from per-frame VBI
 *
 * Collapses the address map into runs, classifies frames that carry no
 * number, votes the disc-level status and gathers the sparse events.
 */
DiscMetadataDocument build_disc_metadata_document(
    const std::vector<DiscMetadataFrame>& frames, VideoSystem system,
    bool is_tff, uint64_t source_first_frame,
    const std::string& generator_version, const std::string& created = "");

/// Serialise to YAML. Byte-deterministic for a given document and detail.
std::string emit_disc_metadata_yaml(const DiscMetadataDocument& doc,
                                    DiscMetadataDetail detail);

/// Disc-level summary as container tag key/value pairs. Values never
/// recovered are omitted rather than written as "unknown".
std::vector<std::pair<std::string, std::string>> disc_metadata_tags(
    const DiscMetadataDocument& doc);

// ---------------------------------------------------------------------------
// Helpers exposed for testing
// ---------------------------------------------------------------------------

/// CLV timecode as a frame count, for run arithmetic.
int64_t clv_to_address(const CLVTimecode& tc, int frame_rate);
/// Inverse of clv_to_address.
CLVTimecode clv_from_address(int64_t address, int frame_rate);

/// Hex bitmap of a frame set, LSB-first within each byte, 1 = member.
std::string disc_frame_set_bitmap(const DiscFrameSet& set,
                                  uint64_t frame_count);

/// Sound mode as the spelling used in the document and container tags.
const char* disc_sound_mode_name(VbiSoundMode mode);

}  // namespace orc

#endif  // ORC_STAGE_PLUGIN_VIDEO_SINK_DISC_METADATA_DOCUMENT_H
