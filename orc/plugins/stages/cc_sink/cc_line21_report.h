/*
 * File:        cc_line21_report.h
 * Module:      orc-core
 * Purpose:     What a recording's line 21 carried, for the Closed Caption
 *              Sink's report: services, XDS, ATVEF triggers and field order
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_CORE_CC_LINE21_REPORT_H
#define ORC_CORE_CC_LINE21_REPORT_H

#include <orc/support/eia608_line21_pairs.h>
#include <orc/support/eia608_service_demux.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cc_xds_decoder.h"

namespace orc {

/// The service as the stage's service parameter and the report spell it:
/// CC1-CC4, TEXT1-TEXT4.
const char* cc_service_label(EIA608Service service);

/**
 * @brief Counts the ATVEF Transport A triggers in TEXT2's characters
 *
 * ATVEF 1.1 sends its triggers in line 21's TEXT2 service (Transport A, the
 * EIA-746-A binding): a URL in angle brackets, then attributes in square
 * brackets, the last of them a checksum of four hex digits —
 * "<http://example.com>[name:Example][4A7B]" [CTA-608-E §7.12]. The checksum
 * is RFC 791's, the ones' complement of the ones' complement sum of the
 * trigger's characters as sixteen-bit words, from the "<" to the checksum's
 * own "[".
 *
 * A trigger is counted only when it is well formed — an http:, https: or
 * lid: URL of the characters RFC 1738 allows in one, then attributes — and
 * its checksum, which CTA-608-E §7.12 requires, holds. A well-formed string
 * whose checksum is missing or fails is counted apart, as not a trigger: a
 * text service's noise or a damaged trigger reads like one often enough.
 *
 * Fed TEXT2's characters one at a time; control codes are not characters of a
 * URL and are not fed [CTA-608-E §7.11], except that a text restart abandons
 * a trigger being read [CTA-608-E §7.10].
 */
class CCAtvefTriggerScanner {
 public:
  void put(char character);
  /// A text restart on TEXT2: a trigger being read is abandoned.
  void restart();
  /// End of the service's stream: a trigger still being read is judged.
  void finish();

  /// Well-formed triggers whose checksum holds.
  uint64_t triggers() const { return triggers_; }
  /// Well-formed strings whose checksum is missing or does not hold.
  uint64_t unchecked() const { return unchecked_; }
  /// The first few triggers' text, for the report.
  const std::vector<std::string>& examples() const { return examples_; }

 private:
  enum class State { kIdle, kUrl, kAfterUrl, kAttribute };
  void complete();
  void reset();

  State state_ = State::kIdle;
  std::string trigger_;
  // Where the last attribute's "[" is in trigger_, and its content.
  size_t last_attribute_at_ = 0;
  std::string last_attribute_;
  uint64_t triggers_ = 0;
  uint64_t unchecked_ = 0;
  std::vector<std::string> examples_;
};

/**
 * @brief Tallies everything line 21 carried over a run, whatever service the
 *        export was asked for
 *
 * Per television field: the lines read, those that carried line 21's
 * waveform, the pairs taken (both parities good, a pair EIA-608 defines) and
 * those of them not null padding. Per service, the characters it carried.
 * The XDS packets, summarised. The ATVEF triggers on TEXT2. Bounded in memory
 * however long the run.
 */
class CCLine21Report {
 public:
  CCLine21Report();

  /// |lines| lines of television field |field| read, |carried| of them with
  /// line 21's waveform.
  void add_lines(int field, uint64_t lines, uint64_t carried);
  /// |pairs| pairs of television field |field| read on line |line| (22 or
  /// 20) where line 21 carried none (EIA608Line21Run::taken_from()).
  void add_neighbour_line(int field, int line, uint64_t pairs);
  /// A pair of either field, in the order the fields were transmitted;
  /// |xds| where it belongs to the Extended Data Service (see CCXdsDecoder).
  void add_pair(const EIA608Line21Run::Pair& pair, bool xds);
  void add_xds_packet(const CCXdsPacket& packet);
  void set_xds_counts(uint64_t good, uint64_t bad) {
    xds_good_ = good;
    xds_bad_ = bad;
  }
  /// The capture's stored fields were read the other way round.
  void set_fields_reversed(bool reversed) { fields_reversed_ = reversed; }

  /// The report, a few lines; |frames| is the run's length.
  std::string text(uint64_t frames) const;

  /// Finish the trigger scan, before text().
  void finish() { atvef_.finish(); }

  uint64_t characters(EIA608Service service) const {
    return characters_[static_cast<size_t>(service)];
  }
  const CCAtvefTriggerScanner& atvef() const { return atvef_; }

 private:
  struct FieldCounts {
    uint64_t lines = 0;
    uint64_t carried = 0;
    uint64_t taken = 0;
    uint64_t not_null = 0;
    // Taken by their first character alone, the second byte's parity failed.
    uint64_t first_only = 0;
    // Both parities held, but no pair EIA-608 defines, or a pair standing
    // alone.
    uint64_t undefined = 0;
    uint64_t isolated = 0;
    // Read on line 22 and on line 20, where line 21 carried none.
    uint64_t line22 = 0;
    uint64_t line20 = 0;
  };
  std::array<FieldCounts, 2> fields_{};
  // Every pair taken, routed as a receiver would route it, so each service's
  // characters are counted however the export was set.
  EIA608ServiceDemux demux_;
  std::array<uint64_t, 8> characters_{};
  CCXdsSummary xds_;
  uint64_t xds_good_ = 0;
  uint64_t xds_bad_ = 0;
  CCAtvefTriggerScanner atvef_;
  bool fields_reversed_ = false;
};

}  // namespace orc

#endif  // ORC_CORE_CC_LINE21_REPORT_H
