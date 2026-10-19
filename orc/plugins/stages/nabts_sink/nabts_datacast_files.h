/*
 * File:        nabts_datacast_files.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     The files datacast services sent, taken out of their payloads
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NABTS_DATACAST_FILES_H
#define ORC_NABTS_DATACAST_FILES_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nabts_datacast.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {

// File transfer messages: 0xFF, a length, a kind and that many bytes. A file
// chunk is kind kNabtsTransferChunkKind of kNabtsTransferChunkLength bytes: a
// four-byte file number (little endian; the Unix time the file was queued), a
// two-byte chunk number, kNabtsTransferChunkData bytes of the file and a
// five-byte trailer.
constexpr uint8_t kNabtsTransferMark = 0xFF;
constexpr uint8_t kNabtsTransferChunkKind = 0x1A;
constexpr uint8_t kNabtsTransferChunkLength = 0x8B;
constexpr size_t kNabtsTransferChunkData = 128;
// Chunks a payload must hold before its framing is believed.
constexpr size_t kNabtsTransferMinChunks = 4;

// A carousel's rows: kNabtsCarouselRowData bytes of the carousel and their
// sum, modulo 256, filling a datacast row.
constexpr size_t kNabtsCarouselRowData = 25;
constexpr size_t kNabtsCarouselRowBytes = kNabtsCarouselRowData + 1;
// Rows a channel must send whole, and the share of them whose sum must hold,
// before it is read as a carousel: a datacast of other data passes the sum
// on one row in 256.
constexpr uint64_t kNabtsCarouselMinRows = 64;
constexpr uint64_t kNabtsCarouselMinSumPercent = 90;
// A file's data in blocks of at most kNabtsCarouselBlockData bytes, each led
// by a header (01 01, the block number and the length, both two bytes little
// endian) and filling kNabtsCarouselBlockRows rows: one bundle of the
// datacast.
constexpr size_t kNabtsCarouselBlockHeader = 6;
constexpr size_t kNabtsCarouselBlockData = 392;
constexpr size_t kNabtsCarouselBlockRows = 16;
// How far past its directory entry a file's first block is looked for, and
// how far from where it should be a later block's header may have moved —
// a lost row the continuity index did not count shifts it by one.
constexpr size_t kNabtsCarouselFirstBlockReach = 1200;
constexpr size_t kNabtsCarouselBlockSlack = 3 * kNabtsCarouselRowData;
// Copies of a block voted on, past which more are not held.
constexpr size_t kNabtsCarouselMaxCopies = 24;

// The share of a file's bytes, read through the carousel's byte table, that
// must be text for the file to be listed as read through it.
constexpr uint64_t kNabtsCarouselTextPercent = 90;
// Whole files of a carousel whose directory sums are tried for the offset its
// keys take from the files' names, and the fewest bytes a file needs for its
// sum to tell one offset from another.
constexpr uint32_t kNabtsCarouselKeyFiles = 16;
constexpr size_t kNabtsCarouselKeyMinBytes = 64;

// Files taken out of one datacast, and the bytes they may hold between them.
constexpr size_t kNabtsCarriedFilesMax = 4096;
constexpr size_t kNabtsCarriedBytesMax = size_t{64} << 20;

/**
 * @brief The carousel byte table's entry for |byte|: the index a byte of a
 *        carousel text file is sent at, or -1 where the recordings never
 *        showed it
 *
 * A byte p of a file under key k is sent as the byte whose entry is
 * (p + k) modulo 256.
 */
int nabts_carousel_byte_index(uint8_t byte);

/**
 * @brief The files a datacast carried, in the framings services sent them in
 *
 * What a datacast means is its service's own, but two framings of files were
 * found on the recordings, and a payload in either yields its files:
 *
 * - A **file transfer**: messages of kNabtsTransferMark, a length, a kind and
 *   that many bytes, the file chunks among them each sent twice, the files
 *   interleaved. A file is its chunks in order from 0 to the last one seen; a
 *   chunk that never arrived is a hole of zeros marked as lost. A message
 *   across a row that went missing is not believed: its bytes after the gap
 *   are another message's.
 *
 * - A **file carousel**: rows of 25 bytes and their sum, whose stream holds a
 *   directory entry for each file — its 8.3 name, its time
 *   (YYYY-MM-DDhh:mm:ss) and its length — and its data in numbered blocks of
 *   a bundle each. The carousel sends every file over and over, so each block
 *   is voted from its copies, byte by byte, over the bytes that arrived; a
 *   missing row is held open in the stream (NabtsDatacastChannel::rows), so a
 *   copy's bytes stay where they belong. The text files of the one carousel
 *   measured are hidden behind a byte substitution keyed by the file's name;
 *   a file whose bytes read as text through it is listed read, any other as
 *   sent.
 *
 * Bounded by kNabtsCarriedFilesMax and kNabtsCarriedBytesMax. Pure: no I/O.
 */
std::vector<NabtsCarriedFile> nabts_datacast_files(
    const NabtsDatacastChannel& datacast);

/**
 * @brief What a file's bytes are, from their own opening: "gif", "png",
 *        "jpeg", "html", "text" or "binary"
 *
 * |present| is NabtsCarriedFile::data_present: the bytes no copy delivered
 * are left out of the share of text; empty where every byte arrived.
 */
std::string nabts_file_format(const std::vector<uint8_t>& data,
                              const std::vector<uint8_t>& present);

/**
 * @brief The text a page of HTML shows
 *
 * The page's tags, styles, scripts and comments dropped, its character
 * references read (&nbsp; as a space), a line ended at each tag that breaks
 * one — paragraph, line break, table row, heading — and runs of blank lines
 * and spaces collapsed. What a reader wants from a memo sent as HTML, not a
 * rendering of it.
 */
std::string nabts_html_text(const std::vector<uint8_t>& html);

}  // namespace orc

#endif  // ORC_NABTS_DATACAST_FILES_H
