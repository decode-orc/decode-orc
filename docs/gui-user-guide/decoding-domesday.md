# Decoding the Domesday discs

The BBC Domesday Project (1986) is published on PAL LaserVision discs:

- the **Community** disc, sides **North** and **South**;
- the **National** disc, sides **A** and **B**.

Community North, Community South and National A are CAV sides. Most of their content is single-frame stills (Ordnance Survey maps, photographs, text pages, pictures of data), with some held pictures and motion sequences. National B is a CLV side.

A single capture of a side can be decoded with a [Quick Project](quick-projects.md). The best result comes from capturing several discs of each side, ideally from more than one pressing, and building a project in which each stage deals with one thing the discs or the players did to the signal. This page explains what each of those stages is for and where it goes. The stage reference pages give every parameter.

## The project

For one side, with several captures:

```
Capture 1 ─ Frame Map ─ FM Beat Cancel ─┐
Capture 2 ─ Frame Map ─ FM Beat Cancel ─┼─ Source Align ─ Stacker ─ Dropout Correction ─ Picture Band Limit ─ Video Sink
Capture 3 ─ Frame Map ─ FM Beat Cancel ─┘                                            │
                                                                                     ├─ Audio Channel Map ─ Audio Sink
                                                                                     └─ EFM Decoder Sink
```

The stages fall into three groups:

- **Per capture:** the stages before Source Align. They undo what went wrong in that capture or what is particular to its disc's master: player skips, and the FM beat.
- **Stacking:** Source Align and the Stacker. They combine the captures, and the repeats of held pictures within each, into one signal with less noise and fewer dropouts.
- **After stacking:** the stages after the Stacker. They work on the combined signal: dropouts no capture could fill, noise above the picture band, and the chroma decode.

The order matters. [Does the order matter?](#does-the-order-matter) below says which stages must come before which, why, and which can go either way.

To run the same project over new batches of captures from a script, see [Stacking multiple discs](../cli-user-guide/multi-disc-stacking.md).

## Per capture

### Source

Use one **TBC Source** (ld-decode `.tbc`) or **CVBS Source** (`.cvbs`) per capture. A stack takes all its captures in one format. See [Source stages](stages/source-stages.md).

### Frame Map: player skips and repeats

A player that skipped or repeated tracks while the disc was captured leaves frames missing or doubled. Every capture then has its own frame numbering, and the Stacker would combine different pictures.

The Frame Map's **Disc Mapper** tool reads the picture numbers (CAV) or time codes (CLV) from the VBI and rewrites the capture's frame list:

- repeats are removed;
- gaps are padded with blank frames, which the Stacker ignores.

Run it on every capture. See [Frame Map](stages/transform-stages.md#frame-map) and [Disc Mapping](../technical/disc-mapping.md).

### FM Beat Cancel: the herringbone on the masters

The Domesday masters recorded a beat between the FM video carrier and the colour signal. It shows as a faint diagonal herringbone in flat saturated colour: the bars, menus and the seas of the maps.

- The beat is on the master, so every capture of that master carries the same beat, and stacking keeps it.
- Captures of different masters carry beats of independent phase. Once they are blended, one fit removes much less.

So the stage goes on each capture, before the Stacker. Run its **FM Beat Calibration** tool on each one first: it measures the master's carrier frequency, which the stage needs to within a few kHz. See [FM Beat Cancel](stages/transform-stages.md#fm-beat-cancel).

## Stacking

### Source Align: the same picture at the same frame

After mapping, the captures still start at different places on the disc. Source Align offsets each one so that output frame N is the same disc picture in every capture. Its **Source Alignment Analysis** tool works the offsets out from the picture numbers or time codes. It also measures where the set's picture band ends (`lumaCutoffMHz`), which Picture Band Limit uses later. See [Source Align](stages/transform-stages.md#source-align).

### Stacker: noise and dropouts

The Stacker combines the captures sample by sample. Noise falls, and a dropout on one capture is filled from the others. Settings worth knowing for Domesday:

- **`mode` `Weighted` and `group_pressings`** (both defaults). Captures of one pressing share the noise recorded into its stamper. The Stacker finds which captures share a pressing and counts each pressing about once, so the pressing with the most captures does not outvote the others.
- **`still_frame_stacking`.** On the CAV sides a still is often held for several frames. With this on, the Stacker also stacks the repeats within each capture. Each repeat was read from a different place on the disc, so it removes the stamper noise that stacking discs of one pressing cannot. On map stills, repeats took about 6.8 dB of noise out, against about 1.3 dB from stacking five captures without them. The Stacker also records how long each picture is held, which the Video Sink's decoder schedule uses (below).
- **`chroma_gain_match`** (default). The decode's high-frequency correction can leave one capture's chroma stronger than the others' over stretches of a side. This brings every capture to the same colour burst level before combining.

See [Stacker](stages/transform-stages.md#stacker).

## After stacking

### Dropout Correction: what stacking could not fill

Where every capture had a dropout, or the captures disagreed, the Stacker leaves the samples flagged. Dropout Correction fills them from the lines around them. Its default method, `interpolate`, keeps map lines and text whole on the Domesday captures, where copying a single line pastes a streak from four lines away. It goes after the Stacker so it only repairs what the other captures could not. See [Dropout Correction](stages/transform-stages.md#dropout-correction).

### Picture Band Limit: noise above the picture

The Domesday pictures end at about 5.4 MHz, but the decode passes everything up to the sample rate's limit. Above the picture there is only disc noise and the ringing after sharp edges.

- Picture Band Limit low-passes the luma there and leaves the colour as it is.
- Use the `lumaCutoffMHz` that Source Alignment Analysis measured, or the PAL default of 5.4 MHz.
- It goes after stacking and dropout correction, so it filters the finished signal once.

It changes only the active picture: the VBI lines, and the test signals on them, pass through unchanged. See [Picture Band Limit](stages/transform-stages.md#picture-band-limit).

!!! note "In development: Channel Equalise"
    The CAV Domesday sides carry an edge smear from mastering: a faint trail after every sharp edge, about a quarter of the step, decaying over a third of a microsecond. National B (CLV) does not have it.

    A **Channel Equalise** stage is being developed to remove it. It will sit beside Picture Band Limit, after stacking.

    Its calibration tool will measure the smear from the test signals on line 19. It will either set the stage's two values or report that the capture has no smear, in which case the stage is left out.

    This page will describe it once it is available.

### Video Sink: decoding the colour

The Video Sink separates luma and chroma and writes the video. The Domesday sides mix two kinds of material, which suit different decoders:

- **Held pictures and motion**, where the picture is the same, or changes slowly, over several frames. `transform3d` separates best here, because it uses the frames either side.
- **Single-frame stills**, where each frame is a different picture: most maps and photographs. Neighbouring frames are unrelated, so a 3D decoder would mix them, and a 2D decoder (`transform2d`) is the right choice.

**Decoder Type** `multiple` with a **Decoder Schedule** follows the material. Fed from a Stacker with `still_frame_stacking`, this schedule decodes the frames the Stacker found held with `transform3d` and everything else, single-frame stills included, with `transform2d`:

```text
transform2d, held:transform3d
```

Two more controls apply:

- **Noise reduction.** With `noise_reduction_mode` `still_only`, it averages only the frames the Stacker found repeated and leaves single-frame stills as decoded. Two maps of one area look alike to its own test, and the Stacker's view keeps them apart.
- **Chroma level.** Set `chroma_gain` and `chroma_phase` only after the decoder is chosen.

See the [Chroma decoder guide](stages/chroma-decoder-guide.md) and [Video Sink](stages/sink-core-stages.md#video-sink).

!!! note "In development: Domesday2D"
    A still-frame decoder for these discs is being developed. It is meant to decode each frame from its own two fields, more accurately than `transform2d`, on maps, text and photographs. Once it is available it will be the schedule's default for the single-frame stills.

### Audio and data

- **Analogue sound.** The analogue sound is mono over much of the discs, as the VBI programme status says. An **Audio Channel Map** with `vbi_mono_average`, after the Stacker, averages the two channels on those frames. The two FM carriers' noise is independent, so the result is quieter: 1.0–1.3 dB on Community North's mono passages. See [Audio Channel Map](stages/transform-stages.md#audio-channel-map).
- **The EFM channel.** Where a side's EFM channel carries data rather than digital audio, the **EFM Decoder Sink** in `data` mode writes its ECMA-130 sectors. Leave its `doubt_erasure_threshold` at 0. The Stacker's `efm_stacking` is best left at `Disabled`, which passes through the EFM of the capture with the fewest dropouts. See [EFM Decoder Sink](stages/sink-core-stages.md#efm-decoder-sink).

## Does the order matter?

Yes, for most of the chain. Each stage either needs something an earlier stage provides, or would spoil what a later stage relies on.

| Rule | Why |
|---|---|
| **Frame Map before Source Align** | Source Alignment Analysis works out one offset per capture. That is only possible once each capture's skips and repeats are gone: an unmapped capture drifts by a frame at every skip, so no single offset lines it up. |
| **Source Align before the Stacker** | The Stacker combines the frames with the same frame number from every input. It does not search for the matching picture, so the inputs must already agree on what each frame number is. |
| **FM Beat Cancel on each capture, before the Stacker** | The beat comes from the master, and each master's beat has its own phase. On one capture the stage fits one beat. In a stack of captures from different masters the beats are blended, the sum no longer follows any one master's phase, and one fit removes much less. |
| **Dropout Correction after the Stacker** | The Stacker fills a dropout with the real picture from the other captures, and needs the dropout flags to know which samples not to use. Dropout Correction replaces flagged samples with an estimate from the lines around them and removes the flags. Placed first, it would hand the Stacker guesses as if they were picture, and the other captures' real samples would only be averaged with them. The Stacker also flags samples the captures disagree on, for Dropout Correction to repair afterwards. |
| **Picture Band Limit after the Stacker and Dropout Correction** | Its low-pass filter reaches many samples either side. Before Dropout Correction it would spread each dropout's spike beyond the flagged region, where the repair no longer reaches. Before the Stacker it would also remove the high band the Stacker measures each capture's noise from. Once on the stack, it filters the signal once instead of once per capture. |
| **The Video Sink's `held:` schedule needs the Stacker's `still_frame_stacking`** | The Stacker records how long each picture is held, and the schedule reads that record. The stages between them pass it through. Without a Stacker that stacks stills, `held:` finds no held pictures and every frame takes the default decoder. |
| **Audio Channel Map (`vbi_mono_average`) after the Stacker** | It reads each frame's VBI programme status, and averages the sound the stack has already combined and aligned. |

Some stages can go either way:

- **FM Beat Cancel and Frame Map** can be in either order on a capture's branch. Frame Map only reorders frames, and FM Beat Cancel works one field at a time. What matters is that FM Beat Cancel stays on the capture's own branch.
- **Picture Band Limit** can sit anywhere between Dropout Correction and the sinks.
- **The in-development Channel Equalise stage** will sit after the Stacker and Dropout Correction, for the same reasons as Picture Band Limit: its correction also spreads each sample over the next few. Either order with Picture Band Limit gives the same result. Neither stage changes the test signals on line 19, so Channel Equalise's calibration measures the same thing in either position.

With a single capture there is no Source Align or Stacker. The chain is Source → Frame Map → FM Beat Cancel → Dropout Correction → Picture Band Limit → sinks, and the rules above still apply. Add a Stacker on its own with `still_frame_stacking` on to stack the repeats of held pictures within the capture.

## Checking the result

- **Preview.** Use the preview on any stage to see what it changed. Select the stage before and after it, and compare the same frame.
- **Test Card F.** Where a side holds a test card, it is a quick check of the decoder and of the colour level.
- **A short section first.** Before a whole side, render a short section with maps, text and a photograph.
