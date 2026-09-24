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
#include <orc/support/pipe_io.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "audio-resample/audio_resampler.h"
#include "tbc_sink_levels.h"
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

// Split a DropoutRun (frame-flat coordinates) into per-field DropoutInfo
// entries and append them to the appropriate output vectors.
// tbc_f1_dropouts ← entries that belong to TBC field 1 (is_first_field=true)
// tbc_f2_dropouts ← entries that belong to TBC field 2
void split_dropout_run(VideoSystem sys, const DropoutRun& run,
                       std::vector<DropoutInfo>& tbc_f1_dropouts,
                       std::vector<DropoutInfo>& tbc_f2_dropouts) {
  if (run.sample_count == 0) return;

  // Walk through the run sample by sample to build per-line entries.
  // Optimisation: advance by line when the run spans whole lines.
  uint64_t offset = run.sample_start;
  uint64_t end_offset = run.sample_start + run.sample_count;

  while (offset < end_offset) {
    auto fls = dropout_util::frame_sample_to_field_line(sys, offset);

    // Determine the width of this line in the frame buffer (accounting for the
    // PAL extra samples) and the width it is stored at in the TBC file.
    int32_t line_width;
    int32_t stored_line_width;
    if (sys == VideoSystem::PAL) {
      // frame_sample_to_field_line uses field-local line; reconstruct frame
      // line to check for non-orthogonal status.
      const int32_t frame_line =
          (fls.field == 1) ? fls.line : (kPalField1Lines + fls.line);
      line_width = static_cast<int32_t>(frame_line_sample_count(
          VideoSystem::PAL, static_cast<size_t>(kPalSamplesPerLineNominal),
          static_cast<size_t>(frame_line)));
      stored_line_width = kPalSamplesPerLineNominal;
    } else if (sys == VideoSystem::PAL_M) {
      line_width = kPalMSamplesPerLine;
      stored_line_width = kPalMSamplesPerLine;
    } else {
      line_width = kNtscSamplesPerLine;
      stored_line_width = kNtscSamplesPerLine;
    }

    // Samples remaining on the current line.
    int32_t samples_on_line = line_width - fls.sample;
    uint64_t run_on_line =
        std::min(static_cast<uint64_t>(samples_on_line), end_offset - offset);

    // The EBU Tech. 3280-E §1.3.1 extra samples on PAL lines 312 and 624 are
    // not written to the TBC, so a run that reaches into them must stop at the
    // stored line width rather than describe samples the file does not have.
    const int32_t di_start = fls.sample;
    const int32_t di_end = std::min(
        fls.sample + static_cast<int32_t>(run_on_line), stored_line_width);
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

bool TBCSinkStageDeps::write_pcm_sidecar(
    const std::vector<int32_t>& audio_stream,
    const std::vector<int32_t>& field_pairs, const std::string& pcm_path) {
  // The metadata already declares the per-field layout, so the file must come
  // out at exactly that length.
  uint64_t declared_pairs = 0;
  for (const int32_t pairs : field_pairs) {
    declared_pairs += static_cast<uint64_t>(pairs);
  }
  if (declared_pairs == 0) return true;

  if (progress_callback_) {
    progress_callback_(0, 1, "Resampling audio for the .pcm sidecar...");
  }

  // One SoXR pass over the whole stream rather than per frame: an independent
  // resample per block would leave a filter transient at every boundary.
  const std::vector<int32_t> resampled = AudioResampler::resample(
      audio_stream, static_cast<double>(kAudioSampleRateHz),
      static_cast<double>(kTbcPcmSampleRateHz));
  if (resampled.empty() && !audio_stream.empty()) {
    ORC_LOG_ERROR("TBCSink: Audio resample to {} Hz failed",
                  kTbcPcmSampleRateHz);
    return false;
  }

  std::shared_ptr<IFileWriterUint8> writer;
  if (stage_services_) {
    writer = stage_services_->create_buffered_file_writer_uint8(
        static_cast<size_t>(4 * 1024 * 1024));
  }
  if (!writer) {
    ORC_LOG_ERROR("TBCSink: File writer service unavailable for .pcm sidecar");
    return false;
  }
  if (!writer->open(pcm_path)) {
    ORC_LOG_ERROR("TBCSink: Failed to open .pcm sidecar for writing: {}",
                  pcm_path);
    return false;
  }

  // Written in chunks so a long export does not need the packed byte form of
  // the whole stream resident on top of the samples it came from.
  constexpr size_t kChunkPairs = 1u << 16;
  for (uint64_t written = 0; written < declared_pairs;) {
    const size_t chunk = static_cast<size_t>(
        std::min<uint64_t>(kChunkPairs, declared_pairs - written));
    const size_t first_value = static_cast<size_t>(written) * 2;
    const size_t available =
        first_value < resampled.size() ? resampled.size() - first_value : 0;
    const std::vector<int32_t> slice(
        resampled.begin() + static_cast<std::ptrdiff_t>(
                                std::min(first_value, resampled.size())),
        resampled.begin() + static_cast<std::ptrdiff_t>(std::min(
                                first_value + std::min(available, chunk * 2),
                                resampled.size())));
    writer->write(tbc_pcm_pack_s16le(slice, chunk));
    written += chunk;
  }

  writer->close();
  ORC_LOG_DEBUG("TBCSink: Wrote {} stereo pairs at {} Hz to {}", declared_pairs,
                kTbcPcmSampleRateHz, pcm_path);
  return true;
}

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
    // The analogue audio .pcm is written after the frame loop: 48000 → 44100
    // is one SoXR pass over the whole stream, so the samples are gathered
    // here and converted once at the end (mirroring the single ingest pass
    // tbc_source runs in the opposite direction).  The per-field
    // `audio_samples` counts, though, go into the field records inside the
    // loop, so the layout is computed up front — it depends only on the
    // system's audio cadence and the frame count.
    std::vector<int32_t> audio_stream;
    std::vector<int32_t> audio_field_pairs;
    if (has_audio) {
      audio_field_pairs = tbc_pcm_field_pair_counts(sys, frame_count);
      audio_stream.reserve(
          static_cast<size_t>(audio_pair_offset(frame_count, sys)) * 2);

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
    const auto collect_sidecars = [&](FrameID frame_id, size_t frame_index,
                                      FieldMetadata& first_field,
                                      FieldMetadata& second_field) {
      if (has_audio) {
        const auto samples =
            representation->get_audio_samples(audio_pair, frame_id);
        const size_t expected =
            static_cast<size_t>(audio_pairs_in_frame(frame_index, sys)) * 2;
        audio_stream.insert(
            audio_stream.end(), samples.begin(),
            samples.begin() + static_cast<std::ptrdiff_t>(
                                  std::min(samples.size(), expected)));
        // A producer that returns short for a frame is padded to the cadence
        // so the stream stays aligned with the video.
        if (samples.size() < expected) {
          audio_stream.resize(audio_stream.size() + expected - samples.size(),
                              0);
        }
        const size_t f1 = frame_index * 2;
        if (f1 + 1 < audio_field_pairs.size()) {
          first_field.audio_samples = audio_field_pairs[f1];
          second_field.audio_samples = audio_field_pairs[f1 + 1];
        }
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

        // Convert each CVBS line to TBC uint16_t samples.
        for (int32_t fl = ep.cvbs_start; fl < ep.cvbs_end; ++fl) {
          const int16_t* line_data =
              representation->get_line(frame_id, static_cast<size_t>(fl));

          // EBU Tech. 3280-E §1.3.1: PAL lines 312 and 624 carry 2 extra
          // samples; drop them so every TBC line is exactly
          // nominal_line_width samples wide.
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

    if (has_audio &&
        !write_pcm_sidecar(audio_stream, audio_field_pairs, pcm_path)) {
      ORC_LOG_ERROR("TBCSink: Failed to write audio sidecar: {}", pcm_path);
      return false;
    }

    ORC_LOG_DEBUG("TBCSink: Successfully exported {} fields", fields_exported);
    return true;

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("TBCSink: Exception during export: {}", e.what());
    return false;
  }
}

}  // namespace orc
