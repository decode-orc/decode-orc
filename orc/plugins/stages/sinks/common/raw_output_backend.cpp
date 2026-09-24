/*
 * File:        raw_output_backend.cpp
 * Module:      orc-core
 * Purpose:     Raw file output backend implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "raw_output_backend.h"

#include <orc/support/logging.h>
#include <orc/support/pipe_io.h>

#include "componentframe.h"

namespace orc {

RawOutputBackend::~RawOutputBackend() {
  if (output_file_.is_open()) {
    output_file_.close();
  }
}

bool RawOutputBackend::initialize(const Configuration& config) {
  // Determine pixel format and Y4M flag from format string in options
  auto it = config.options.find("format");
  if (it == config.options.end()) {
    ORC_LOG_ERROR("RawOutputBackend: No format specified in options");
    return false;
  }

  format_string_ = it->second;

  if (format_string_ == "rgb") {
    pixel_format_ = OutputWriter::RGB48;
    output_y4m_ = false;
  } else if (format_string_ == "yuv") {
    pixel_format_ = OutputWriter::YUV444P16;
    output_y4m_ = false;
  } else if (format_string_ == "y4m") {
    pixel_format_ = OutputWriter::YUV444P16;
    output_y4m_ = true;
  } else {
    ORC_LOG_ERROR("RawOutputBackend: Unknown format '{}'", format_string_);
    return false;
  }

  // Open output file, or redirect to stdout for the "-" convention (see
  // orc/support/pipe_io.h). A real named pipe on disk still goes through
  // ofstream::open() below like any other path — only the literal "-"
  // token needs the stdout redirect.
  if (config.output_path == orc::pipe_io::kStdioPathToken) {
    out_ = &orc::pipe_io::stdout_binary_stream();
  } else {
    output_file_.open(config.output_path, std::ios::binary);
    if (!output_file_.is_open()) {
      ORC_LOG_ERROR("RawOutputBackend: Failed to open output file: {}",
                    config.output_path);
      return false;
    }
    out_ = &output_file_;
  }

  // Create and configure OutputWriter
  writer_ = std::make_unique<OutputWriter>();
  OutputWriter::Configuration writer_config;
  writer_config.paddingAmount = config.padding_amount;
  writer_config.pixelFormat = pixel_format_;
  writer_config.outputY4m = output_y4m_;

  // Update configuration (may modify video params for padding)
  orc::SourceParameters mutable_params = config.video_params;
  writer_->updateConfiguration(mutable_params, writer_config);

  // Store dimensions for reporting
  active_width_ =
      mutable_params.active_video_end - mutable_params.active_video_start;
  int active_height = mutable_params.last_active_frame_line -
                      mutable_params.first_active_frame_line;
  output_height_ = active_height;  // Will include padding

  // Write stream header if needed
  std::string stream_header = writer_->getStreamHeader();
  if (!stream_header.empty()) {
    out_->write(stream_header.data(),
                static_cast<std::streamsize>(stream_header.size()));
    if (!out_->good()) {
      ORC_LOG_ERROR("RawOutputBackend: Failed to write stream header");
      return false;
    }
  }

  ORC_LOG_DEBUG("RawOutputBackend: Initialized {} output to {}", format_string_,
                config.output_path);
  writer_->printOutputInfo();

  return true;
}

bool RawOutputBackend::writeFrame(const ::ComponentFrame& frame) {
  if (!writer_ || !out_) {
    ORC_LOG_ERROR("RawOutputBackend: Not initialized");
    return false;
  }

  // Write frame header if needed
  std::string frame_header = writer_->getFrameHeader();
  if (!frame_header.empty()) {
    out_->write(frame_header.data(),
                static_cast<std::streamsize>(frame_header.size()));
    if (!out_->good()) {
      ORC_LOG_ERROR("RawOutputBackend: Failed to write frame header");
      return false;
    }
  }

  // Convert frame to output format
  OutputFrame output_frame;
  writer_->convert(frame, output_frame);

  // Write output data
  const char* data = reinterpret_cast<const char*>(output_frame.data());
  std::streamsize size =
      static_cast<std::streamsize>(output_frame.size() * sizeof(uint16_t));
  out_->write(data, size);

  if (!out_->good()) {
    ORC_LOG_ERROR("RawOutputBackend: Failed to write frame data");
    return false;
  }

  frames_written_++;
  return true;
}

bool RawOutputBackend::finalize() {
  if (output_file_.is_open()) {
    output_file_.close();
  }
  if (out_) {
    ORC_LOG_DEBUG("RawOutputBackend: Wrote {} frames", frames_written_);
    out_ = nullptr;
  }

  return true;
}

std::string RawOutputBackend::getFormatInfo() const {
  std::string pixel_name;
  switch (pixel_format_) {
    case OutputWriter::RGB48:
      pixel_name = "RGB48";
      break;
    case OutputWriter::YUV444P16:
      pixel_name = "YUV444P16";
      break;
    case OutputWriter::GRAY16:
      pixel_name = "GRAY16";
      break;
    default:
      pixel_name = "unknown";
      break;
  }

  if (output_y4m_) {
    return "Y4M (" + pixel_name + ")";
  } else {
    return pixel_name;
  }
}

}  // namespace orc
