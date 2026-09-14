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
  (void)observation_context;

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

    uint64_t frames_processed = 0;
    for (FrameID fid = frame_rng.first; fid <= frame_rng.last; ++fid) {
      if (pCancelRequested_ && pCancelRequested_->load()) {
        vbi_writer->close();
        ORC_LOG_WARN("DaphneVBISink: Export cancelled by user");
        if (pIsProcessing_) pIsProcessing_->store(false);
        return false;
      }

      frames_processed++;
      if (frames_processed % 10 == 0 && progress_callback_) {
        progress_callback_(frames_processed, total_frames,
                           "Exporting frame " +
                               std::to_string(frames_processed) + "/" +
                               std::to_string(total_frames));
      }
    }

    vbi_writer->close();

    ORC_LOG_DEBUG(
        "DaphneVBISink: Wrote VBI header for {} frames (no observer data in "
        "VFrameR)",
        frames_processed);
    return true;

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("Exception during VBI export: {}", e.what());
    return false;
  }
}
}  // namespace orc
