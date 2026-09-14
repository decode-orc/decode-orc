# Audio Sink

Extracts one audio channel pair from the processed video pipeline and writes it to a standard WAV file, synchronised to the processed video timeline. Use this stage when you want the audio independently from the full TBC output.

## When to use

Add this sink in parallel with the TBC Sink stage when your LaserDisc or tape source carries audio and you need it as a standalone file — for example to process it in a DAW, verify audio/video sync, or archive it separately. Because it is a parallel sink, adding it does not affect the video output.

## What it does

The pipeline can carry up to 8 stereo audio channel pairs (analogue capture audio, decoded EFM digital audio, imported WAV files, or channel pairs derived by transforms). The `channel_pair` parameter selects which channel pair this sink writes.

All pipeline audio is stereo, sampled at exactly 48,000 Hz and frame-locked (synchronous) to the video, following SMPTE 272M-1994. The sink gathers the selected channel pair frame by frame — so any frame trimming or reordering performed upstream is reflected in the output — and writes 24-bit signed little-endian stereo PCM in a standard RIFF WAV container declaring 48,000 Hz, with no sample-rate or bit-depth conversion. Frames without audio are written as silence of the correct length, preserving audio/video sync.

## Parameters

### output_path (string)
Path to the output WAV file. Required. The file will be created or overwritten at trigger time.

`-` writes the WAV stream to the CLI process's standard output instead of a file — CLI only; the GUI rejects this value. The header's `data` size is computed from the frame range before any samples are written, so nothing needs to seek back afterwards and the whole file streams correctly in one forward pass.

### channel_pair
Audio channel pair to write. Channel pair indices are 0-based, matching the CVBS container's `_audio_<p>.wav` numbering (0–7). Default 0. In the GUI this is a drop-down restricted to the channel pairs the input actually carries (shown as `<n> - <description>` where a description is present). Triggering fails if the selected channel pair does not exist in the input.

## Notes

Undecoded digital audio carried as an EFM t-value stream or AC3 RF (Dolby Digital) must be extracted with the EFM Decoder Sink or AC3 RF Sink stages respectively — or, for EFM, decoded into a pipeline channel pair with the EFM Audio Decode transform and then written by this sink. Audio stacking (when processing multiple source captures) must be performed upstream, for example via the Stacker stage's `audio_stacking` parameter.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
