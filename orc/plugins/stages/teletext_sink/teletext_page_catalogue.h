/*
 * File:        teletext_page_catalogue.h
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Bounded catalogue of the teletext pages an analysed range
 *              carried, merged from decoded page snapshots
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_PAGE_CATALOGUE_H
#define ORC_TELETEXT_PAGE_CATALOGUE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "vbi-services/teletext_page_decoder.h"
#include "vbi-services/vbi_analysis_results.h"
#include "vbi-services/vbi_identity_attestation.h"

namespace orc {

/**
 * @brief Packets a sub-page's own transmissions lost, from its row copies
 *
 * A row the carousel brought round @c times_seen times should have arrived
 * @c times_seen times. TeletextPageSnapshot::row_copies says how many copies of
 * it were actually combined, and the shortfall is what the recording lost of
 * this page — a sound per-page count, unlike anything derived from the field
 * slots the page occupied: a service inserting on several VBI lines interleaves
 * its magazines, so a field that came back short is short for whichever page
 * that packet belonged to, and which one is not knowable.
 *
 * Rows that never arrived at all are excluded. A row with no copies is either
 * one the service never sent — which most pages do, to space themselves out —
 * or one lost every single time, and nothing here can tell those apart. So this
 * under-reports, which is the right way to be wrong: the figure exists to
 * decide whether a gap on screen is worth blaming on the recording, and a
 * figure that accused pages that arrived whole would be worse than none.
 *
 * Meaningful only where the row squasher was attached: without it the decoder
 * reports one copy per received row whatever the carousel did, and every page
 * seen twice would look half lost. The caller says so by not asking.
 */
uint64_t teletext_subpage_lost_packets(
    const TeletextCataloguedSubPage& subpage);

/**
 * @brief The same for one version of a sub-page: its assembly and how many
 * times that version appeared
 *
 * Rows are counted by TeletextPageSnapshot::row_arrivals, which is not bounded
 * by how many copies the squasher keeps: a page the carousel brought round a
 * hundred times has only kept its latest copies, and against a hundred
 * appearances those would read as a hundred-odd packets lost per row.
 */
uint64_t teletext_version_lost_packets(const TeletextPageSnapshot& page,
                                       uint64_t times_seen);

/**
 * @brief Accumulates decoded page snapshots into a bounded page catalogue
 *
 * A trigger run decodes the whole frame range in one pass, which on a long
 * recording is hundreds of thousands of page transmissions. Catalogueing them
 * — one entry per {magazine, page number, sub-code}, replaced as better
 * assemblies arrive — bounds what the run holds by the number of pages the
 * service carries rather than by the length of the recording.
 *
 * The sub-code is part of the identity because a page number can carry a
 * sequence of sub-pages the service cycles through (ETSI EN 300 706 Annex A.1:
 * sub-pages of a page intended for display are coded Mxx-0001 to Mxx-0079, a
 * page with none is coded Mxx-0000). Keying on the page number alone would
 * leave whichever sub-page the carousel happened to send last, with the rest of
 * the sequence discarded as if it had been a retransmission of the same page.
 *
 * This is the same identity the row squasher keys its copies on — its
 * TeletextPageKey carries the sub-code too — so a catalogue entry holds exactly
 * the copies that were combined into it, and rows of one sub-page are never
 * shown as another's.
 *
 * Snapshots must be merged in ascending temporal order, which is the order the
 * decoder emits them in.
 *
 * Thread safety: none; confine an instance to one thread.
 */
class TeletextPageCatalogue {
 public:
  /**
   * Upper bound on catalogued sub-pages. The cap keeps a multi-service or
   * noisily sliced recording from growing the catalogue without limit (each
   * entry holds a 40x25 page grid, so ~1 KB apiece). When full, the least
   * recently seen sub-page is dropped — with its page, if it was the last one —
   * and the dataset is flagged truncated.
   *
   * A single service can address 800 display pages (8 magazines x 100 BCD page
   * numbers), so this sits above a full carousel with room for sub-pages and
   * for a second service to share the recording, at ~2 MB of grids. It is well
   * under the spec-conformant worst case of 800 x 79 sub-pages, which no real
   * transmission approaches.
   */
  static constexpr std::size_t kMaxCataloguedSubPages = 2048;

  /**
   * Upper bound on the sub-pages held for any one page number, so that a page
   * whose sub-codes churn cannot crowd the rest of the service out of the
   * catalogue. ETSI EN 300 706 Annex A.1 codes the sub-pages of a display page
   * Mxx-0001 to Mxx-0079, which with the no-sub-pages code Mxx-0000 is what
   * this allows.
   */
  static constexpr std::size_t kMaxSubPagesPerPage = 80;

  /**
   * Upper bound on the earlier versions one sub-page keeps
   * (TeletextCataloguedSubPage::earlier_versions); the least recently shown go
   * first. A subtitle page replaces its content with every caption, and a
   * service can rewrite a news page every few minutes; sixteen shows how a page
   * developed without letting one page hold a recording's worth of them.
   */
  static constexpr std::size_t kMaxEarlierVersionsPerSubPage = 16;

  /**
   * Upper bound on earlier versions over the whole catalogue: each holds a
   * page grid (~20 KB), so this keeps them to tens of megabytes. Past it a
   * replaced version is counted as dropped rather than kept.
   */
  static constexpr std::size_t kMaxEarlierVersions = 2048;

  explicit TeletextPageCatalogue(
      std::size_t max_subpages = kMaxCataloguedSubPages,
      std::size_t max_subpages_per_page = kMaxSubPagesPerPage);

  /**
   * @brief Merge one decoded page snapshot
   *
   * @param snapshot Page as rendered by TeletextPageDecoder
   * @param frame_id Frame carrying the snapshot's header packet
   *
   * A snapshot whose transmission is still in progress updates the sub-page's
   * content but does not count as another appearance; nor does a header
   * re-sent part-way through the page's own transmission, which the decoder
   * reports under the appearance's original header field index.
   *
   * Snapshots are filed by content run (see
   * TeletextPageSnapshot::content_run), each run being a version of the
   * sub-page: a run seen for the first time is a new version, and one seen
   * before — a rotating page's screen coming round again — updates the version
   * it already is. Versions beyond the bounds above are dropped, least
   * recently shown first.
   */
  void merge(const TeletextPageSnapshot& snapshot, uint64_t frame_id);

  /**
   * @brief Remove the page numbers the recording never actually named
   *
   * A page is identified by its magazine and page number (ETSI EN 300 706
   * §9.3.1.1) and its sub-code (§9.3.1.2), and every digit of all three is
   * carried by its own Hamming 8/4 byte. §8.2 gives that code minimum distance
   * 4, so a burst of three bit errors — the ordinary error on a band-limited
   * recording rather than the exotic one — resolves silently to a neighbouring
   * codeword. That does not damage the page it happens to: it *duplicates* it,
   * at a number the service never transmitted, and this catalogue then holds
   * the copy beside the original with no way to tell them apart.
   *
   * So the catalogue counts, per sub-page, how often the identity arrived as
   * transmitted (TeletextCataloguedSubPage::times_attested), and this pass acts
   * on it. A sub-page no appearance ever attested is not a page:
   *
   *  - where exactly one attested identity differs from it in a single
   *    hexadecimal digit, it is a misreading of that one, and its appearances
   *    and frame extent are added to it. Its rows cannot be: the row squasher
   *    combined them under the misread page's own key long before the catalogue
   *    saw them, and there is nothing here to merge.
   *  - otherwise it is removed. Several attested neighbours make the misreading
   *    ambiguous — a service carrying both 1/03 and 1/07 is ordinary — and none
   *    at all is a false lock on noise.
   *
   * Withheld entirely on a catalogue where nothing was ever attested, which
   * offers no baseline to judge the rest against (see
   * vbi_identity_reconciliation_applies()). On an undamaged source every byte
   * arrives as transmitted, so the pass finds nothing and costs one walk of the
   * catalogue.
   *
   * Call once, after the last merge() and before pages().
   */
  VbiIdentityReconciliation reconcile_identities();

  /**
   * @brief Remove the versions of a page decoded by transmission that only one
   *        transmission ever showed
   *
   * Read by transmission (TeletextPageSeparation::kTransmission), every
   * transmission of a page can become a version of its own. The service that
   * mode is for, VirText, is a slideshow: every screen of a page comes round
   * again each cycle, so a version no second transmission repeated is not a
   * screen but a transmission no screen could be found in — the rows of a page
   * run together with another page's after a lost header, say, one
   * transmission in thousands. Where some version
   * of the sub-page was shown more than once, the versions shown once are
   * removed; a sub-page every version of which was shown once keeps them all,
   * there being nothing better to show. The sub-page's appearances are left as
   * they are: the header arrived, whatever its rows were.
   *
   * Call once, after the last merge() and before pages().
   *
   * @return Versions removed
   */
  std::size_t drop_unrepeated_versions();

  /// Page numbers catalogued so far
  std::size_t size() const { return pages_.size(); }

  /// Sub-pages catalogued so far, over all page numbers — what the cap bounds
  std::size_t subpage_count() const { return subpage_count_; }

  /// True once a cap has dropped at least one sub-page
  bool truncated() const { return truncated_; }

  /// Sub-pages the caps dropped, and of those the page numbers dropped with
  /// their last sub-page
  uint64_t subpages_dropped() const { return subpages_dropped_; }
  uint64_t pages_dropped() const { return pages_dropped_; }

  /// Catalogue contents, ascending by {magazine, page number}, each page's
  /// sub-pages ascending by sub-code
  std::vector<TeletextCataloguedPage> pages() const;

 private:
  struct VersionState {
    TeletextPageVersion version;
    /// As SubEntry::counted_header_field, for this version's own count
    int64_t counted_header_field = -1;
    /// Monotonic counter of the last update, for the version bounds
    uint64_t last_touched = 0;
  };

  struct SubEntry {
    TeletextCataloguedSubPage subpage;
    /// Header field index of the newest appearance already counted, so a
    /// transmission re-emitted as a fragment is not counted twice
    int64_t counted_header_field = -1;
    /// The sub-page's versions by content run, and the run shown last. The
    /// sub-page's own |page| is filled from the current version by pages().
    std::map<int, VersionState> versions;
    int current_run = 0;
    /// Monotonic counter of the last update, for the sub-page bounds
    uint64_t last_touched = 0;
  };

  struct Entry {
    bool subtitle = false;
    // Keyed by sub-code so iteration is sub-page order: Annex A.1 numbers the
    // sub-pages of a display page sequentially from Mxx-0001, so ascending
    // sub-code is the sequence the service cycles through.
    std::map<int, SubEntry> subpages;
  };

  // The sub-page of |entry| a cap drops first: the least recently touched of
  // those no appearance has attested, failing any such the least recently
  // touched of all. An unattested identity is most likely a misread one (see
  // reconcile_identities()), and dropping it first is what keeps a page whose
  // sub-codes are a clock from losing its real sub-pages to its misreadings.
  // Never called on an empty one.
  static std::map<int, SubEntry>::iterator oldest_subpage(Entry& entry);

  void enforce_subpage_bounds(Entry& page_entry);

  // Drop versions beyond kMaxEarlierVersionsPerSubPage for |sub_entry| and
  // beyond kMaxEarlierVersions over the catalogue, least recently shown first
  // and never the one a sub-page is showing now.
  void enforce_version_bounds(SubEntry& sub_entry);

  std::size_t max_subpages_;
  std::size_t max_subpages_per_page_;
  // Keyed by {displayed magazine, page number} so iteration is page order.
  std::map<std::pair<int, int>, Entry> pages_;
  std::size_t subpage_count_ = 0;
  // Versions held beyond each sub-page's current one, over the catalogue.
  std::size_t earlier_versions_ = 0;
  uint64_t subpages_dropped_ = 0;
  uint64_t pages_dropped_ = 0;
  uint64_t touch_counter_ = 0;
  bool truncated_ = false;
};

}  // namespace orc

#endif  // ORC_TELETEXT_PAGE_CATALOGUE_H
