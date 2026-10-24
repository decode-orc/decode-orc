/*
 * File:        vbi_frame_read_ahead.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Decodes stored frames ahead of sequential readers, on as many
 *              decoders as they need
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "vbi_frame_read_ahead.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <system_error>
#include <utility>

namespace orc {

namespace {

// Decoded bytes a chunk aims at.  A decoder that has to reposition for its
// next chunk pays a seek — on a FLAC capture with no SEEKTABLE a bisection of
// the file, several reads and frame parses — and 4 MiB is some tens of
// milliseconds of decoding, which that is a small share of.  It is also what a
// transport decodes forward rather than seek over (vbi_transport.cpp), so one
// chunk of a lone decoder costs the same as the reads it replaces.
constexpr uint64_t kChunkTargetBytes = 4ull * 1024 * 1024;

// Bounds on a chunk, so that a capture with enormous or tiny frames still
// gets a window worth having.
constexpr uint32_t kMinChunkFrames = 16;
constexpr uint32_t kMaxChunkFrames = 256;

uint64_t distance_between(uint64_t a, uint64_t b) {
  return a > b ? a - b : b - a;
}

VBIReadAheadConfig sanitised(VBIReadAheadConfig config) {
  config.chunk_frames = std::max(1u, config.chunk_frames);
  config.max_decoders = std::max(1u, config.max_decoders);
  return config;
}

// Numbers for read-ahead instances, never reused, so that a thread's note of
// the window it was last counted in cannot be taken for a note of another
// read-ahead's that happens to occupy the same address.
std::atomic<uint64_t> g_next_instance{1};

// The read-ahead and window the calling thread last asked of, which spares
// VBIFrameReadAhead::count_reader() looking the thread up again on every
// frame it asks for.
struct ReaderNote {
  uint64_t instance = 0;
  uint64_t generation = 0;
};
thread_local ReaderNote t_counted_in;

}  // namespace

// Every frame a decoder fills is a buffer of the same size, taken from here
// and given back when its last holder lets go of it — the window when the
// frame is released, or the reader it was handed to, whichever is later.
//
// The buffers are recycled rather than freed because of where a freed one
// would go: a decoder allocates on its own thread and a reader frees on
// another, so the allocator returns the buffer to the decoder thread's arena,
// which keeps it for that thread to allocate from again.  On a large machine
// each decoder's arena held about 4 MB of freed frames that way at the peak of
// a pass, on top of the window's own (VmRSS sampled from /proc).
// Recycled, the buffers are allocated once, as many as the window and its
// readers have held at once, and reused for the rest of the pass.
//
// Thread safety: take() and the buffers' deleters may run on any thread.
class VBIFrameReadAhead::FramePool
    : public std::enable_shared_from_this<VBIFrameReadAhead::FramePool> {
 public:
  // |frame_bytes| per buffer; at most |max_kept| buffers are kept for reuse,
  // which is the most the window and its decoders hold at once.  A buffer
  // given back beyond that is freed.
  FramePool(size_t frame_bytes, size_t max_kept)
      : frame_bytes_(frame_bytes), max_kept_(max_kept) {
    // Reserved up front so that giving a buffer back never allocates: it
    // happens in a shared_ptr deleter, which must not throw.
    free_.reserve(max_kept_);
  }

  FramePool(const FramePool&) = delete;
  FramePool& operator=(const FramePool&) = delete;

  // A buffer of frame_bytes bytes, holding whatever the frame that last used
  // it left there: the caller writes every byte before anyone reads one.
  // Throws std::bad_alloc as the allocation it replaces did.
  std::shared_ptr<std::vector<uint8_t>> take() {
    std::unique_ptr<std::vector<uint8_t>> buffer;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (!free_.empty()) {
        buffer = std::move(free_.back());
        free_.pop_back();
      }
    }
    if (buffer == nullptr) {
      buffer = std::make_unique<std::vector<uint8_t>>(frame_bytes_);
      allocated_.fetch_add(1, std::memory_order_relaxed);
    }
    // Should the control block not be had, the shared_ptr constructor hands
    // the buffer to the deleter, which puts it back.
    return std::shared_ptr<std::vector<uint8_t>>(
        buffer.release(),
        [pool = shared_from_this()](std::vector<uint8_t>* returned) {
          pool->give_back(returned);
        });
  }

  // Buffers allocated so far.
  uint64_t allocated() const {
    return allocated_.load(std::memory_order_relaxed);
  }

 private:
  void give_back(std::vector<uint8_t>* returned) noexcept {
    std::unique_ptr<std::vector<uint8_t>> buffer(returned);
    const std::lock_guard<std::mutex> lock(mutex_);
    if (free_.size() < max_kept_) {
      free_.push_back(std::move(buffer));  // within the reserved capacity
    }
  }

  const size_t frame_bytes_;
  const size_t max_kept_;
  std::atomic<uint64_t> allocated_{0};
  std::mutex mutex_;
  std::vector<std::unique_ptr<std::vector<uint8_t>>> free_;
};

uint32_t vbi_read_ahead_chunk_frames(uint64_t frame_bytes) {
  if (frame_bytes == 0) {
    return kMinChunkFrames;
  }
  const uint64_t frames = (kChunkTargetBytes + frame_bytes - 1) / frame_bytes;
  return static_cast<uint32_t>(
      std::clamp<uint64_t>(frames, kMinChunkFrames, kMaxChunkFrames));
}

VBIFrameReadAhead::VBIFrameReadAhead(VBIReadAheadConfig config,
                                     VBIByteSourceOpener opener)
    : config_(sanitised(config)),
      opener_(std::move(opener)),
      capacity_(static_cast<uint64_t>(config_.trailing_frames) +
                (static_cast<uint64_t>(config_.max_decoders) + 1u) *
                    config_.chunk_frames),
      // The window's slots and a frame in the hands of each decoder.
      pool_(std::make_shared<FramePool>(
          static_cast<size_t>(config_.frame_bytes),
          static_cast<size_t>(capacity_) + config_.max_decoders)),
      instance_(g_next_instance.fetch_add(1)),
      slot_ready_(static_cast<size_t>(capacity_)),
      slots_(static_cast<size_t>(capacity_)) {
  // The handles' one allocation, made before any thread exists.
  decoders_.reserve(config_.max_decoders);
}

VBIFrameReadAhead::~VBIFrameReadAhead() { stop(); }

void VBIFrameReadAhead::stop() {
  std::vector<std::thread> decoders;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    decoders.swap(decoders_);
    wake_all_readers();
  }
  work_available_.notify_all();
  for (std::thread& decoder : decoders) {
    if (decoder.joinable()) {
      decoder.join();
    }
  }
}

VBIFrameReadAhead::Stats VBIFrameReadAhead::stats() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  Stats out = stats_;
  out.buffers = pool_->allocated();
  return out;
}

VBIFrameReadAhead::FrameBytes VBIFrameReadAhead::fetch(uint64_t frame_index) {
  std::unique_lock<std::mutex> lock(mutex_);

  if (stopping_ || disabled_ || frame_index >= config_.frame_count) {
    ++stats_.declined;
    return nullptr;
  }

  if (!in_window(frame_index)) {
    // A request just behind the window is a reader straggling behind the
    // pass the window serves.  It is served by the caller and not counted
    // towards finding a reader, so stragglers never pull the window back.
    const bool straggler = active_ && frame_index < base_ &&
                           base_ - frame_index <= config_.sequential_distance;
    const std::optional<uint64_t> earlier =
        straggler ? std::nullopt : continued_request(frame_index);
    if (!earlier.has_value()) {
      if (!straggler) {
        note_declined(frame_index);
      }
      ++stats_.declined;
      return nullptr;
    }
    // Opened at the lower of the two requests: a reader working several
    // frames at once asks for them out of order, and the frames between the
    // two it was recognised by are the ones it asks for next.
    move_window(std::min(frame_index, *earlier));
  }

  note_request(frame_index);
  count_reader();
  if (!in_window(frame_index)) {
    // Another reader has carried the window more than the trailing margin
    // past this frame.
    ++stats_.declined;
    return nullptr;
  }

  const uint64_t generation = generation_;
  const size_t slot_index = static_cast<size_t>(frame_index % capacity_);
  for (;;) {
    Slot& slot = slots_[slot_index];
    if (slot.frame == frame_index) {
      if (slot.state == SlotState::kReady) {
        // Handed over rather than shared, so the frame's buffer goes back to
        // the pool as soon as the reader is done with it rather than when the
        // window releases the frame: the trailing margin is then the frames
        // still to be asked for, not 128 frames already placed (7.5 MB on a
        // .tbc VBI crop).
        ++stats_.served;
        slot.state = SlotState::kServed;
        return std::move(slot.bytes);
      }
      if (slot.state == SlotState::kFailed ||
          slot.state == SlotState::kServed) {
        ++stats_.declined;
        return nullptr;
      }
    }
    if (stopping_ || disabled_ || generation_ != generation ||
        !in_window(frame_index)) {
      ++stats_.declined;
      return nullptr;
    }

    ++waiting_readers_;
    add_decoder_if_starved();
    slot_ready_[slot_index].wait(lock);
    --waiting_readers_;
  }
}

bool VBIFrameReadAhead::in_window(uint64_t frame_index) const {
  return active_ && frame_index >= base_ && frame_index - base_ < capacity_ &&
         frame_index < config_.frame_count;
}

std::optional<uint64_t> VBIFrameReadAhead::continued_request(
    uint64_t frame_index) const {
  std::optional<uint64_t> nearest;
  for (size_t index = 0; index < declined_count_; ++index) {
    const uint64_t distance = distance_between(declined_[index], frame_index);
    if (distance <= config_.sequential_distance &&
        (!nearest.has_value() ||
         distance < distance_between(*nearest, frame_index))) {
      nearest = declined_[index];
    }
  }
  return nearest;
}

void VBIFrameReadAhead::note_declined(uint64_t frame_index) {
  declined_[declined_next_] = frame_index;
  declined_next_ = (declined_next_ + 1) % declined_.size();
  declined_count_ = std::min(declined_count_ + 1, declined_.size());
}

void VBIFrameReadAhead::move_window(uint64_t frame_index) {
  ++generation_;
  ++stats_.window_moves;
  readers_ = 0;
  reader_ids_.clear();
  active_ = true;
  base_ = frame_index;
  next_claim_ = frame_index;
  furthest_ = frame_index;
  for (Slot& slot : slots_) {
    slot = Slot{};
  }
  // The reader has been found; what it was declined for is history.
  declined_count_ = 0;
  declined_next_ = 0;

  // Readers waiting on the window's old position give up and read for
  // themselves, and the decoders start on the new one.
  wake_all_readers();
  if (decoders_.empty()) {
    start_decoder();
  }
  work_available_.notify_all();
}

void VBIFrameReadAhead::note_request(uint64_t frame_index) {
  if (frame_index <= furthest_) {
    return;
  }
  furthest_ = frame_index;
  const uint64_t trailing = config_.trailing_frames;
  const uint64_t new_base = furthest_ > trailing ? furthest_ - trailing : 0u;
  if (new_base > base_) {
    release_before(new_base);
  }
  // The claim limit follows the furthest request, so a whole chunk may just
  // have opened up.  One decoder is enough to take it; it wakes another if
  // there is more.
  if (claimable()) {
    work_available_.notify_one();
  }
}

void VBIFrameReadAhead::release_before(uint64_t new_base) {
  const uint64_t end = std::min(new_base, base_ + capacity_);
  for (uint64_t frame_index = base_; frame_index < end; ++frame_index) {
    const size_t slot_index = static_cast<size_t>(frame_index % capacity_);
    Slot& slot = slots_[slot_index];
    if (slot.frame == frame_index) {
      slot = Slot{};
    }
    // A reader may be waiting on this frame whether or not a decoder has
    // claimed it: readers that outrun the decoders wait on frames nobody has
    // claimed yet, and the window can pass such a frame before anybody does.
    // The claim pointer then jumps past it, so no decoder will ever signal
    // its slot again; the reader has to be woken here, to read the frame
    // itself, or it waits for ever.
    slot_ready_[slot_index].notify_all();
  }
  base_ = new_base;
  next_claim_ = std::max(next_claim_, base_);
}

uint64_t VBIFrameReadAhead::claim_limit() const {
  // Decoders run no further ahead of the furthest reader than one chunk more
  // than there are decoders, so the window's memory grows with the decoding a
  // pass actually needs rather than with its capacity.
  const uint64_t ahead =
      (static_cast<uint64_t>(stats_.decoders) + 1u) * config_.chunk_frames;
  const uint64_t limit = std::min(base_ + capacity_, furthest_ + 1u + ahead);
  return std::min(limit, config_.frame_count);
}

bool VBIFrameReadAhead::claimable() const {
  if (!active_) {
    return false;
  }
  const uint64_t limit = claim_limit();
  if (next_claim_ >= limit) {
    return false;
  }
  // Whole chunks only, except at the end of the capture: a decoder taking
  // whatever sliver of the window had just opened would reposition every few
  // frames.
  return limit - next_claim_ >= config_.chunk_frames ||
         limit == config_.frame_count;
}

void VBIFrameReadAhead::start_decoder() {
  if (stopping_ || disabled_ || open_failed_ ||
      decoders_.size() >= config_.max_decoders) {
    return;
  }
  try {
    decoders_.emplace_back(&VBIFrameReadAhead::run_decoder, this);
  } catch (const std::system_error&) {
    // No thread to be had.  The decoders already running carry on; with none
    // at all nothing would ever decode, so the read-ahead stands aside.
    if (decoders_.empty()) {
      disabled_ = true;
      wake_all_readers();
    }
    return;
  }
  ++stats_.decoders;
  decoded_at_last_start_ = stats_.frames_decoded;
}

void VBIFrameReadAhead::count_reader() {
  if (t_counted_in.instance == instance_ &&
      t_counted_in.generation == generation_) {
    return;
  }
  t_counted_in = ReaderNote{instance_, generation_};
  // A thread reading two captures in turn comes back here for every frame,
  // and is still one reader of each.
  if (!reader_ids_.insert(std::this_thread::get_id()).second) {
    return;
  }
  ++readers_;
  stats_.readers = std::max(stats_.readers, readers_);
}

uint32_t VBIFrameReadAhead::decoder_limit() const {
  if (config_.readers_per_decoder == 0) {
    return config_.max_decoders;
  }
  const uint32_t wanted = (readers_ + config_.readers_per_decoder - 1u) /
                          config_.readers_per_decoder;
  return std::clamp(wanted, 1u, config_.max_decoders);
}

void VBIFrameReadAhead::add_decoder_if_starved() {
  // A reader is waiting.  That calls for another decoder only if every
  // decoder is busy — an idle one means the wait is on a frame already being
  // decoded, not on a shortage of decoders — and only once a further chunk
  // has been decoded since the last one started, so a pass that starts with a
  // crowd of readers all waiting on the first chunk grows its decoders a
  // chunk at a time rather than all at once.  Nor beyond one decoder for every
  // readers_per_decoder readers: a crowd of readers all waiting at the front
  // of the window are waiting on the frames being decoded there, which no
  // further decoder brings any sooner.
  if (idle_decoders_ > 0 || decoders_.size() >= decoder_limit() ||
      stats_.frames_decoded < decoded_at_last_start_ + config_.chunk_frames) {
    return;
  }
  start_decoder();
}

void VBIFrameReadAhead::wake_all_readers() {
  for (std::condition_variable& ready : slot_ready_) {
    ready.notify_all();
  }
}

void VBIFrameReadAhead::run_decoder() {
  std::string error_message;
  std::unique_ptr<IVBIByteSource> source;
  try {
    source = opener_ ? opener_(error_message) : nullptr;
  } catch (const std::exception&) {
    source = nullptr;
  }
  if (source != nullptr) {
    // This decoder skips over the chunks the others take, and decoding
    // through them would be decoding them twice.
    source->set_forward_decode_limit(0);
  }

  std::unique_lock<std::mutex> lock(mutex_);
  if (source == nullptr) {
    // The capture could not be opened a second time.  No further decoder is
    // tried, and if none is left running the read-ahead stands aside and
    // every reader reads for itself.
    open_failed_ = true;
    ++failed_decoders_;
    if (failed_decoders_ >= stats_.decoders) {
      disabled_ = true;
      wake_all_readers();
    }
    return;
  }

  for (;;) {
    ++idle_decoders_;
    work_available_.wait(
        lock, [this] { return stopping_ || disabled_ || claimable(); });
    --idle_decoders_;
    if (stopping_ || disabled_) {
      return;
    }

    const uint64_t generation = generation_;
    const uint64_t first = next_claim_;
    const uint64_t count =
        std::min<uint64_t>(config_.chunk_frames, claim_limit() - first);
    next_claim_ = first + count;
    for (uint64_t frame_index = first; frame_index < first + count;
         ++frame_index) {
      Slot& slot = slots_[static_cast<size_t>(frame_index % capacity_)];
      slot.frame = frame_index;
      slot.state = SlotState::kPending;
      slot.bytes.reset();
    }
    // More of the window may be claimable than this chunk took.
    if (claimable()) {
      work_available_.notify_one();
    }

    for (uint64_t frame_index = first; frame_index < first + count;
         ++frame_index) {
      if (frame_index < base_) {
        continue;  // Released unread: every reader has moved past it.
      }

      lock.unlock();
      std::shared_ptr<std::vector<uint8_t>> bytes;
      bool read_ok = false;
      try {
        // A recycled buffer still holds an earlier frame; a read that does not
        // fill it fails the frame, so none of that is ever served.
        bytes = pool_->take();
        std::string read_error;
        const size_t produced =
            source->read_at(frame_index * config_.frame_bytes, bytes->size(),
                            bytes->data(), read_error);
        read_ok = read_error.empty() && produced == bytes->size();
      } catch (const std::exception&) {
        read_ok = false;
      }
      lock.lock();

      if (stopping_) {
        return;
      }
      if (generation_ != generation) {
        break;  // The window moved; the rest of the chunk is no longer wanted.
      }
      const size_t slot_index = static_cast<size_t>(frame_index % capacity_);
      Slot& slot = slots_[slot_index];
      if (slot.frame == frame_index && slot.state == SlotState::kPending) {
        // A frame that could not be read is marked so, and whoever wants it
        // reads it through their own path, which reports the transport's
        // error in its own words.
        slot.state = read_ok ? SlotState::kReady : SlotState::kFailed;
        if (read_ok) {
          slot.bytes = std::move(bytes);
          ++stats_.frames_decoded;
        }
        slot_ready_[slot_index].notify_all();
      }
    }
  }
}

}  // namespace orc
