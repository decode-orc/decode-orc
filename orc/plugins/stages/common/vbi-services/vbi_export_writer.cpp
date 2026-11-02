/*
 * File:        vbi_export_writer.cpp
 * Module:      orc-vbi-services
 * Purpose:     Where a VBI data service's export writes its files
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "vbi_export_writer.h"

#include <fmt/format.h>
#include <orc/plugin/orc_stage_services.h>
#include <orc/support/logging.h>

#include <filesystem>
#include <memory>
#include <string_view>
#include <system_error>

namespace orc {
namespace {

// What the host's writer buffers before it writes: an export is a run of
// files from a few bytes to tens of megabytes, written once each.
constexpr size_t kExportBufferBytes = 1U << 20U;

}  // namespace

bool vbi_export_put(const VbiExportWriter& write, VbiExportTally& tally,
                    const std::string& path, const uint8_t* data, size_t size) {
  if (write(path, data, size)) {
    ++tally.files;
    return true;
  }
  tally.failures.push_back(path);
  return false;
}

bool vbi_export_put_text(const VbiExportWriter& write, VbiExportTally& tally,
                         const std::string& path, const std::string& text) {
  return vbi_export_put(write, tally, path,
                        reinterpret_cast<const uint8_t*>(text.data()),
                        text.size());
}

void vbi_export_readme(const VbiExportWriter& write, VbiExportTally& tally,
                       const std::string& opening,
                       const std::vector<const char*>& written,
                       const VbiExportPart* parts, size_t part_count) {
  if (written.empty()) {
    return;
  }
  std::string readme = opening;
  for (const char* name : written) {
    for (const VbiExportPart* known = parts; known != parts + part_count;
         ++known) {
      if (std::string_view(known->name) == name) {
        readme += fmt::format("{}\n  {}\n\n", known->name, known->holds);
      }
    }
  }
  readme += "This export holds:\n";
  for (const std::string& line : tally.summary) {
    readme += "  - " + line + "\n";
  }
  vbi_export_put_text(write, tally, "README.txt", readme);
}

bool vbi_export_path_contained(const std::string& path) {
  if (path.empty() || path.front() == '/' || path.front() == '\\') {
    return false;
  }
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find_first_of("/\\", start);
    if (end == std::string::npos) {
      end = path.size();
    }
    const std::string part = path.substr(start, end - start);
    if (part == ".." || (start == 0 && part.size() == 2 && part[1] == ':')) {
      return false;  // climbs out, or names a drive
    }
    start = end + 1;
  }
  return true;
}

VbiExportWriter vbi_export_writer(IStageServices* services,
                                  const std::string& root) {
  return [services, root](const std::string& path, const uint8_t* data,
                          size_t size) {
    if (services == nullptr || !vbi_export_path_contained(path)) {
      ORC_LOG_WARN("Export: not writing {}: it is not inside the export", path);
      return false;
    }
    const std::filesystem::path file = std::filesystem::path(root) / path;
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error) {
      ORC_LOG_WARN("Export: could not make the folder for {}: {}",
                   file.string(), error.message());
      return false;
    }
    const std::shared_ptr<IFileWriterUint8> writer =
        services->create_buffered_file_writer_uint8(kExportBufferBytes);
    if (!writer || !writer->open(file.string())) {
      ORC_LOG_WARN("Export: could not write {}", file.string());
      return false;
    }
    writer->write(data, size);
    writer->close();
    return true;
  };
}

}  // namespace orc
