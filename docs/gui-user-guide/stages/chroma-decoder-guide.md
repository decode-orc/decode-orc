# Chroma decoder guide

The Video Sink's **Decoder Type** setting separates a composite video signal
into luma and chroma before it is written as colour video. This guide explains
the built-in decoder choices, their trade-offs, and the controls that affect
them. It applies to the Video Sink's `decoder_type` parameter; externally
installed decoder stages document their own settings.

Chroma decoding cannot correct a wrong video standard, poor time-base
correction, or a bad colour burst. Set the project format and input type
correctly first, then compare decoder choices on representative material.

## Start here

| Input | First choice | When to try another choice |
|---|---|---|
| Composite PAL or PAL-M | `pal2d` for a quick, robust baseline | Compare `transform2d` and `transform3d` when colour/luma separation matters more than render time. |
| Composite NTSC | `ntsc2d` | Compare `ntsc3d` on static or slowly changing material where extra separation is worth the cost. |
| Separate Y/C | `pal2d` for PAL/PAL-M; `ntsc2d` for NTSC | The luma and chroma are already separate, so the composite comb/Transform distinction largely does not apply. |
| Genuinely monochrome composite | `mono` | Use a colour decoder only when the source actually carries a colour subcarrier. |

The defaults are deliberately conservative: `pal2d` for PAL/PAL-M and
`ntsc2d` for NTSC. They do not need temporal context and make a useful
reference render before trying the more expensive 3D modes.

## Composite and Y/C input

The PAL and NTSC decoder families are not interchangeable. Use the family
matching the project video format; PAL-M uses the PAL-family choices.

The choices matter most for **composite** input, where luma and chroma occupy
the same signal and must be separated. A separate **Y/C** source has already
done that separation:

- `transform2d` and `transform3d` cannot operate on Y/C input. The Video Sink
  changes either selection to `pal2d` when the export starts (the Decoder Type
  setting itself is switched to `pal2d`), and decodes any Transform ranges of a
  `multiple` schedule with `pal2d` too. The parameter description in the
  stage's parameter dialog says so when the input is Y/C.
- The NTSC decoder's Y/C path skips the 1D, 2D, and 3D comb stages and
  demodulates the supplied chroma channel directly. Choosing `ntsc1d`,
  `ntsc2d`, `ntsc3d`, or `ntsc3dnoadapt` therefore does not select a different
  comb filter for Y/C input.

For Y/C, use the normal `pal2d` or `ntsc2d` setting and concentrate on burst
quality, `chroma_gain`, `chroma_phase`, and noise reduction instead.

## PAL and PAL-M decoders

| Decoder | What it does | Use it when | Trade-off |
|---|---|---|---|
| `pal2d` | Uses the PALcolour two-dimensional FIR filter. This is a conventional PAL decoder rather than a Transform PAL filter. | You need the default baseline, a quick render, or a tolerant option for a poor or non-standard signal. | Less effective luma/chroma separation than the Transform filters on material that suits them. |
| `transform2d` | Uses a frequency-domain Transform PAL filter within one field. It identifies chroma by its expected frequency-domain symmetry. | You want Transform PAL separation without using information from other fields. It is the first Transform mode to compare against `pal2d`. | Slower than `pal2d`; its result still depends on the current field's signal quality. |
| `transform3d` | Extends the Transform PAL filter across multiple fields, adding temporal information to the frequency-domain separation. | You are making a quality-focused render and have checked both still and moving scenes. | The most expensive PAL option and needs surrounding fields. Motion, dropouts, or unstable colour can change its artefacts relative to `transform2d`; compare rather than assuming it always wins. |

Transform PAL separates likely chroma before the PAL decoder reconstructs U/V.
It is normally the quality-oriented path. `transform3d` can retain more useful
colour detail on stable content, while `transform2d` avoids relying on temporal
similarity. Neither Transform mode is available for Y/C input.

### Simple PAL

`simple_pal` applies only to `transform2d` and `transform3d`. It replaces the
normal PALcolour two-dimensional U/V post-filter with a one-dimensional UV
filter.

- Leave it **off** for the normal Transform PAL result.
- Turn it **on** only when the source's PAL phase is stable and preserving
  vertical colour detail is more important than phase-error tolerance.
- Turn it back **off** if adjacent lines acquire different hues (Hanover bars)
  or the colour becomes less stable.

It is not a faster substitute for `pal2d`, and it has no effect with that
decoder.

### Transform threshold

`transform_threshold` controls how closely two reflected frequency components
must match before Transform PAL keeps them as chroma. The range is `0.0` to
`1.0` and the default is `0.4`. Higher values are more selective: fewer
components qualify as chroma. Like `simple_pal`, it is offered only for
`transform2d`, `transform3d` and `multiple`.

Leave it at the default unless a comparison exposes a specific problem:

- Raise it cautiously when noise or picture detail is being classified as
  chroma, producing false colour.
- Lower it cautiously when valid, fine chroma is being rejected.

Check saturation, coloured edges, and fine repeating detail after every change;
this is a similarity test, not a general quality control.

## SECAM decoder

SECAM sources are stored as PAL: the CVBS format and ld-decode-family TBCs
have no SECAM system, and SECAM shares the 625-line, 50 Hz structure and the
PAL 4fsc sampling. Select `secam` in a PAL project to decode them; it is not
offered in PAL-M or NTSC projects, and the decoder refuses any source that is
not 625-line PAL.

| Decoder | What it does | Use it when | Trade-off |
|---|---|---|---|
| `secam` | Band-pass separates the subcarrier, undoes the high-frequency "bell" precorrection, FM-demodulates each line, converts frequency to D'R or D'B, undoes the low-frequency precorrection, and takes the missing colour difference from the neighbouring lines of the same field (ITU-R BT.1700 Annex 1 Part C). | The source is SECAM, e.g. a SECAM capture stored as PAL, like from the vhs-decode project. Or a videosynth project with `chroma_encoding: SECAM` (which is not implemented yet, but might be available in a future release). | Luma loses the 3.3–5.4 MHz band where the SECAM subcarrier sits; fine luma detail in that band shows as false colour unless `secam_cross_colour_suppression` is enabled; vertical colour resolution is halved, as in any SECAM receiver. |

Which lines carry D'R and which D'B is detected per field from the undeviated
subcarrier on the back porch (line identification, BT.1700 Part C item 13):
4.40625 MHz on D'R lines, 4.25 MHz on D'B lines. A field without a usable
reference is decoded without colour.

With `secam_field_identification` (on by default) the decision is taken
from the field identification lines instead (frame lines 7–15 and 320–328,
BT.1700 Part C item 13): their trapezoid starts at the rest frequency just
after line blanking and settles at +350 kHz on D'R lines and −350 kHz on D'B
lines. Only lines with that ramp shape count, so picture or data content on
those lines cannot pose as identification signals. This also decodes sources whose back-porch reference
is missing or damaged. Fields without usable identification signals — only
sources that keep the vertical interval carry them, and they are often
stripped in programme exchange — fall back to the back-porch reference. Turn
it off to rely on the back-porch reference alone.

Cross-colour — fine luma detail at the subcarrier frequency decoding as
colour — can be reduced in three ways (see the switch below). Chroma is taken through a tight
3.75–4.95 MHz band-pass, while luma has its own wider 3.3–5.4 MHz band-stop
so no subcarrier residue remains in the picture. Where the demodulated
subcarrier envelope departs from the field's typical level by more than a
factor of 1.3 (fully suppressed at 1.8), or its frequency leaves the
3.9–4.756 MHz range SECAM allows, the colour there is faded out, because an
intact FM subcarrier has a constant envelope and stays within that range.
Finally the decoded colour difference is band-limited to 1.3 MHz.

The first two measures are switched by `secam_cross_colour_suppression`, which
is off by default: the decoder then uses the plain wide band-pass, keeping
genuine colour on very fine detail but showing cross-colour. Turn it on when
fine luma detail produces false colour; the 1.3 MHz band limit always
applies. 

### SECAM temporal chroma

Every SECAM line carries only one colour difference, D'R or D'B, so the other
one has to come from somewhere else. A frame has 625 lines, an odd number, so
every line swaps between D'R and D'B from one frame to the next. Taking the
missing component from the lines above and below therefore locates a
horizontal colour edge only to within one line, and in alternate frames on
opposite sides: strongly red or blue edges visibly jump up and down by a line
every two fields (for example on a 75 % bars-over-red test pattern).

`secam_temporal_chroma` (on by default) removes this: where the luma shows
the picture has not changed since the previous frame, the missing component
is taken from the same line of that frame, which carried it. Moving areas
fall back to the neighbouring lines. Turn it off if motion produces colour
trails, or to halve the SECAM decoding time.

`chroma_gain` scales the decoded colour and `noise_reduction` applies as usual;
`chroma_phase` has no meaning for FM chroma and is ignored.

## NTSC decoders

| Decoder | What it does | Use it when | Trade-off |
|---|---|---|---|
| `ntsc1d` | Uses a line-local band-pass separation. It cannot distinguish luma detail at the subcarrier frequency from chroma very well. | Diagnostic renders, rapid comparisons, or a fallback when no spatial/temporal comparison is wanted. | Usually the weakest colour quality: dot crawl and cross-colour/luma leakage are expected. |
| `ntsc2d` | Uses an adaptive three-line comb within the field, comparing the neighbouring lines before blending them. | The general starting point and the current NTSC default. It gives a substantial improvement over 1D without temporal processing. | Cannot use matching information from adjacent fields or frames. |
| `ntsc3d` | Builds on the 1D and 2D results, then adaptively selects the best matching candidate from nearby lines, fields, and frames. | Static or slowly changing content, animation, and quality-focused renders where the additional compute cost is acceptable. | Requires surrounding frames and is slower. The adaptive chooser is designed to avoid bad temporal matches, but moving material must still be checked for artefacts. |
| `ntsc3dnoadapt` | Performs 3D separation against the previous frame without the adaptive candidate selection. | Controlled comparisons, debugging, or unusually stable content where you intentionally want the fixed previous-frame behaviour. | Motion or a bad previous frame can turn luma differences into colour artefacts. It is not the normal quality choice. |

The 3D mode does not blindly average whole frames. For each sample, the
adaptive mode evaluates spatial and temporal candidates and can retain the 2D
result when it is the better match. `ntsc3dnoadapt` disables that protection and
always uses the previous-frame candidate.

### NTSC 3D controls

Both controls are offered only in NTSC projects, range from `0.0` to `10.0`,
and default to `1.0`.

`adapt_threshold` applies only to `ntsc3d`. Higher values strengthen the
candidate selector's bias toward non-1D candidates, including candidates from
nearby fields and frames. Keep the default `1.0` initially. Lower it if a
comparison suggests that motion is being treated as temporally similar;
increase it only when representative moving material remains clean and the
extra temporal separation is visibly useful.

`chroma_weight` is an advanced implementation control, not a saturation
control. In the current adaptive implementation it scales the same
spatial/temporal candidate preference used by `adapt_threshold`, so raising
it has the same effect as raising `adapt_threshold`: line, field and frame
candidates are favoured more strongly over the same-line 1D ones. It is best
left at its default `1.0` unless you are comparing renders on both static and
moving material. It has no practical effect with `ntsc3dnoadapt`, because that
mode bypasses candidate selection and always chooses the previous frame.

## Shared controls

| Control | Applies to | What it changes | Practical guidance |
|---|---|---|---|
| `chroma_gain` | PAL, SECAM and NTSC colour decoders | Multiplies the decoded chroma components, changing saturation. Range `0.0`–`10.0`, default `1.0`. | Start at `1.0`. Adjust only after decoder selection; it cannot fix false colour or bad separation. |
| `chroma_phase` | PAL and NTSC colour decoders (ignored by `secam`) | Rotates the chroma reference, changing hue. Range −180 to 180 degrees, default `0.0`. | Use small, global corrections after confirming the input standard. It cannot repair hue errors that vary across the picture. |
| `noise_reduction` | All built-in decoder paths | Range `0.0`–`12.0` dB. Picture that stays the same from frame to frame is averaged over successive frames, while moving picture, cuts and a disc of stills (every frame a different picture) are smoothed within each frame instead, unless `noise_reduction_mode` is `still_only`. | Default `0.0` is off. On film and video, start at 6 and raise it while the still areas stay free of trails behind slow, low-contrast movement; `noise_reduction_motion` below 1 protects such movement. On a disc of stills every frame gets the within-frame filter only, which tops out near 6 dB, so a higher setting changes nothing there. |
| `noise_reduction_mode` | All built-in decoder paths, when `noise_reduction` is above 0 | `full` (default) or `still_only`. `still_only` keeps the averaging over frames where the picture is held and drops the within-frame filter, so moving picture, cuts and single-frame stills are left as decoded. | Use `still_only` on a disc of stills whose stills you want untouched: held title and test cards are still cleaned. Fed from a Stacker with `still_frame_stacking`, it averages only the frames the Stacker found repeated. |
| `noise_reduction_motion` | All built-in decoder paths, when `noise_reduction` is above 0 | Multiplies the motion thresholds that decide what counts as still picture. Range `0.5`–`4.0`, default `1.0`. Above 1 treats more change as still (more averaging, more risk of trails); below 1 the reverse. | Leave at `1.0`; lower it if slow, low-contrast movement leaves trails. |
| `ntsc_phase_comp` | NTSC colour decoders (offered only in NTSC projects) | Measures the burst phase on each line and uses it while demodulating chroma. | Enabled by default. Keep it on unless an A/B comparison on the actual source shows more stable colour without it; it is especially relevant when NTSC phase alignment varies. |

`mono` does not use chroma gain or phase. Its current
Video Sink configuration passes the composite signal through as luma; it does
not enable a separate chroma-removal notch filter. It is therefore intended for
monochrome sources or diagnosis, not simply as a way to desaturate a colour
composite source.

### Noise reduction limits

- The frame-to-frame average is recursive: each output frame carries part of
  the one before it. Movement too faint to be told apart from noise, such as a
  slow fade or a dim edge drifting across a dark background, is averaged as if
  it were still, so it lags slightly behind for a few frames. Lower
  `noise_reduction_motion` if a trail is visible.
- The within-frame filter reaches just under 6 dB. Above that setting, still
  areas keep getting cleaner while moving areas, cuts and stills stay where
  they were, so on busy footage the moving parts of the picture look noisier
  than the still parts.
- The within-frame filter takes its noise level from the picture itself, so a
  densely detailed still, such as a map or a page of fine print, reads as
  noisier than it is and loses a little of its finest low-contrast lines.
- For other kinds of noise reduction, the FFmpeg denoisers (for example
  `hqdn3d` or `nlmeans`) remain available through `video_filter` in FFmpeg
  output mode, with or without this control.

## Different decoders for different frames

Some sources mix material that suits different decoders. A LaserDisc such as
Domesday has motion sequences, where `transform3d` separates best, alongside
long runs of single-frame stills, where `transform2d` avoids blending
information from the unrelated pictures either side.

Set **Decoder Type** to `multiple` and give the ranges in **Decoder
Schedule**:

```text
transform3d, 1001-1500:transform2d, 2001-2400:transform2d
```

- Each entry is `first-last:decoder`, or `first:decoder` for one frame. Frame
  numbers are those of the frames fed to the Video Sink, 1-based as in the
  preview.
- A decoder without a range — `transform3d` above — decodes every frame the
  ranges do not list. Without one, every frame must be inside a range, or
  the export is refused before it starts.
- Entries may come in any order, but ranges cannot overlap and at most one
  decoder may be given without a range. The parameter dialog accepts only the
  decoders the Decoder Type list offers for the project (not `multiple`). A
  schedule that cannot be parsed keeps the node's status yellow, and a decoder
  that does not suit the source (for example in a hand-edited project file) is
  refused when the export starts, before anything is written.
- Each frame is decoded exactly as an export with its decoder alone would
  decode it, including that decoder's surrounding-field context. The preview
  shows each frame with its scheduled decoder.
- The decoder-specific controls (`transform_threshold`, `simple_pal`, the
  SECAM and NTSC 3D settings) stay available and apply to every range using
  that decoder. `noise_reduction` runs over the whole output, across range
  boundaries.

Use the preview to find where the material changes, then check a few frames
either side of each boundary in the export.

On a disc stacked with the Stacker's `still_frame_stacking`, the schedule can
follow the pictures instead of hand-written ranges:

```text
transform2d, held:transform3d
```

The Stacker reports, frame by frame, how long each picture is held, from the
repeats it stacks. `held:` decodes a frame with its decoder only where the
picture is held for at least as many frames either side as that decoder reads
(4 for `transform3d`), so the 3D filter never mixes two pictures, and every
other frame, single-frame stills included, takes the default. Ranges still
win, so a range can overrule it where the Stacker's view is wrong.

## A repeatable comparison

1. Keep the source, Video Parameters, output format, and all tuning controls
   unchanged; begin with `noise_reduction` at `0.0`.
2. Render a short section containing fine coloured detail, sharp luma edges,
   a still shot, and motion. A decoder that is best on a title card may be poor
   on moving live footage.
3. Compare `pal2d` against both Transform modes for PAL/PAL-M, or `ntsc2d`
   against `ntsc3d` for NTSC. Check for false colour, dot crawl, colour bleed,
   and lost detail at the same locations.
4. Choose the decoder first. Then correct global saturation and hue with
   `chroma_gain` and `chroma_phase`, and add the minimum necessary
   `noise_reduction`.
5. Record the chosen decoder and non-default controls in the project. A
   different capture of the same programme can need a different choice.
