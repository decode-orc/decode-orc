# Stacker

Combines between 1 and 16 frame-aligned input sources into a single superior output by combining the pixel data available across all sources. When only one input is connected the stage acts as a passthrough, unless `still_frame_stacking` is on. With multiple inputs, corresponding pixels from each source are combined per pixel using the selected stacking mode (noise-weighted, mean or median), recovering pixels that are in dropout in one source but valid in another.

## When to use

Use Stacker after Source Align when you have multiple captures of the same LaserDisc that contain dropouts. Example: three captures with heavy surface damage — Stacker overlaps each capture's pixel data and, using differential dropout detection, can recover a pixel that is corrupted in one or two captures as long as at least one source has a clean reading. Three or more sources are required for differential dropout detection.

Turn on `still_frame_stacking` for discs that hold still pictures over many frames, such as slides, maps and menus on CAV discs. The repeats of a still within each capture are then stacked as well, which reduces noise and fills dropouts even from a single capture. It also finds the held drawings in limited animation, and leaves the frames where a drawing moves (see *Limits* under `still_frame_stacking`).

## What it does

For each output frame, Stacker fetches the frame with the *same frame id* from every input source and applies the selected stacking mode pixel by pixel. It relies on Frame Map and Source Align having put the sources on a common frame numbering; it does not search for a matching frame itself. Earlier versions re-aligned each source by looking for a frame whose measured colour frame index matched the reference's, searching up to four frames either way. That measurement is taken against each source's own sample grid, so a source whose line starts sit one sample from the reference's reports a mismatch on the same picture and gets pulled to a neighbouring frame — displacing its video and its audio together. Sample-grid differences are corrected by `sample_align` instead, which shifts samples rather than changing frame. The output covers every frame id that any input carries, so pictures at the end of the longer captures are kept even when the first input stops earlier. Each output frame is described by the first input that has a real picture at that id; it is marked as padding only when every input is padding there, and then reads back as that padding frame. Before combining, each input's two fields are compared with the per-sample median of all the inputs, on low-passed luma: an input whose field correlates with the others at less than 0.8 carries a different picture there (a player skip in mid-frame, or any remaining mapping error) and is left out of that field, while its other field is still used. With only two inputs there is no majority to say which one is wrong, so when they disagree both are kept and the whole field is marked as dropout for a later stage to repair. Fields with too little picture detail to judge, such as black frames, are not checked. Each exclusion is logged with the frame, field and input. Every mode is an averaging or median policy over the usable source values for that pixel — no mode selects a whole "best" source or frame. A value a source flags as dropout is not usable; where at least one source has a clean value, only the clean values are combined. Where every source is flagged, differential dropout detection (composite sources, 3 or more inputs) keeps the flagged values within 10 % of their median, as ld-disc-stacker does, and combines those; failing that the pixel is black, or with `passthrough` the combination of the flagged values. Either way the output pixel stays marked as dropout (severity 50). Audio and EFM data can be combined independently of video using their own stacking settings. AC3 RF is not stacked: each frame's symbols come from the source with the fewest dropouts there that carries AC3 RF, so they continue past the end of the first input. The stage caches stacked frames in an LRU cache to avoid redundant recomputation during preview navigation.

Note that stacking reduces noise only when the sources contain independent noise (separate captures). Sources that are identical apart from dropouts will stack to the same underlying signal, so signal-to-noise measurements will not improve on dropout-free picture areas.

## Parameters

### mode (string)
Stacking algorithm, applied per pixel to the usable (non-dropout) values across all sources. Values: `Weighted`, `Mean`, `Median`. Default: `Weighted`. See *Choosing a video mode* below.

| Mode | Behaviour per pixel |
|------|--------------------|
| `Weighted` | Mean of the values within `reject_sigma` times the noise scale of the median, each weighted by its source's measured noise and pressing (see below), rounded to the nearest code. With 3 or more values, those further from the median are rejected; with 2, both are always used. Marks samples the sources do not agree on as dropout (see below). |
| `Mean` | Arithmetic mean of all usable source values. |
| `Median` | Median of the usable source values (mean of the middle two for an even count). |

`Mean` and `Median` divide with truncation, which biases their output about half a code low; they keep doing so, so saved projects reproduce exactly. `Weighted` rounds, with ties to even.

**The mode is stored with the project.** `Smart Mean` and `Auto` are deprecated. `Smart Mean` averaged the values within a fixed threshold of the median (`smart_threshold`, default 15 codes), and `Auto` was `Smart Mean` with 3 or more usable values and `Mean` otherwise; a fixed threshold rejects most good values on real captures (see *Choosing a video mode*), and `Weighted` does the same job with a threshold set from the measured noise. A project whose Stacker stored `Smart Mean` or `Auto`, or stored no mode at all (which ran `Auto`), is loaded as `Weighted`; the log warns that the node's output will differ from earlier runs, and the project is marked as changed so saving it stores `Weighted`. `smart_threshold` is no longer a parameter: a stored value is dropped when the project is loaded. There is no way to reproduce an old `Smart Mean` or `Auto` result exactly. `Smart Neighbor` and `Neighbor` were never implemented and always stacked as `Median`; they are no longer offered, and a project that stored one is loaded as `Median`.

**Measuring each source's noise.** `Weighted` measures every source's noise in each 500-frame segment, the first time a frame in it is stacked, from the same spread of frames as `sample_align`. Once the sample grids are aligned the sources carry the same picture, so the difference between two sources is noise alone, but only where the picture cannot leak into it. Captures through different players and decodes differ by a few per cent in gain and a few codes in level, and a gain difference leaves every edge and grating in the pair's difference. On a detailed picture that leak once doubled the measured noise of the three captures of a disc whose gain differed from the rest, and they were weighted out of the stack. So the pair noise is measured in blocks of 32 samples by 8 lines of active picture, on a decimated grid and away from dropouts. Only blocks where the picture common to all the sources is flat count: on a blank frame that is every block, on a test card the plain areas. Only frames whose flattest blocks are about as flat as the flattest frame's count, because a frame of film grain or saturated colour has no flat block at all. Within a block the difference is taken about its own mean, so a level difference never counts. What little picture a flat block holds is taken out using the pair's luma gain difference, which the stage measures from low-passed luma against the common picture. A block far noisier than the pair's typical block holds sparkle and is left out. If too few flat blocks are found, a pair falls back to the variance of its differences over the whole decimated picture with a robust clip. The debug log records each source's luma gain and level against the others, and any pairs that fell back.

Captures of one side usually come from more than one pressing, and every disc of a pressing carries the noise recorded into its stamper. Two discs of one pressing therefore differ only by their own noise, while discs of different pressings also differ by both pressings' noise. The stage groups the sources by pressing from that pattern: for two sources of one pressing, their differences with each third source differ by the same amount whichever third source is taken, while for sources of different pressings that amount jumps between third sources of one pressing and of the other. Pressings do not change within a side, so the sources are grouped once, from 16 frames spread across the whole stack: a short stretch where picture detail leaks into some pairs' differences would otherwise look like a pressing and split one. Each segment of 500 frames then solves each source's own noise and each pressing's shared noise from all the pairs, and weights the sources to give the least noise in the combined picture. Each pressing counts about once however many of its discs are stacked, and within a pressing the cleaner discs count more. Weighting each source by its total noise instead would mark the discs of an under-represented pressing as noisier and pull the stack towards the larger pressing, when shared noise only averages out across pressings. With no shared pressing the weights reduce to 1/σ² of each source's noise. A pair's variance is measured to an accuracy proportional to its size, so the solve weights each pair by the inverse of the variance it models for it. That keeps one source whose differences carry something besides noise, such as chroma that does not line up with the others over a stretch, from steering the rest: it is weighted down, and the others keep their usual noise.

Stampers are made from mothers, and the stampers of one mother share its noise as well as each carrying their own, so pressings fall into families. Two discs of different pressings of one family differ by both pressings' noise, and discs of different families by both families' noise as well. The pressing test sees two pressings of one family as unrelated as any other two, so the stage finds the families by fit: starting from every pressing in a family of its own, it merges two families while the merge makes the model explain the pair variances clearly better (see *Checking the model*). The pair variances cannot always say which two to merge: with four groups P, Q, R and S, merging P with Q and merging R with S explain them equally well, yet weight the discs in opposite ways. Every disc of a title went through the same generations, master to mother to stamper, so the noise each carries in all should be about the same, and the stage takes the merge that leaves the sources' σ most alike. On Community South the eight discs of one family form two pressings. No grouping without families explained their pair variances to better than 45 %, and the stacks fell back to equal weights. With the family found, the model fits to 5–18 %, and the three discs outside the family carry about 60 % of the weight, as their independent noise deserves. A family only shows beside at least two other groups; with fewer, its noise cannot be told from the other groups'. The weights for families are those that give the least noise in the combined picture, as for pressings: each family counts about once however many of its pressings and discs are stacked.

A disc that is alone in its pressing cannot be told from one that is merely noisy, as its pressing noise looks like its own, so it is weighted as a noisy disc. Pressings can only be told apart with 4 or more sources; with 3 or fewer the sources are weighted by their noise alone. With two sources the pair cannot be split, so each is given half.

**Checking the model.** The weights are the best possible only if the pressings are right, and under its own model they can never do worse than an equal mean, so the model cannot vouch for itself. What the stage can check is whether the model explains the measurements it was fitted to. With 3 or more sources it compares the pair variances the fitted model implies with the measured ones. A right model fits every pair to within a few per cent. If any pair misfits by more than 35 %, the segment is stacked with equal weights instead, and each source's σ is half the mean variance of its pairs, the spread of its samples about the others' that rejection needs. This happens when the pressings or families are wrong, for example when the discs of one pressing are split, or when `group_pressings` is off for sources of more than one pressing. The log warns once, naming the worst pair and its misfit. Otherwise it records the model's predicted noise for the stack with the fitted weights, with equal weights and for the best single source, and how well it fits.

A segment that cannot be measured holds the nearest segment that could, as `sample_align` does; a source never measured nearby takes the typical figures of the others, and with nothing measured every source is given σ = 10 codes and equal weight. The log records the pressings and any families once, when they are found, and each source's σ and share of the weight whenever a σ changes by more than a quarter, as a `weights` report record (see *Report records*); the debug log adds the same figures for every segment, and the pair variances behind them. Source Align's Source Alignment Analysis makes the same measurement, and reports the groups before you stack, along with each source's level and any source whose fine detail is soft. For YC sources the noise is measured on luma and the same weights are used for chroma.

A source's σ here is its noise against the picture common to all of them, its own plus its pressing's. The noise scale is 1.2 times the median σ of the sources, so at the default `reject_sigma` of 5 a value is rejected when it lies more than 6 median σ from the median: 28 codes on a typical clean set, about 60 on a noisy one.

**Marking low-confidence samples.** `Weighted` marks an output sample as dropout with severity 25 when the sources do not agree on it: when no majority of the usable values lies within the rejection distance of the result, or when exactly two usable values differ by more than that distance. Severity 50 remains for samples that no source could supply. Dropout Correct can then repair the marked samples; with the other modes they pass downstream unmarked. Only the active picture is marked, as for still-frame copies (see *Copies are used only in the active picture*): its lines, and on them only the active video samples. On the VBI lines, NTSC lines 21 and 284 and in horizontal blanking the sources part on the edges of the data and of sync, a fraction of a sample apart, and the marks would only invite a repair the data cannot take. On 61 frames of a four-disc Dragon's Lair stack, about 3,300 of the marked runs on picture lines lay wholly in blanking.

### reject_sigma (double)
`Weighted` only: how far from the per-sample median, in multiples of the noise scale, a value may lie and still be used. The same distance decides which samples are marked as low-confidence. Range: 2–10. Default: `5`.

### group_pressings (bool)
`Weighted` only: find which sources share a pressing, as described under *Measuring each source's noise*, and count each pressing about once. Default: `true`. Turn it off for sources you know to be one pressing, such as when Source Align's Source Alignment Analysis puts them all in one group but the Stacker's log reports more than one: every source is then taken as one pressing and weighted by its own noise alone, while the noise measurement and rejection stay as they are. Leave it on when the sources may come from more than one pressing: with it off, one pressing cannot explain the noise between the pressings, so the model check (see *Checking the model*) fails and the sources are stacked equally, where the right pressings would count each pressing about once.

### no_diff_dod (bool)
Disable differential dropout detection. When `true`, a pixel every source flags as dropout is never recovered from the flagged values; it is black, or with `passthrough` the combination of all the flagged values. Default: `false`.

Differential dropout detection used to keep flagged values within 500 codes of their median, half the 10-bit range, which kept nearly everything; it now keeps values within 10 % of the median, as ld-disc-stacker does. That changes the output only at pixels every source flags, 0–4 per million on the study captures, and those pixels stay marked as dropout.

### passthrough (bool)
Applies where every source flags a pixel as dropout and differential dropout detection recovers nothing. When `true`, the pixel is the combination of the sources' flagged values, still marked as dropout; when `false` it is black. Default: `false`. This parameter was stored but had no effect in earlier versions, and changes output only at such pixels.

### sample_align (bool)
Measure each source's 4FSC sample grid against the first input and shift it into line before stacking. Default: `true`.

Independent decodes of the same disc do not necessarily start a line on the same sample. ld-decode locates lines from sync, and PAL's TBC layout — 1135.0064 samples per line, closed with 4 extra samples per frame — does not pin subcarrier phase to sample index the way NTSC's 910 samples per line does. Real captures have measured up to 12 samples apart. One 4FSC sample is 90° of subcarrier, so combining a source that is a sample out erodes chroma instead of reinforcing it: the stack comes out desaturated, and the more sources that disagree, the worse it gets.

The offset is measured by correlating active picture against the first input, over ±32 samples. The correlation runs on luma low-passed by a 4-sample box: composite repeats every 4 samples because of the subcarrier, so raw composite correlates almost as well at the wrong cycle. The lag found that way is then refined on the raw samples within 2 samples of it, where the subcarrier makes the exact lag stand out.

The offset drifts across a side by a sample or more, so it is measured per segment of 500 frames, from a few frames spread through each segment, the first time a frame in that segment is stacked. A segment that cannot be measured (no frames in common with the reference, too little picture detail, or a correlation under 0.5) holds the offset of the nearest segment that could, up to 4 segments away; beyond that the source is stacked unshifted there. The log records each source's offset whenever it changes from one measured segment to the next, as a `grid` report record (see *Report records*).

The first input sets the grid the stack is made on. Where it has no frame to measure against — past the end of a short capture, or across a stretch it is missing — the other inputs are measured against a stand-in: the first input that has frames there and has been measured against the first input in some other segment. The stand-in's offset from the first input in the nearest such segment is added to every offset measured against it, so the output stays on the first input's grid; offsets drift by a sample or so across a side, so the nearest segment's is the one taken. The log reports the substitution once per run of segments, at INFO and as a `reference` record. Where no input both has frames there and was ever measured against the first input, the segment holds offsets from its neighbours as above, and the log warns that those frames are stacked without grid correction. Source Align's capture comparison suggests which input to put first.

Set to `false` to combine sources on their own grids, as earlier versions did.

### chroma_gain_match (bool)
Measure each source's colour burst on every frame and bring its chroma to the sources' median burst before stacking. Default: `true`.

The decode's high-frequency correction can differ between captures of one disc over stretches of a side. On five captures of one side, one or two of them carried their chroma, and their burst with it, 13–23 % stronger than the others over stretches of a few hundred to a few thousand frames, changing from one frame to the next. Their luma still matched the others', so the field agreement check passes them. Combined as they were, that chroma did not line up with the other sources': `Mean` averaged it in, and `Weighted` measured it as noise and gave the source almost no weight there, even though its luma was as good as the others'.

The burst sets the chroma level, as a receiver's automatic colour control takes it. For each frame the stage measures every source's burst amplitude on the active lines of both fields, using only lines whose burst window holds a clean subcarrier and no dropout, and takes the sources' median as the reference. A source whose burst lies more than 5 % from it has its subcarrier band scaled to match. The mismatch rises from nothing below about 3 MHz to its full size at the subcarrier and keeps rising above it, so the correction follows the same shape (1 − cos ωT)²: unity at the subcarrier, nothing at DC, and luma below about 3 MHz left as it was. Still-frame copies are matched to the same reference, each from its own burst, and the noise measured for `Weighted` is taken after the match. For YC sources the chroma plane is scaled. Frames without a measurable burst, and gains beyond 1.5 either way, are left alone. The debug log records each correction.

Set to `false` to combine the sources' chroma as it is, as earlier versions did.

### luma_level_match (bool)
Measure each source's luma gain and level against the picture common to every source on every frame and bring it to the typical source's before stacking. Default: `true`.

Captures through different players and decodes differ by a few per cent in contrast and a few codes in level. On Community South one capture was 7 % up in gain and 11–18 codes up in level against the other ten, and two others 3.5–4 % down. Combined as they were, the stack's contrast followed whichever sources carried the most weight. Its per-sample rejection also read the difference as disagreement: on white the lifted capture sat about 60 codes from the others, against a rejection distance of about 80, so noise pushed some of its samples out exactly where it carried the most weight. Matching the levels took the stack's noise on a held test card, with still-frame repeats, from 1.16 to 1.06 (8-bit units), against 1.23 for `Mean`.

For each frame the stage fits every source's luma, low-passed over one subcarrier cycle on the active lines away from dropouts, to the mean of all the sources' as a gain and a level, and expresses each relative to the median source's. A source then has its whole frame mapped onto the median source's scale. A source within 0.2 % and half a code of the median is left exactly as it was, as are frames with too little picture to show a gain (a black frame), gains beyond 15 % either way, and levels beyond 40 codes, which are a different picture rather than a different player. The match comes before the chroma match, which then sets the subcarrier band from the burst as before. Still-frame copies take their source's correction. For YC sources the luma plane is matched and chroma is left to the chroma match. The noise measured for `Weighted` does not depend on it, as that measurement is already immune to gain and level. The debug log records each correction.

Turning it on changes the output of saved projects, which load with it on. Set to `false` to combine the sources' luma as it is, as earlier versions did.

### Choosing a video mode

Use `Weighted`. The other modes are kept so that projects built with them reproduce exactly; none of them has done better on real captures except in the one case below.

The modes were compared on captures of four LaserDisc sides, 5 or 6 discs each, by stacking all but one disc and measuring how close the result came to the disc held out. The table gives the improvement over a single disc in dB, with the rate of large errors (more than 8.5 IRE out) in parts per million. A perfect average of independent captures would gain 6.0 dB with 4 discs and 7.0 dB with 5.

| Mode | Community South, 5 discs | Community North, 4 | National B, 5 | EcoDisc, 4 |
|------|------|------|------|------|
| `Weighted`¹ | **6.42** / 24 | **2.75** / 1,766 | **5.01** / 1,900 | **6.17** / 344 |
| `Mean` | 6.22 / 37 | 1.75 / 1,726 | 4.83 / 1,536 | 3.74 / 418 |
| `Median` | 5.24 / 20 | 0.35 / 4,062 | 1.71 / 9,203 | 5.32 / 322 |
| `Auto`, or `Smart Mean` at 15 (deprecated)² | 5.88 / 27 | 0.02 / 8,213 | 1.05 / 15,062 | 5.18 / 437 |

¹ Measured with the study's first version of the mode, which weighted each disc by its own noise. The stage now also groups the discs by pressing; in the same hold-one-out test that did as well, and on National B about 1 dB better than `Mean`, except where holding a disc out left another alone in its pressing (see below).

² With 4 or 5 discs nearly every pixel has 3 or more clean values, so `Auto` ran as `Smart Mean` at its default threshold almost everywhere.

Community North and National B gain less, and show more large errors, because their discs come from two pressings and the disc held out carries its own pressing's noise; compare the modes with each other, not one set with another.

Why the other modes fall short:

- `Mean` treats every disc as equally good. One noisy or soft disc pulls the whole stack down, as on EcoDisc, and a pressing with more discs in the stack counts for more than one with fewer.
- `Median` throws away most of the averaging: with 4 or 5 discs it gains little more than half what a mean does.
- `Smart Mean` and `Auto`, now deprecated, rejected at a fixed 15 codes. That is tighter than the noise of most captures, around 10 codes per disc plus the difference between pressings, so they rejected good values and fell back towards the median. Raising the threshold to 60 brought `Smart Mean` close to `Mean`, but never to `Weighted`, which is why it was retired rather than given a new default.

When to choose something else:

- **2 sources.** No mode can outvote a bad value with two discs, so `Weighted` and `Mean` give almost the same picture. `Weighted` still weights the cleaner disc more and marks the samples the two disagree on as dropout, so prefer it.
- **A disc alone in its pressing.** `Weighted` cannot tell a disc that is the only one of its pressing from a merely noisy one, so it gives it little weight. That is usually right, but if the disc is known to be good, `Mean` may do better. If the Stacker reports pressings that Source Alignment Analysis does not, turn `group_pressings` off. Source Align's Source Alignment Analysis shows the pressing groups before you stack, and the Stacker's log shows each source's share of the weight.
- **Reproducing an earlier result.** Keep `Mean` or `Median` if the project was built with one. `Smart Mean` and `Auto` are no longer available.

To check a choice on your own captures, run Source Align's Source Alignment Analysis first: it reports which discs share a pressing, how much of their noise the pressing accounts for, and which are soft. After stacking, the log lists each source's measured noise and weight in `Weighted` and the noise the model predicts for the stack; a disc with almost no weight is either noisy or alone in its pressing, and a warning that the model does not explain the inputs means the segment was stacked equally.

### still_frame_stacking (bool)
Also stack the repeats of a held still picture within each source. Default: `false`.

Many discs hold a still picture for several consecutive frames. Each repeat is recorded at a different place on the disc, so its noise and dropouts are independent of the other repeats'. That includes the noise recorded into the stamper, which stacking discs of one pressing cannot remove. With this on, each output frame also stacks the repeats of its picture found in each source. It works with a single input too.

For each output frame and each source, a frame is taken as a copy when it meets all of these:

- **At the colour cadence.** It is a multiple of 4 frames away in PAL and PAL-M, or of 2 in NTSC, and no more than `still_frame_search` frames away. Only those frames carry the same subcarrier phase, so only they can be added into the composite. Slow motion also moves further over a cadence than over one frame, which keeps it out.
- **The same picture, field by field.** Each field is compared with the source's own frame as 32×18 block means of low-passed luma. Repeats are not identical apart from noise: their level drifts slowly, by a gain that changes down the field. So each row of blocks is first fitted with an offset and a gain, and what is left must be within the noise. A local change (a cursor, a caption), a fade or a cut fails. A block is excused when its excess lies on 3 lines or fewer, as an unflagged dropout does. Blocks touching a flagged dropout are not judged.
- **The same samples, field by field.** Block means barely change when a thin outline moves a few samples, as a character drawn over a held background does in limited animation. So each field that passes is also compared sample by sample: the difference of the low-passed luma, cut into blocks of 16 samples by 8 lines, with each block's mean taken out. No block may carry more than 5 times the median block's energy, which is the pair's own noise. A block's 3 lines furthest from its mean are left out first, as an unflagged dropout covers a line or three. A field also fails as a fade when the difference follows the picture's detail by a gain of more than 0.4 % and more than 1 % of blocks carry over 2.5 times the median: a fade of small text on black, which leaves too few bright blocks for the level test above. The outermost block columns are not judged, because the edge between picture and blanking jitters, and neither are blocks touching a flagged dropout.
- **The same subcarrier phase.** The phase is measured, not read from the colour frame index. A frame a sample out of step with the frame being stacked (a grid slip within the capture) fails, and is dropped rather than shifted. A frame without chroma passes on the cadence alone.
- **An independent read.** A frame bit-identical to the frame being stacked is the same read repeated by Frame Map, not a copy.

A field takes copies from a source only when it has at least two. Every false match measured on real discs was a single pair, and with only two values the combine cannot reject an outlier between them.

**Reference noise.** The same-picture test needs each source's noise. It is measured per 500-frame segment, the first time a frame in the segment is stacked, on 24 pairs of frames spread through the segment, each a cadence apart. The pairs showing the same picture are found first against a within-frame estimate, which reads high on LaserDisc, then again against the noise those pairs gave, three times over. A segment with too few matching pairs holds the nearest segment's noise, up to 4 segments away. Beyond that, the source takes no copies there. Each decision uses only the two frames and this noise, so the copies a frame takes do not depend on which frames were stacked before. Preview and export therefore agree. The log records each source's reference noise whenever it changes by more than a quarter.

**How copies are stacked.** A copy is an extra input for the fields it matched, on its source's sample grid and with its source's luma level correction. In `Weighted`, the copies of one frame share no stamper noise with the frame they are stacked into, only with each other, so in each field they are weighted as that frame's copies of the field combined: a disc whose pressing has no other copy of that frame counts as an independent disc there, not as one of its pressing. The inputs are weighted per field the same way, as the field agreement check can leave a source's field out. Weighted rejection, differential dropout detection and low-confidence marking then apply as they do to inputs. Each copy is checked against its own source's frame, so the field agreement check runs over the inputs only. A copy of a field the check left out is left out with it.

Copies are used only in the active picture: its lines, and on them only the active video samples. VBI lines (picture numbers, VITC, teletext, closed captions), horizontal blanking, the frame descriptor, audio and EFM come from the frame itself, because they differ on every frame. Blanking differs from frame to frame of a held picture, alike on every disc: on one Volcanoes still, the repeats disagreed around the sync edge, and stacking them there flagged about 300 one-sample low-confidence dropouts per frame where the inputs alone flag about 13. The frame descriptor does gain one thing: how far the frame's picture is held either side, from the furthest copy before and after it that matched in both fields. The Video Sink's `held:` decoder schedule entry reads it, to decode held pictures with a 3D decoder and everything else, single-frame stills included, with a 2D one. In NTSC this also excludes lines 21 and 284, the first active lines, which carry closed captions. With a single input, a frame without copies passes through unchanged, as does every sample that takes none, dropout flags included.

At DEBUG the log lists the copies each frame used, for example `frame 9500: 4 still-frame copies: input 1 frame 9492, input 1 frame 9496, input 1 frame 9504 (field 1 only), input 1 frame 9508`.

**What it gains.** On the map stills and a photograph on Volcanoes side 1, repeats took about 6.8 dB of noise out, whether stacked into one capture or five. Stacking the five captures without repeats took out only about 1.3 dB, because most of their noise was recorded into the stamper and is the same on every disc.

**Limits.** On limited animation a drawing held for two frames or more is a true still and takes copies; one that moves does not. A move much smaller than a sample, or a change at very low contrast, can still pass. The other way, fine detail with timebase jitter can cost a true still its copies: on a painted snow scene of `cc_test` about one field pair in ten was rejected for faint differences in fine grass. A fade slower than about 0.4 % between copies is stacked as a still, which averages the copies' levels.

**Cost.** Each frame of each source is measured once, each pair of frames that passes the block-mean test is compared sample by sample once, and a frame with copies combines several times as many values. On 200 frames of five captures, export took 11.0 s instead of 2.8 s on stills and 3.4 s instead of 2.8 s on motion, with 11–13 % more memory. With the option off nothing changes. Frames are stacked on as many threads as read the stack: an export's decode workers, or the read-ahead that the Video Sink's VBI pre-scan and the analysis sinks use. Threads that need the same frame, or the same frame's signature, wait for one build rather than repeat it.

### still_frame_search (int)
How many frames either side of each frame to look for copies. Range: 2–50. Default: `24`, which tries ±4 to ±24 in PAL and ±2 to ±24 in NTSC, so every frame of a hold up to 25 frames long (one second in PAL) reaches all of its repeats. PAL needs at least 4. A wider search finds more copies on long holds, but each frame then reads more frames, up to `2 × still_frame_search / cadence` more per source. On a held test card stacked from eleven captures (Community South), 24 took the noise from 1.26 to 1.06 units against 8, the same as 50, and export took 19.0 s instead of 15.0 s for 140 frames; on single-frame map stills, where there is nothing more to find, 14.0 s instead of 12.7 s. Projects saved with a value keep it.

### pressing_repair (bool)
`Weighted` only: repair defects that every disc of one pressing carries, from the other pressings. Default: `false`. Needs two pressings or more, as the noise model groups them (see *Measuring each source's noise*); does nothing on a set of one pressing, and only composite sources are repaired.

A defect in a stamper is on every disc pressed from it. Those discs agree on it, so rejection keeps it, and a player may not flag it. On each active picture line, each pressing's consensus (the median of its sources) is compared with the consensus of the other pressings, on luma (a 4-sample box, which nulls the subcarrier). A pressing is taken to carry a defect where both hold:

- it differs from the other pressings by more than the rejection distance;
- it departs from its own field neighbours, the lines above and below, by that much more than the other pressings depart from theirs. The picture is the same on every pressing, so picture detail departs from the neighbours alike on all of them; only a defect departs on one.

A run of 6 such samples or more, and 2 samples either side, is replaced by the stack of the other pressings' sources, when that brings the line's luma nearer the neighbours (the luma guard). A pressing of a single source is not judged: its departures are that capture's own, which rejection leaves out wherever the other sources outnumber it. Impulses and dips shorter than 6 samples are not repaired. Replaced samples are not marked as dropouts, as they come from a pressing without the defect; the `frame` report record counts them (`repairedSamples`), and every replaced run is logged at DEBUG with its line, samples and the two consensus levels. A run the luma guard refuses is logged too.

**What it does.** On a videosynth case of the Domesday model whose first pressing carries unflagged dropouts on every capture, repair brought the stack's low band from 0.96 to 0.43 IRE from the truth in PAL and from 1.06 to 0.43 in NTSC: the stack without defects reads 0.43. On the same model without the defects it changed no sample, its test card and multiburst included. On Community North (11 captures, three pressings), 2,000 frames of maps took 77 repairs, 46 of which changed the stack visibly: dark and bright spikes on every disc of the seven-disc pressing, where the others carried smooth picture, and single lines whose picture began a few samples early on that pressing alone. The others were in samples the player had flagged on most of that pressing's discs, which the stack already took from the other pressings. 2,000 frames of photographs took 2 repairs, neither of which changed the stack. No repair inspected was false. The streak on picture 3001 that prompted it (field 2, line 208) is flagged by ld-decode on every disc of its pressing, so the stack removes it with this off.

### audio_stacking (string)
Method used to combine audio samples from multiple sources. Values: `Disabled`, `Mean`, `Median`. Default: `Mean`. When `Disabled`, audio from the source with the fewest dropouts is used. The method applies per channel pair, to every channel pair present in all inputs; channel pairs not common to all inputs pass through from the source with the fewest dropouts. All pipeline audio is 48 kHz frame-locked 24-bit stereo, so sources are combined sample by sample at the same frame position; combined values saturate at the 24-bit range.

### efm_stacking (string)
Method used to combine EFM t-values from multiple sources. Values: `Disabled`, `Confidence`, `Mean`, `Median`. Default: `Disabled`, which passes the EFM of the source with the fewest dropouts through untouched. That is the default because no combining mode has yet been shown to beat it — see *Choosing an EFM mode* below.

Each EFM byte on the pipeline packs the t-value into its low nibble and the producing source's doubt about that t-value into the high nibble.

| Mode | Behaviour |
|------|-----------|
| `Disabled` | The chosen source's bytes pass through untouched, doubt included. |
| `Confidence` | Sources are aligned on the EFM frame-sync grid and vote on where the transitions are, weighted by their doubt. Emits a doubt of its own. |
| `Mean` | Arithmetic mean of the t-values at each sample index, emitted with zero doubt. |
| `Median` | Median of the t-values at each sample index, emitted with zero doubt. |

### Choosing an EFM mode

`Disabled` is the default and, on the evidence so far, the mode to use. Measured against six independent captures of the same BBC Domesday side (National A, aligned on the EFM stream, 8,000 frames each), passing the best single source through recovered more sectors than any combining mode:

| mode | good sectors | uncorrectable | C1 uncorrectable | C2 uncorrectable |
|------|--------------|---------------|------------------|------------------|
| `Disabled` (best source alone) | 21,695 | 8 | 4,209 | 553 |
| `Confidence`, three captures | 21,577 | 140 | 929 | 10,800 |
| `Mean`, six captures | 5 | 10 | 6,826 | 29,932 |

`Mean` is not merely worse, it is destructive: five good sectors out of some 21,700. On a synthetic three-capture ensemble it made the decode fail outright.

`Confidence` behaves far better than that and improves C1 - it more than halved C1 uncorrectable in the run above - but C2 uncorrectable rises sharply at the same time, and the recovered-sector count falls below a single capture. Combining three copies of one capture reproduces that capture exactly, so the fault is in the voting rather than in the surrounding machinery; the working theory is that the whole-slot alignment is occasionally off by one channel frame, which would yield exactly this signature, a clean copy of the wrong frame. Until that is settled, treat `Confidence` as experimental.

`Mean` and `Median` are kept only for reproducing earlier results, and have two problems `Confidence` does not.

The first is that they average. A t-value is a quantised symbol, not a measurement: where one source reads T3 and another T11, their mean of T7 is a reading neither source made and one that is wrong for both, and the demodulator then decodes it with full confidence. `Confidence` only ever emits a t-value some source actually reported.

The second, and the more serious, is that they combine by sample index. EFM t-values are run lengths, not samples on a shared time axis, so one spurious or missed transition in a capture shifts every following index in it and the two streams are averaged out of step from there on. A video frame carries around 36,600 t-values, so a single early insertion spoils the rest of the frame. `Confidence` combines on the channel bit axis instead, cut into channel frames on each source's own T11+T11 frame sync (IEC 60908 §20.2, 588 bits). An inserted transition splits a run into two that sum to the same length and a missed one merges two runs into one of the same length, so on that axis neither moves anything after it, and the sync grid re-anchors the alignment every 588 bits regardless.

Within a channel frame each source votes on the bit offsets its transitions sit at, with a weight taken from its doubt, and against offsets falling inside its runs. Offsets that win are kept, the result is repaired to legal T3–T11 run lengths, and each output run carries a doubt derived from how strongly its bounding transitions won and from how much the sources that reported it doubted it. Where only one source covers a channel frame, or the vote cannot be repaired into a legal frame, the frame from the source with the fewest dropouts is used unchanged; so are the partial channel frames at each end of a video frame's t-values.

The doubt this mode emits is worth more than the doubt on its inputs, because it is evidence of a different kind: a producer's doubt is one capture's opinion of its own reading, while a disagreement between independent captures is an observation. Both `efm_sink` and `efm_audio_decode` can act on it through their `doubt_erasure_threshold` parameter, which turns doubted t-values into C1/C2 erasure hints.

## Report records

What the stage measures is also logged for scripts, one line per record: `StackerStage report: ` followed by a JSON object on the same line. The prefix and the object's `v` key (1) are the contract; the human-readable lines beside them, most of them now at DEBUG, may change. Run `orc-cli --log-file` to keep them. Frames are numbered from 1, as in the GUI. The stage measures lazily, in whatever order frames are asked for, so records arrive in that order, not by frame.

| `kind` | When | Keys besides `v`, `kind` and `frames` (first and last frame of the block) |
|---|---|---|
| `weights` | `Weighted` mode: a block's noise, logged when any input's σ moves by more than a quarter or the fit check changes | `inputs`: one object per input, `input` (from 1), `sigma` (codes; `null` when the input was not measured), `share` (per cent of the weight), `stamperGroup` and `family` (letters, as Source Align names them); `fitTrusted`; `fellBackToMean` (the inputs are stacked with equal weights because the fit was not trusted); `weightedSigma`, `equalSigma`, `bestSingleSigma` (the model's predicted stack σ; when the fit is not trusted, the untrusted model's); `worstPair` (the two inputs, from 1, whose noise the model explains worst), `worstMisfitPercent` and `rmsMisfitPercent` (how far the model is from the measured pair noise, worst pair and over all pairs; above 35 % on the worst pair the fit is not trusted), `pairs` (pairs compared) |
| `grid` | An input's sample grid offset changes | `input`, `offset` (samples from the first input), `fractional_offset` (the same to a fraction of a sample; only the whole part is corrected), `correlation`, `reference` (the input it was measured against: 1, or a stand-in) |
| `reference` | The first input has no frame to measure grids against | `input` (the stand-in, or `null` when there is none and the frames are stacked without grid correction), `offset` (the stand-in's offset from the first input) |
| `frame` | Each stacked frame, at DEBUG only (`orc-cli --log-level debug`), as there is one per frame | `inputs` and `copies`: per field, `[field 1, field 2]`, the inputs and the still-frame copies whose field was stacked (an input left out by the field agreement check is not counted); `lowConfidenceSamples` (samples the inputs disagreed on, `Weighted` only) and `unrecoverableSamples` (samples no input could supply, and whole fields the inputs disagreed on), both marked as dropouts in the output; `repairedSamples` (samples `pressing_repair` replaced, not marked) |

For example:

```
StackerStage report: {"v":1,"kind":"grid","frames":[2501,3000],"input":3,"offset":-1,"correlation":0.981,"reference":1}
```

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
