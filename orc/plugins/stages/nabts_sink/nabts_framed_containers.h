/*
 * File:        nabts_framed_containers.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     The containers a framed datacast service sends its pages,
 *              pictures and other files in, unpacked
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NABTS_FRAMED_CONTAINERS_H
#define ORC_NABTS_FRAMED_CONTAINERS_H

#include <cstddef>
#include <optional>

#include "vbi-services/vbi_analysis_results.h"

namespace orc {

// The fixed bytes of a file container: between the length of its address and
// the address itself, and after the address up to its stream.
constexpr size_t kNabtsFileContainerHead = 18;
constexpr size_t kNabtsFileContainerTail = 14;
// The most bytes of an archive's path that may have been lost or received as
// no character a path holds: each is read as '_', and the file it names is
// placed by what the rest of its path says (nabts_web_mirror()).
constexpr size_t kNabtsArchivePathUnreadable = 2;
// The most bytes the head of a web server's response a file was cached as
// may take.
constexpr size_t kNabtsResponseHeadMax = 8192;
// The record of an archive container after its path, ahead of its stream,
// and where in it the file's name begins and how long it may be: of one
// opening "WTZY", and of the shorter kind opening "WTZX".
constexpr size_t kNabtsArchiveRecordBytes = 224;
constexpr size_t kNabtsArchiveNameAt = 72;
constexpr size_t kNabtsArchiveNameBytes = 128;
constexpr size_t kNabtsShortArchiveRecordBytes = 160;
constexpr size_t kNabtsShortArchiveNameAt = 8;
constexpr size_t kNabtsShortArchiveNameBytes = 152;

/// Whether |file| is one of a framed service's containers: it opens "WTF1",
/// or "WTZY" or "WTZX" and the length of its path.
bool nabts_framed_container(const NabtsCarriedFile& file);

/**
 * @brief The file a framed service's container holds, unpacked; nothing where
 *        |file| is no container or nothing of it unpacks
 *
 * A framed service (nabts_framed_services()) sends most of its files in one
 * of three containers of its own:
 *
 * - **A file container**, opening "WTF1": its byte 17 the length of the
 *   address the file was fetched from, the address — a URL — after it, then
 *   kNabtsFileContainerTail bytes and a zlib stream (nabts_inflate()), whose
 *   own Adler-32 checks it.
 * - **An archive container**, opening "WTZY": the length of the path the
 *   file was cached under in decimal, ended by a zero — a field of three
 *   bytes, whose last a length of one digit leaves as it was — and the path;
 *   then a record of kNabtsArchiveRecordBytes — the length of the stream
 *   after it, the CRC-32 of the file it holds, and the file's name at
 *   kNabtsArchiveNameAt, ended by a zero — and a stream imploded by PKWARE's
 *   Data Compression Library (nabts_explode()). The path opens with the
 *   folder the service's client cached its files in ("cache\"), whose name
 *   is put right where one of its bytes was received wrong; any other byte of
 *   the path lost or received as no character a path holds, up to
 *   kNabtsArchivePathUnreadable of them, is read as '_', and the file marked
 *   NabtsCarriedFile::source_misread.
 * - **A short archive container**, opening "WTZX": the same, its record
 *   kNabtsShortArchiveRecordBytes long with the file's name at
 *   kNabtsShortArchiveNameAt, and its path one within that folder.
 *
 * The file unpacked is named by the last part of its address, or by its name,
 * its address or path kept as NabtsCarriedFile::source; it keeps the
 * container's addresses, transfer, time and copies. A container that holds a
 * web server's whole response — its status line ("HTTP/"), its headers and
 * an empty line within kNabtsResponseHeadMax bytes, then its body — gives the
 * body as the file, named and placed by the address its "CachedURL" header
 * gives, where it gives one, the head kept as
 * NabtsCarriedFile::response_head. An archive's CRC-32 is
 * its checksum, and checksum_holds whether the bytes unpacked have it. A
 * container broken off — where its bytes went missing, or arrived wrong —
 * gives its file as far as it unpacked, NabtsCarriedFile::cut_short.
 *
 * Pure: no I/O. Bounded by kNabtsCarriedBytesMax.
 */
std::optional<NabtsCarriedFile> nabts_framed_container_content(
    const NabtsCarriedFile& file);

}  // namespace orc

#endif  // ORC_NABTS_FRAMED_CONTAINERS_H
