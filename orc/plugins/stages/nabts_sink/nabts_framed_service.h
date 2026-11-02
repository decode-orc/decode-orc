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
#include <string>
#include <utility>
#include <vector>

#include "nabts_datacast.h"
#include "nabts_datacast_files.h"
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
// A service of fewer frames than kNabtsFramedMinFrames is listed where at
// least this many of them were counted one after another and carry one
// object on: a misread name puts a frame here and there, never two that
// follow each other.
constexpr size_t kNabtsFramedMinFollowingFrames = 2;
// The widest gap in a service's counts whose frames are looked for among the
// bundles not read as its frames, and how many counts either side of a count
// missing the frames are that bracket when the bundle filling it arrived,
// with the frames read nearest it either side: the family's addresses send a
// frame or two out of turn, and a wider reach takes in the rows of frames
// either side that a lost packet moved into a bundle of their own.
constexpr int64_t kNabtsFramedPutBackGap = 16;
constexpr int64_t kNabtsFramedPutBackReach = 2;

/**
 * @brief What one address's datacast held of the framed services: its
 *        bundles, how each read, and the files its frames went into
 *
 * Counted where the datacast is laid out in bundles of a frame's rows, the
 * frames of a datacast laid out otherwise counted by service alone.
 */
struct NabtsFramedChannelUse {
  uint16_t channel = 0;
  /// Bundles of a frame's sixteen rows the datacast held.
  uint64_t bundles = 0;
  /// Frames read for each service listed, by its name.
  std::vector<std::pair<uint32_t, uint64_t>> frames;
  /// Of those, the frames put back from the frames sent around them: their
  /// header row lost, failing its code or misread.
  uint64_t frames_put_back = 0;
  /// Of the frames read, those whose bytes are part of a file listed.
  uint64_t frames_in_files = 0;
  /// Bundles not read as a frame: their header row lost, or failing its code,
  /// with nothing to put it back; their header row opening no frame; frames
  /// of a name no service listed bears (a misread name, or too few frames to
  /// list); and frames of a service read on this address though its block of
  /// addresses is another (a misread address).
  uint64_t header_lost = 0;
  uint64_t header_damaged = 0;
  uint64_t no_header = 0;
  uint64_t unlisted = 0;
  uint64_t other_block = 0;
  /// The files its frames are part of, as indices into
  /// NabtsFramedDatacasts::files, ascending.
  std::vector<size_t> files;
};

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

/// The framed services a recording carried, the files their frames held, and
/// what each address that carried them held.
struct NabtsFramedDatacasts {
  std::vector<NabtsFramedService> services;
  std::vector<NabtsCarriedFile> files;
  /// One an address that carried frames, ascending by address.
  std::vector<NabtsFramedChannelUse> channels;
};

/**
 * @brief What an address's datacast payload is, in words: its bundles and
 *        frames as |use| counts them, and the files of |framed| its frames
 *        went into, a line each
 *
 * The text a datacast's listing gives for its payload
 * (NabtsDatacastListing::carried).
 */
std::string nabts_framed_channel_text(const NabtsFramedChannelUse& use,
                                      const NabtsFramedDatacasts& framed);

/**
 * @brief Every bundle of the addresses |framed| accounts for, in one line:
 *        how many, on how many addresses, read as frames — put back, part of
 *        a file listed — and not read, by why
 *
 * Empty where no address held a bundle of a frame's rows.
 */
std::string nabts_framed_bundles_summary(const NabtsFramedDatacasts& framed);

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
 * A frame read twice is read once, row by row from its copies. A service is
 * listed where enough of its frames were counted close together
 * (kNabtsFramedMinFrames), or where fewer were but at least
 * kNabtsFramedMinFollowingFrames follow each other carrying one object on.
 *
 * A header can be misread and still pass its row code — a row two bytes wrong
 * that the code changed into another codeword — and a header row can be lost
 * or fail its code while the frame's other rows arrive. So a frame whose count
 * is far from both the frames that arrived either side of it, while those two
 * agree, is taken out of the count; and each count missing from a run, in a
 * gap of at most kNabtsFramedPutBackGap, is filled by the one bundle not read
 * as the service's frame — its header row lost or failing its code, a frame
 * so taken out, or a frame of a name no service listed bears, on the
 * service's block of addresses — that arrived between the frames counted
 * within kNabtsFramedPutBackReach of it and the frames read nearest it, where
 * that bundle could fill no other count. The frame's header is put back from
 * the frames counted either side of it — the object they carry on and the place
 * between theirs, or the first place of the object the frame after it carries
 * on — and a frame of a misread name or count must agree with that in its
 * object or its place. Its header row's own bytes of the object are taken as
 * received where the row arrived, and are lost where it did not. A frame
 * counted between two frames of one object whose places are two frames
 * apart, whose own header names another object or place, had them misread
 * with its count right: they are put back too.
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
 * kNabtsFramedMinObjectFrames other frames were. A version whose first frame
 * arrived without the bytes of its header that say where its file begins —
 * the length of its name — is tried with the length the objects numbered
 * either side of it give their names, its file's opening then lost: where it
 * is one of the service's archive containers whose opening can be put back
 * (nabts_framed_container_reopened()), the file it holds is taken, checked.
 * Files a service sent in numbered parts — NAME.000, NAME.001 and on, of one
 * time, all of them whole — are joined into one, NAME, listed after them. A
 * file in one of the service's containers is listed as the file it holds,
 * unpacked (nabts_framed_container_content()). A ZIP archive — a
 * self-extracting one too (nabts_archive_members()) — is listed with its
 * members (NabtsCarriedFile::contents), and each member whose CRC-32 holds is
 * listed after the files as a file of its own named ARCHIVE/MEMBER, once for
 * each content it had, a table among them read as its records; versions of
 * an archive that, voted together, hold more members whose CRC-32 holds than
 * any one of them does were one archive a misread split apart, and are one
 * again. Where an archive's tables are a catalogue of services
 * (nabts_service_catalogue()), its contents open with the catalogue, and each
 * service is told the names it gives the services of its data channel
 * (NabtsFramedService::catalogue), the latest catalogue answering.
 *
 * Each address that carried frames is accounted for (NabtsFramedChannelUse):
 * its bundles, the frames of each service it read and put back, the bundles
 * not read and why, and the files its frames went into.
 *
 * The files' bytes between them are held to |max_file_bytes| as the files
 * are built: an object that would take more than half of what is left (its
 * bytes and the record of which arrived are each its size) is not built, and
 * one that takes more than what is left once unpacked is left out, the rest
 * read either way. A size a misread place gives an object without its header
 * can be tens of megabytes.
 *
 * Pure: no I/O. Bounded by kNabtsFramedMaxBytes and |max_file_bytes|.
 */
NabtsFramedDatacasts nabts_framed_services(
    const std::vector<NabtsDatacastChannel>& datacasts,
    size_t max_file_bytes = kNabtsCarriedBytesMax);

}  // namespace orc

#endif  // ORC_NABTS_FRAMED_SERVICE_H
