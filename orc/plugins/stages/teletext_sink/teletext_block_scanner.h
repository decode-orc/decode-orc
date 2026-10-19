/*
 * File:        teletext_block_scanner.h
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Block-parallel frame scanning for VBI teletext recovery
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_BLOCK_SCANNER_H
#define ORC_TELETEXT_BLOCK_SCANNER_H

#include <orc/stage/common_types.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "teletext_frame_slicer.h"
#include "teletext_scan_state.h"

namespace orc {

// Fields per frame. Named because it is the stride of the block buffer as well
// as the loop bound, and the two must not drift apart.
constexpr size_t kFieldsPerFrame = 2;

// Frames sliced per block, first and last. Blocks exist because the pass
// learns as it goes (see TeletextScanSnapshot): every frame of a block is
// sliced against one frozen view of what is known, and the block is then
// emitted in order, which is what advances it.
//
// So the size trades learning latency against everything else. Early blocks
// are small because that is when there is most to learn — a recording's data
// phase is pinned within the first few dozen frames rather than the first few
// hundred — and they grow because a large block amortises the per-block
// hand-off and leaves the workers less often idle at its tail. The ceiling is
// a memory bound: a block holds every line it sliced, at about 1,6 kB each,
// so 128 frames of a 625-line window is roughly 7 MB, and two are held at
// once, the one being emitted and the one being sliced.
//
// The sizes are part of what the pass recovers, not only of how fast: every
// frame of a block is sliced against what the pass knew when the block
// started, so changing them changes the output.
constexpr uint64_t kFirstScanBlockFrames = 16;
constexpr uint64_t kMaxScanBlockFrames = 128;

// Threads to slice with: |requested|, or one per hardware thread when that is
// 0. Never more than there are frames to slice, and never fewer than one —
// which runs everything on the calling thread and starts none.
size_t resolve_worker_count(uint64_t frame_count, int32_t requested);

/**
 * @brief Slices blocks of frames on threads kept for the whole pass
 *
 * Recovering a line reads that line and nothing else, so the fields of a block
 * are independent and are handed out to the pool's threads as they come free.
 * VideoFrameRepresentation's const accessors are documented safe to call
 * concurrently, which is what lets them all read one representation.
 *
 * A block is started and finished separately so the caller can do other work
 * while it is sliced. The pass uses that to emit block k while block k+1 is
 * sliced: emission is ordered and stays on one thread, and until it overlapped
 * the slicing every worker stood idle while it ran. What block k+1 is sliced
 * against — the snapshot — is fixed when it is started, so nothing it yields
 * depends on how far emission of block k had got.
 *
 * |out| is indexed by frame offset within the block times kFieldsPerFrame plus
 * the field index, so the caller can walk it back in the strict temporal order
 * emission needs. Each frame owns its own elements, so no two threads touch
 * the same one and no locking is needed on the hot path.
 *
 * Thread safety: start() and finish() are called alternately from one thread,
 * which must not touch |out| in between. The threads are started once, in the
 * constructor, rather than per block.
 */
class TeletextBlockSlicer {
 public:
  // |worker_count| counts the calling thread, which takes its share of a block
  // in finish(): one starts no threads at all and slices everything there.
  explicit TeletextBlockSlicer(size_t worker_count);

  // Abandons a block still in progress — its workers stop at the frame they
  // are on — and joins the threads.
  ~TeletextBlockSlicer();

  TeletextBlockSlicer(const TeletextBlockSlicer&) = delete;
  TeletextBlockSlicer& operator=(const TeletextBlockSlicer&) = delete;

  // Hand a block to the pool's threads and return at once. Everything passed
  // by reference must outlive the matching finish(); |snapshot| is copied.
  void start(const VideoFrameRepresentation& representation,
             const TeletextFrameSlicer& slicer, FrameID first_frame,
             uint64_t first_frame_index, uint64_t frame_count,
             const TeletextScanSnapshot& snapshot,
             const std::atomic<bool>* cancel_requested,
             std::vector<TeletextFieldScan>& out);

  // Slice what is left of the block on the calling thread, wait for the
  // pool's threads to finish theirs, and throw if any of them failed — an
  // exception escaping a std::thread would otherwise take the process with it.
  void finish();

 private:
  struct Block {
    const VideoFrameRepresentation* representation = nullptr;
    const TeletextFrameSlicer* slicer = nullptr;
    FrameID first_frame = 0;
    uint64_t first_frame_index = 0;
    uint64_t frame_count = 0;
    TeletextScanSnapshot snapshot;
    const std::atomic<bool>* cancel_requested = nullptr;
    std::vector<TeletextFieldScan>* out = nullptr;
  };

  void run_worker();
  // Take frames of the current block until none are left.
  void slice_frames();

  std::mutex mutex_;
  std::condition_variable block_started_;
  std::condition_variable block_finished_;
  Block block_;
  uint64_t block_generation_ = 0;
  size_t workers_busy_ = 0;
  bool stopping_ = false;
  std::string failure_;
  std::atomic<uint64_t> next_frame_{0};
  std::vector<std::thread> threads_;
};

/**
 * @brief Slice one block of frames, in parallel
 *
 * One block on |worker_count| threads, start to finish: TeletextBlockSlicer
 * for a caller with a single block to slice.
 *
 * Throws if a worker did, once they have all been joined.
 */
void slice_block(const VideoFrameRepresentation& representation,
                 const TeletextFrameSlicer& slicer, FrameID first_frame,
                 uint64_t first_frame_index, uint64_t frame_count,
                 const TeletextScanSnapshot& snapshot, size_t worker_count,
                 const std::atomic<bool>* cancel_requested,
                 std::vector<TeletextFieldScan>& out);

}  // namespace orc

#endif  // ORC_TELETEXT_BLOCK_SCANNER_H
