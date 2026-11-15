# Audio Import

Attaches an external WAV or FLAC file to the pipeline as a new audio channel pair. Use it to bring separately captured or restored audio — a cleaned-up soundtrack, a commentary recording, an externally decoded recording, the audio capture record an RF capture system saves alongside a tape's video — into the project so sinks can select, embed, or export it alongside the decoder-provided audio.

## When to use

Use Audio Import whenever audio that did not come from the video capture should travel with the video through the pipeline. The imported channel pair is appended after all existing pairs (existing pair numbers are unchanged) and reports origin `IMPORTED`. If the recording needs nudging into sync with the picture, follow with an Audio Align stage targeting the new channel pair.

## What it does

The stage wraps the incoming frame representation and appends one audio channel pair served from the audio file. Video, dropout hints, EFM/AC3 signal data, and all existing audio channel pairs pass through untouched.

The file can be a WAV or a FLAC file; the stage tells them apart by their content, not by the file name.

- A WAV must be RIFF/WAVE or RF64 (the 64-bit form for WAV files larger than 4 GiB), PCM, stereo, 16- or 24-bit, at any sample rate.
- A FLAC file must be stereo and 24-bit or narrower, at any sample rate. A FLAC file with an ID3v2 tag in front is accepted. 32-bit FLAC is refused, because the pipeline carries 24-bit audio and the extra bits would be lost.

Anything else fails validation with a reason. A RIFF/WAVE file larger than 4 GiB is refused: its 32-bit header cannot give the size of its samples (a tool that writes such a file without switching to RF64 leaves a header that describes only part of it), so the import would otherwise lose most of the audio.

On import the material is converted to the pipeline's only audio form — 48 kHz synchronous (frame-locked) 24-bit stereo, per SMPTE 272M:

- 16-bit samples are widened to 24-bit (exactly reversible); 24-bit samples are used as-is. FLAC samples narrower than 24 bits are widened the same way.
- The stream is resampled from the file's sample rate to exactly 48000 Hz (high-quality SoXR conversion). A 48000 Hz input passes through unchanged and is read straight from the file for each frame. A FLAC file is decoded as it is read; jumping to a distant frame seeks in the file rather than decoding everything before it. Any other rate is converted as frames are read, only as far ahead as needed, into a cache file on disk (about 1.4 GB per hour, removed when the stage's output is released), so memory use does not depend on the length of the recording. The cache goes in the per-user cache directory (`~/.cache/decode-orc/audio` on Linux, `~/Library/Caches/decode-orc/audio` on macOS, `%LOCALAPPDATA%\decode-orc\cache\audio` on Windows); set the `ORC_AUDIO_CACHE_DIR` environment variable to put it elsewhere (or `ORC_CACHE_DIR`, which moves every decode-orc scratch file, to put it in an `audio` directory under that).
- The result is segmented into per-frame blocks following the audio frame sequence (PAL: 1920 stereo pairs per frame; NTSC/PAL-M: 1602/1601 pairs over a 5-frame sequence). Material shorter than the project is padded with silence; excess is truncated.

Appending the channel pair counts toward the pipeline's limit of 8 channel pairs; the stage fails validation if the input already carries 8 pairs.

## Parameters

### wav_path
File path, required. The WAV or FLAC file to attach. Shown as **Audio File** in the parameter dialog; the parameter keeps its original name so existing projects load unchanged.

### pair_name
String, default empty. Human-readable name for the channel pair (shown by sinks, e.g. as the stream title in Video Sink). Empty uses the audio file's name.

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
