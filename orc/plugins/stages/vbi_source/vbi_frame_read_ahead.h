/*
 * File:        vbi_frame_read_ahead.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Decodes stored frames ahead of sequential readers, on as many
 *              decoders as they need
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_VBI_FRAME_READ_AHEAD_H
#define ORC_VBI_FRAME_READ_AHEAD_H

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "vbi_byte_source.h"

namespace orc {

// Opens an independent byte source over the capture being read.  Every
// decoder the read-ahead runs owns one, so no two decoders ever share a stream
// position.  Returns nullptr with an error message when it cannot.
using VBIByteSourceOpener =
    std::function<std::unique_ptr<IVBIByteSource>(std::string& error_message)>;

// Shape of the read-ahead window.
struct VBIReadAheadConfig {
  // Bytes per stored frame, and whole stored frames the capture holds.
  uint64_t frame_bytes = 0;
  uint64_t frame_count = 0;

  // Frames one decoder claims and decodes in order before it claims again.
  //
  // A decoder whose next chunk does not follow its last has to reposition,
  // which on a FLAC capture with no SEEKTABLE is a bisection of the file, so a
  // chunk is sized to make that a small share of the chunk's own decode.
  uint32_t chunk_frames = 64;

  // Frames kept behind the furthest frame asked for.  Readers working several
  // frames at once ask slightly out of order, and a frame behind the window is
  // one the caller has to read for itself.
  uint32_t trailing_frames = 64;

  // Most decoders that may run at once.  They are started as readers are seen
  // to wait on them, so a reader slower than one decoder rarely has more.
  uint32_t max_decoders = 1;

  // Readers one decoder is started for.  However long readers wait, no more
  // decoders run than one for every this many threads that have asked for a
  // frame since the window was placed, rounded up: a wait shows the readers
  // are ahead of the decoding, not that another decoder would bring the frame
  // they wait on any sooner, and each decoder holds a chunk of the window
  // ahead of them.  Zero leaves max_decoders the only bound.
  uint32_t readers_per_decoder = 0;

  // Two frames asked for outside the window no further apart than this are
  // taken for one reader moving through the capture in order, and the window
  // moves to it.  Anything else is a random access, which the caller serves.
  uint32_t sequential_distance = 64;
};

// Chunk size for a frame of |frame_bytes|: about 4 MiB of decoded bytes,
// which on the captures in circulation is 64 to 122 frames.
uint32_t vbi_read_ahead_chunk_frames(uint64_t frame_bytes);

// Decodes a capture's stored frames ahead of the readers that ask for them in
// order, so that a pass over the whole capture never waits on the transport
// and never makes it seek.
//
// Why it exists: a FLAC-wrapped capture decodes strictly forward from a stream
// position, and several readers working through a capture at once ask for
// frames a little out of order.  Serving them from one decoder makes every
// request behind the decoder a seek and a re-decode, and serialises the whole
// pass behind that one decoder whatever the number of readers.  Here the
// decoding happens on the read-ahead's own threads, in order, into a window of
// decoded frames the readers take from, and more decoders are started — each
// with its own byte source, each taking its own chunks of the capture — for as
// long as readers are found waiting on them, up to one for every few readers
// (VBIReadAheadConfig::readers_per_decoder).
//
// The window follows the readers.  It opens where two requests close together
// land outside it, keeps a trailing margin behind the furthest frame asked for
// and decodes a few chunks ahead of it, and releases everything else, so its
// memory is bounded by (max_decoders + 1) chunks plus the margin.  A frame
// leaves it when its reader takes it, and the frames' buffers are recycled
// rather than freed (FramePool), so a pass allocates no more of them than the
// window and its readers have held at once.  A request that is neither in the
// window nor continuing a reader is declined, and the caller reads the frame
// itself: random access — a preview scrubbing, an observer sampling frames
// here and there — keeps the caller's own seek path and never moves the window
// away from a pass it is not part of.
//
// The bytes of a frame are the bytes of the capture whichever decoder
// produced them, so nothing a reader receives depends on how many decoders
// ran or in what order they finished.
//
// Thread safety: fetch() may be called from any number of threads at once.
// stop() and the destructor may be called while readers wait, and wake them.
class VBIFrameReadAhead {
 public:
  // A stored frame's bytes, shared rather than copied between the window and
  // the readers holding them.
  using FrameBytes = std::shared_ptr<const std::vector<uint8_t>>;

  VBIFrameReadAhead(VBIReadAheadConfig config, VBIByteSourceOpener opener);
  ~VBIFrameReadAhead();

  VBIFrameReadAhead(const VBIFrameReadAhead&) = delete;
  VBIFrameReadAhead& operator=(const VBIFrameReadAhead&) = delete;

  // The bytes of stored frame |frame_index|, waiting for them if the window
  // holds the frame but has not decoded it yet.  nullptr when the read-ahead
  // does not have the frame and will not: a random access, a frame a decoder
  // could not read, a frame already handed out, or a read-ahead that has
  // stopped.  The caller then reads the frame itself, which is also how a
  // transport error reaches it with the transport's own message.
  //
  // A frame is handed out once: its reader keeps what it makes of the frame
  // (the stage caches the frame's placement), so the window lets the bytes go
  // with it rather than hold them through the trailing margin.
  FrameBytes fetch(uint64_t frame_index);

  // Stop every decoder and wake every reader; fetch() declines from then on.
  // Idempotent, and called by the destructor.
  void stop();

  // What the read-ahead has done, for diagnostics and tests.
  struct Stats {
    uint64_t served = 0;          // fetches answered from the window
    uint64_t declined = 0;        // fetches left to the caller
    uint64_t window_moves = 0;    // times the window was placed or moved
    uint64_t frames_decoded = 0;  // frames the decoders delivered
    uint32_t decoders = 0;        // decoder threads started
    uint32_t readers = 0;         // most threads that asked of one window
    uint64_t buffers = 0;         // frame buffers allocated
  };
  Stats stats() const;

 private:
  static constexpr uint64_t kNoFrame = UINT64_MAX;

  enum class SlotState : uint8_t { kEmpty, kPending, kReady, kFailed, kServed };

  // One frame of the window.  Slot n holds whichever window frame is n modulo
  // the capacity; |frame| says which, so a decoder finishing a frame the
  // window has since released can tell.
  struct Slot {
    uint64_t frame = kNoFrame;
    SlotState state = SlotState::kEmpty;
    FrameBytes bytes;
  };

  // Everything below runs with mutex_ held.
  bool in_window(uint64_t frame_index) const;
  // The declined request |frame_index| continues, if it continues one: the
  // nearest within sequential_distance of it.
  std::optional<uint64_t> continued_request(uint64_t frame_index) const;
  void note_declined(uint64_t frame_index);
  void move_window(uint64_t frame_index);
  void note_request(uint64_t frame_index);
  void release_before(uint64_t new_base);
  uint64_t claim_limit() const;
  bool claimable() const;
  void start_decoder();
  void add_decoder_if_starved();
  void wake_all_readers();
  // Count the calling thread among the window's readers, once per window.
  void count_reader();
  // Decoders the readers counted so far call for (readers_per_decoder).
  uint32_t decoder_limit() const;

  // A decoder thread: opens its own byte source, then claims and decodes
  // chunks until stopped.
  void run_decoder();

  // Buffers for the decoded frames, shared with the deleters of the frames
  // handed out so that it outlives every one of them.
  class FramePool;

  const VBIReadAheadConfig config_;
  const VBIByteSourceOpener opener_;
  // Slots in the window: the trailing margin plus a chunk per decoder and one
  // more, which is the furthest the decoders are allowed ahead.
  const uint64_t capacity_;
  const std::shared_ptr<FramePool> pool_;
  // Distinguishes this read-ahead from every other in the process, for the
  // reader count (count_reader()).
  const uint64_t instance_;

  mutable std::mutex mutex_;
  // Idle decoders wait here for a chunk to claim.
  std::condition_variable work_available_;
  // Readers wait on the slot of the frame they want, so a frame arriving
  // wakes the reader waiting for it rather than every reader of the pass.
  std::vector<std::condition_variable> slot_ready_;

  std::vector<Slot> slots_;
  bool active_ = false;       // a window has been placed
  bool stopping_ = false;     // stop() was called
  bool disabled_ = false;     // no decoder could open the capture
  bool open_failed_ = false;  // a decoder could not; no more are started
  uint32_t failed_decoders_ = 0;
  uint64_t generation_ = 0;  // bumped whenever the window moves
  uint64_t base_ = 0;        // first frame of the window
  uint64_t next_claim_ = 0;  // first frame no decoder has claimed
  uint64_t furthest_ = 0;    // furthest frame asked for since the window moved

  // The last few requests declined, for recognising a reader.
  std::array<uint64_t, 4> declined_{};
  size_t declined_count_ = 0;
  size_t declined_next_ = 0;

  uint32_t idle_decoders_ = 0;
  uint32_t waiting_readers_ = 0;
  // Threads that have asked for a frame of the window since it was placed.
  uint32_t readers_ = 0;
  std::unordered_set<std::thread::id> reader_ids_;
  // Frames decoded when the last decoder was started: another is started only
  // after a further chunk has been decoded with readers still waiting.
  uint64_t decoded_at_last_start_ = 0;

  std::vector<std::thread> decoders_;
  Stats stats_;
};

}  // namespace orc

#endif  // ORC_VBI_FRAME_READ_AHEAD_H
