/*
 * File:        vbi_export_writer.h
 * Module:      orc-vbi-services
 * Purpose:     Where a VBI data service's export writes its files
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_VBI_EXPORT_WRITER_H
#define ORC_VBI_EXPORT_WRITER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace orc {

class IStageServices;

/// Writes one file of an export: @p path under the export's folder, its parts
/// separated by '/', and @p size bytes from @p data. Answers whether the file
/// was written.
using VbiExportWriter = std::function<bool(const std::string& path,
                                           const uint8_t* data, size_t size)>;

/// What an export wrote.
struct VbiExportTally {
  uint64_t files = 0;
  /// The paths that could not be written.
  std::vector<std::string> failures;
  /// A line per kind of data written, in the order it was written.
  std::vector<std::string> summary;
};

/// Writes one file of an export through @p write, counting it in @p tally or
/// naming it among the failures. Answers whether it was written.
bool vbi_export_put(const VbiExportWriter& write, VbiExportTally& tally,
                    const std::string& path, const uint8_t* data, size_t size);
bool vbi_export_put_text(const VbiExportWriter& write, VbiExportTally& tally,
                         const std::string& path, const std::string& text);

/// What one part of an export — a folder, or a file at its top — holds, as
/// its README.txt says.
struct VbiExportPart {
  const char* name;
  const char* holds;
};

/// Writes an export's README.txt: @p opening, then each of the @p part_count
/// @p parts that @p written names, with what it holds, then what @p tally's
/// summary says the export holds. Nothing where nothing was written.
void vbi_export_readme(const VbiExportWriter& write, VbiExportTally& tally,
                       const std::string& opening,
                       const std::vector<const char*>& written,
                       const VbiExportPart* parts, size_t part_count);

/// Whether @p path stays inside the folder it is relative to: not empty, not
/// absolute, and no part of it "..". The names in an export come from what a
/// recording carried, which a damaged or hostile one can make anything.
bool vbi_export_path_contained(const std::string& path);

/// A writer that puts each file under @p root through the host's buffered
/// file writer, making the folders its path needs first. A path
/// vbi_export_path_contained() refuses is not written.
VbiExportWriter vbi_export_writer(IStageServices* services,
                                  const std::string& root);

}  // namespace orc

#endif  // ORC_VBI_EXPORT_WRITER_H
