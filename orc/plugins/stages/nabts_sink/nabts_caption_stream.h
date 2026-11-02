/*
 * File:        nabts_caption_stream.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     The caption service as it arrived: one entry per caption, each
 *              combined only from its own copies (CEA-516 §7.3.10)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NABTS_CAPTION_STREAM_H
#define ORC_NABTS_CAPTION_STREAM_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "nabts_record.h"
#include "nabts_record_catalogue.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {

/**
 * @brief Collects the caption service a recording carried, caption by caption
 *
 * CEA-516 §7.3.10.1 sends captioning as records at one address that change
 * version every time the caption does ("Each time the caption content is
 * changed, the Version Number shall be changed"), each repeated a few times to
 * aid error recovery (§7.1.3). Y16 has sixteen values (§5.2.7.13), so over a
 * recording of any length every version is used again and again for captions
 * that have nothing in common but the number: a two-hour recording can carry
 * 1,750 captions under sixteen versions. The identity
 * the record catalogue keys on — channel, address and version (§5.2.1) — names
 * a caption only while it is on air.
 *
 * So captions are taken in the order they arrived rather than catalogued. A
 * caption is a run of consecutive arrivals at one {channel, address} that carry
 * one version; the next arrival there with another version starts the next
 * caption. Only the copies of a run are voted together (see
 * nabts_vote_record()), and a caption is dated by the frame its run began at,
 * which is when a receiver acquiring the service would have first shown it.
 *
 * An arrival belongs to the caption service when it reads the Caption Flag of
 * §5.2.7.3 set and its header either arrived undamaged, names a non-cyclic
 * presentation record (§5.2.2.3, "typically used in non-cyclic teletext
 * applications such as captioning"), or comes from an address the caption
 * service is already using. A flag byte is Hamming 8/4 and a burst sets a bit
 * silently, so a page whose header had to be corrected does not leave the
 * catalogue on the strength of one bit alone. The converse damage —
 * a caption whose flag byte was corrected clear — is recognised where it
 * matters: a non-cyclic arrival with a corrected header, at an address the
 * caption service already uses, is taken as a caption too.
 *
 * Memory is bounded. At most kMaxCaptions captions are held, each as its voted
 * data (at most kNabtsMaxGroupBytes, §8.4.2.5) with one mask byte per data
 * byte where a byte was lost, plus the copies of the caption still arriving at
 * each of at most kMaxOpenAddresses addresses (kMaxCopiesPerCaption each).
 * Captions past the bound are counted and not kept, so an export is the first
 * kMaxCaptions of the service rather than a sample of it.
 *
 * Arrivals must be added in transmission order, which is the order the record
 * assembler emits messages in.
 *
 * Thread safety: none; confine an instance to one thread.
 */
class NabtsCaptionStream {
 public:
  /**
   * Captions held. The densest recording this was measured on (about two
   * hours) carries 1,750, of 100 bytes or so each; this is room for about nine
   * times that. The worst case — every caption a full 68-packet group whose
   * every copy lost bytes — is 16,384 x 1,904 bytes of data and as much again
   * of mask, about 62 MB, where a real service's captions of a few lines each
   * come to under 5 MB.
   */
  static constexpr std::size_t kMaxCaptions = 16384;

  /// Copies of one caption held for its vote. §7.1.3 has a non-cyclic record
  /// repeated a few times at most, so this is never reached by a service.
  static constexpr std::size_t kMaxCopiesPerCaption = 16;

  /// Addresses a caption can be arriving at at once — one per caption service
  /// (§7.3.10.1 allows several, e.g. one per language). A misread address
  /// opens one of these too, so the oldest is closed when the bound is met.
  static constexpr std::size_t kMaxOpenAddresses = 16;

  explicit NabtsCaptionStream(std::size_t max_captions = kMaxCaptions,
                              std::size_t max_copies = kMaxCopiesPerCaption);

  /// Whether |message| belongs to the caption service (see the class comment).
  bool accepts(const NabtsMessage& message) const;

  /**
   * @brief Take in one arrival of a caption
   *
   * @param message  Message as joined by NabtsRecordAssembler
   * @param frame_id Frame carrying the last packet that completed it
   */
  void add(const NabtsMessage& message, uint64_t frame_id);

  /**
   * @brief The captions held, in transmission order
   *
   * Ascending by the frame each caption's run began at. A caption whose run is
   * still open is included as it stands. Where no copy of a caption arrived
   * whole, its data is the vote of that caption's own copies and nothing else.
   * The page is left for nabts_interpret_records() to present, as for any
   * record.
   */
  std::vector<NabtsCataloguedRecord> captions() const;

  /// Captions held so far, open ones included.
  std::size_t size() const { return closed_.size() + open_.size(); }

  /// Captions that arrived after kMaxCaptions were held and were not kept.
  uint64_t dropped() const { return dropped_; }

 private:
  /// A caption still arriving: what is known of it so far.
  struct Run {
    NabtsCataloguedRecord record;
    /// Whether the kept copy arrived complete and undamaged.
    bool kept_copy_intact = false;
    /// Damaged copies eligible to vote, oldest first; empty once an undamaged
    /// copy has arrived.
    std::vector<NabtsRecordCopy> copies;
    /// Arrival order of the run's first copy, for closing the oldest.
    uint64_t sequence = 0;
  };

  using Address = std::pair<uint16_t, uint64_t>;

  /// Start a run for |message| at |frame_id|.
  Run open_run(const NabtsMessage& message, uint64_t frame_id);

  /// Fold |message| into |run|, as the catalogue does a copy into its entry.
  void extend_run(Run& run, const NabtsMessage& message,
                  uint64_t frame_id) const;

  /// |run| as a caption: its record with the vote across its copies taken.
  static NabtsCataloguedRecord finish(const Run& run);

  /// Move |run| to the closed captions.
  void close(const Run& run);

  std::size_t max_captions_;
  std::size_t max_copies_;
  std::vector<NabtsCataloguedRecord> closed_;
  std::map<Address, Run> open_;
  /// Every address a caption has arrived at, which is what lets an arrival
  /// whose flag byte was corrected clear be recognised as a caption.
  std::set<Address> caption_addresses_;
  /// The version last refused at each address once the bound was met, so a
  /// refused caption is counted once rather than once per copy.
  std::map<Address, uint8_t> refused_version_;
  uint64_t next_sequence_ = 0;
  uint64_t dropped_ = 0;
};

}  // namespace orc

#endif  // ORC_NABTS_CAPTION_STREAM_H
