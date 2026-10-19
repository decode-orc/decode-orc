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
  changes either selection to `pal2d` before decoding.
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
must match before Transform PAL keeps them as chroma. The default is `0.4`.
Higher values are more selective: fewer components qualify as chroma.

Leave it at the default unless a comparison exposes a specific problem:

- Raise it cautiously when noise or picture detail is being classified as
  chroma, producing false colour.
- Lower it cautiously when valid, fine chroma is being rejected.

Check saturation, coloured edges, and fine repeating detail after every change;
this is a similarity test, not a general quality control.

## SECAM decoder

SECAM sources are stored as PAL: the CVBS format and ld-decode-family TBCs
have no SECAM system, and SECAM shares the 625-line, 50 Hz structure and the
PAL 4fsc sampling. Select `secam` in a PAL project to decode them.

| Decoder | What it does | Use it when | Trade-off |
|---|---|---|---|
| `secam` | Band-pass separates the subcarrier, undoes the high-frequency "bell" precorrection, FM-demodulates each line, converts frequency to D'R or D'B, undoes the low-frequency precorrection, and takes the missing colour difference from the neighbouring lines of the same field (ITU-R BT.1700 Annex 1 Part C). | The source is SECAM, e.g. a videosynth project with `chroma_encoding: SECAM` or a SECAM capture stored as PAL. | Luma loses the 3.3–5.4 MHz band where the SECAM subcarrier sits; fine luma detail in that band shows as false colour unless `secam_cross_colour_suppression` is enabled; vertical colour resolution is halved, as in any SECAM receiver. |

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
applies. ### SECAM temporal chroma

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

`chroma_gain` scales the decoded colour and
`luma_nr` applies as usual; `chroma_phase` has no meaning for FM chroma and is
ignored.

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

`adapt_threshold` applies only to `ntsc3d`. Higher values strengthen the
candidate selector's bias toward non-1D candidates, including candidates from
nearby fields and frames. Keep the default `1.0` initially. Lower it if a
comparison suggests that motion is being treated as temporally similar;
increase it only when representative moving material remains clean and the
extra temporal separation is visibly useful.

`chroma_weight` is an advanced implementation control, not a saturation
control. In the current adaptive implementation it scales the same
spatial/temporal candidate preference used by `adapt_threshold`; it is best
left at its default `1.0` unless you are comparing renders on both static and
moving material. It has no practical effect with `ntsc3dnoadapt`, because that
mode bypasses candidate selection and always chooses the previous frame.

## Shared controls

| Control | Applies to | What it changes | Practical guidance |
|---|---|---|---|
| `chroma_gain` | PAL and NTSC colour decoders | Multiplies the decoded chroma components, changing saturation. | Start at `1.0`. Adjust only after decoder selection; it cannot fix false colour or bad separation. |
| `chroma_phase` | PAL and NTSC colour decoders | Rotates the chroma reference, changing hue. | Use small, global corrections after confirming the input standard. It cannot repair hue errors that vary across the picture. |
| `luma_nr` | All built-in decoder paths | Applies luma noise reduction through high-frequency coring. | Default `0.0` is off. Raise only enough to reduce objectionable luma noise, then inspect fine edges and texture for lost detail. |
| `chroma_nr` | NTSC colour decoders | Applies equivalent coring to the NTSC I/Q chroma components. | Default `0.0` is off. Use sparingly for chroma speckle/noise and check coloured detail for smearing. |
| `ntsc_phase_comp` | NTSC colour decoders | Measures the burst phase on each line and uses it while demodulating chroma. | Enabled by default. Keep it on unless an A/B comparison on the actual source shows more stable colour without it; it is especially relevant when NTSC phase alignment varies. |

`mono` does not use chroma gain, phase, or chroma noise reduction. Its current
Video Sink configuration passes the composite signal through as luma; it does
not enable a separate chroma-removal notch filter. It is therefore intended for
monochrome sources or diagnosis, not simply as a way to desaturate a colour
composite source.

## A repeatable comparison

1. Keep the source, Video Parameters, output format, and all tuning controls
   unchanged; begin with `luma_nr` and `chroma_nr` at `0.0`.
2. Render a short section containing fine coloured detail, sharp luma edges,
   a still shot, and motion. A decoder that is best on a title card may be poor
   on moving live footage.
3. Compare `pal2d` against both Transform modes for PAL/PAL-M, or `ntsc2d`
   against `ntsc3d` for NTSC. Check for false colour, dot crawl, colour bleed,
   and lost detail at the same locations.
4. Choose the decoder first. Then correct global saturation and hue with
   `chroma_gain` and `chroma_phase`, and add the minimum necessary noise
   reduction.
5. Record the chosen decoder and non-default controls in the project. A
   different capture of the same programme can need a different choice.
