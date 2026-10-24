/*
 * File:        nabts_catalogue_view.h
 * Module:      nabts_sink stage plugin
 * Purpose:     The record catalogue as an SDK CatalogueDataset the host can
 *              browse
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_SINK_NABTS_CATALOGUE_VIEW_H
#define ORC_NABTS_SINK_NABTS_CATALOGUE_VIEW_H

#include <orc/stage/tooling/catalogue_results.h>

#include <string>
#include <vector>

#include "naplps_receiver_pace.h"
#include "naplps_render_grid.h"
#include "vbi-services/nabts_page.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {

/**
 * @brief The receiver choices the viewer offers, coarsest first
 *
 * One per NaplpsRenderMode, each keyed by the name the mode is stored under, so
 * an option the reader picks names a mode straight back through
 * naplps_render_mode_from_name().
 */
std::vector<CatalogueViewOption> naplps_view_options();

/**
 * @brief Id of the toggle offering the reader a reconstruction of the bytes
 *        the recording lost (naplps_reconstruct.h)
 *
 * Round-tripped by the host untouched, so it is named once here and compared
 * against rather than spelled out at each end.
 */
inline constexpr const char* kNabtsReconstructToggleId = "reconstruct_lost";

/// The reconstruction toggle, built with |active| the state it is in.
CatalogueViewToggle naplps_reconstruct_toggle(bool active);

/**
 * @brief Id of the toggle that plays each page at the pace a receiver of the
 *        day drew it (NaplpsInterpreter::set_receiver_pace())
 */
inline constexpr const char* kNabtsReceiverPaceToggleId = "receiver_pace";

/// The receiver pace toggle, built with |active| the state it is in.
CatalogueViewToggle naplps_receiver_pace_toggle(bool active);

/**
 * @brief Id of the toggle that plays each page at the Panasonic TU-2000x's
 *        pace rather than the Norpak TTX 650's
 *
 * A second toggle rather than a view option, because the receiver a page is
 * timed by is independent of the one it is drawn for: folded into the view
 * dropdown it would multiply the four drawings out. On by itself it plays the
 * page at the TU-2000x's pace, and with Receiver pace on as well it is the
 * TU-2000x's pace that plays, so every combination of the two means one thing
 * and each box shows what the reader set.
 */
inline constexpr const char* kNabtsTu2000xPaceToggleId =
    "receiver_pace_tu2000x";

/// The TU-2000x pace toggle, built with |active| the state it is in.
CatalogueViewToggle naplps_tu2000x_pace_toggle(bool active);

/// The receiver whose pace the two toggles ask for: the TU-2000x where its
/// own is on, the TTX 650 where only Receiver pace is, and none otherwise.
NaplpsReceiverPace naplps_receiver_pace_for(bool receiver_pace,
                                            bool tu2000x_pace);

/**
 * @brief Build the browsable catalogue the host's viewer reads
 *
 * Each record is an item, and the records of a More chain (§5.2.7.6) are the
 * sub-pages of one page, stepped through rather than listed. A presentation
 * record (types 0, 1 and 3) becomes a display list, an
 * application record (type 2) a function listing, and the caption service — the
 * records CEA-516 §5.2.7.3 flags, read in order — becomes one further item of
 * its own, because reading a caption service one record at a time tells a
 * viewer nothing about it.
 *
 * @p mode is the receiver resolution the display lists are resolved against;
 * nothing else in the catalogue depends on it.
 *
 * @p reconstruct rebuilds the bytes each presentation record lost, from the
 * recording's other records and the dataset's reference records
 * (naplps_reconstruct.h), and says on each page what it did. Like @p mode it
 * changes how the records read rather than what was recovered: the record data
 * the catalogue carries is what arrived, and a run's exported packet stream and
 * record files are untouched either way. Off by default, here and in the
 * viewer, so that a caller who has not asked for a reading of the recording is
 * given the recording.
 *
 * @p receiver_pace plays each page at the pace the Norpak TTX 650 drew it, so
 * a page is seen being drawn and runs as long as it ran on the set, a fill or
 * a clear sweeping across the screen; off, a page moves only at its WAITs
 * (§5.3.2.8). @p tu2000x_pace plays it at the Panasonic TU-2000x's pace
 * instead, whether or not @p receiver_pace is on
 * (naplps_receiver_pace_for()). The two toggles report the states they were
 * given. What is drawn is the same either way.
 */
CatalogueDataset build_nabts_catalogue(
    const NabtsAnalysisDataset& data,
    NaplpsRenderMode mode = NaplpsRenderMode::kReference,
    bool reconstruct = false, bool receiver_pace = false,
    bool tu2000x_pace = false);

/// What the viewer calls the keys a page links to its macros: game pages tell
/// the viewer to "Press Option 1-9" (CatalogueSchema::keypad_label).
inline constexpr const char* kNabtsKeypadLabel = "Option keys";

/**
 * @brief The marks the list gives the pages of a game (CatalogueItem::badges)
 *
 * A page played on the Option keys — one that links keys to macros it defines
 * and never runs itself (X3.110 §5.5, Table D1 item 7), as game pages do —
 * and a page of a quiz played by page number: a page that offers two or more
 * pages each drawn over the page that offers it, and those pages. Both start
 * "Game", so the word finds every game in the viewer's find box and the
 * report.
 */
inline constexpr const char* kNabtsKeypadGameBadge = "Game: keypad";
inline constexpr const char* kNabtsQuizGameBadge = "Game: quiz";

/**
 * @brief A page of the catalogue played on the keys it links
 *
 * The page build_nabts_catalogue() lists as @p item_id — presented as the
 * catalogue presents it, its Support Record and More chain included — and then
 * @p keys pressed on it in order, each a digit "0" to "9" running the macro the
 * page linked to it (NaplpsInterpreter::press_key()). A key that is not a
 * digit is ignored. The payload is the page as it then stands, drawn as the
 * catalogue draws it under the same options, its time starting at the last
 * key. A payload of kind kNone for an id that names no page.
 *
 * Replays the one page rather than the catalogue: the cost is its presentation
 * and the keys, plus a copy of the records and, with @p reconstruct, the
 * rebuild of every record a reconstruction makes.
 */
CataloguePayload build_nabts_played_payload(
    const NabtsAnalysisDataset& data, const std::string& item_id,
    const std::vector<std::string>& keys,
    NaplpsRenderMode mode = NaplpsRenderMode::kReference,
    bool reconstruct = false, bool receiver_pace = false,
    bool tu2000x_pace = false);

/**
 * @brief One NAPLPS record snapshot as a drawable display list
 *
 * @p mode names the receiver the page is drawn against, which the list carries
 * so a renderer can size what X3.110 sizes in physical pixels. A pixel mode
 * deposits the page into that receiver's grid and lists the pixels
 * (naplps_emit_raster_page()). The vector mode lists the figures: it runs the
 * per-character primitives the interpreter emitted into coalesced text runs,
 * resolves colours out of the four-bits-per-gun GRB of X3.110 §5.3.1 and
 * indexes the downloadable glyphs, so the host draws without a NAPLPS decoder.
 * Exposed for the unit tests.
 */
CatalogueDisplayList nabts_page_display_list(
    const NabtsPageSnapshot& snapshot,
    NaplpsRenderMode mode = NaplpsRenderMode::kReference);

}  // namespace orc

#endif  // ORC_NABTS_SINK_NABTS_CATALOGUE_VIEW_H
