# Preview Window

## Overview

The Preview Window displays the video output of the currently active pipeline
stage. It provides frame-by-frame navigation, channel selection, dropout
visualisation, and a suite of signal-analysis tools: Frame Scope, Frame Timing,
Waveform Monitor, Vectorscope, Video Histogram, and several metadata observers.

## Navigation Bar

| Control | Description |
|---------|-------------|
| << | Jump to the first frame or field. |
| < | Step back one frame or field. Hold to step continuously. |
| ▶ / ⏸ | Play back at the project frame rate (PAL: 25 fps; NTSC: ~30 fps), or at the audio rate when a channel pair is selected. Press again to pause. Stops automatically at the last frame. |
| > | Step forward one frame or field. Hold to step continuously. |
| >> | Jump to the last frame or field. |
| Frame spinbox | Type a frame (or field) number to jump there directly. Numbers are 1-based, matching every other frame number shown in the GUI. |
| Slider | Drag to scrub through the entire range. Rendering is debounced during rapid movement. |

## Control Row

| Control | Description |
|---------|-------------|
| Preview Mode | Select the output type: **Frame**, **Top Field**, **Bottom Field**, or other modes offered by the selected stage. |
| Channel | For YC (separate luma/chroma) sources: **Y+C** (composite display), **Luma (Y)**, or **Chroma (C)**. Hidden for composite CVBS sources. |
| Aspect Ratio | **1:1 (Square)** shows the samples as square pixels; **4:3 (Display)** shows the picture at its display shape. |
| Zoom 1:1 | Resize the preview window so the image is displayed at its native pixel resolution. |
| Dropouts: Off/On | Toggle the dropout overlay. When enabled, samples flagged as dropout are highlighted in red over the preview image. |
| Audio | Choose an audio channel pair (listed as `index: name`) to play with the preview, or **Mute/None**. Greyed out, with a tooltip saying why, when the stage's output carries no audio. |
| Volume | Playback volume for the selected pair. Can be set before pressing play and takes effect immediately, including mid-playback. |

## Audio Playback

Select a channel pair in the **Audio** control and press play: the pair is
played at normal speed and the preview follows it.

Audio is the clock and the video chases it. Preview rendering is not
guaranteed to reach real time — a 3D chroma decoder or a neural stage can take
seconds per frame — so instead of slowing the audio to match, the preview shows
whichever frame the audio has reached and skips the ones the renderer could not
deliver in time. Sound is always continuous; on a heavy pipeline the picture
simply updates less often.

A few things worth knowing:

- **The first play may pause to prepare the audio.** Some sources decode their
  whole audio stream on first access — an EFM disc decode can take minutes.
  A progress window appears if the wait is long enough to notice, and the
  prepared audio is kept, so pausing and playing again is instant. A CVBS
  container reads audio per frame and needs no preparation at all.
- **Preparing audio can use a lot of memory.** A TBC, imported-WAV or EFM source
  holds the decoded audio in RAM for the whole disc; this can be hundreds of
  megabytes on a feature-length title.
- **The selection follows you between stages.** Channel-pair numbering is a
  property of the stage's output, so the selector re-reads it whenever you view
  a different stage. The pair you chose is remembered by its name and origin
  and selected again on a stage that carries it; on a stage that does not, the
  selector shows **Mute/None** until you return to one that does.
- **Changing pair during playback keeps playing**, on the new pair, from the
  position reached.
- **Choosing Mute/None during playback** ends the audio session, so the preview
  reverts to timer-paced video-only playback. Picking a pair again resumes
  audio-mastered playback from the position reached.
- **Editing the pipeline stops playback.** Changing a parameter or the graph
  invalidates the prepared audio; press play again to restart.
- **Scrubbing restarts the audio** at the new position rather than trying to
  catch up.
- Most projects carry no audio at all. The **Audio** control is then disabled
  and play behaves exactly as it always has.

## Frame Scope

Click anywhere in the preview image to open the **Frame Scope** (called the
Line Scope in 1.x), a line oscilloscope showing every sample of that line.
Holding the button down and dragging moves the scope from line to line. The
scope follows the preview: stepping or playing redraws the same line of the new
frame.

- Horizontal axis: time along the line, in microseconds.
- Vertical axis: signal level in the project's amplitude unit (IRE, millivolts
  or 10-bit samples, set in File → New/Edit Project). The axis is not clamped:
  it reaches below sync tip and above peak white.
- Dashed reference lines mark sync tip, blanking, black (when it differs from
  blanking), white and peak, taken from the video parameters in effect at the
  previewed stage.
- **Channel** (Y/C sources only): **Luma (Y)**, **Chroma (C)**, **Both (Y & C)**
  (the default) or **Y+C** (summed into a composite signal).
- **Line numbering**: **Frame flat (0-based)**, **Frame sequential (1-based)**,
  **Field relative** or **Broadcast interlaced**. The mode is remembered between
  sessions and only changes the labels, not the line shown.
- **Up/Down** step to adjacent lines without closing the scope; hold to repeat.
- Click or drag on the plot to move the sample marker; the cross-hairs on the
  preview image follow it, and a read-out gives the marker's time, sample number
  and value.

## File Menu

| Action | Shortcut | Description |
|--------|----------|-------------|
| Export PNG... | Ctrl+Shift+E | Save the currently displayed preview image as a PNG file. |

## Observers Menu

Observers decode metadata from the video signal and display it in floating
dialogs that update as you navigate frames.

| Action | Shortcut | Description |
|--------|----------|-------------|
| VBI Decoder | Ctrl+Shift+V | LaserDisc VBI data for the current frame: the biphase words on lines 16–18, picture number, CLV timecode, chapter number, user code, stop/lead-in/lead-out codes and programme status. |
| NTSC Observer | Ctrl+Shift+N | Per-field FM code (presence, data and field flag) and white flag. |
| Video Parameters | Ctrl+Shift+H | The video parameters in effect at the previewed stage, and per-field quality measurements. See below. |
| Closed Captions | Ctrl+Shift+C | Decode the EIA-608 line 21 caption service into a running transcript. |

NTSC Observer and Closed Captions are available only on NTSC projects; on a
project of another standard the entry is greyed out.

Observations are computed in the background and kept, so returning to a frame
already seen is immediate. While an observer window waits for a frame's results
it shows **Computing observations…**; the main window's status bar reports the
background work as **Computing observations…**, **Checking observations…**
(stored results being checked as complete) or **Observations paused during
playback…**.

## View Menu

Signal analysis tools displayed as waveform or vector graphs in floating
dialogs. An entry only appears when the previewed stage can supply that view.

| Action | Shortcut | Description |
|--------|----------|-------------|
| Frame Timing | Ctrl+Shift+T | Show the signal across all lines of the current frame as one continuous waveform, with markers at each line start. Useful for diagnosing sync pulse position and line structure. |
| Waveform Monitor | Ctrl+Shift+W | Sample histogram across multiple lines, with selectable channel, sample range, field, and intensity. Available for supported stages only. |
| Vectorscope | Ctrl+Shift+S | U/V chroma vector plot. Shows the decoded (grading) plot on a chroma-decoding stage's colour output and the composite (measurement) plot on a CVBS or Y/C output. |
| Video Histogram | Ctrl+Shift+I | Distribution of luma and colour-difference levels in the decoded picture. |

### Waveform Monitor

Displays a stacked histogram of sample values across a range of lines.

| Control | Description |
|---------|-------------|
| Channel | `Y+C (Composite)` accumulates the full composite signal; `Y (Luma only)` uses the separate luma channel when the source has one, and otherwise the composite signal low-pass filtered to remove the colour subcarrier. |
| Range | `Active video` accumulates only the visible part of each line; `Whole frame` includes sync and blanking. |
| Field | `Frame` accumulates both fields together; `Field 1` and `Field 2` accumulate one field only. The colour subcarrier phase alternates from field to field, so comparing the two fields separates field-correlated interference (subcarrier leakage into sync, for example) from noise that is random across the frame. Disabled when the preview supplies a single field, which has no second field to compare against. |
| Phosphor | Draws a green trace on black, in the style of an analogue oscilloscope. |
| Intensity | Brightness gain applied to the accumulated trace. |

Only available when the selected stage provides this view.

### Vectorscope

Plots chroma as a scatter on a vectorscope graticule. There are two
acquisitions, and they answer different questions:

| Acquisition | What it plots |
|-------------|---------------|
| **Decoded (grading)** | The U/V planes the chroma decoder produced — after demodulation, after comb/delay-line filtering, and after the PAL V-switch has been undone. This is a post-production colour-grading scope: it shows what the decoder output looks like, which is what to use when comparing decoder settings. |
| **Composite (measurement)** | Chroma demodulated straight from the composite carrier (or the C channel of a Y/C source) against a burst-locked subcarrier reference, with no delay-line averaging and no V-switch correction, so the burst and both PAL line phases are in the data set. This is a technical measurement scope: it shows what the *signal* looks like. |

**You do not choose between them.** The acquisition follows the output you are
previewing: a colour-domain output has decoder planes to plot, a signal-domain
one has a carrier to demodulate, so selecting the stage — and, on a decoding
sink, the preview mode — already settles it. The scope opens on any stage, and
the Acquisition box reports which of the two you are looking at. Switching
stages re-acquires; the two are different data sets, not two renderings of one.

The composite acquisition's display is not delay-line compensated, so a PAL
graticule carries **two** sets of colour-bar targets — upper case for the +V
line phase, lower case for the −V phase — and **two** burst boxes at 135° and
225° (ITU-R BT.470-6 Table 2 item 2.16). PAL-M is drawn the same way: ITU-R
BT.1700-1 Annex 1 Part B gives it PAL colour encoding, V-switch included, on
the 525-line raster, so only its burst amplitude follows the NTSC levels. NTSC
has a single set of targets and a single burst box on the −U axis at 180°
(SMPTE 170M-2004 §8.4).

What the scope takes off the frame is set in two groups, one for each axis of
the raster. **Line View** picks the region along each line and **Field View**
the lines down the frame; within each group the options are alternatives, so
exactly one is in force at a time.

Both acquisitions take the same **Field View**, in the same numbering, so one
can be pointed at exactly the lines the other is showing — which is what makes
the two plots of a frame comparable. Line numbers count through the interlaced
frame: line 1 is the top line and consecutive numbers alternate fields, the
same numbering the active picture is stated in. Only **Line View** is
composite-only: the decoded planes hold active picture, with no sync, porch or
burst to choose between.

| Control | Description |
|---------|-------------|
| Acquisition | Reports which acquisition is in force. Not a choice — see above. |
| Line View | Composite only. **Active line** samples the active picture window, **Whole line** the entire line including sync and porches, **Burst only** the colour-burst window on the back porch. |
| Field View | **Active field** plots the active picture only, which is what the decoded acquisition shows, and is the default: the two acquisitions then cover the same lines of the frame. **Whole field** plots every line of the frame. **Selected line(s)** plots a named range, the way a real instrument's line-select works, and takes the lines literally rather than intersecting them with the active picture. |
| Start line / End line | Enabled by **Selected line(s)**. Untick **End line** to plot the start line on its own. The spin boxes only offer lines the plot can show: a frame of the system being plotted (625 for PAL, 525 for NTSC and PAL-M), and — when Field Selection names one field — only that field's lines, since consecutive frame lines alternate fields. |
| Field Selection | Choose which field (both, first, or second) contributes to the plot. Restricting it to one field also restricts which lines **Selected line(s)** may name. |
| Graticule | Target set to overlay: none, 75 %, 100 %, or both. |
| Colorize | Tint each plotted point by its chroma position. Turn it off for a single-colour trace, which is how an instrument's CRT reads. |
| Defocus | Add Gaussian scatter to the trace to aid reading at high dot density. |
| Draw Trace Lines | Join consecutive samples so the plot shows the beam path rather than isolated points. |
| Gain | Trace intensity, like an instrument's intensity knob. On the composite plot brightness is proportional to how long the beam dwells on a point, as a phosphor's is: a colour-bar vector, where the beam rests for the width of the bar on every line, saturates, while the transit between two vectors is crossed once a line and stays faint. Raising Gain lifts the faint detail into view without moving the vectors. The decoded plot keeps its own brightness law. |

#### Measurement readouts

Shown in the composite acquisition, computed from the burst on every active
line of the frame — the field view changes what is plotted, not what is
measured, so narrowing the range does not move the readings:

| Readout | Meaning |
|---------|---------|
| Burst | Mean burst peak amplitude in IRE, and as a percentage of the nominal for the system (EBU Tech. 3280-E §1.2: PAL 300 mV p-p; SMPTE 170M-2004 §8.4: NTSC 40 IRE p-p). 100 % means the burst is at its specified amplitude. |
| Jitter | RMS deviation of the per-line burst phase from the mean phase of its own V-switch group — subcarrier phase jitter. |
| Lines | Number of lines that contributed to the burst reference. |
| V-switch split err | PAL and PAL-M only. Departure of the two burst vectors from their nominal 90° separation. |
| Chroma/burst | Mean active-picture chroma amplitude divided by the mean burst amplitude. |

The subcarrier reference is measured from the burst rather than assumed from
the nominal subcarrier frequency, which is how an instrument's burst-locked
oscillator behaves. Each line's burst is that line's phase truth; a local
straight-line fit over a few tens of lines acts as the oscillator's flywheel,
so any drift in where a file places the subcarrier at the start of a line is
followed rather than plotted, while per-line phase error stays visible as
spread around the burst vectors and in the Jitter readout. This matters
because the drift need not be small: a file whose lines sit on a plain
integer-sample grid, rather than advancing by the nominal 283.7516 subcarrier
cycles a line, walks about 0.58° a line — a full turn over a PAL frame, enough
to smear a frame of colour bars into arcs at the bar radii.

The composite trace is drawn in front of the graticule, as it is on a bench
instrument, so a vector that lands exactly on its target is not hidden under
the target's own crosshair, and the beam is given the finite spot a CRT has —
without one, a vector that never moves lands on a single canvas pixel and is
smaller than the mark it is meant to be read against.

Full brightness is set from the trace itself rather than from a fixed number
of hits, because how bright a vector looks depends on how tightly it lands. A
clean source puts every line's colour bar on the same pixel; noise on a real
capture spreads it over a disc and divides the dwell on any one pixel by the
area of that disc. The reference follows that spreading, so the vectors read
the same either way, and the transits between them are held faint
independently — a frame of colour bars lays down several times as much ink
joining its vectors as landing on them, and scaling the two together washes
the plot out.

Chroma is band-limited around the subcarrier before demodulation, using the
same raised-cosine shape and 1.1 MHz sizing as the PAL decoder's chroma filter.
Without it the whole luminance band folds into the plot and every sync edge and
picture transition throws a full-amplitude rotating vector across the display.
Sharp luminance edges still leave small spikes — that is what a wideband
instrument shows, and it is the same effect that produces cross-colour.

If the burst amplitude reading looks wrong, cross-check it against the **Burst
Level** analysis sink, which measures the same thing independently over a whole
recording.

Whole-frame, whole-line acquisitions are subsampled when they would otherwise
exceed the acquisition's sample ceiling; the info line reports the stride when
one is in effect. Narrow the line range to sample every sample of a line.

### Video Histogram

Plots how many pixels of the current picture fall at each signal level, one
chart per component. **Channels** chooses the colour-difference pair shown
beside luma: **YUV** or **YIQ**. The heading above the charts names the field,
picture size and system.

### VBI Decoder

Decodes and displays Vertical Blanking Interval content for the current frame,
including closed captions, VITC timecode, and LaserDisc programme metadata such
as chapter and frame numbers. Teletext itself is decoded by the Teletext Sink
stage rather than here; what this dialog shows is the LaserDisc
programme-status flag saying whether the disc carries a teletext service.

### NTSC Observer

Shows the per-field FM code (presence, data and field flag) and white flag.

### Video Parameters

Shows the video parameters in effect at the previewed stage — system, frame
width, burst and active ranges, sync/black/white levels — and, per field, the
colour frame index, burst level, white SNR, black PSNR, quality score and
dropout count. A `video_params` stage upstream changes the values shown.
