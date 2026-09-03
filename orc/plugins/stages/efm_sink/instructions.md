# EFM Decoder Sink

Accumulates EFM t-values from the pipeline and decodes them through a full EFM decode pipeline, producing either a PCM WAV audio file or ECMA-130 binary sector data. This is the primary stage for extracting digital audio or data content from LaserDiscs.

## When to use

Use the audio mode when extracting CD-quality digital audio from a LaserDisc. Enable `audacity_labels` to get chapter markers in Audacity, and `no_timecodes` for early CAV discs that pre-date the EFM timecode specification. Use the data mode when extracting ECMA-130 data sectors from a CD-Video or data LaserDisc; enable `output_metadata` to capture a record of missing or corrupt sectors for quality assessment.

## What it does

The stage collects EFM t-values field by field from the upstream VideoFieldRepresentation and feeds them through the EfmProcessor decode pipeline, which performs demodulation, error detection, error correction (CIRC), and de-interleaving.

Each EFM byte on the pipeline packs the t-value into its low nibble and the producer's confidence — as a doubt value, 0 trusted to 15 distrusted — into its high nibble. That confidence travels the DAG intact, but this stage consumes t-values, so it strips the doubt nibble as the t-values enter the decoder. The decode is therefore unaffected by whether the source recorded any confidence. (The doubt is not yet used as an error-correction erasure hint; that is a future refinement of the CIRC stages.) In audio mode the result is written as 44.1 kHz 16-bit stereo PCM in a RIFF WAV container. In data mode the result is written as raw ECMA-130 binary sectors. Optional sidecar files (Audacity labels, bad-sector maps, decode reports) are written alongside the main output when their respective flags are enabled.

## Parameters

### output_path (string)
Path to the output file. Use a `.wav` extension for audio mode and `.bin` for data mode. Required.

### decode_mode (string)
Decode target. Values: `audio` (PCM WAV output) or `data` (ECMA-130 sector output). Default: `audio`.

### no_timecodes (boolean)
Disable timecode verification during decode. Needed for early CAV discs that pre-date the EFM timecode specification. On such discs the Q-channel timeline is absent or untrustworthy — every block may carry track number 0 (which otherwise marks a lead-in) — so the decoder ignores the decoded timeline and instead synthesises a contiguous one, treating every section as programme audio (track 1). Leave this off for normal discs: it overrides genuine lead-in/lead-out and track boundaries. Default: `false`.

### audacity_labels (boolean)
Write an Audacity label file alongside the audio output, marking chapter boundaries and positions of missing samples. Audio mode only. Default: `false`.

### no_audio_concealment (boolean)
Disable interpolation-based error concealment for uncorrectable audio samples. When disabled, affected samples are zeroed instead of interpolated. Audio mode only. Default: `false`.

### ignore_preemphasis (boolean)
Ignore the 50/15 µs pre-emphasis CONTROL flag (IEC 60908 §17.5) and write the audio exactly as decoded. When unchecked (default), sections flagged as pre-emphasised are de-emphasised during decode with a 50/15 µs filter, so the output plays back with a flat response. Enable this only if you want the raw pre-emphasised samples (e.g. to apply de-emphasis yourself downstream). When Audacity labels are enabled, the label for a pre-emphasised track is annotated `Preemphasis:50/15us(removed)` when de-emphasis was applied, or `Preemphasis:50/15us` when this flag is set. Audio mode only. Default: `false`.

### video_sync (boolean)
Align the start of the decoded audio with the video timeline of the capture. The EFM t-values are demodulated from the same FM signal as the video, so the decoded audio has a fixed, computable relationship to the video: the CIRC de-interleaver's constant 111-frame latency (666 stereo pairs, 15.1 ms of warm-up filler at the stream head) is removed, and the input the decoder consumed while acquiring sync (discarded t-values, pre-sync frames, and any settle or lead-in sections) is restored as silence. The result starts in sync with the video — the same sync the analogue audio has — so it can be muxed against the video without manual correction. Ignored when `zero_pad` is enabled (zero-pad anchors the audio to disc absolute time 00:00:00 instead). Disable for bit-exact legacy output. The adjustment actually applied is stated in the decode report (Part C, decode boundaries). Audio mode only. Default: `true`.

### offset_ms (double)
Additional sync slip in milliseconds applied at the head of the audio output, on top of the `video_sync` alignment. Positive values delay the audio relative to the video (silence is prepended); negative values advance it (samples are trimmed). Applied even when `video_sync` is disabled. Audio mode only. Default: `0`.

### zero_pad (boolean)
Zero-pad the start of the audio output so that playback begins at 00:00:00.0 relative to the first valid EFM timecode. Overrides `video_sync`. Audio mode only. Default: `false`.

### no_wav_header (boolean)
Output raw PCM samples without a RIFF WAV header. Useful when the output will be piped into another tool that expects headerless PCM. Audio mode only. Default: `false`.

### output_metadata (boolean)
Write a bad-sector map file alongside the sector output recording the position of any missing or corrupt sectors. Data mode only. Default: `false`.

### report (boolean)
Write a detailed decode statistics report file alongside the main output. Default: `false`.

## Reading the decode report

The report separates losses caused by the input from losses caused by the decode boundaries, because only the former say anything about the disc.

The CIRC de-interleaver has a fixed latency of 111 F1 frames, so every decode necessarily begins with 111 frames of warm-up filler and ends by draining the delay lines. Neither carries disc data, so neither is counted as an error:

- **Pipeline warm-up** — the de-interleaver only delays its output, so no disc data is lost here.
- **Pipeline drain** — the newest 111 frames are still spread across the delay lines at end of stream and cannot be assembled from complete codewords. On a normal disc this falls in the lead-out, which is digital silence by construction (IEC 60908-1999 §17.5.1), and costs nothing. If the capture stops inside the programme area the report says so and warns.

Codewords containing warm-up or drain symbols are excluded from the C1/C2 uncorrectable percentages and from the post-C2 data-loss figure — from both the numerator and the denominator. A stream with no invalid EFM symbols, no sync losses and no missing sections therefore reports 0.0000 % and grades `EXCELLENT`.

Concealed and muted samples are broken down twice over: by disc region (lead-in, pause, programme, lead-out) and by cause (warm-up, drain, genuine). Each breakdown accounts for every sample. Region boundaries are computed on the **audio** timeline: subcode rides in the F3 frame and is not de-interleaved, so the 111-frame latency (666 samples per channel, 15.1 ms) is subtracted from each section's Q time before a sample is attributed.

The overall assessment is driven only by genuine losses in the programme area:

| Grade | Condition |
|---|---|
| `EXCELLENT` | No concealment or muting in the programme area, no uncorrectable C1/C2, no missing sections |
| `GOOD` | Programme concealment < 0.10 %, no muting |
| `FAIR` | Programme concealment < 1.0 % |
| `POOR` | Programme loss >= 1.0 %, or missing / uncorrectable sections |

Warnings (`!`) are raised only for genuine losses and name the affected track and timecode. Structural boundary information appears as informational (`i`) text in Part C.

## Notes

The upstream source stage must supply EFM data — the pipeline will abort if no EFM data is present in the VideoFieldRepresentation. Audio and data modes are mutually exclusive; set `decode_mode` before triggering. Parameters marked "audio only" or "data only" are silently ignored when the corresponding mode is not active.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
