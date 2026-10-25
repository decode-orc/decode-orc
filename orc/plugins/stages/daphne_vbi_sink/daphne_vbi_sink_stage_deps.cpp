/*
 * File:        daphne_vbi_sink_stage_deps.cpp
 * Module:      orc-core
 * Purpose:     Generate .VBI binary files, dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Matt Ownby
 */

#include "daphne_vbi_sink_stage_deps.h"

#include <orc/plugin/orc_stage_services.h>
#include <orc/stage/file_io_interface.h>
#include <orc/support/logging.h>
#include <orc/support/pipe_io.h>

#include <cstddef>
#include <memory>
#include <utility>

#include "daphne_vbi_writer_util.h"

namespace orc {
void DaphneVBISinkStageDeps::init(TriggerProgressCallback progress_callback,
                                  std::atomic<bool>* pIsProcessing,
                                  std::atomic<bool>* pCancelRequested) {
  progress_callback_ = std::move(progress_callback);
  pIsProcessing_ = pIsProcessing;
  pCancelRequested_ = pCancelRequested;
}

bool DaphneVBISinkStageDeps::write_vbi(
    const VideoFrameRepresentation* representation, const std::string& vbi_path,
    IObservationContext& observation_context) {
  std::string final_vbi_path = vbi_path;
  if (!orc::pipe_io::is_pipe_path(vbi_path)) {
    const std::string tbc_ext = ".vbi";
    if (vbi_path.length() < tbc_ext.length() ||
        vbi_path.compare(vbi_path.length() - tbc_ext.length(), tbc_ext.length(),
                         tbc_ext) != 0) {
      final_vbi_path += ".vbi";
      ORC_LOG_DEBUG("Added .vbi extension: {}", final_vbi_path);
    }
  }

  // The observers below only report what they decoded, not whether the frame
  // existed, so this sink has no get_frame()-style signal it could use to
  // notice a piped/unbounded source's real end. Refuse cleanly rather than
  // spin through the source's huge placeholder frame_range() writing empty
  // records.
  if (representation->has_unbounded_frame_range()) {
    ORC_LOG_ERROR(
        "DaphneVBISink: input source has an unbounded frame range (a "
        "piped/live source left frame_count at 0) — this sink cannot "
        "detect the real end of such a source; set an explicit frame_count "
        "on it first.");
    return false;
  }

  const auto frame_rng = representation->frame_range();
  const uint64_t total_frames = frame_rng.count();

  if (progress_callback_) {
    progress_callback_(0, total_frames, "Preparing VBI export...");
  }

  try {
    ORC_LOG_DEBUG("Opening VBI file for writing: {}", final_vbi_path);

    std::shared_ptr<IFileWriter<uint8_t>> vbi_writer;
    if (stage_services_) {
      class FileWriter8Adapter final : public IFileWriter<uint8_t> {
       public:
        explicit FileWriter8Adapter(std::shared_ptr<IFileWriterUint8> impl)
            : impl_(std::move(impl)) {}

        using IFileWriter<uint8_t>::open;
        bool open(const std::string& filepath,
                  std::ios::openmode mode [[maybe_unused]]) override {
          path_ = filepath;
          return impl_ && impl_->open(filepath);
        }

        void write(const uint8_t* data, size_t count) override {
          if (impl_) impl_->write(data, count);
        }
        void write(const std::vector<uint8_t>& data) override {
          if (impl_) impl_->write(data);
        }
        void flush() override {
          if (impl_) impl_->flush();
        }
        void close() override {
          if (impl_) impl_->close();
        }
        uint64_t bytes_written() const override { return 0; }
        bool is_open() const override { return true; }
        const std::string& filepath() const override { return path_; }

       private:
        std::shared_ptr<IFileWriterUint8> impl_;
        std::string path_;
      };

      auto writer8 = stage_services_->create_buffered_file_writer_uint8(
          static_cast<size_t>(1 * 1024 * 1024));
      if (writer8) {
        vbi_writer = std::make_shared<FileWriter8Adapter>(writer8);
      }
    }
    if (!vbi_writer) {
      ORC_LOG_ERROR("Failed to create VBI writer service");
      return false;
    }
    if (!vbi_writer->open(final_vbi_path)) {
      ORC_LOG_ERROR("Failed to open VBI file for writing: {}", final_vbi_path);
      return false;
    }

    std::shared_ptr<DaphneVBIWriterUtil> daphne_vbi_writer_util =
        std::make_shared<DaphneVBIWriterUtil>();
    daphne_vbi_writer_util->init(vbi_writer.get());
    daphne_vbi_writer_util->write_header();

    // Biphase supplies lines 16/17/18; white flag is NTSC-only and writes
    // nothing for other systems, which the record reports as no flag.
    std::unique_ptr<IObserverHandle> biphase_observer;
    std::unique_ptr<IObserverHandle> white_flag_observer;
    if (observation_service_) {
      biphase_observer = observation_service_->create_observer("biphase");
      white_flag_observer = observation_service_->create_observer("white_flag");
    }
    if (!biphase_observer) {
      ORC_LOG_WARN(
          "DaphneVBISink: observation service unavailable; VBI records will "
          "carry no decoded lines");
    }

    uint64_t frames_processed = 0;
    for (FrameID fid = frame_rng.first; fid <= frame_rng.last; ++fid) {
      if (pCancelRequested_ && pCancelRequested_->load()) {
        vbi_writer->close();
        ORC_LOG_WARN("DaphneVBISink: Export cancelled by user");
        if (pIsProcessing_) pIsProcessing_->store(false);
        return false;
      }

      // Daphne addresses records by field number, so a frame the input
      // cannot supply still gets its two records to keep later fields
      // aligned.
      if (representation->has_frame(fid)) {
        if (biphase_observer) {
          biphase_observer->process_frame(*representation, fid,
                                          observation_context);
        }
        if (white_flag_observer) {
          white_flag_observer->process_frame(*representation, fid,
                                             observation_context);
        }
      }
      // Observers key each field as FieldID(frame_id * 2 + field_index).
      daphne_vbi_writer_util->write_observations(FieldID(fid * 2),
                                                 observation_context);
      daphne_vbi_writer_util->write_observations(FieldID(fid * 2 + 1),
                                                 observation_context);

      frames_processed++;
      if (frames_processed % 10 == 0 && progress_callback_) {
        progress_callback_(frames_processed, total_frames,
                           "Exporting frame " +
                               std::to_string(frames_processed) + "/" +
                               std::to_string(total_frames));
      }
    }

    vbi_writer->close();

    ORC_LOG_DEBUG("DaphneVBISink: Wrote {} VBI records for {} frames",
                  frames_processed * 2, frames_processed);
    return true;

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("Exception during VBI export: {}", e.what());
    return false;
  }
}
}  // namespace orc
