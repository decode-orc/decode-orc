/*
 * File:        observation_pass_test.cpp
 * Module:      orc-tests/core/unit/analysis
 * Purpose:     Unit tests for the parallel disc-analysis observer sweep
 *
 * Tests: every frame observed exactly once; parallel and sequential runs agree;
 * fallback to single-threaded when a private worker source cannot be built;
 * cancellation; worker exceptions rethrown on the calling thread; progress and
 * output context touched only by the calling thread.
 * Frame data is synthesised in memory; no I/O is performed.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <observation_pass.h>
#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/field_id.h>
#include <orc/stage/frame_descriptor.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/frame_line_util.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace orc {
namespace tests {

namespace {

constexpr size_t kFrameCount = 64;

size_t pal_frame_sample_count() {
  return frame_line_sample_offset(
      VideoSystem::PAL, static_cast<size_t>(kPalSamplesPerLineNominal),
      static_cast<size_t>(kPalFrameLines));
}

SourceParameters make_pal_params() {
  SourceParameters p{};
  p.system = VideoSystem::PAL;
  p.frame_width_nominal = kPalSamplesPerLineNominal;
  p.frame_height = kPalFrameLines;
  p.blanking_level = kPalBlanking;
  p.white_level = kPalWhite;
  p.active_video_start = kPalActiveVideoStart;
  return p;
}

// Manchester-encode a 24-bit word (MSB set) the way the biphase decoder reads
// it: a 2 us cell per bit, 1 -> low half then high half, 0 -> the reverse.
void write_biphase_word(int16_t* line, size_t width, uint32_t word) {
  const double cell = kPalSampleRate * 2e-6;
  const auto low = static_cast<int16_t>(kPalBlanking);
  const auto high = static_cast<int16_t>(kPalWhite);
  const auto start = static_cast<double>(kPalActiveVideoStart);

  for (size_t i = 0; i < width; ++i) line[i] = low;

  for (int bit = 23; bit >= 0; --bit) {
    const double cell_start = start + (23 - static_cast<double>(bit)) * cell;
    const double mid = cell_start + cell / 2.0;
    const bool is_one = ((word >> bit) & 1u) != 0u;
    for (auto s = static_cast<size_t>(cell_start); s < static_cast<size_t>(mid);
         ++s) {
      if (s < width) line[s] = is_one ? low : high;
    }
    for (auto s = static_cast<size_t>(mid);
         s < static_cast<size_t>(cell_start + cell); ++s) {
      if (s < width) line[s] = is_one ? high : low;
    }
  }
}

// The word carried on VBI line 16 of frame |frame|: distinct per frame, so a
// test can tell which frame's data reached which observation.
uint32_t word_for_frame(FrameID frame) {
  return 0x800000u | (static_cast<uint32_t>(frame) & 0x00FFFFu);
}

std::vector<int16_t> make_frame(FrameID frame) {
  const auto spl = static_cast<size_t>(kPalSamplesPerLineNominal);
  std::vector<int16_t> samples(pal_frame_sample_count(),
                               static_cast<int16_t>(kPalBlanking));
  const size_t f1 = field1_lines(VideoSystem::PAL);
  for (size_t field = 0; field < 2; ++field) {
    const size_t flat_line = (field == 0 ? 0 : f1) + 15;  // VBI line 16
    write_biphase_word(
        samples.data() +
            frame_line_sample_offset(VideoSystem::PAL, spl, flat_line),
        frame_line_sample_count(VideoSystem::PAL, spl, flat_line),
        word_for_frame(frame));
  }
  return samples;
}

// In-memory source over kFrameCount synthesised PAL frames. Each instance owns
// its buffer, mirroring the private-source-per-worker contract.
class SyntheticSource : public VideoFrameRepresentation {
 public:
  explicit SyntheticSource(FrameID throw_on = kNoThrow) : throw_on_(throw_on) {
    frames_.reserve(kFrameCount);
    for (FrameID f = 0; f < kFrameCount; ++f) frames_.push_back(make_frame(f));
  }

  static constexpr FrameID kNoThrow = ~FrameID{0};

  FrameIDRange frame_range() const override { return {0, kFrameCount - 1}; }
  size_t frame_count() const override { return kFrameCount; }
  bool has_frame(FrameID id) const override { return id < kFrameCount; }

  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    if (!has_frame(id)) return std::nullopt;
    FrameDescriptor d;
    d.frame_id = id;
    d.system = VideoSystem::PAL;
    d.height = static_cast<size_t>(kPalFrameLines);
    d.samples_total = frames_[id].size();
    d.samples_per_line_nominal = static_cast<size_t>(kPalSamplesPerLineNominal);
    return d;
  }

  const sample_type* get_frame(FrameID id) const override {
    if (!has_frame(id)) return nullptr;
    if (id == throw_on_) {
      throw std::runtime_error("synthetic source failure");
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      touched_.insert(id);
      reader_threads_.insert(std::this_thread::get_id());
    }
    return frames_[id].data();
  }

  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    return has_frame(id) ? frames_[id] : std::vector<sample_type>{};
  }

  std::optional<SourceParameters> get_video_parameters() const override {
    return make_pal_params();
  }

  std::set<FrameID> touched_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return touched_;
  }

  size_t reader_thread_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reader_threads_.size();
  }

 private:
  std::vector<std::vector<int16_t>> frames_;
  FrameID throw_on_;
  mutable std::mutex mutex_;
  mutable std::set<FrameID> touched_;
  mutable std::set<std::thread::id> reader_threads_;
};

// Records which thread every call arrived on, so a test can assert the pass
// keeps progress reporting on the caller's thread.
class ThreadRecordingProgress : public AnalysisProgress {
 public:
  void setProgress(int) override { record(); }
  void setStatus(const std::string&) override { record(); }
  void setSubStatus(const std::string&) override { record(); }
  void reportPartialResult(const AnalysisResultItem&) override { record(); }

  bool isCancelled() const override {
    record();
    return cancel_after_ > 0 &&
           calls_.load(std::memory_order_relaxed) >= cancel_after_;
  }

  void cancel_after_calls(int n) { cancel_after_ = n; }

  size_t thread_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return threads_.size();
  }
  bool only_thread_is(std::thread::id id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return threads_.size() == 1 && *threads_.begin() == id;
  }

 private:
  void record() const {
    calls_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex_);
    threads_.insert(std::this_thread::get_id());
  }

  mutable std::mutex mutex_;
  mutable std::set<std::thread::id> threads_;
  mutable std::atomic<int> calls_{0};
  int cancel_after_ = 0;
};

ObservationWorkerSourceFactory synthetic_factory(
    std::vector<std::shared_ptr<SyntheticSource>>* created = nullptr) {
  return [created]() {
    ObservationWorkerSource worker;
    auto src = std::make_shared<SyntheticSource>();
    if (created) created->push_back(src);
    worker.source = src;
    worker.owner = src;
    return worker;
  };
}

ObservationPassOptions parallel_options() {
  ObservationPassOptions options;
  options.max_workers = 4;
  options.chunk_frames = 8;  // 8 chunks over kFrameCount, so workers interleave
  return options;
}

// The raw biphase word recorded for a field, or 0 when absent.
int32_t vbi_word(const ObservationContext& ctx, FieldID field_id) {
  const auto value = ctx.get(field_id, "biphase", "vbi_line_16");
  if (!value || !std::holds_alternative<int32_t>(*value)) return 0;
  return std::get<int32_t>(*value);
}

}  // namespace

TEST(ObservationPass, ObservesEveryFrame_WhenRunInParallel) {
  SyntheticSource shared;
  ObservationContext out;
  ThreadRecordingProgress progress;

  const auto outcome =
      run_disc_analysis_observers(shared, shared.frame_range(), out, &progress,
                                  synthetic_factory(), parallel_options());

  EXPECT_FALSE(outcome.cancelled);
  EXPECT_GT(outcome.workers_used, 1u) << "expected a parallel run";

  for (FrameID f = 0; f < kFrameCount; ++f) {
    EXPECT_EQ(vbi_word(out, FieldID(f * 2)),
              static_cast<int32_t>(word_for_frame(f)))
        << "frame " << f << " field 0";
    EXPECT_EQ(vbi_word(out, FieldID(f * 2 + 1)),
              static_cast<int32_t>(word_for_frame(f)))
        << "frame " << f << " field 1";
  }
}

// Chunks are observed in whatever order workers claim them; the merged result
// must not depend on that.
TEST(ObservationPass, ProducesSameObservationsAsSequentialRun) {
  SyntheticSource sequential_source;
  ObservationContext sequential;
  ObservationPassOptions single;
  single.max_workers = 1;
  const auto sequential_outcome = run_disc_analysis_observers(
      sequential_source, sequential_source.frame_range(), sequential, nullptr,
      synthetic_factory(), single);
  EXPECT_EQ(sequential_outcome.workers_used, 1u);

  SyntheticSource parallel_source;
  ObservationContext parallel;
  run_disc_analysis_observers(parallel_source, parallel_source.frame_range(),
                              parallel, nullptr, synthetic_factory(),
                              parallel_options());

  for (FrameID f = 0; f < kFrameCount; ++f) {
    for (uint64_t half = 0; half < 2; ++half) {
      const FieldID fid(f * 2 + half);
      EXPECT_EQ(sequential.get_all_observations(fid),
                parallel.get_all_observations(fid))
          << "field " << fid.value();
    }
  }
}

// Every frame belongs to exactly one chunk, so no frame is read twice and none
// is skipped. Workers read only their own source, never the shared one.
TEST(ObservationPass, ReadsEachFrameOnce_FromWorkerSourcesOnly) {
  SyntheticSource shared;
  std::vector<std::shared_ptr<SyntheticSource>> workers;
  ObservationContext out;

  run_disc_analysis_observers(shared, shared.frame_range(), out, nullptr,
                              synthetic_factory(&workers), parallel_options());

  EXPECT_TRUE(shared.touched_frames().empty())
      << "the shared source must not be read by a parallel run";

  std::set<FrameID> union_of_workers;
  for (const auto& worker : workers) {
    for (FrameID id : worker->touched_frames()) {
      EXPECT_TRUE(union_of_workers.insert(id).second)
          << "frame " << id << " was observed by more than one worker";
    }
    EXPECT_LE(worker->reader_thread_count(), 1u)
        << "a worker source must be read from a single thread";
  }
  EXPECT_EQ(union_of_workers.size(), kFrameCount);
}

TEST(ObservationPass, FallsBackToSequential_WhenWorkerSourceCannotBeBuilt) {
  SyntheticSource shared;
  ObservationContext out;

  // A factory that cannot build a private source stands for a DAG that will
  // not clone; the pass must still produce a complete result.
  const auto outcome = run_disc_analysis_observers(
      shared, shared.frame_range(), out, nullptr,
      [] { return ObservationWorkerSource{}; }, parallel_options());

  EXPECT_EQ(outcome.workers_used, 1u);
  EXPECT_FALSE(outcome.cancelled);
  EXPECT_EQ(shared.touched_frames().size(), kFrameCount)
      << "the sequential path reads the shared source";
  for (FrameID f = 0; f < kFrameCount; ++f) {
    EXPECT_EQ(vbi_word(out, FieldID(f * 2)),
              static_cast<int32_t>(word_for_frame(f)));
  }
}

TEST(ObservationPass, RunsSequentially_WhenNoFactoryIsSupplied) {
  SyntheticSource shared;
  ObservationContext out;

  const auto outcome = run_disc_analysis_observers(
      shared, shared.frame_range(), out, nullptr, nullptr, parallel_options());

  EXPECT_EQ(outcome.workers_used, 1u);
  EXPECT_EQ(shared.touched_frames().size(), kFrameCount);
}

TEST(ObservationPass, ReportsCancelled_WhenProgressIsCancelled) {
  SyntheticSource shared;
  ObservationContext out;
  ThreadRecordingProgress progress;
  progress.cancel_after_calls(1);  // cancel at the first opportunity

  const auto outcome =
      run_disc_analysis_observers(shared, shared.frame_range(), out, &progress,
                                  synthetic_factory(), parallel_options());

  EXPECT_TRUE(outcome.cancelled);
}

// AnalysisProgress carries no thread-safety guarantee, so the pass must only
// ever call it from the thread that invoked the pass.
TEST(ObservationPass, CallsProgressOnlyOnTheCallingThread) {
  SyntheticSource shared;
  ObservationContext out;
  ThreadRecordingProgress progress;

  run_disc_analysis_observers(shared, shared.frame_range(), out, &progress,
                              synthetic_factory(), parallel_options());

  EXPECT_GT(progress.thread_count(), 0u) << "progress was never called";
  EXPECT_TRUE(progress.only_thread_is(std::this_thread::get_id()));
}

// A source that throws mid-sweep must surface on the calling thread, not
// terminate the process from a worker.
TEST(ObservationPass, RethrowsWorkerException_OnTheCallingThread) {
  SyntheticSource shared;
  ObservationContext out;

  const auto throwing_factory = []() {
    ObservationWorkerSource worker;
    auto src = std::make_shared<SyntheticSource>(FrameID{5});
    worker.source = src;
    worker.owner = src;
    return worker;
  };

  EXPECT_THROW(
      run_disc_analysis_observers(shared, shared.frame_range(), out, nullptr,
                                  throwing_factory, parallel_options()),
      std::runtime_error);
}

TEST(ObservationPass, DoesNothing_WhenRangeIsEmpty) {
  SyntheticSource shared;
  ObservationContext out;
  ThreadRecordingProgress progress;

  const auto outcome =
      run_disc_analysis_observers(shared, FrameIDRange{1, 0}, out, &progress,
                                  synthetic_factory(), parallel_options());

  EXPECT_FALSE(outcome.cancelled);
  EXPECT_TRUE(shared.touched_frames().empty());
  EXPECT_EQ(progress.thread_count(), 0u);
}

}  // namespace tests
}  // namespace orc
