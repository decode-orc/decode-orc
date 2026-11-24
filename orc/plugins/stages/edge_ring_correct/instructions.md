# Edge Ring Correct

Removes the ring after sharp picture edges and the white halo after thin dark lines that PAL LaserDisc pictures carry, window by window along the source, and only where its own calibration has certified the correction better on frames it did not fit on. Its output is composite, like its input.

## When to use

Use it on PAL LaserDisc sources whose black-to-white edges ring (a dark dip a few samples after a rising edge, decaying over about a microsecond) or whose maps and line drawings show a white halo just right of every dark line. The Domesday Community sides carry both. Place it after the Stacker, Dropout Correction and Channel Equalise, before the Video Sink. Run its **Edge Ring Calibration** tool first: it sets `ring_track`. With an empty track the stage passes every frame through unchanged.

## What it does

The ring is a luma-band oscillation of about 5 MHz, which in the composite lies in the chroma band; correcting the composite, before the chroma decoder, removes it from the decoded luma and from the cross-colour it would otherwise cause.

Sharp steps (at least 25 IRE, with a flat lead-in and the far side held) and thin dark lines (at least 15 IRE deep on ground of at least 45 IRE) are located on the line's luma: on the composite line itself where neither far ground carries a subcarrier above 5 IRE, otherwise on the vertical [1 2 1] comb over field lines r-2, r and r+2 where the feature is vertically aligned. Candidates are found on a [1 2 2 2 1]/8 notch of the line (zeros at the subcarrier and half the sample rate), which the ring cannot move.

At each feature a template is subtracted from the composite line, scaled by the feature's own step or depth:

- rising and falling steps: the mean excess of the window's measured steps over the ideal step, from 2 to 20 samples after the 50 % crossing (the edge itself, and the samples before it, are never changed);
- thin dark lines: the mean asymmetry of the window's measured lines (after minus before), from 2 to 20 samples after the centre.

Each component's gain is scaled per frame by the frame's own measured excess projected on the template (clipped to 0..2; with fewer than 8 measured steps, or 12 lines, the window's gain, or none for lines). Nothing else is filtered: flat areas, blanking, sync, burst and VBI lines are untouched. A frame in no certified window passes **bit-identical**. Only PAL composite sources are processed; any other source passes through with a warning.

## Parameters

### ring_track (string)
The certified corrections, one per window of frames, separated by `;`: `first-end:f|w:rise_gain,fall_gain,line_gain:rise:fall:line`, where `f` scales the gains per frame and `w` keeps them fixed, and the templates are comma lists in units of 1e-5 of the step (117, 117 and 81 bins at a quarter sample). Set it with Edge Ring Calibration, which ties the windows to this input's frame numbers. Default: empty (no correction).

## Tools

**Edge Ring Calibration** (`edge_ring_calibration`) tiles the input with windows of about 1024 frames and splits each window's frames by frame number mod 3 into a fit third, a certifying third and a reporting third. Of every 8th frame of a third it reads the 12 with the most measurable features, at least 6 frames apart. Templates are fitted on the fit third (a component needs 30 measured steps of its direction, or 40 lines). Candidates (per-frame then fixed gains; every subset of the fitted components; full then half gain) are tried in turn on the certifying third, paired before/after with a 300-draw bootstrap over frames, each field's per-frame gain estimated on the other field of its frame. The first candidate is kept whose 97.5 % bound of the change is within 0.1 % of the step on every edge measure (rising and falling ring RMS, pre-edge RMS and tail; line halo RMS and halo at +3 samples), within 0.05 dB on flat-area noise and 0.1 IRE on level, and whose summed edge measures improve by at least 0.5 % of the step. A window with no certified candidate is left out of the track. The report lists every window with the reporting third's measures before and after. **Apply to Stage** sets `ring_track`. From the command line: `orc-cli tools run disc.orcprj edge_ring_calibration --node <node> --apply`.
