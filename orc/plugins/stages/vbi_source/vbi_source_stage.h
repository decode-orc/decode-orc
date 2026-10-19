/*
 * File:        vbi_source_stage.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Raw VBI capture source stage: places VBI records into CVBS
 * frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_SOURCE_STAGE_H
#define ORC_VBI_SOURCE_STAGE_H

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/video_frame_representation.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "vbi_byte_source.h"
#include "vbi_source_format.h"

namespace orc {

// Dependency injection interface for the VBI source stage.
//
// A raw VBI capture has no sidecars: the whole of the stage's external I/O is
// opening one file and reading bytes out of it, so this interface is the byte
// transport and nothing else.  Unit tests substitute an in-memory byte source
// and never touch the filesystem.
class IVBISourceStageDeps {
 public:
  virtual ~IVBISourceStageDeps() = default;

  // Validate that input_path exists and is a readable regular file.
  virtual bool validate_input_file(const std::string& input_path,
                                   std::string& error_message) const = 0;

  // Open the capture for reading, transparently unwrapping FLAC and Matroska.
  // Returns nullptr with an error message when it cannot be opened.
  virtual std::unique_ptr<IVBIByteSource> open_byte_source(
      const std::string& input_path, std::string& error_message) const = 0;
};

// Raw VBI capture source stage.
//
// Third-party VBI captures — bt8x8 and family card dumps, cropped VBI-only
// .tbc files — hold nothing but the teletext lines.  This stage reads those
// line records and lays them onto CVBS frames at the timing point and the
// amplitude the standard puts them at, so that the existing teletext decoders
// see them exactly as they see them on a native decode (design §1).
//
// The rest of each frame is blanking.  A capture carries no sync, no vertical
// interval and no burst, and nothing that reads this stage's output looks for
// them: the teletext slicer locks to the clock run-in within the line it is
// handed.  Manufacturing a whole television signal around the data would cost
// far more than placing the data does and would be spent entirely on samples
// nobody reads.
//
// Nothing is written to disk: the stage's only product is the in-memory
// CVBS_U10_4FSC representation.  Connect the CVBS sink to export it.
//
// Frames are built lazily, one at a time, on the frame a consumer asks for.
// A PAL frame is 1,4 MB against the 64 KiB of VBI records it comes from, so a
// four-hour capture is 24 GB of records and 522 GB of frames; materialising it
// is not an option (design §5.7).
//
// The user's whole choice is one named preset per capture format, and the list
// is filtered to the project's own television system: a PAL project is offered
// one and an NTSC project two.  A preset is a complete configuration — the
// container geometry, the data service the capture carries, the level policy,
// the capture offset and the field order — because every one of those is a
// property of the format rather than something a user could be expected to
// know.  There is no "custom" preset: a format nobody has measured cannot be
// described by guessing at its fields, and one that has been measured is a
// single data entry in the preset table.
//
// The data service is part of the preset rather than an axis of its own,
// because nothing in a capture records which of the two 525-line services it
// carries — they share their lines and their bit rate and differ in framing
// code and packet length — and because what it decides is less than its name
// suggests.  It supplies the data region the record is clipped to, the pattern
// the capture offset is fitted against, the windows the logic levels are read
// from, and the amplitude those levels map onto.  On a card capture all four
// matter.  On a TBC-derived capture the record is copied index for index and
// its levels are absolute, so only the first survives: the service decides how
// much of the line is copied and nothing else.  It is carried onto the output
// either way, so a decoder is told rather than left to assume — and whether the
// host can decode what was placed is the slicer's business, not this stage's.
class VBISourceStage : public DAGStage,
                       public ParameterizedStage,
                       public IStagePreviewCapability {
 public:
  explicit VBISourceStage(std::shared_ptr<IVBISourceStageDeps> deps = nullptr);
  ~VBISourceStage() override = default;

  void set_deps_override(std::shared_ptr<IVBISourceStageDeps> deps) {
    deps_ = std::move(deps);
  }

  // DAGStage interface
  std::string version() const override { return "1.0.0"; }
  ORC_STAGE_INSTRUCTIONS_MD

  NodeTypeInfo get_node_type_info() const override;

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& inputs,
      const std::map<std::string, ParameterValue>& parameters,
      ObservationContext& observation_context) override;

  size_t required_input_count() const override { return 0; }
  size_t output_count() const override { return 1; }

  // ParameterizedStage interface
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem project_format, SourceType source_type) const override;
  using ParameterizedStage::get_parameter_descriptors;

  std::map<std::string, ParameterValue> get_parameters() const override;
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override;

  // IStagePreviewCapability
  StagePreviewCapability get_preview_capability() const override;

 private:
  // Every configured value the stage holds, in the spelling the project file
  // and the parameter surface use.
  //
  // There are three, because a preset is a complete configuration: the
  // container geometry, the data service the capture carries, and the level,
  // offset and field-order policies that follow from both are all properties
  // of the format rather than choices the user has to make.  What is left is
  // the file, which format it is, and one policy about dropped frames.
  struct Configuration {
    std::string input_path;
    // A freshly constructed stage is a 625-line one until told otherwise; the
    // descriptor's own default replaces this with the project's system's
    // format as soon as one is known.
    std::string format_preset = "bt8x8 card dump, 8-bit (WST)";
    std::string drops = "preserve";

    // Identity of the configuration, for the frame cache and the artifact ID.
    std::string cache_key() const;
  };

  // The container descriptor a configuration names.  Returns false with an
  // error message for a preset that does not exist.
  static bool make_source_format(const Configuration& configuration,
                                 VBISourceFormat& out_format,
                                 std::string& error_message);

  // Read the stage's configuration out of an execute() parameter map, falling
  // back to the value held by the stage for anything the map omits.
  Configuration configuration_from(
      const std::map<std::string, ParameterValue>& parameters) const;

  Configuration configuration_;

  mutable std::mutex execute_mutex_;
  std::string cached_key_;
  ArtifactPtr cached_representation_;

  std::shared_ptr<IVBISourceStageDeps> deps_;
};

}  // namespace orc

#endif  // ORC_VBI_SOURCE_STAGE_H
