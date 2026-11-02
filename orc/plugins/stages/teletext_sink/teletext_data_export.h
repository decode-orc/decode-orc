/*
 * File:        teletext_data_export.h
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Everything a teletext run decoded, written as the files it is
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_TELETEXT_DATA_EXPORT_H
#define ORC_TELETEXT_DATA_EXPORT_H

#include <string>

#include "vbi-services/teletext_page_decoder.h"
#include "vbi-services/vbi_analysis_results.h"
#include "vbi-services/vbi_export_writer.h"

namespace orc {

/**
 * @brief A page as a receiver showed it, as text
 *
 * Every row, the header first, a '\n' after each: each cell as the character
 * it shows, in UTF-8 — a G0 character in the set and national option sub-set
 * in force at it (EN 300 706 §15.2, §15.6.2), a Level 1.5 character from a
 * packet X/26 as itself, a block mosaic as the sextant it draws (the Symbols
 * for Legacy Computing block), and a spacing attribute as a space. A row not
 * received is empty, and spaces at the end of a row are dropped.
 */
std::string teletext_page_text(const TeletextPageSnapshot& page);

/**
 * @brief Everything @p dataset holds, each kind of data as the files it is
 *
 * The layout, each part only where the recording carried that kind:
 *
 *  - README.txt: what each part below holds, and how much of it there is;
 *  - report.txt: @p report, the run's report, where there is one;
 *  - pages/MPP/MPP-SSSS.txt: each sub-page as text (teletext_page_text()),
 *    the version on air last; MPP-SSSS.vN.txt each version it replaced, the
 *    oldest first; and pages/index.tsv;
 *  - service-data.txt: what packets 8/30 said, a line each;
 *  - data-lines/TAG.bin: each independent data line's and data page's payload,
 * in the order it arrived, named as the run's own exports name them
 *    (TeletextDataLine::tag); and data-lines/index.tsv.
 *
 * A file that cannot be written does not stop the rest. Pure but for @p write.
 */
VbiExportTally teletext_export_data(const TeletextAnalysisDataset& dataset,
                                    const std::string& report,
                                    const VbiExportWriter& write);

}  // namespace orc

#endif  // ORC_TELETEXT_DATA_EXPORT_H
