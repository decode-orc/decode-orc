/*
 * File:        tbc_sink_stage_deps.cpp
 * Module:      orc-core
 * Purpose:     TBC Sink Stage dependencies implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "tbc_sink_stage_deps.h"

#include <orc/plugin/orc_stage_services.h>
#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/file_io_interface.h>
#include <orc/stage/observation/colour_frame_phase_query.h>
#include <orc/support/dropout_util.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>
#include <orc/support/pal_line_locked_resample.h>
#include <orc/support/pipe_io.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tbc_sink_levels.h"
#include "tbc_sink_pcm_writer.h"
#include "tbc_sink_sidecars.h"

namespace orc {

void TBCSinkStageDeps::init(TriggerProgressCallback progress_callback,
                            std::atomic<bool>* pIsProcessing,
                            std::atomic<bool>* pCancelRequested) {
  progress_callback_ = std::move(progress_callback);
  pIsProcessing_ = pIsProcessing;
  pCancelRequested_ = pCancelRequested;
}

namespace {

// PAL: the frame is the 4fsc lattice, the TBC file is line-locked, so the run
// is mapped back to the line-locked samples that cover it in each field it
// touches (the inverse of the TBC source's lattice resample) and then split
// at the 1135-sample TBC line boundaries.
void split_pal_dropout_run(const DropoutRun& run,
                           std::vector<DropoutInfo>& tbc_f1_dropouts,
                           std::vector<DropoutInfo>& tbc_f2_dropouts) {
  const int64_t run_first = static_cast<int64_t>(run.sample_start);
  const int64_t run_last =
      run_first + static_cast<int64_t>(run.sample_count) - 1;
  for (int field = 1; field <= 2; ++field) {
    const auto field_start =
        static_cast<int64_t>(pal_line_locked::lattice_field_start(field));
    const auto field_last =
        field_start +
        static_cast<int64_t>(pal_line_locked::lattice_field_samples(field)) - 1;
    const int64_t first = std::max(run_first, field_start);
    const int64_t last = std::min(run_last, field_last);
    if (last < first) continue;

    const auto span = pal_line_locked::line_locked_span(
        field, first - field_start, last - field_start);
    auto& out = (field == 1) ? tbc_f1_dropouts : tbc_f2_dropouts;
    constexpr int64_t kLine = kPalSamplesPerLineNominal;
    for (int64_t m = span.first; m <= span.last;) {
      const int64_t line = m / kLine;
      const int64_t line_end = std::min(span.last + 1, (line + 1) * kLine);
      DropoutInfo di;
      di.line = static_cast<uint32_t>(line);
      di.start_sample = static_cast<uint32_t>(m - line * kLine);
      di.end_sample = static_cast<uint32_t>(line_end - line * kLine);
      out.push_back(di);
      m = line_end;
    }
  }
}

// Split a DropoutRun (frame-flat coordinates) into per-field DropoutInfo
// entries and append them to the appropriate output vectors.
// tbc_f1_dropouts ← entries that belong to TBC field 1 (is_first_field=true)
// tbc_f2_dropouts ← entries that belong to TBC field 2
void split_dropout_run(VideoSystem sys, const DropoutRun& run,
                       std::vector<DropoutInfo>& tbc_f1_dropouts,
                       std::vector<DropoutInfo>& tbc_f2_dropouts) {
  if (run.sample_count == 0) return;
  if (sys == VideoSystem::PAL) {
    split_pal_dropout_run(run, tbc_f1_dropouts, tbc_f2_dropouts);
    return;
  }

  // Walk through the run sample by sample to build per-line entries.
  // Optimisation: advance by line when the run spans whole lines.
  uint64_t offset = run.sample_start;
  uint64_t end_offset = run.sample_start + run.sample_count;

  while (offset < end_offset) {
    auto fls = dropout_util::frame_sample_to_field_line(sys, offset);

    // NTSC and PAL_M lines are all the same width in the frame and the file.
    const int32_t line_width =
        (sys == VideoSystem::PAL_M) ? kPalMSamplesPerLine : kNtscSamplesPerLine;

    // Samples remaining on the current line.
    int32_t samples_on_line = line_width - fls.sample;
    uint64_t run_on_line =
        std::min(static_cast<uint64_t>(samples_on_line), end_offset - offset);

    const int32_t di_start = fls.sample;
    const int32_t di_end = fls.sample + static_cast<int32_t>(run_on_line);
    if (di_end > di_start) {
      DropoutInfo di;
      di.line = static_cast<uint32_t>(fls.line);
      di.start_sample = static_cast<uint32_t>(di_start);
      di.end_sample = static_cast<uint32_t>(di_end);

      // ld-decode always writes the isFirstField=true field first, so for all
      // systems VFR field 1 (top) → TBC field 1 and VFR field 2 (bottom) →
      // TBC field 2 (cvbs_signal_constants.h kNtscField1Lines /
      // kPalMField1Lines; colour_frame_phase_observer.cpp).
      if (fls.field == 1) {
        tbc_f1_dropouts.push_back(di);
      } else {
        tbc_f2_dropouts.push_back(di);
      }
    }

    offset += run_on_line;
  }
}

}  // namespace

bool TBCSinkStageDeps::write_tbc_and_metadata(
    const VideoFrameRepresentation* representation, const std::string& tbc_path,
    size_t audio_channel_pair, IObservationContext& observation_context) {
  (void)observation_context;

  // The "-" stdio convention (matching CVBS Sink and the *_stream_source
  // stages): the primary .tbc payload alone goes to stdout. ld-decode's TBC
  // metadata is a SQLite database (needs to seek, which a pipe can't do) and
  // the audio/EFM sidecars are separate files a single pipe cannot also
  // carry, so all of that is dropped when piping — same trade-off CVBS Sink
  // makes for its own sidecars.
  const bool piping = orc::pipe_io::is_pipe_path(tbc_path);

  std::string final_tbc_path = tbc_path;
  if (!piping) {
    const std::string tbc_ext = ".tbc";
    if (tbc_path.length() < tbc_ext.length() ||
        tbc_path.compare(tbc_path.length() - tbc_ext.length(), tbc_ext.length(),
                         tbc_ext) != 0) {
      final_tbc_path += ".tbc";
      ORC_LOG_DEBUG("Added .tbc extension: {}", final_tbc_path);
    }
  }

  const std::string db_path = piping ? std::string() : final_tbc_path + ".db";
  const std::string sidecar_base =
      piping ? std::string() : tbc_sidecar_base(final_tbc_path);
  const std::string pcm_path = piping ? std::string() : sidecar_base + ".pcm";
  const std::string efm_path = piping ? std::string() : sidecar_base + ".efm";

  auto frame_rng = representation->frame_range();
  size_t frame_count = static_cast<size_t>(frame_rng.count());
  size_t expected_field_count = frame_count * 2;

  if (progress_callback_) {
    progress_callback_(0, expected_field_count, "Preparing export...");
  }

  // Sidecars come off the DAG input, not off any file the original source
  // read: the pipeline surface is the same whether the chain started at a
  // TBC source, a CVBS source, or a decoder stage that produced the audio.
  //
  // Analogue audio is the lowest-numbered channel pair; a pipeline carrying
  // several pairs (EFM digital audio, an imported WAV) exports only that one,
  // because the ld-decode sidecar layout has room for exactly one.
  const size_t audio_pair_count = representation->audio_channel_pair_count();
  bool has_audio = audio_pair_count > 0;
  size_t audio_pair = audio_channel_pair;
  if (has_audio && audio_pair >= audio_pair_count) {
    ORC_LOG_WARN(
        "TBCSink: Audio channel pair {} is not present ({} available); "
        "exporting pair 0",
        audio_pair, audio_pair_count);
    audio_pair = 0;
  }
  bool has_efm = representation->has_efm();

  if (piping) {
    if (has_audio) {
      ORC_LOG_WARN(
          "TBCSink: Piping to stdout ('-') carries only the .tbc payload; "
          "dropping the .pcm audio sidecar");
      has_audio = false;
    }
    if (has_efm) {
      ORC_LOG_WARN(
          "TBCSink: Piping to stdout ('-') carries only the .tbc payload; "
          "dropping the .efm sidecar");
      has_efm = false;
    }
    ORC_LOG_WARN(
        "TBCSink: Piping to stdout ('-') carries only the .tbc payload; no "
        ".db metadata sidecar is written");
  }

  try {
    ORC_LOG_DEBUG("Opening TBC file for writing: {}", final_tbc_path);
    if (!piping) {
      ORC_LOG_DEBUG("Opening metadata database: {}", db_path);
    }

    // Open TBC writer (16 MB buffer).
    std::shared_ptr<IFileWriter<uint16_t>> tbc_writer;
    if (stage_services_) {
      class FileWriter16Adapter final : public IFileWriter<uint16_t> {
       public:
        explicit FileWriter16Adapter(std::shared_ptr<IFileWriterUint16> impl)
            : impl_(std::move(impl)) {}

        using IFileWriter<uint16_t>::open;
        bool open(const std::string& filepath,
                  std::ios::openmode mode [[maybe_unused]]) override {
          path_ = filepath;
          return impl_ && impl_->open(filepath);
        }

        void write(const uint16_t* data, size_t count) override {
          if (impl_) impl_->write(data, count);
        }
        void write(const std::vector<uint16_t>& data) override {
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
        std::shared_ptr<IFileWriterUint16> impl_;
        std::string path_;
      };

      auto writer16 = stage_services_->create_buffered_file_writer_uint16(
          static_cast<size_t>(16 * 1024 * 1024));
      if (writer16) {
        tbc_writer = std::make_shared<FileWriter16Adapter>(writer16);
      }
    }
    if (!tbc_writer) {
      ORC_LOG_ERROR("Failed to create TBC writer service");
      return false;
    }
    if (!tbc_writer->open(final_tbc_path)) {
      ORC_LOG_ERROR("Failed to open TBC file for writing: {}", final_tbc_path);
      return false;
    }

    if (!piping && !metadata_writer_->open(db_path)) {
      ORC_LOG_ERROR("Failed to open metadata database for writing: {}",
                    db_path);
      tbc_writer->close();
      return false;
    }

    // Retrieve source parameters for TBC-domain signal levels.
    auto video_params = representation->get_video_parameters();
    if (!video_params) {
      ORC_LOG_ERROR("No video parameters available");
      metadata_writer_->close();
      tbc_writer->close();
      return false;
    }
    // The capture row's `decoder` column is constrained to the two ld-decode
    // family values, so anything else the pipeline reports (a CVBS source, a
    // synthesised signal) is written as the ld-decode default.
    if (video_params->decoder != "ld-decode" &&
        video_params->decoder != "vhs-decode") {
      video_params->decoder = "ld-decode";
    }

    const VideoSystem sys = video_params->system;
    const bool is_ntsc_like =
        (sys == VideoSystem::NTSC || sys == VideoSystem::PAL_M);
    const int32_t tbc_blanking =
        is_ntsc_like ? kTbcNtscBlanking : kTbcPalBlanking;

    // CVBS_U10_4FSC → ld-decode 16-bit: a lossless ×64 widening that leaves
    // the signal's own levels (NTSC-J black, any video_params override)
    // untouched in the samples; tbc_capture_levels() records them alongside.
    const TbcSinkLevelScale level_scale = make_tbc_sink_level_scale(sys);

    const size_t padded_lines = calculate_padded_field_height(sys);

    // Signal geometry.
    int32_t frame_lines_total, field1_cvbs_line_count, nominal_line_width;
    if (sys == VideoSystem::PAL) {
      frame_lines_total = kPalFrameLines;
      field1_cvbs_line_count = kPalField1Lines;
      nominal_line_width = kPalSamplesPerLineNominal;  // 1135
    } else if (sys == VideoSystem::PAL_M) {
      frame_lines_total = kPalMFrameLines;
      field1_cvbs_line_count = kPalMField1Lines;
      nominal_line_width = kPalMSamplesPerLine;
    } else {
      frame_lines_total = kNtscFrameLines;
      field1_cvbs_line_count = kNtscField1Lines;
      nominal_line_width = kNtscSamplesPerLine;
    }

    // Store total frame count; the writer derives number_of_sequential_fields
    // for the DB column as number_of_sequential_frames * 2.
    video_params->number_of_sequential_frames =
        static_cast<int32_t>(frame_count);
    if (!piping && !metadata_writer_->write_video_parameters(*video_params)) {
      ORC_LOG_ERROR("Failed to write video parameters");
      metadata_writer_->close();
      tbc_writer->close();
      return false;
    }

    // --- Sidecars -----------------------------------------------------------
    // The analogue audio .pcm is written as the loop walks frames: each
    // frame's 48 kHz pairs go through one sequential SoXR pass down to
    // 44100 Hz, so the file is the same as a single whole-stream resample
    // without the stream ever being held. The per-field `audio_samples`
    // counts in the field records depend only on the system's audio cadence
    // and the frame index.
    std::unique_ptr<TbcPcmStreamWriter> pcm_writer;
    if (has_audio) {
      PcmAudioParameters pcm_params;
      pcm_params.bits = kTbcPcmBits;
      pcm_params.is_signed = true;
      pcm_params.is_little_endian = true;
      pcm_params.sample_rate = static_cast<double>(kTbcPcmSampleRateHz);
      if (!metadata_writer_->write_pcm_audio_parameters(pcm_params)) {
        ORC_LOG_WARN(
            "TBCSink: Failed to record .pcm audio parameters; the sidecar "
            "layout will fall back to the ld-decode default on re-import");
      }

      std::shared_ptr<IFileWriterUint8> writer;
      if (stage_services_) {
        writer = stage_services_->create_buffered_file_writer_uint8(
            static_cast<size_t>(4 * 1024 * 1024));
      }
      if (!writer) {
        ORC_LOG_ERROR(
            "TBCSink: File writer service unavailable for .pcm sidecar");
        metadata_writer_->close();
        tbc_writer->close();
        return false;
      }
      if (!writer->open(pcm_path)) {
        ORC_LOG_ERROR("TBCSink: Failed to open .pcm sidecar for writing: {}",
                      pcm_path);
        metadata_writer_->close();
        tbc_writer->close();
        return false;
      }
      pcm_writer = std::make_unique<TbcPcmStreamWriter>(
          std::move(writer), tbc_pcm_total_pairs(sys, frame_count));
      if (!pcm_writer->ok()) {
        pcm_writer->abandon();
        metadata_writer_->close();
        tbc_writer->close();
        return false;
      }
    }

    if (has_audio) {
      // Primed once the outputs are known to be openable, and metered here
      // rather than inside the first get_audio_samples() call: a pair fed by
      // EFM audio decode runs a whole-stream decode on first touch.
      representation->prime_audio_decode(
          [this](uint64_t done, uint64_t total, const std::string& what) {
            if (progress_callback_) progress_callback_(done, total, what);
          });
    }

    // EFM streams straight out as the loop walks frames: it is one byte per
    // T-value with no conversion.
    std::shared_ptr<IFileWriterUint8> efm_writer;
    if (has_efm && stage_services_) {
      efm_writer = stage_services_->create_buffered_file_writer_uint8(
          static_cast<size_t>(4 * 1024 * 1024));
      if (efm_writer && !efm_writer->open(efm_path)) {
        ORC_LOG_ERROR("TBCSink: Failed to open EFM sidecar for writing: {}",
                      efm_path);
        efm_writer.reset();
      }
    }
    const bool writing_efm = static_cast<bool>(efm_writer);

    // Every early return past this point has to release the sidecar writers
    // as well as the TBC and metadata ones.
    const auto close_outputs = [&]() {
      if (pcm_writer) pcm_writer->abandon();
      if (efm_writer) efm_writer->close();
      metadata_writer_->close();
      tbc_writer->close();
    };

    ORC_LOG_DEBUG(
        "TBCSink: {} frames → {} fields; blanking={} scale={} sys={} audio={} "
        "efm={}",
        frame_count, expected_field_count, tbc_blanking, level_scale.scale,
        static_cast<int>(sys), has_audio, writing_efm);

    metadata_writer_->begin_transaction();
    size_t fields_exported = 0;

    // Append this frame's audio and EFM to the sidecar streams, and fill in
    // the two field records' counts.  Runs for padding frames too: dropping
    // their audio would shorten the stream and slide everything after them
    // out of sync with the video.
    std::vector<int32_t> silence;
    size_t frames_with_sidecars = 0;
    const auto collect_sidecars = [&](FrameID frame_id, size_t frame_index,
                                      FieldMetadata& first_field,
                                      FieldMetadata& second_field) {
      if (pcm_writer) {
        const auto samples =
            representation->get_audio_samples(audio_pair, frame_id);
        const size_t expected =
            static_cast<size_t>(audio_pairs_in_frame(frame_index, sys)) * 2;
        const size_t have = std::min(samples.size(), expected) & ~size_t{1};
        pcm_writer->append(samples.data(), have / 2);
        // A producer that returns short for a frame is padded to the cadence
        // so the stream stays aligned with the video.
        if (have < expected) {
          silence.assign(expected - have, 0);
          pcm_writer->append(silence.data(), silence.size() / 2);
        }
        frames_with_sidecars = frame_index + 1;
        const TbcPcmFieldPairs pairs =
            tbc_pcm_frame_field_pairs(sys, frame_index);
        first_field.audio_samples = pairs.first_field;
        second_field.audio_samples = pairs.second_field;
      }

      if (writing_efm) {
        const auto tvalues = representation->get_efm_samples(frame_id);
        if (!tvalues.empty()) efm_writer->write(tvalues);
        const TbcEfmFieldSplit split =
            tbc_efm_split_frame_bytes(tvalues.size());
        first_field.efm_t_values = split.first_field;
        second_field.efm_t_values = split.second_field;
      }
    };

    for (FrameID frame_id = frame_rng.first; frame_rng.contains(frame_id);
         ++frame_id) {
      const size_t frame_index =
          static_cast<size_t>(frame_id - frame_rng.first);

      if (pCancelRequested_->load()) {
        metadata_writer_->commit_transaction();
        close_outputs();
        ORC_LOG_WARN("TBCSink: Export cancelled by user");
        pIsProcessing_->store(false);
        return false;
      }

      auto frame_desc = representation->get_frame_descriptor(frame_id);
      if (!frame_desc || frame_desc->is_padding_frame) {
        // Padding frames: emit two blanking-level fields to keep the file
        // sequential without corrupting frame count.
        size_t blank_field_samples =
            padded_lines * static_cast<size_t>(nominal_line_width);
        std::vector<uint16_t> blank(blank_field_samples,
                                    static_cast<uint16_t>(tbc_blanking));
        std::array<FieldMetadata, 2> pad_fields;
        for (int f = 0; f < 2; ++f) {
          pad_fields[f].seq_no = static_cast<int32_t>(fields_exported + 1 + f);
          pad_fields[f].is_first_field = (f == 0);
          // Preserve padding identity so it round-trips: a re-imported TBC
          // marks these fields pad=true and the frame is skipped by the
          // observer pass (issue #77).
          pad_fields[f].is_pad = true;
        }
        collect_sidecars(frame_id, frame_index, pad_fields[0], pad_fields[1]);
        for (int f = 0; f < 2; ++f) {
          tbc_writer->write(blank);
          metadata_writer_->write_field_metadata(pad_fields[f]);
          ++fields_exported;
        }
        continue;
      }

      // A piped, unbounded source (frame_count left at 0) declares a huge
      // placeholder range up front, and its frame_desc is always present
      // (it does not depend on real production progress) — so it never
      // takes the padding branch above. get_frame() is the one accessor
      // that does reflect real progress: once the source is exhausted, it
      // is nullptr for this and every later frame_id, so stop here rather
      // than padding out the tail with blanking as if it were legitimate
      // content.
      if (representation->is_exhausted() &&
          !representation->get_frame(frame_id)) {
        // Exhausted on a read error, not a clean end: fail rather than
        // report a truncated file as a success.
        const std::string stream_error = representation->stream_error();
        if (!stream_error.empty()) {
          metadata_writer_->commit_transaction();
          close_outputs();
          ORC_LOG_ERROR("TBCSink: Input stream failed: {}", stream_error);
          pIsProcessing_->store(false);
          return false;
        }
        break;
      }

      // Measure the colour-sequence phase from the burst signal so the exported
      // TBC carries a correct per-field fieldPhaseID (1-4 NTSC, 1-8 PAL/PAL_M),
      // independent of whatever the input source did or did not provide.
      const orc::observation::FramePhase frame_phase =
          orc::observation::measure_frame_phase(*representation, frame_id);

      // Build per-field dropout lists from the frame-flat DropoutRuns.
      std::vector<DropoutInfo> tbc_f1_dropouts, tbc_f2_dropouts;
      for (const auto& run : representation->get_dropout_hints(frame_id)) {
        split_dropout_run(sys, run, tbc_f1_dropouts, tbc_f2_dropouts);
      }

      // Describe the two TBC fields to extract from this CVBS frame.
      //
      // ld-decode always writes the isFirstField=true field first (it skips a
      // leading second field), and that field is the top spatial field, so for
      // all systems VFR field 1 (top) → TBC field 1 and VFR field 2 (bottom) →
      // TBC field 2.  This is the ordering tbc_source reads back and the one
      // the colour_frame_phase observer measures against; inverting it here
      // transposed every field pair on export (issue #257).
      //
      // PAL:    TBC field 1 (is_first_field=true,  313 lines) = VFR [0, 313)
      //         TBC field 2 (is_first_field=false, 312 lines) = VFR [313, 625)
      // NTSC:   TBC field 1 (is_first_field=true,  263 lines) = VFR [0, 263)
      //         TBC field 2 (is_first_field=false, 262 lines) = VFR [263, 525)
      // PAL_M:  TBC field 1 (is_first_field=true,  263 lines) = VFR [0, 263)
      //         TBC field 2 (is_first_field=false, 262 lines) = VFR [263, 525)
      //
      // The shorter second field is padded out to the stored field height
      // below, matching the way ld-decode stores both fields at equal height.
      struct FieldExtract {
        int32_t cvbs_start;
        int32_t cvbs_end;  // exclusive
        bool is_first_field;
        const std::vector<DropoutInfo>* dropouts;
      };

      std::array<FieldExtract, 2> extract_plan;
      std::vector<int16_t> pal_lattice_field;
      extract_plan[0] = {0, field1_cvbs_line_count, true, &tbc_f1_dropouts};
      extract_plan[1] = {field1_cvbs_line_count, frame_lines_total, false,
                         &tbc_f2_dropouts};

      // Both field records are built before either is written so the sidecar
      // pass can fill in the frame's audio and EFM counts across the pair.
      std::array<FieldMetadata, 2> field_metas;
      for (size_t f = 0; f < field_metas.size(); ++f) {
        field_metas[f].seq_no = static_cast<int32_t>(fields_exported + 1 + f);
        field_metas[f].is_first_field = extract_plan[f].is_first_field;
        const int32_t field_phase_id = extract_plan[f].is_first_field
                                           ? frame_phase.field1_phase_id
                                           : frame_phase.field2_phase_id;
        if (field_phase_id >= 0) {
          field_metas[f].field_phase_id = field_phase_id;
        }
      }
      collect_sidecars(frame_id, frame_index, field_metas[0], field_metas[1]);

      for (size_t plan_index = 0; plan_index < extract_plan.size();
           ++plan_index) {
        const auto& ep = extract_plan[plan_index];
        const size_t actual_lines =
            static_cast<size_t>(ep.cvbs_end - ep.cvbs_start);

        std::vector<uint16_t> field_buffer;
        field_buffer.reserve(padded_lines *
                             static_cast<size_t>(nominal_line_width));

        if (sys == VideoSystem::PAL) {
          // The CVBS frame is the 4fsc lattice; the TBC file is line-locked.
          // Gather the field's lattice samples (including the two extra
          // samples on its last row) and resample them back to 1135 samples
          // per line — the inverse of the TBC source's conversion.
          const int field = ep.is_first_field ? 1 : 2;
          pal_lattice_field.assign(
              pal_line_locked::lattice_field_samples(field),
              static_cast<int16_t>(kPalBlanking));
          size_t cursor = 0;
          for (int32_t fl = ep.cvbs_start; fl < ep.cvbs_end; ++fl) {
            const size_t width = frame_line_sample_count(
                VideoSystem::PAL,
                static_cast<size_t>(kPalSamplesPerLineNominal),
                static_cast<size_t>(fl));
            const int16_t* line_data =
                representation->get_line(frame_id, static_cast<size_t>(fl));
            if (line_data) {
              std::copy(line_data, line_data + width,
                        pal_lattice_field.begin() +
                            static_cast<std::ptrdiff_t>(cursor));
            }
            cursor += width;
          }
          field_buffer.resize(
              pal_line_locked::line_locked_field_samples(field));
          pal_line_locked::to_line_locked(
              field, pal_lattice_field.data(), field_buffer.data(),
              [&level_scale](float v) { return level_scale.to_tbc_value(v); });
        }

        // Convert each CVBS line to TBC uint16_t samples (NTSC, PAL_M: the
        // line-locked and subcarrier-locked grids coincide there).
        for (int32_t fl = ep.cvbs_start;
             sys != VideoSystem::PAL && fl < ep.cvbs_end; ++fl) {
          const int16_t* line_data =
              representation->get_line(frame_id, static_cast<size_t>(fl));

          const size_t read_width = static_cast<size_t>(nominal_line_width);

          if (!line_data) {
            for (size_t s = 0; s < read_width; ++s) {
              field_buffer.push_back(static_cast<uint16_t>(tbc_blanking));
            }
          } else {
            for (size_t s = 0; s < read_width; ++s) {
              field_buffer.push_back(level_scale.to_tbc(line_data[s]));
            }
          }
        }

        // Pad the shorter field (always is_first_field=false) to padded_lines.
        if (actual_lines < padded_lines) {
          const size_t padding_lines = padded_lines - actual_lines;
          for (size_t p = 0; p < padding_lines; ++p) {
            for (int32_t s = 0; s < nominal_line_width; ++s) {
              field_buffer.push_back(static_cast<uint16_t>(tbc_blanking));
            }
          }
        }

        tbc_writer->write(field_buffer);

        metadata_writer_->write_field_metadata(field_metas[plan_index]);

        // Write per-field dropout info.
        FieldID export_field_id(fields_exported);
        for (const auto& di : *ep.dropouts) {
          metadata_writer_->write_dropout(export_field_id, di);
        }

        ++fields_exported;
      }

      if (fields_exported % 20 == 0 && progress_callback_) {
        progress_callback_(fields_exported, expected_field_count,
                           "Exporting field " +
                               std::to_string(fields_exported) + "/" +
                               std::to_string(expected_field_count));
      }
    }

    // write_video_parameters() above ran before the loop with frame_count
    // as the declared field count — for a piped/unbounded source that is a
    // huge placeholder, not the real length. Correct it now that the real
    // count (fields_exported) is known, so the persisted database never
    // claims an arbitrary size.
    metadata_writer_->update_sequential_field_count(
        static_cast<int32_t>(fields_exported));

    metadata_writer_->commit_transaction();
    metadata_writer_->close();
    tbc_writer->close();
    if (efm_writer) {
      efm_writer->close();
      ORC_LOG_DEBUG("TBCSink: Wrote EFM sidecar {}", efm_path);
    }

    if (pcm_writer) {
      // Padded to the frames actually exported: a source that ended early
      // (an unbounded one) declared a placeholder length up front.
      if (!pcm_writer->finish(tbc_pcm_total_pairs(sys, frames_with_sidecars))) {
        ORC_LOG_ERROR("TBCSink: Failed to write audio sidecar: {}", pcm_path);
        return false;
      }
      ORC_LOG_DEBUG("TBCSink: Wrote {} stereo pairs at {} Hz to {}",
                    pcm_writer->pairs_written(), kTbcPcmSampleRateHz, pcm_path);
    }

    ORC_LOG_DEBUG("TBCSink: Successfully exported {} fields", fields_exported);
    return true;

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("TBCSink: Exception during export: {}", e.what());
    return false;
  }
}

}  // namespace orc
