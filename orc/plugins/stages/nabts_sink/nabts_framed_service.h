/*
 * File:        nabts_framed_service.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Datacast services that send their data in counted frames
 *              striped across a family of addresses, under a fixed pad
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NABTS_FRAMED_SERVICE_H
#define ORC_NABTS_FRAMED_SERVICE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "nabts_datacast.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {

// One frame: the sixteen data rows of a bundle that sends one check row
// ahead of them.
constexpr size_t kNabtsFrameRows = 16;
constexpr size_t kNabtsFrameRowBytes = 26;
constexpr size_t kNabtsFrameBytes = kNabtsFrameRows * kNabtsFrameRowBytes;
// The frame's header, and the bytes of its object after it.
constexpr size_t kNabtsFrameHeaderBytes = 19;
constexpr size_t kNabtsFramePayloadBytes =
    kNabtsFrameBytes - kNabtsFrameHeaderBytes;
// The object's own header that opens its first frame, ahead of its name.
constexpr size_t kNabtsFrameObjectHeaderBytes = 14;
// Frames that must name a service, counted close together — none more than
// kNabtsFramedCloseCounts after the one before — for it to be listed as one:
// a service sends its frames in turn, and a name only scattered frames bear
// is a misread one.
constexpr uint64_t kNabtsFramedMinFrames = 16;
constexpr int64_t kNabtsFramedCloseCounts = 16;
// A family's addresses are a block of sixteen. A service's frames are read
// from the block holding most of them, and from any other holding at least
// one part in this many as many: a frame read on another came on an address
// misread, its rows another address's.
constexpr size_t kNabtsFramedBlockShare = 4;
// Frame headers of each service kept for the viewer.
constexpr size_t kNabtsFramedHeadersShown = 12;
// Bytes the services' frames may hold between them, as read and as written
// out in order.
constexpr size_t kNabtsFramedMaxBytes = size_t{64} << 20;
// How far back of the frame read before it a frame's count may fall and
// still be one sent before it: the frames of a family's addresses arrive a
// few out of turn. A count further back starts the count again.
constexpr int64_t kNabtsFramedReorderFrames = 64;
// Frames read of an object, where its first frame was not, for it to be
// listed.
constexpr size_t kNabtsFramedMinObjectFrames = 2;

/**
 * @brief The pad the frames of these services are sent under
 *
 * Each byte of a frame is sent XORed with the pad's byte at its place. The
 * pad is the same for every frame of every service of this kind on the
 * recordings: what a frame of zeros arrives as, which the frames of an object
 * of zeros its service sent over and over show. Bytes 2-3 and 9-12 are left
 * as sent: what they hold under the pad — the frame's check, which neither a
 * CRC nor a sum of the frame fits, and the service's name — nothing shows.
 */
const std::array<uint8_t, kNabtsFrameBytes>& nabts_frame_pad();

/**
 * @brief Whether |frame|, the pad taken off, opens with a frame header:
 *        bytes 0-1 and 4 zero
 */
bool nabts_frame_header(const uint8_t* frame);

/// The framed services a recording carried and the files their frames held.
struct NabtsFramedDatacasts {
  std::vector<NabtsFramedService> services;
  std::vector<NabtsCarriedFile> files;
};

/**
 * @brief The services among |datacasts| that send their data in frames, and
 *        the files those frames hold
 *
 * A service of this kind sends each frame as the sixteen data rows of a
 * bundle, the bundle's check row ahead of them, and spreads its frames over a
 * family of addresses — sixteen of them, 0F0 to 0FF say — taking turns. With
 * the pad taken off (nabts_frame_pad()), a frame opens with its header:
 *
 * - bytes 0-1 and 4: zero;
 * - bytes 2-3: a check of the service's own, kept as sent;
 * - bytes 5-6: the frame's count, least significant first, one more than the
 *   frame sent before it on any of the family's addresses;
 * - bytes 9-12: the service's name, kept as sent;
 * - bytes 7-8: zero, but in the frame that ends its object;
 * - bytes 13-14: the number of the object the frame is part of;
 * - bytes 15-18: where in the object its bytes go, least significant first.
 *
 * The rest of the frame is the object's. An object is a file: its first frame
 * (at 0) opens with the object's own header — two zero bytes, its number
 * again, its time (Unix seconds), its size, and its name as a length and that
 * many bytes — the file's bytes after it; each later frame carries the bytes
 * from where its header says, the one that ends it as many as are left. A
 * frame cut short by filler (RFC 2728 §3.2) is read as far as it goes.
 *
 * In a datacast whose bundles hold a frame's sixteen rows, a frame is looked
 * for only where a bundle starts; a service's frames are taken from its block
 * of addresses (kNabtsFramedBlockShare). A frame is read where its header row
 * arrived and passed its row code; its
 * rows that did not arrive are left out of it, and a row taken as received —
 * failing its code — is used where no copy of it without one arrived. The
 * counts are taken in the order the frames arrived, so a count that wraps is
 * followed and one that falls back further than kNabtsFramedReorderFrames
 * starts a new run; counts missing within a run are frames sent and not read.
 * A frame read twice is read once, row by row from its copies.
 *
 * A service sends an object over and over, and may change it between one
 * pass and the next. A pass runs while the places of the object's frames
 * rise; passes whose frames read alike wherever both hold one are one version
 * of the object, and one that reads differently starts the next. Versions of
 * one of the service's containers (nabts_framed_container()) that, voted
 * together, unpack whole with the container's check holding were one file
 * whose passes a misread split apart, and are one version again. Each version
 * is put together from its frames — the commonest reading of each row of a
 * frame read more than once — as a NabtsCarriedFile ("framed service"),
 * named and dated by its header, or by another version's where its first
 * frame was not read, or by its number where no version's was and at least
 * kNabtsFramedMinObjectFrames other frames were. Files a service sent in
 * numbered parts — NAME.000, NAME.001 and on, of one time, all of them whole
 * — are joined into one, NAME, listed after them. A file in one of the
 * service's containers is listed as the file it holds, unpacked
 * (nabts_framed_container_content()).
 *
 * Pure: no I/O. Bounded by kNabtsFramedMaxBytes for the frames gathered and
 * by kNabtsCarriedBytesMax for the files, in total across every service; a
 * file that would pass the total is left out and those after it still read.
 */
NabtsFramedDatacasts nabts_framed_services(
    const std::vector<NabtsDatacastChannel>& datacasts);

}  // namespace orc

#endif  // ORC_NABTS_FRAMED_SERVICE_H
