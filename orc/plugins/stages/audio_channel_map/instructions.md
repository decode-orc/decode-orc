# Audio Channel Map

Routes the left/right channels of audio channel pairs following SMPTE 272M. Use it to remove a channel pair, extract a channel as mono in place, or copy a channel as mono into another channel pair. One operation per stage instance; chain instances for compound routing.

## When to use

LaserDisc bilingual discs and tapes with independent linear/Hi-Fi programmes carry two unrelated mono programmes as the left and right channels of a single stereo pair. To split such a dual-mono pair into two independent, named mono pairs:

1. `copy_right_to_target` with target `new`, **Add description** ticked and description `French language` — appends a pair holding the right programme as mono.
2. `left_to_mono` on the original pair, **Add description** ticked and description `English language` — keeps the left programme in place.

(Or `delete` the original pair instead of step 2 if you routed both channels to new pairs first.)

The in-place `left_to_mono` / `right_to_mono` operations fix a one-sided recording without changing the pair count.

`vbi_mono_average` is for LaserDiscs whose analogue sound is mono over some or all of the disc. Such a disc records the same programme on both FM carriers, and says so in its VBI programme status. The two carriers' noise is independent, so the average of left and right is quieter than either channel, and a click on one channel is halved. On the mono stretches of Domesday Community North (silent passages, one capture and an 11-capture stack alike), the average is 1.0–1.3 dB quieter than either channel; the rest of the noise is common to both channels. Put it after the Stacker, on the analogue channel pair.

Per SMPTE 272M-1994 §6.4, a mono programme must occupy one channel of the pair while the other channel is silent (all zeros). Every mono operation here follows that rule — the chosen channel is placed on the left and the right channel is zeroed; a channel is never duplicated across both.

The operation is a pure per-sample channel remap on the synchronous 48 kHz 24-bit pipeline audio; timing and per-frame sample counts are untouched.

## What it does

The stage wraps the incoming frame representation and remaps channels on the fly. Video, dropout hints, EFM/AC3 signal data, and all untouched channel pairs pass through unchanged.

- `delete` removes the selected channel pair. The pair count drops by one and every later channel pair shifts down one index.
- `left_to_mono` / `right_to_mono` overwrite the selected pair in place with the chosen channel on the left and the right channel silenced (`[L, 0]` or `[R, 0]`). The pair is marked origin `DERIVED`.
- `copy_left_to_target` / `copy_right_to_target` read the selected (source) pair and write the chosen channel as mono to the **target** pair, leaving the source untouched. With target `new` the mono pair is appended (subject to the 8-pair limit); with an existing target that pair is overwritten. The written pair is marked `DERIVED`.
- `vbi_mono_average` reads each frame's VBI programme status (IEC 60857, lines 16–18) and, on the frames whose status says the sound is mono, replaces both channels of the selected pair with their mean (`[(L+R)/2, (L+R)/2]`). Every other frame passes through untouched: stereo, bilingual, the data frames of an LV-ROM disc, and frames whose status word is missing or fails parity. A frame counts as mono when at least one field's status says so and neither field's says otherwise. The mean goes to both channels, rather than the SMPTE 272M `[M, 0]` the other mono operations write, because the pair stays a stereo pair: the frames around a mono stretch keep both channels. The pair keeps its origin.

  A disc can say mono and still carry the programme on one carrier alone: on Dragon's Lair the left channel is near silent (−63 dBFS) or carries its own sound, and the mean would halve the programme or mix the two. So a frame is averaged only when its two channels also carry the same programme. Their levels must be within 6 dB, and either they correlate at 0.5 or better or the louder channel is at or below −50 dBFS. Below that level a mono frame is the carriers' independent noise, which doesn't correlate, so only the balance is checked. On Domesday Community North's mono stretches 99–100 % of frames pass, and the silent-passage gain is unchanged. On Dragon's Lair no frame louder than −50 dBFS passes. A frame that fails passes through untouched, and its picture isn't read for the status.

  Reading the status means reading the frame's video, so an audio-only export through this operation reads the whole picture too. On a single CVBS capture that took about 5 minutes for a full side, against seconds without it. The status applies only to the disc's analogue sound, so don't select an EFM or imported pair.

By default the resulting pair keeps its existing description. Tick **Add description** to give it a new one (see `set_description` / `description` below).

When the input cannot satisfy the operation — the selected channel pair does not exist, a chosen target pair does not exist, or appending would exceed the 8-pair limit — the stage passes its input through unchanged and logs a warning. Audio and video reach the output untouched; nothing downstream is interrupted. The most common cause is a source that carries no audio at all (for example a plain TBC without an accompanying audio import), in which case the parameter dialog says so.

## Parameters

### channel_pair
String, default `0`. The channel pair the operation reads from — the source for the copy operations, or the pair modified/deleted for the others. Channel-pair numbers are 0-based, matching the CVBS container `_audio_<n>.wav` numbering. In the GUI this is a dropdown restricted to the channel pairs the node's input actually carries (index plus description).

### operation
String, default `left_to_mono`. One of:

| Value | Effect |
|-------|--------|
| `delete` | Remove the channel pair; later pairs shift down one index |
| `left_to_mono` | Keep the left channel, silence the right (`[L, 0]`), in place |
| `right_to_mono` | Move the right channel to the left, silence the right (`[R, 0]`), in place |
| `copy_left_to_target` | Copy the left channel as mono to the target pair; source untouched |
| `copy_right_to_target` | Copy the right channel as mono to the target pair; source untouched |
| `vbi_mono_average` | On frames whose VBI programme status says mono and whose channels carry the same programme, replace both channels with their mean; other frames untouched |

### target_pair
String, default `new`. Destination for the copy operations (shown only when the operation is `copy_left_to_target` or `copy_right_to_target`). `new` appends a new channel pair; otherwise the mono channel overwrites the chosen existing pair. In the GUI this is a dropdown offering `new` plus the channel pairs the input carries.

### set_description
Boolean, default off. **Add description** — shown for every operation except `delete`. When off, the pair the operation produces keeps its existing description. Tick it to reveal the `description` field and give the pair a new name.

### description
String, default empty. Name for the channel pair the operation produces — the target pair for the copy operations, or the source pair for the in-place operations (e.g. `English language`). Shown only when **Add description** is ticked.

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
