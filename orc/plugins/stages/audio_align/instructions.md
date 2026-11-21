# Audio Align

Shifts one audio channel pair in time relative to the video to correct audio/video synchronisation. Positive offsets delay the audio (insert lead-in); negative offsets advance it (trim from the start). Only the target channel pair moves — video, all other channel pairs, and undecoded EFM/AC3 signal data are untouched.

## When to use

Use Audio Align when a channel pair is consistently early or late against the picture — for example an EFM digital audio channel pair whose decode start-up (sync acquisition) left it slightly offset, or an imported WAV that was captured with a different lead-in than the video. Add one instance per channel pair that needs adjustment; multiple instances may target different channel pairs.

## What it does

The stage wraps the incoming frame representation and serves the target channel pair shifted by the requested offset, converted to a whole number of stereo pairs at the pipeline's synchronous 48 kHz rate (exactly 48 pairs per millisecond).

Each frame's audio window is assembled from the neighbouring frames' samples, with silence filling past either end of the frame range. Per-frame sample counts are unchanged — including the NTSC/PAL-M 1602/1601 audio frame sequence — so the channel pair remains spec-conformant synchronous audio.

Sample values and the sample rate are never changed; the shift is pure placement, no resampling.

A zero offset passes the input through unchanged. So does a target channel pair the input does not carry: the stage logs a warning and forwards audio and video untouched rather than interrupting the pipeline. The most common cause is a source that carries no audio at all (for example a plain TBC without an accompanying audio import), in which case the parameter dialog says so.

## Parameters

### channel_pair
The audio channel pair to shift, default `0`. Channel-pair numbers are 0-based, matching the CVBS container `_audio_<n>.wav` numbering. In the GUI this is a drop-down restricted to the channel pairs the input actually carries (shown as `<n> - <description>` where a description is present).

### offset_ms
Floating point milliseconds, default `0.0`, range ±3,600,000. Positive delays the audio relative to the video; negative advances it. The offset is rounded to the nearest whole stereo pair at 48 kHz (about 0.021 ms resolution).

## Tools

This stage has no interactive tools.

## Video systems

All four: PAL, NTSC, PAL-M and SECAM. SECAM audio follows PAL's cadence, 1920 stereo pairs a frame at 48 kHz.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
