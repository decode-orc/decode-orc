/*
 * File:        eia608_line21_pairs.h
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     Which line 21 byte pairs a receiver takes: their parity, the
 *              codes EIA-608 defines for each field, and the field order
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_EIA608_LINE21_PAIRS_H
#define ORC_EIA608_LINE21_PAIRS_H

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context_interface.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace orc {

// The free functions below keep no state: they are reentrant.

/// One field's line 21 as the closed caption observer read it: whether the
/// line carried line 21's waveform, the two bytes' seven data bits and whether
/// each byte's odd parity over all eight bits held.
struct EIA608FieldReading {
  bool present = false;
  uint8_t data0 = 0;
  uint8_t data1 = 0;
  bool parity0_valid = false;
  bool parity1_valid = false;
  /// The line it was read on: 21, or for the line next to it
  /// (eia608_other_line_reading()) 22 or 20.
  int line = 21;
};

/// The closed caption observer's reading of field |field_id|, from its
/// "closed_caption" observations; a field it did not read carries nothing.
EIA608FieldReading eia608_field_reading(const IObservationContext& context,
                                        FieldID field_id);

/// Where line 21 of field |field_id| carried no pair, the observer's reading
/// of the line next to it that did — 22, then 20 — from its "other_*"
/// observations; nothing where it read none. Whether that line carries
/// captions is the run's question (EIA608Line21Run).
EIA608FieldReading eia608_other_line_reading(const IObservationContext& context,
                                             FieldID field_id);

/// A byte pair as a receiver takes it, seven-bit bytes
struct EIA608Pair {
  uint8_t data0 = 0;
  uint8_t data1 = 0;
  /// The second byte's parity failed: the first, a character, is taken alone
  /// and data1 is null.
  bool first_only = false;
};

/**
 * @brief What a receiver takes of one field's line 21, by its bytes' parity
 *
 * Each byte carries odd parity over its eight bits [CTA-608-E §4], and a byte
 * whose parity failed is not read as anything. A pair whose first byte failed
 * is dropped whole, since it may have been a control code; one whose second
 * byte failed keeps its first only when that is a character, as a receiver
 * displays the character it did receive. A control code needs both bytes, and
 * its encoder's second copy stands in for a damaged one [CTA-608-E §D.2].
 *
 * The FCC's rule for a receiver shows a solid block for a character that
 * failed, and a block and the second byte for a control code's first byte
 * that failed [47 CFR 15.119(i)(3), (j)(1)], as cc_decoder does (the
 * Unlicense; lib/cc_decode.py, decode_cea608_row()). On the recordings
 * measured that rule put 983 blocks among the pairs the Closed Caption Sink
 * takes: cc_decoder or etv's decoder read a character cleanly at 150 of them
 * and null padding at 53, and neither could read the other 780 — damaged
 * lines, most of them on one tape. So a damaged character is left
 * out rather than shown.
 *
 * @return The pair, or nothing for a line without line 21's waveform or a
 *         pair a receiver would not take.
 */
std::optional<EIA608Pair> eia608_pair_by_parity(
    const EIA608FieldReading& reading);

/**
 * @brief The pair as a receiver on television field |television_field| acts
 *        on it: a byte that has no function there is ignored alone
 *
 * A first byte 00h-0Fh is ignored and the second byte treated normally
 * [47 CFR 15.119(i)(1)] — except on field 2, where 01h-0Fh carries the
 * Extended Data Service [CTA-608-E §8.6]. A second byte 01h-1Fh after a
 * character or null has no function either [47 CFR 15.119(j)] and is ignored,
 * the character kept: a text service can end a row's last
 * character with 01h, and refusing the pair lost the character. Nulls stand
 * for the bytes ignored.
 *
 * @param television_field 0 for field 1, 1 for field 2
 */
EIA608Pair eia608_pair_for_field(int television_field, EIA608Pair pair);

/**
 * @brief Whether a byte pair is one EIA-608 defines for its field
 *
 * Seven-bit bytes, parity removed. A character pair is two characters
 * (20h-7Fh) or nulls; a control pair is a code of CTA-608-E's tables — a
 * preamble address, a mid-row code, a miscellaneous control code, a tab
 * offset, a special or extended character, an attribute (Tables 3, 4, 49 and
 * 51-53 and the extended character sets). First bytes 01h-0Fh are the
 * Extended Data Service's and belong to television field 2 only: "control
 * codes with values 0x01 to 0x0F have no valid purpose on line 21, field 1"
 * [CTA-608-E §D.2]. A receiver acts on nothing else.
 *
 * @param television_field 0 for field 1, 1 for field 2
 */
bool eia608_pair_is_defined(int television_field, uint8_t data0, uint8_t data1);

/**
 * @brief Which of a capture's stored fields is television field 1
 *
 * The first field stored of each frame is normally television field 1, but a
 * capture that starts on field 2 — field reversal, from a time base corrector
 * or a tape machine [CTA-608-E §3.2.2] — stores every frame the other way
 * round, and its CC1 is read where CC3 belongs. Line 21 says which field is
 * which: field 2 spells the miscellaneous control codes 15h and 1Dh where field
 * 1 spells them 14h and 1Ch [CTA-608-E §8.4, §8.5], and only field 2 carries
 * the Extended Data Service's control codes 01h-0Fh [CTA-608-E §D.2].
 *
 * Fed the pairs read with both parities good; reversed when the stored fields
 * show field reversal's signs at least eight times and four times as often as
 * the normal order's.
 *
 * Thread-compatible.
 */
class EIA608FieldOrder {
 public:
  /// A pair of stored field |stored_field| (0 the first of the frame).
  void add(int stored_field, uint8_t data0, uint8_t data1);
  /// The signs another count of the same run found, added to these.
  void absorb(const EIA608FieldOrder& other) {
    normal_ += other.normal_;
    reversed_ += other.reversed_;
  }
  /// Whether the capture's first stored field is television field 2.
  bool reversed() const;

 private:
  // Signs of each order: a field-1 spelling on the first stored field, or a
  // field-2 one (or XDS) on the second, is the normal order's.
  uint64_t normal_ = 0;
  uint64_t reversed_ = 0;
};

/**
 * @brief Line 21's byte pairs over a run, as a receiver takes them
 *
 * Fed each stored field's reading in transmission order, it hands back every
 * pair whose parity a receiver would take (eia608_pair_by_parity), on the
 * television field the capture's field order puts it (EIA608FieldOrder), as
 * a receiver there acts on it (eia608_pair_for_field), marked as defined for
 * that field (eia608_pair_is_defined) and as standing alone or not. A pair
 * standing alone is one whose field carries no line 21 data around it — fewer
 * than three pairs with both parities good within thirty frames either side,
 * its own included. A captioned field carries the clock run-in and a pair on
 * every field, null padding when there is nothing to say [CTA-608-E §8.2], and
 * a decoder enables its display only on a run of valid data [CTA-608-E §C.21]:
 * a line of noise that reads as a pair stands alone.
 *
 * The field order is decided from the whole run, so nothing is handed back
 * until the run has been read: eight bytes are kept for each line that
 * carried a pair whose first byte's parity held, and four more where both
 * parities held.
 *
 * A recording whose time base slipped a line carries its captions on line 22
 * or 20 instead — on line 22 only, or on 20, 21 and 22 by turns. Where line 21
 * carried no pair, the pair read on the line next to it joins the run if, over
 * the run, that line reads as a caption line does: twenty fields or more, three
 * quarters of them with both parities good (cc_decoder's measure, the
 * Unlicense; lib/cc_decode.py, Cea608Lines), and eight control codes among
 * them, which a caption stream carries and a test signal reading as one
 * fixed pair does not — and only where line 21 carried nothing for fifteen
 * frames either side: where the caption line has moved, not where line 21
 * lost a field. Its pairs then count towards the field order and the stream
 * around each pair as line 21's do.
 *
 * Thread-compatible.
 */
class EIA608Line21Run {
 public:
  /// One pair of the run
  struct Pair {
    /// Frames from the run's first
    uint32_t frame_offset = 0;
    /// The stored field it was read from: 0 the first of its frame
    int stored_field = 0;
    /// The television field the field order puts it on: 0 for field 1 (CC1,
    /// CC2, TEXT1, TEXT2), 1 for field 2 (CC3, CC4, TEXT3, TEXT4, XDS)
    int television_field = 0;
    uint8_t data0 = 0;
    uint8_t data1 = 0;
    /// The second byte's parity failed: the first, a character, alone
    bool first_only = false;
    /// A pair EIA-608 defines for its television field
    bool defined = false;
    /// Defined, but its field carries no line 21 data around it
    bool isolated = false;

    /// Taken as data: defined, and not standing alone.
    bool taken() const { return defined && !isolated; }
  };

  /// The reading of stored field |stored_field| (0 or 1) of the run's frame
  /// |frame_offset|; frames in ascending order. |other| is the line next to
  /// line 21 where line 21 carried no pair (eia608_other_line_reading()).
  void add(uint32_t frame_offset, int stored_field,
           const EIA608FieldReading& reading,
           const EIA608FieldReading& other = {});

  /// Lines read from stored field |stored_field|, and of them those that
  /// carried line 21's waveform.
  uint64_t lines(int stored_field) const;
  uint64_t carried(int stored_field) const;

  /// Pairs of stored field |stored_field| taken from line |line| (22 or 20)
  /// where line 21 carried none: zero unless the run shows that line carrying
  /// captions.
  uint64_t taken_from(int stored_field, int line) const;

  /// Whether the run's first stored field is television field 2.
  bool fields_reversed() const { return field_order().reversed(); }

  /// Every pair a receiver would take by its parity, in transmission order.
  void for_each(const std::function<void(const Pair&)>& fn) const;

 private:
  struct ReadPair {
    uint32_t frame_offset = 0;
    uint8_t data0 = 0;
    uint8_t data1 = 0;
    uint8_t stored_field = 0;
    bool first_only = false;
  };

  // A pair read on the line next to line 21, kept apart until the run says
  // whether that line carries captions.
  struct OtherRead {
    ReadPair read;
    uint8_t line = 0;  // 0 for line 22, 1 for line 20
  };
  // What one stored field's line next to line 21 read over the run.
  struct LineTally {
    uint64_t read = 0;   // fields with its waveform
    uint64_t good = 0;   // of them, both parities good
    uint64_t codes = 0;  // of those, control codes (first byte 01h-1Fh)
  };

  // Whether line |line| (0 for 22, 1 for 20) of stored field |field| reads
  // as a caption line over the run.
  bool carries_captions(size_t field, size_t line) const;
  // Whether |other| joins the run: its line carries captions, and line 21
  // carried nothing around it.
  bool takes(const OtherRead& other) const;
  // The field order over line 21 and the lines next to it that carry
  // captions.
  EIA608FieldOrder field_order() const;
  // Whether |frames| — the frames of a stored field's pairs with both
  // parities good, ascending — put line 21 data around |frame|.
  static bool in_stream(const std::vector<uint32_t>& frames, uint32_t frame);

  std::vector<ReadPair> read_;
  std::array<uint64_t, 2> lines_{};
  std::array<uint64_t, 2> carried_{};
  // Per stored field, the frames of its pairs with both parities good.
  std::array<std::vector<uint32_t>, 2> good_frames_;
  EIA608FieldOrder field_order_;
  std::vector<OtherRead> other_read_;
  // Per stored field, the frames line 21 carried its waveform on, ascending.
  std::array<std::vector<uint32_t>, 2> line21_frames_;
  // Per stored field and line next to line 21 (0 for 22, 1 for 20).
  std::array<std::array<LineTally, 2>, 2> other_tally_{};
  std::array<std::array<EIA608FieldOrder, 2>, 2> other_order_{};
};

}  // namespace orc

#endif  // ORC_EIA608_LINE21_PAIRS_H
