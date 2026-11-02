/*
 * File:        nabts_data_export.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Everything a NABTS run decoded, written as the files it is
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NABTS_DATA_EXPORT_H
#define ORC_NABTS_DATA_EXPORT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "vbi-services/vbi_analysis_results.h"
#include "vbi-services/vbi_export_writer.h"

namespace orc {

/// Writes one file of an export (VbiExportWriter).
using NabtsExportWriter = VbiExportWriter;

/// What an export wrote (VbiExportTally).
using NabtsExportTally = VbiExportTally;

/// The name a carried file is written under, less any number that keeps two
/// of one name apart: its own name, each character no file system takes as
/// '_', then the time the service dated it ("-20010203T040506") where it did.
/// @ref extension is what its bytes are rather than what its name says, where
/// they say ("gif", "png", "jpg"), else its name's own, else one for its
/// format ("htm", "txt", "bin").
struct NabtsCarriedFileName {
  std::string base;
  std::string extension;
};
NabtsCarriedFileName nabts_carried_file_name(const NabtsCarriedFile& file);

/// The packet addresses that carried @p file: "243", or "0F0-0FF" for a file
/// a framed service spread over a family of them.
std::string nabts_carried_file_addresses(const NabtsCarriedFile& file);

/// A table as tab-separated values: a line of @p columns, then a line per
/// row, a tab or line end inside a value written as a space.
std::string nabts_table_tsv(const std::vector<std::string>& columns,
                            const std::vector<std::vector<std::string>>& rows);

/**
 * @brief A network data service's items, written into a folder of their own
 *
 * A folder per channel and record address, "CFD-CF0/", each item in it as a
 * document (nabts_network_message_document()) named
 * nabts_network_message_file_stem() + ".txt", with its binary heading block as
 * .block.bin and a page's NAPLPS code as .naplps beside it where it has them;
 * then index.tsv, a line per item. Paths are relative to the folder. Answers
 * how many of the items were written.
 */
size_t nabts_export_network_messages(
    const std::vector<NabtsNetworkMessage>& messages,
    const NabtsExportWriter& write, NabtsExportTally& tally);

/**
 * @brief Everything @p dataset holds, each kind of data as the files it is
 *
 * The layout, each folder only where the recording carried that kind:
 *
 *  - README.txt: what each folder below holds, and how much of it there is;
 *  - report.txt: @p report, the run's report, where there is one;
 *  - records/CHN/KEY.rec: each record as its copies voted it, named for the
 *    identity CEA-516 §5.2.1 gives it (nabts_record_key()), a byte no copy
 *    delivered written as zero; and records/index.tsv;
 *  - captions/captions.srt: the caption service as subtitles, read as the
 *    catalogue reads it; and captions/records/NNNN-KEY.rec, each caption's
 *    record in the order the captions arrived;
 *  - datacasts/CHN.bin: each datacast's payload as decoded; CHN.txt the text
 *    it holds, CHN.carried.txt what it carries where the stage reads its
 *    framing, and CHN.tsv a table it holds (an automation playlist's events);
 *    and datacasts/index.tsv;
 *  - files/ADDRESSES/NAME: the files the datacasts carried
 *    (nabts_carried_file_name()), with what the stage read of a file whose
 *    format it knows beside it as NAME.contents.txt; and files/index.tsv;
 *  - frames/FIRST-LAST.ID.frames: each framed service's frames, the pad off,
 *    in the order their count gives, and FIRST-LAST.ID.txt how they read;
 *  - web/: the framed services' files as the web sites they came from
 *    (nabts_web_mirror()), with its index.html and manifest.tsv;
 *  - messages/: the network data services' items
 *    (nabts_export_network_messages());
 *  - private/CHN-typeT.txt: groups of a type the standard reserves, as they
 *    arrived;
 *  - fragments.txt: the runs of bytes recovered that no record could be
 *    placed in (NabtsAnalysisDataset::unplaced_runs).
 *
 * A file that cannot be written does not stop the rest. Pure but for @p write.
 */
NabtsExportTally nabts_export_data(const NabtsAnalysisDataset& dataset,
                                   const std::string& report,
                                   const NabtsExportWriter& write);

}  // namespace orc

#endif  // ORC_NABTS_DATA_EXPORT_H
