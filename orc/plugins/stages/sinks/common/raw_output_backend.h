/*
 * File:        raw_output_backend.h
 * Module:      orc-core
 * Purpose:     Raw file output backend (RGB, YUV, Y4M)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef ORC_CORE_RAW_OUTPUT_BACKEND_H
#define ORC_CORE_RAW_OUTPUT_BACKEND_H

#include <fstream>
#include <memory>
#include <ostream>

#include "output_backend.h"
#include "outputwriter.h"

namespace orc {

/**
 * @brief Output backend for raw video files
 *
 * Wraps the existing OutputWriter class to provide raw RGB, YUV, and Y4M
 * output. Maintains backward compatibility with existing output functionality.
 */
class RawOutputBackend : public OutputBackend {
 public:
  RawOutputBackend() = default;
  ~RawOutputBackend() override;

  bool initialize(const Configuration& config) override;
  bool writeFrame(const ::ComponentFrame& frame) override;
  bool finalize() override;
  std::string getFormatInfo() const override;

 private:
  std::unique_ptr<OutputWriter> writer_;
  std::ofstream output_file_;  // owns the file when output_path is a real path
  std::ostream* out_ =
      nullptr;  // output_file_, or stdout when output_path is "-"
  OutputWriter::PixelFormat pixel_format_;
  bool output_y4m_;
  std::string format_string_;
  int frames_written_ = 0;

  // Configuration (stored for getFormatInfo)
  int active_width_ = 0;
  int output_height_ = 0;
};

}  // namespace orc

#endif  // ORC_CORE_RAW_OUTPUT_BACKEND_H
