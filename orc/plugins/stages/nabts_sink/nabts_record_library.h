/*
 * File:        nabts_record_library.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Recorded NABTS record data, indexed so bytes a recording lost
 *              can be looked up by the bytes either side of them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NABTS_RECORD_LIBRARY_H
#define ORC_NABTS_RECORD_LIBRARY_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace orc {

/// Bytes of exact agreement a fill needs on each side of a hole, at the least:
/// the length of the anchors the index is keyed on.
constexpr size_t kNabtsLibraryAnchorBytes = 12;

/// Agreement is measured up to this far out on each side. Past it, a longer
/// match says no more about the bytes in the hole.
constexpr size_t kNabtsLibraryMaxContextBytes = 64;

/// Agreement on both sides together that a fill needs before it is used.
/// Chosen on complete recordings, with the lost packets of later tapes of the
/// same service punched into every record and the other complete
/// recordings to fill from: two thirds of the lost stretches filled, 98.9 % of
/// the fills byte for byte what was sent. Asking for more agreement buys a few
/// tenths of a percent of precision for fewer fills, and the page loses more
/// to a stretch left empty than it gains from the rare wrong one — the share
/// of pages drawn exactly as sent is highest here (see instructions.md).
constexpr size_t kNabtsLibraryTrustedContextBytes = 56;

/// A side of the context that uses fewer distinct byte values than this is a
/// run of padding or of one repeated operand, which matches anywhere and so
/// vouches for nothing.
constexpr size_t kNabtsLibraryMinDistinctBytes = 4;

/// Where two recordings offer different fills, the better one is used only when
/// its context is longer than the other's by at least this.
constexpr size_t kNabtsLibraryPreferenceMarginBytes = 16;

/// Where records that agree around a hole disagree inside it and none agrees
/// far enough further to be preferred, a fill offered by at least this many
/// times as many records as any other is used all the same — provided at least
/// kNabtsLibraryDominantMinRecords offer it and one of them agrees for
/// kNabtsLibraryDominantContextBytes around the hole. A service writes most of
/// a page from templates, so a stretch several pages share turns up in a
/// dozen records and a stray reading of it in one. Chosen on the same holdout
/// as kNabtsLibraryTrustedContextBytes: of the stretches the other rules left
/// ambiguous, 42 % settled, 99.6 % of them byte for byte what was sent, against
/// one in twenty wrong for a lead of two to five times. Zero turns it off.
constexpr size_t kNabtsLibraryDominantRatio = 10;
constexpr size_t kNabtsLibraryDominantMinRecords = 2;
constexpr size_t kNabtsLibraryDominantContextBytes = 32;

/// The thresholds a fill is held to. The defaults are the constants above;
/// the holdout the constants were chosen on sweeps the others.
struct NabtsLibraryPolicy {
  size_t trusted_context_bytes = kNabtsLibraryTrustedContextBytes;
  size_t min_distinct_bytes = kNabtsLibraryMinDistinctBytes;
  size_t preference_margin_bytes = kNabtsLibraryPreferenceMarginBytes;
  size_t dominant_ratio = kNabtsLibraryDominantRatio;
  size_t dominant_min_records = kNabtsLibraryDominantMinRecords;
  size_t dominant_context_bytes = kNabtsLibraryDominantContextBytes;
};

/// One record's data, as a recording delivered it.
struct NabtsLibraryRecord {
  /// Where the record came from, for the provenance a reconstruction reports:
  /// "this recording", or the reference stream's file name.
  std::string source;
  /// Channel, address and version, as the records viewer names the record.
  std::string key;
  std::vector<uint8_t> data;
  /// Non-zero where the byte arrived; empty when every byte did. A byte that
  /// never arrived is neither context nor fill.
  std::vector<uint8_t> present;
};

/// Bytes the library offers for a hole, and how strongly the recordings back
/// them.
struct NabtsLibraryFill {
  /// Exactly as long as the hole.
  std::vector<uint8_t> bytes;
  /// Record (index into NabtsRecordLibrary::records()) the fill was read from:
  /// the one with the longest context, where several agree.
  size_t record = 0;
  /// Bytes of exact agreement to the left and right of the hole, in that
  /// record, capped at kNabtsLibraryMaxContextBytes.
  size_t context_left = 0;
  size_t context_right = 0;
  /// Recordings, counted by record, that offered these same bytes.
  uint32_t agreeing_records = 0;
};

/// Why a hole got no fill, for the reconstruction's report.
enum class NabtsLibraryRefusal : uint8_t {
  /// No record has the bytes either side of the hole at the hole's distance.
  kNoMatch,
  /// A record does, but agrees on too little around the hole to vouch for it.
  kTooLittleContext,
  /// Records agree around the hole but disagree inside it, and none agrees
  /// enough more than the others to be preferred.
  kAmbiguous,
};

/// What the library made of one hole.
struct NabtsLibraryAnswer {
  std::optional<NabtsLibraryFill> fill;
  /// Meaningful only when there is no fill.
  NabtsLibraryRefusal refusal = NabtsLibraryRefusal::kNoMatch;
};

/**
 * @brief Recorded record data, indexed by what surrounds each stretch of it
 *
 * A recording that lost a packet lost a stretch of every record the packet
 * carried a block of. A service writes its pages from templates — the same
 * palette set-up, logo, frame and map outline on page after page, day after
 * day — so the lost stretch very often stands, complete, in another record:
 * another page of the same recording, or the same page in another recording
 * that did not lose that packet. Where the bytes either side of the hole occur
 * in such a record exactly the hole's length apart, what lies between them
 * there is what the hole lost.
 *
 * That inference is only as good as the agreement around it, so a fill is
 * offered only when the agreement is long (kNabtsLibraryTrustedContextBytes
 * across both sides), is not a run of padding or of one repeated byte, and
 * every record that agrees as well offers the same bytes. What was measured on
 * complete recordings with lost packets punched in decides those thresholds.
 *
 * The index holds a sample of the anchors (kNabtsLibraryAnchorBytes long) that
 * lie wholly in delivered bytes: one position in kSampleStride, chosen by the
 * anchor's own hash so that the same bytes are sampled wherever they occur. A
 * hole's left and right contexts are each searched at every position, so a
 * context of kNabtsLibraryMaxContextBytes almost always meets a sampled anchor
 * of any record it matches.
 *
 * Memory is linear in the library's delivered bytes — about four bytes of
 * index per byte of data. build() is O(N log N) in them; fill() is
 * O(C log N + M·C) for context length C and M candidate alignments.
 *
 * Not thread safe while being built; fill() is const and may be called from
 * several threads once build() has returned.
 */
class NabtsRecordLibrary {
 public:
  /// Add |record|. Records added after build() are not searched until it is
  /// called again.
  void add(NabtsLibraryRecord record);

  /// Index every record added so far.
  void build();

  const std::vector<NabtsLibraryRecord>& records() const { return records_; }

  /**
   * @brief What the library offers for data[start, end), a stretch that never
   *        arrived
   *
   * @param present Parallel to @p data; empty when every byte arrived. The
   *                context is read only from bytes that did.
   * @param exclude Index of a library record that is @p data itself, whose
   *                own bytes must not be offered back to it; records().size()
   *                or more excludes nothing.
   */
  NabtsLibraryAnswer fill(const std::vector<uint8_t>& data,
                          const std::vector<uint8_t>& present, size_t start,
                          size_t end, size_t exclude,
                          const NabtsLibraryPolicy& policy = {}) const;

  /**
   * @brief What the library offers in place of data[start, end), bytes that
   *        arrived but are known to be wrong
   *
   * As fill(), held to the same agreement, except that the other bytes marked
   * in @p wrong — known to be wrong too, as a byte failing its parity is —
   * neither count as agreement nor end the context: a recording's damage
   * comes in bursts, and a wrong byte beside another would otherwise leave
   * neither of them any context at all. A byte that is not marked is compared
   * as fill() compares it, so one real difference still ends the context.
   *
   * @param wrong Parallel to @p data: non-zero at each byte known to be
   *              wrong, [start, end) included.
   */
  NabtsLibraryAnswer correct(const std::vector<uint8_t>& data,
                             const std::vector<uint8_t>& present,
                             const std::vector<uint8_t>& wrong, size_t start,
                             size_t end, size_t exclude,
                             const NabtsLibraryPolicy& policy = {}) const;

 private:
  /// fill() and correct(): @p wrong is null for fill().
  NabtsLibraryAnswer search(const std::vector<uint8_t>& data,
                            const std::vector<uint8_t>& present,
                            const std::vector<uint8_t>* wrong, size_t start,
                            size_t end, size_t exclude,
                            const NabtsLibraryPolicy& policy) const;

  /// One sampled anchor: where it starts, in which record, and its hash.
  struct Anchor {
    uint64_t hash = 0;
    uint32_t record = 0;
    uint32_t offset = 0;
    bool operator<(const Anchor& other) const { return hash < other.hash; }
  };

  /// One anchor in kSampleStride is indexed.
  static constexpr uint64_t kSampleStride = 4;

  std::vector<NabtsLibraryRecord> records_;
  std::vector<Anchor> anchors_;  // sorted by hash
};

/// Whether byte @p index of a record arrived, given its presence mask.
inline bool nabts_library_present(const std::vector<uint8_t>& present,
                                  size_t index) {
  return present.empty() || (index < present.size() && present[index] != 0);
}

}  // namespace orc

#endif  // ORC_NABTS_RECORD_LIBRARY_H
