# EFM Audio Decode

Decodes the EFM t-value stream carried by the pipeline (LaserDisc digital audio) to CD audio and appends it to the representation as a new audio channel pair. The appended pair is in the standard pipeline audio form: 48 kHz synchronous (frame-locked) 24-bit stereo, converted from the decoded 44.1 kHz 16-bit CD audio at the point of production. Its first sample is synchronous with the first frame of this stage's input.

## When to use

Use EFM Audio Decode on ld-decode LaserDisc sources that carry an `.efm` sidecar when you want the disc's digital audio available as a pipeline channel pair — for example so the Video Sink embeds it alongside the analogue audio, or so the Audio Sink or CVBS Sink can export it. If you want a bit-exact, un-resampled WAV file of the disc's CD audio, the EFM Decoder Sink does that without adding a channel pair.

Place this stage after dropout correction and stacking so it decodes the best available EFM stream, and after any frame mapping or source alignment so the decoded pair is born aligned to the output timeline.

This transform is audio-only. EFM data discs (ECMA-130 sectors) cannot become an audio channel pair; on a data disc the decode fails and the appended pair is silent (use the EFM Decoder Sink in data mode instead).

## What it does

The stage wraps the incoming frame representation and appends one audio channel pair (named from the `Channel Pair Name` parameter, default `EFM digital audio`; origin EFM). The name surfaces in the CVBS container and as the embedded stream title in the Video Sink. Video, dropout hints, undecoded EFM/AC3 signal data, and all existing audio channel pairs pass through untouched.

Each EFM byte on the pipeline packs the t-value into its low nibble and the producer's confidence — as a doubt value, 0 trusted to 15 distrusted — into its high nibble. That confidence travels the DAG intact, but this stage consumes t-values, so it strips the doubt nibble as the t-values enter the decoder. The decode is therefore unaffected by whether the source recorded any confidence. (The doubt is not yet used as an error-correction erasure hint; that is a future refinement of the CIRC stages.)

EFM decoding is whole-stream sequential (the CIRC interleaving spans sectors), so it cannot run frame-by-frame. The decode therefore runs lazily, at most once, on the first access to the appended pair's audio: the stage gathers the t-values across the input's full frame range, runs the shared EFM decode pipeline, widens the decoded 44.1 kHz 16-bit CD audio to 24-bit, resamples it to 48 kHz (SoXR HQ), and caches the converted frame-locked PCM in a scratch file on disk. Video-only preview and project validation never trigger the decode.

The EFM t-values are demodulated from the same FM signal as the video, so the decoded audio has a fixed, computable relationship to the video timeline. The stage aligns the head of the decoded stream automatically: the CIRC de-interleaver's constant 111-frame latency (666 stereo pairs, 15.1 ms of warm-up filler at the stream head) is removed, and the input the decoder consumed while acquiring sync (discarded t-values, pre-sync frames, and any settle or lead-in sections) is restored as silence. After alignment the pair's first sample is synchronous with the first frame of this stage's input — the same sync the analogue audio has. If the audio still needs nudging, use the **offset_ms** parameter (or a downstream Audio Align stage for post-hoc, per-pair adjustment).

Appending the pair counts toward the pipeline's 8-channel-pair limit; the stage fails validation if the input already carries 8 channel pairs.

## Parameters

### no_timecodes
Boolean, default `false`. Disable timecode verification during decode. Needed for early CAV discs that pre-date the EFM timecode specification. On such discs the Q-channel timeline is absent or untrustworthy — every block may carry track number 0 (which otherwise marks a lead-in) — so the decoder ignores the decoded timeline and synthesises a contiguous one, treating every section as programme audio (track 1). Leave this off for normal discs: it overrides genuine lead-in/lead-out and track boundaries.

### no_audio_concealment
Boolean, default `false`. Disable interpolation-based audio error concealment; uncorrectable samples are left as decoded instead of being concealed.

### ignore_preemphasis
Boolean, default `false`. Ignore the 50/15 µs pre-emphasis CONTROL flag (IEC 60908 §17.5) and decode the audio exactly as stored. When unchecked (default), sections flagged as pre-emphasised are de-emphasised during decode with a 50/15 µs filter, so the appended channel pair has a flat response. Enable this only if you want the raw pre-emphasised samples.

### pair_name
String, default `EFM digital audio`. Human-readable name for the decoded EFM audio channel pair. It surfaces in the CVBS container and as the embedded stream title in the Video Sink. If left empty it falls back to `EFM digital audio`.

### offset_ms
Double, default `0`. Additional sync slip in milliseconds applied on top of the automatic video-timeline alignment. Positive values delay the audio relative to the video; negative values advance it. Leave at 0 unless the decoded audio still needs nudging after the automatic alignment.

### report
Boolean, default `false`. Enable to write a detailed decode statistics report (the same per-stage CIRC/error/timing statistics the EFM Decoder Sink writes) once the lazy decode runs. When enabled, set the report destination in **report_path**.

The report separates losses caused by the input from losses caused by the decoder's own de-interleave warm-up and end-of-stream drain, and attributes concealment to disc regions (lead-in, pause, programme, lead-out) on the latency-compensated audio timeline. Only genuine losses in the programme area affect the overall assessment. See the EFM Decoder Sink instructions for a full description.

### report_path
File path, default empty. Destination for the decode report; only used when **report** is enabled. Because this stage decodes into a scratch cache rather than a user-named output file, the report location is given explicitly here rather than being derived from an output filename. If **report** is enabled but this path is empty, no report is written.

## Tools

This stage has no interactive tools.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
