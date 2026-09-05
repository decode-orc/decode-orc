/*
 * File:        disc_metadata_document.cpp
 * Module:      orc-stage-plugin-video-sink
 * Purpose:     LaserDisc VBI metadata document for player emulation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "disc_metadata_document.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

namespace orc {
namespace {

// The CLV indicator code (IEC 60856/60857 s10.1.7), inserted on line 17 of
// CLV discs. The biphase observer logs it but publishes no observation, so it
// is recovered here from the raw words as a fallback disc-format signal.
constexpr int32_t kClvIndicatorCode = 0x87FFFF;

std::string to_str(int64_t v) { return std::to_string(v); }

// Fixed-width decimal, for timecode fields.
std::string pad2(int v) {
  if (v < 0) v = 0;
  if (v < 10) return "0" + std::to_string(v);
  return std::to_string(v);
}

// Six lowercase hex digits, or "------" when the line did not decode.
std::string hex24(int32_t word) {
  if (word < 0) return "------";
  static const char* digits = "0123456789abcdef";
  std::string out(6, '0');
  uint32_t v = static_cast<uint32_t>(word) & 0xFFFFFFu;
  for (int i = 5; i >= 0; --i) {
    out[static_cast<size_t>(i)] = digits[v & 0xFu];
    v >>= 4;
  }
  return out;
}

bool raw_all_absent(const DiscMetadataFrame& f) {
  for (int32_t w : f.raw_vbi) {
    if (w >= 0) return false;
  }
  return true;
}

bool raw_has_clv_indicator(const DiscMetadataFrame& f) {
  for (int32_t w : f.raw_vbi) {
    if (w == kClvIndicatorCode) return true;
  }
  return false;
}

// Collapse a sorted list of file frames into {start, count} ranges.
std::vector<std::pair<uint64_t, uint64_t>> collapse(
    const std::vector<uint64_t>& frames) {
  std::vector<std::pair<uint64_t, uint64_t>> out;
  for (uint64_t f : frames) {
    if (!out.empty() && out.back().first + out.back().second == f) {
      ++out.back().second;
    } else {
      out.push_back({f, 1});
    }
  }
  return out;
}

// --- Majority voting -------------------------------------------------------

class BoolVote {
 public:
  void add(bool v) { v ? ++true_ : ++false_; }
  std::optional<bool> result() const {
    if (true_ == 0 && false_ == 0) return std::nullopt;
    return true_ >= false_;
  }

 private:
  uint64_t true_ = 0;
  uint64_t false_ = 0;
};

class ModeVote {
 public:
  void add(VbiSoundMode m) { ++counts_[static_cast<int>(m)]; }
  std::optional<VbiSoundMode> result() const {
    if (counts_.empty()) return std::nullopt;
    // Ties resolve to the lowest enum value so the vote is deterministic.
    auto best = counts_.begin();
    for (auto it = counts_.begin(); it != counts_.end(); ++it) {
      if (it->second > best->second) best = it;
    }
    return static_cast<VbiSoundMode>(best->first);
  }

 private:
  std::map<int, uint64_t> counts_;
};

}  // namespace

// ---------------------------------------------------------------------------
// DiscFrameSet
// ---------------------------------------------------------------------------

void DiscFrameSet::add(uint64_t file_frame) {
  if (!ranges.empty() &&
      ranges.back().first + ranges.back().second == file_frame) {
    ++ranges.back().second;
    return;
  }
  ranges.push_back({file_frame, 1});
}

uint64_t DiscFrameSet::count() const {
  uint64_t total = 0;
  for (const auto& r : ranges) total += r.second;
  return total;
}

bool DiscFrameSet::contains(uint64_t file_frame) const {
  for (const auto& r : ranges) {
    if (file_frame < r.first) return false;
    if (file_frame < r.first + r.second) return true;
  }
  return false;
}

uint64_t DiscFrameSet::count_before(uint64_t file_frame) const {
  uint64_t total = 0;
  for (const auto& r : ranges) {
    if (r.first >= file_frame) break;
    total += std::min(r.second, file_frame - r.first);
  }
  return total;
}

// ---------------------------------------------------------------------------
// Frame rate and CLV arithmetic
// ---------------------------------------------------------------------------

int disc_nominal_frame_rate(VideoSystem system) {
  return system == VideoSystem::PAL ? 25 : 30;
}

std::pair<int, int> disc_exact_frame_rate(VideoSystem system) {
  if (system == VideoSystem::PAL) return {25, 1};
  return {30000, 1001};
}

int64_t clv_to_address(const CLVTimecode& tc, int frame_rate) {
  const int64_t seconds =
      (static_cast<int64_t>(tc.hours) * 60 + tc.minutes) * 60 + tc.seconds;
  return seconds * frame_rate + tc.picture_number;
}

CLVTimecode clv_from_address(int64_t address, int frame_rate) {
  if (address < 0) address = 0;
  CLVTimecode tc{};
  tc.picture_number = static_cast<int>(address % frame_rate);
  const int64_t seconds = address / frame_rate;
  tc.seconds = static_cast<int>(seconds % 60);
  const int64_t minutes = seconds / 60;
  tc.minutes = static_cast<int>(minutes % 60);
  tc.hours = static_cast<int>(minutes / 60);
  return tc;
}

const char* disc_sound_mode_name(VbiSoundMode mode) {
  switch (mode) {
    case VbiSoundMode::STEREO:
      return "stereo";
    case VbiSoundMode::MONO:
      return "mono";
    case VbiSoundMode::AUDIO_SUBCARRIERS_OFF:
      return "audio_subcarriers_off";
    case VbiSoundMode::BILINGUAL:
      return "bilingual";
    case VbiSoundMode::STEREO_STEREO:
      return "stereo_stereo";
    case VbiSoundMode::STEREO_BILINGUAL:
      return "stereo_bilingual";
    case VbiSoundMode::CROSS_CHANNEL_STEREO:
      return "cross_channel_stereo";
    case VbiSoundMode::BILINGUAL_BILINGUAL:
      return "bilingual_bilingual";
    case VbiSoundMode::MONO_DUMP:
      return "mono_dump";
    case VbiSoundMode::STEREO_DUMP:
      return "stereo_dump";
    case VbiSoundMode::BILINGUAL_DUMP:
      return "bilingual_dump";
    case VbiSoundMode::FUTURE_USE:
      return "future_use";
  }
  return "future_use";
}

std::string disc_frame_set_bitmap(const DiscFrameSet& set,
                                  uint64_t frame_count) {
  const size_t bytes = static_cast<size_t>((frame_count + 7) / 8);
  std::vector<uint8_t> bits(bytes, 0);
  for (const auto& r : set.ranges) {
    for (uint64_t f = r.first; f < r.first + r.second && f < frame_count; ++f) {
      bits[static_cast<size_t>(f / 8)] |=
          static_cast<uint8_t>(1u << (f % 8));  // LSB-first within each byte
    }
  }
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes * 2);
  for (uint8_t b : bits) {
    out.push_back(digits[(b >> 4) & 0xF]);
    out.push_back(digits[b & 0xF]);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Address map
// ---------------------------------------------------------------------------

namespace {

// The address carried by a frame, in the map's own address space.
std::optional<int64_t> frame_address(const DiscMetadataFrame& f,
                                     DiscAddressKind kind, int frame_rate) {
  if (kind == DiscAddressKind::CavPicture) {
    if (f.picture_number) return static_cast<int64_t>(*f.picture_number);
    return std::nullopt;
  }
  if (kind == DiscAddressKind::ClvTimecode) {
    if (f.clv_timecode) return clv_to_address(*f.clv_timecode, frame_rate);
    return std::nullopt;
  }
  return std::nullopt;
}

// Why a frame might carry no number. A pulldown frame's VBI is intact and
// simply has no picture code; a decode failure leaves nothing at all.
enum class HoleHint { Unknown, LikelyUnnumbered, LikelyUndecoded };

HoleHint hole_hint(const DiscMetadataFrame& f) {
  // Nothing decoded at all: the number existed, we did not read it.
  if (raw_all_absent(f)) return HoleHint::LikelyUndecoded;
  // IEC 60856/60857 s10.1.5: chapter numbers are inserted in the fields
  // "which do not have an insertion of picture numbers", so a clean frame
  // carrying a chapter code and no picture code is positively a frame the
  // disc never numbered.
  if (f.chapter_number) return HoleHint::LikelyUnnumbered;
  return HoleHint::Unknown;
}

// Classify the holes between two numbered frames.
//
//   delta == k + 1 -> every hole advanced the count  -> all undecoded
//   delta == 1     -> no hole advanced the count     -> all unnumbered
//   otherwise      -> a mix; resolvable only if the per-frame hints account
//                     for it exactly, else the caller must break the run.
//
// Returns false when the gap cannot be attributed.
bool classify_holes(const std::vector<uint64_t>& holes,
                    const std::vector<HoleHint>& hints, int64_t delta,
                    std::vector<uint64_t>& unnumbered_out,
                    std::vector<uint64_t>& undecoded_out) {
  const int64_t k = static_cast<int64_t>(holes.size());
  if (k == 0) return delta == 1;

  if (delta == k + 1) {  // all advanced
    for (uint64_t h : holes) undecoded_out.push_back(h);
    return true;
  }
  if (delta == 1) {  // none advanced
    for (uint64_t h : holes) unnumbered_out.push_back(h);
    return true;
  }
  if (delta < 1 || delta > k + 1) return false;  // discontinuity, not a mix

  // Mixed. Attribute what the hints settle, then see whether the remainder
  // falls into exactly one category; anything else is a genuine ambiguity and
  // the honest response is a run boundary, not a guess.
  int64_t need_advance = delta - 1;  // holes that advanced the count
  int64_t need_hold = k - need_advance;
  int64_t hinted_advance = 0;
  int64_t hinted_hold = 0;
  for (HoleHint h : hints) {
    if (h == HoleHint::LikelyUndecoded) ++hinted_advance;
    if (h == HoleHint::LikelyUnnumbered) ++hinted_hold;
  }
  const int64_t rest_advance = need_advance - hinted_advance;
  const int64_t rest_hold = need_hold - hinted_hold;
  if (rest_advance < 0 || rest_hold < 0) return false;
  if (rest_advance != 0 && rest_hold != 0) return false;

  for (size_t i = 0; i < holes.size(); ++i) {
    switch (hints[i]) {
      case HoleHint::LikelyUndecoded:
        undecoded_out.push_back(holes[i]);
        break;
      case HoleHint::LikelyUnnumbered:
        unnumbered_out.push_back(holes[i]);
        break;
      case HoleHint::Unknown:
        if (rest_advance != 0) {
          undecoded_out.push_back(holes[i]);
        } else {
          unnumbered_out.push_back(holes[i]);
        }
        break;
    }
  }
  return true;
}

DiscAddressMap build_address_map(const std::vector<DiscMetadataFrame>& frames,
                                 DiscAddressKind kind, int frame_rate) {
  DiscAddressMap map;
  map.kind = kind;
  if (kind == DiscAddressKind::None) {
    for (uint64_t i = 0; i < frames.size(); ++i) map.unmapped.add(i);
    return map;
  }

  const uint64_t n = frames.size();
  std::vector<uint64_t> unmapped;
  std::vector<uint64_t> unnumbered;
  std::vector<uint64_t> undecoded;

  uint64_t i = 0;
  while (i < n) {
    auto addr = frame_address(frames[i], kind, frame_rate);
    if (!addr) {  // cannot start a run without a number to anchor it
      unmapped.push_back(i);
      ++i;
      continue;
    }

    const uint64_t run_start = i;
    const int64_t run_address = *addr;
    uint64_t last_numbered = i;
    int64_t last_address = *addr;

    std::vector<uint64_t> holes;
    std::vector<HoleHint> hints;
    std::vector<uint64_t> run_unnumbered;
    std::vector<uint64_t> run_undecoded;

    uint64_t j = i + 1;
    while (j < n) {
      auto a = frame_address(frames[j], kind, frame_rate);
      if (!a) {
        holes.push_back(j);
        hints.push_back(hole_hint(frames[j]));
        ++j;
        continue;
      }
      if (!classify_holes(holes, hints, *a - last_address, run_unnumbered,
                          run_undecoded)) {
        break;  // discontinuity or unattributable gap: the run ends here
      }
      holes.clear();
      hints.clear();
      last_numbered = j;
      last_address = *a;
      ++j;
    }

    map.runs.push_back({run_start, last_numbered - run_start + 1, run_address});
    unnumbered.insert(unnumbered.end(), run_unnumbered.begin(),
                      run_unnumbered.end());
    undecoded.insert(undecoded.end(), run_undecoded.begin(),
                     run_undecoded.end());

    // Holes trailing the last numbered frame of the run cannot be classified
    // - there is no following number to measure the advance against.
    for (uint64_t h : holes) unmapped.push_back(h);

    // Resume at j: either the end of the frames, or the numbered frame whose
    // gap could not be attributed, which anchors the next run. Both are
    // strictly greater than run_start, so the outer loop always advances.
    i = j;
  }

  std::sort(unmapped.begin(), unmapped.end());
  std::sort(unnumbered.begin(), unnumbered.end());
  std::sort(undecoded.begin(), undecoded.end());
  for (uint64_t f : unmapped) map.unmapped.add(f);
  for (uint64_t f : unnumbered) map.unnumbered.add(f);
  for (uint64_t f : undecoded) map.undecoded.add(f);
  return map;
}

DiscAddressKind choose_address_kind(
    const std::vector<DiscMetadataFrame>& frames) {
  uint64_t cav = 0;
  uint64_t clv = 0;
  for (const auto& f : frames) {
    if (f.picture_number) ++cav;
    if (f.clv_timecode) ++clv;
  }
  if (cav == 0 && clv == 0) return DiscAddressKind::None;
  return cav >= clv ? DiscAddressKind::CavPicture
                    : DiscAddressKind::ClvTimecode;
}

DiscStatusSummary vote_status(const std::vector<DiscMetadataFrame>& frames) {
  DiscStatusSummary s;
  BoolVote size_v, side_v, cx_v, tt_v, digital_v, fm_v, dump_v;
  BoolVote a2_copy_v, a2_std_v;
  ModeVote mode_v, a2_mode_v;

  s.fields_total = static_cast<uint64_t>(frames.size()) * 2;
  for (const auto& f : frames) {
    for (const auto& ps : f.programme_status) {
      if (!ps) continue;
      ++s.fields_with_status;
      if (ps->parity_valid) ++s.fields_parity_valid;
      size_v.add(ps->is_12_inch);
      side_v.add(ps->is_side_1);
      cx_v.add(ps->cx_enabled);
      tt_v.add(ps->has_teletext);
      digital_v.add(ps->is_digital);
      fm_v.add(ps->is_fm_multiplex);
      dump_v.add(ps->is_programme_dump);
      mode_v.add(ps->sound_mode);
    }
    for (const auto& a2 : f.amendment2_status) {
      if (!a2) continue;
      s.amendment2_present = true;
      a2_copy_v.add(a2->copy_permitted);
      a2_std_v.add(a2->is_video_standard);
      a2_mode_v.add(a2->sound_mode);
    }
    if (!s.user_code && f.user_code) s.user_code = f.user_code;
  }

  s.is_12_inch = size_v.result();
  s.is_side_1 = side_v.result();
  s.cx_enabled = cx_v.result();
  s.has_teletext = tt_v.result();
  s.is_digital = digital_v.result();
  s.fm_multiplex = fm_v.result();
  s.programme_dump = dump_v.result();
  s.sound_mode = mode_v.result();
  s.a2_copy_permitted = a2_copy_v.result();
  s.a2_video_standard = a2_std_v.result();
  s.a2_sound_mode = a2_mode_v.result();

  // Agreement: the fraction of status fields whose whole word matches the
  // vote. A strict measure - one dissenting bit counts the field as
  // disagreeing - because that is what "how much do I trust this" means here.
  if (s.fields_with_status > 0) {
    uint64_t agree = 0;
    for (const auto& f : frames) {
      for (const auto& ps : f.programme_status) {
        if (!ps) continue;
        if (s.is_12_inch && ps->is_12_inch == *s.is_12_inch && s.is_side_1 &&
            ps->is_side_1 == *s.is_side_1 && s.cx_enabled &&
            ps->cx_enabled == *s.cx_enabled && s.has_teletext &&
            ps->has_teletext == *s.has_teletext && s.is_digital &&
            ps->is_digital == *s.is_digital && s.fm_multiplex &&
            ps->is_fm_multiplex == *s.fm_multiplex && s.programme_dump &&
            ps->is_programme_dump == *s.programme_dump && s.sound_mode &&
            ps->sound_mode == *s.sound_mode) {
          ++agree;
        }
      }
    }
    s.agreement =
        static_cast<double>(agree) / static_cast<double>(s.fields_with_status);
  }
  return s;
}

DiscEvents gather_events(const std::vector<DiscMetadataFrame>& frames) {
  DiscEvents ev;
  std::vector<uint64_t> lead_in;
  std::vector<uint64_t> lead_out;
  int32_t current_chapter = -1;
  for (uint64_t i = 0; i < frames.size(); ++i) {
    const auto& f = frames[i];
    if (f.lead_in) lead_in.push_back(i);
    if (f.lead_out) lead_out.push_back(i);
    if (f.stop_code) ev.stop_codes.push_back(i);
    if (f.chapter_number && *f.chapter_number != current_chapter) {
      ev.chapters.push_back({i, *f.chapter_number});
      current_chapter = *f.chapter_number;
    }
  }
  ev.lead_in = collapse(lead_in);
  ev.lead_out = collapse(lead_out);
  return ev;
}

}  // namespace

DiscMetadataDocument build_disc_metadata_document(
    const std::vector<DiscMetadataFrame>& frames, VideoSystem system,
    bool is_tff, uint64_t source_first_frame,
    const std::string& generator_version, const std::string& created) {
  DiscMetadataDocument doc;
  doc.generator_version = generator_version;
  doc.created = created;
  doc.system = system;
  doc.frame_rate_nominal = disc_nominal_frame_rate(system);
  doc.frame_rate_exact = disc_exact_frame_rate(system);
  doc.is_tff = is_tff;
  doc.frame_count = frames.size();
  doc.source_first_frame = source_first_frame;
  doc.source_last_frame = frames.empty()
                              ? source_first_frame
                              : source_first_frame + frames.size() - 1;

  const DiscAddressKind kind = choose_address_kind(frames);
  doc.address_map = build_address_map(frames, kind, doc.frame_rate_nominal);
  doc.status = vote_status(frames);
  doc.events = gather_events(frames);

  if (kind == DiscAddressKind::CavPicture) {
    doc.disc_format_known = true;
    doc.disc_is_cav = true;
  } else if (kind == DiscAddressKind::ClvTimecode) {
    doc.disc_format_known = true;
    doc.disc_is_cav = false;
  } else {
    // No address recovered; the CLV indicator code is the remaining signal.
    for (const auto& f : frames) {
      if (raw_has_clv_indicator(f)) {
        doc.disc_format_known = true;
        doc.disc_is_cav = false;
        break;
      }
    }
  }

  doc.raw_vbi.reserve(frames.size());
  for (const auto& f : frames) doc.raw_vbi.push_back(f.raw_vbi);
  return doc;
}

// ---------------------------------------------------------------------------
// YAML emitter
// ---------------------------------------------------------------------------

namespace {

const char* system_name(VideoSystem system) {
  switch (system) {
    case VideoSystem::PAL:
      return "PAL";
    case VideoSystem::NTSC:
      return "NTSC";
    case VideoSystem::PAL_M:
      return "PAL_M";
    case VideoSystem::Unknown:
      return "unknown";
  }
  return "unknown";
}

const char* address_kind_name(DiscAddressKind kind) {
  switch (kind) {
    case DiscAddressKind::CavPicture:
      return "cav_picture";
    case DiscAddressKind::ClvTimecode:
      return "clv_timecode";
    case DiscAddressKind::None:
      return "none";
  }
  return "none";
}

std::string bool_str(bool v) { return v ? "true" : "false"; }

std::string ranges_str(const std::vector<std::pair<uint64_t, uint64_t>>& r) {
  std::string out = "[";
  for (size_t i = 0; i < r.size(); ++i) {
    if (i) out += ", ";
    out += "[" + to_str(static_cast<int64_t>(r[i].first)) + ", " +
           to_str(static_cast<int64_t>(r[i].second)) + "]";
  }
  out += "]";
  return out;
}

// A frame set is emitted as ranges or as a bitmap, whichever is smaller.
// Ranges stay readable while few frames are affected; the bitmap is one bit
// per file frame - 6.75 KB for a 54,000-frame side - and assumes nothing
// about the pulldown pattern being regular. It is not, across edits.
void emit_frame_set(std::ostringstream& o, const char* key,
                    const DiscFrameSet& set, uint64_t frame_count,
                    const char* comment) {
  if (set.empty()) return;
  const std::string ranges = ranges_str(set.ranges);
  const std::string bitmap = disc_frame_set_bitmap(set, frame_count);
  o << "  " << key << ":";
  if (comment && *comment) o << "  # " << comment;
  o << "\n";
  o << "    count: " << set.count() << "\n";
  if (ranges.size() <= bitmap.size()) {
    o << "    encoding: ranges\n";
    o << "    ranges: " << ranges << "\n";
  } else {
    o << "    encoding: bitmap\n";
    o << "    bitmap: \"" << bitmap << "\"\n";
  }
}

}  // namespace

std::string emit_disc_metadata_yaml(const DiscMetadataDocument& doc,
                                    DiscMetadataDetail detail) {
  std::ostringstream o;

  // Header. The comment makes the file identifiable by eye, the directive
  // declares the dialect so no parser has to infer it, and format /
  // format_version are the reader's compatibility gate. format_version is
  // always quoted: unquoted, YAML reads it as a float and "1.10" silently
  // becomes 1.1.
  o << "# decode-orc disc metadata\n";
  o << "%YAML 1.2\n";
  o << "---\n";
  o << "format: " << kDiscMetadataFormat << "\n";
  o << "format_version: \"" << kDiscMetadataFormatVersion << "\"\n";
  o << "\n";

  o << "generator:\n";
  o << "  application: decode-orc\n";
  if (!doc.generator_version.empty()) {
    o << "  version: \"" << doc.generator_version << "\"\n";
  }
  if (!doc.created.empty()) o << "  created: \"" << doc.created << "\"\n";
  o << "\n";

  o << "video:\n";
  o << "  system: " << system_name(doc.system) << "\n";
  o << "  frame_rate: " << doc.frame_rate_nominal << "\n";
  o << "  frame_rate_exact: [" << doc.frame_rate_exact.first << ", "
    << doc.frame_rate_exact.second << "]\n";
  o << "  field_order: " << (doc.is_tff ? "tff" : "bff") << "\n";
  o << "  frame_count: " << doc.frame_count << "\n";
  o << "  fields_per_frame: 2\n";
  o << "\n";

  o << "source:\n";
  o << "  first_frame: " << doc.source_first_frame << "\n";
  o << "  last_frame: " << doc.source_last_frame << "\n";
  o << "\n";

  const auto& s = doc.status;
  o << "disc:\n";
  if (doc.disc_format_known) {
    o << "  format: " << (doc.disc_is_cav ? "CAV" : "CLV") << "\n";
  }
  if (s.is_side_1) o << "  side: " << (*s.is_side_1 ? 1 : 2) << "\n";
  if (s.is_12_inch) o << "  size_inches: " << (*s.is_12_inch ? 12 : 8) << "\n";
  if (s.cx_enabled) o << "  cx_enabled: " << bool_str(*s.cx_enabled) << "\n";
  if (s.has_teletext) o << "  teletext: " << bool_str(*s.has_teletext) << "\n";
  if (s.is_digital) {
    o << "  digital_video: " << bool_str(*s.is_digital) << "\n";
  }
  if (s.sound_mode) {
    o << "  sound_mode: " << disc_sound_mode_name(*s.sound_mode) << "\n";
  }
  if (s.fm_multiplex) {
    o << "  fm_multiplex: " << bool_str(*s.fm_multiplex) << "\n";
  }
  if (s.programme_dump) {
    o << "  programme_dump: " << bool_str(*s.programme_dump) << "\n";
  }
  if (s.user_code) o << "  user_code: \"" << *s.user_code << "\"\n";
  if (s.amendment2_present) {
    o << "  amendment2:\n";
    o << "    present: true\n";
    if (s.a2_copy_permitted) {
      o << "    copy_permitted: " << bool_str(*s.a2_copy_permitted) << "\n";
    }
    if (s.a2_video_standard) {
      o << "    video_standard: " << bool_str(*s.a2_video_standard) << "\n";
    }
    if (s.a2_sound_mode) {
      o << "    sound_mode: " << disc_sound_mode_name(*s.a2_sound_mode) << "\n";
    }
  }
  o << "  confidence:\n";
  o << "    fields_total: " << s.fields_total << "\n";
  o << "    fields_with_status: " << s.fields_with_status << "\n";
  o << "    fields_parity_valid: " << s.fields_parity_valid << "\n";
  // Four decimal places, formatted by hand so the output cannot pick up a
  // locale's decimal separator.
  const int64_t agreement_ppm =
      static_cast<int64_t>(std::llround(s.agreement * 10000.0));
  o << "    agreement: " << (agreement_ppm / 10000) << "."
    << pad2(static_cast<int>((agreement_ppm / 100) % 100))
    << pad2(static_cast<int>(agreement_ppm % 100)) << "\n";
  o << "\n";

  const auto& m = doc.address_map;
  o << "address_map:\n";
  o << "  kind: " << address_kind_name(m.kind) << "\n";
  o << "  source: " << m.source << "\n";
  if (!m.runs.empty()) {
    o << "  runs:\n";
    for (const auto& r : m.runs) {
      o << "    - {file_frame: " << r.file_frame << ", count: " << r.count
        << ", ";
      if (m.kind == DiscAddressKind::ClvTimecode) {
        const CLVTimecode tc =
            clv_from_address(r.address, doc.frame_rate_nominal);
        o << "time: \"" << pad2(tc.hours) << ":" << pad2(tc.minutes) << ":"
          << pad2(tc.seconds) << "." << pad2(tc.picture_number) << "\"";
      } else {
        o << "picture: " << r.address;
      }
      o << "}\n";
    }
  }
  emit_frame_set(o, "unnumbered", m.unnumbered, doc.frame_count,
                 "no number on the disc; these do NOT advance the count");
  emit_frame_set(o, "undecoded", m.undecoded, doc.frame_count,
                 "number exists but was not recovered; these DO advance it");
  emit_frame_set(o, "unmapped", m.unmapped, doc.frame_count,
                 "covered by no run at all");
  o << "\n";

  const auto& e = doc.events;
  o << "events:\n";
  if (!e.lead_in.empty()) {
    o << "  lead_in: " << ranges_str(e.lead_in) << "\n";
  }
  if (!e.lead_out.empty()) {
    o << "  lead_out: " << ranges_str(e.lead_out) << "\n";
  }
  if (!e.stop_codes.empty()) {
    o << "  stop_codes: [";
    for (size_t i = 0; i < e.stop_codes.size(); ++i) {
      if (i) o << ", ";
      o << e.stop_codes[i];
    }
    o << "]\n";
  }
  if (!e.chapters.empty()) {
    o << "  chapters:\n";
    for (const auto& c : e.chapters) {
      o << "    - {file_frame: " << c.first << ", chapter: " << c.second
        << "}\n";
    }
  }

  if (detail == DiscMetadataDetail::Full && !doc.raw_vbi.empty()) {
    o << "\n";
    o << "raw_vbi:\n";
    o << "  encoding: hex24-per-field\n";
    o << "  lines: [16, 17, 18]\n";
    o << "  # One line per file frame, six words:\n";
    o << "  #   f1_l16 f1_l17 f1_l18 f2_l16 f2_l17 f2_l18\n";
    o << "  # \"------\" means the line was not decoded.\n";
    o << "  data: |\n";
    for (const auto& w : doc.raw_vbi) {
      o << "    ";
      for (size_t k = 0; k < w.size(); ++k) {
        if (k) o << " ";
        o << hex24(w[k]);
      }
      o << "\n";
    }
  }

  return o.str();
}

std::vector<std::pair<std::string, std::string>> disc_metadata_tags(
    const DiscMetadataDocument& doc) {
  std::vector<std::pair<std::string, std::string>> tags;
  tags.push_back({"ORC_DISC_METADATA", kDiscMetadataFilename});
  tags.push_back({"ORC_DISC_METADATA_VERSION", kDiscMetadataFormatVersion});
  if (doc.disc_format_known) {
    tags.push_back({"ORC_DISC_FORMAT", doc.disc_is_cav ? "CAV" : "CLV"});
  }
  const auto& s = doc.status;
  if (s.is_side_1) {
    tags.push_back({"ORC_DISC_SIDE", *s.is_side_1 ? "1" : "2"});
  }
  if (s.is_12_inch) {
    tags.push_back({"ORC_DISC_SIZE_INCHES", *s.is_12_inch ? "12" : "8"});
  }
  tags.push_back({"ORC_VIDEO_SYSTEM", system_name(doc.system)});

  // First and last address the map actually states, for CAV discs.
  if (doc.address_map.kind == DiscAddressKind::CavPicture &&
      !doc.address_map.runs.empty()) {
    const auto& first = doc.address_map.runs.front();
    const auto& last = doc.address_map.runs.back();
    // address(f) = address0 + (f - f0) - |unnumbered n [f0, f)|, evaluated at
    // the run's final frame, which is numbered by construction.
    const uint64_t last_frame = last.file_frame + last.count - 1;
    const uint64_t held =
        doc.address_map.unnumbered.count_before(last_frame) -
        doc.address_map.unnumbered.count_before(last.file_frame);
    const int64_t last_address = last.address +
                                 static_cast<int64_t>(last.count) - 1 -
                                 static_cast<int64_t>(held);
    tags.push_back({"ORC_FIRST_PICTURE", to_str(first.address)});
    tags.push_back({"ORC_LAST_PICTURE", to_str(last_address)});
  }
  if (s.sound_mode) {
    tags.push_back({"ORC_SOUND_MODE", disc_sound_mode_name(*s.sound_mode)});
  }
  if (s.cx_enabled) {
    tags.push_back({"ORC_CX", *s.cx_enabled ? "on" : "off"});
  }
  return tags;
}

}  // namespace orc
