# Edge Ring Correct

Removes the ring after sharp picture edges and the white halo after thin dark lines that PAL LaserDisc pictures carry. It works window by window along the source, and only where its own calibration has certified the correction better on frames it did not fit on. Its output is composite, like its input.

## When to use

Use it on PAL LaserDisc sources whose edges ring (a dark dip a few samples after a rising edge, a lighter one after a falling edge, decaying over about a microsecond) or whose maps and line drawings show a white halo just right of every dark line. The Domesday Community sides carry both. Place it after the Stacker, Dropout Correction and Channel Equalise, before the Video Sink. Run its **Edge Ring Calibration** tool first: it sets `ring_track`. With an empty track the stage passes every frame through unchanged.

## What it does

The ring is a luma-band oscillation of about 5 MHz, which in the composite lies in the chroma band; correcting the composite, before the chroma decoder, removes it from the decoded luma and from the cross-colour it would otherwise cause.

**Events.** Every sharp transition of at least 12 IRE is an event, found on each line's [1 2 2 2 1]/8 notch (zeros at the subcarrier and at half the sample rate; symmetric with unit gain, so a transition keeps its step and centroid). This finds vertical, slanted and curved edges and lettering alike, in colour or not, and the ring itself cannot make an event. A falling event followed within 7 samples by a rising one is a thin dark line. Nothing is found on samples the capture flags as dropouts.

**Corrections.** At each event a template is subtracted from the composite line, scaled by the event's step:

- falling steps: the window's mean excess of falling edges over the ideal step, from 2 to 20 samples after the edge;
- rising steps: the ring depends on where the edge starts and ends (the player's response is level-dependent), so rising templates are kept per start level (3 bins) and end level (5 bins), and per edge class — isolated edges (flat lead-in) and fill edges (the edge of a grey or textured fill onto paper), judged on the line's luma (the composite where it carries no subcarrier, else the vertical comb) the same way when measuring and when correcting. Their shapes are pooled over the whole source's fit frames; their gains are certified per window;
- thin dark lines, on line-art frames only (maps, line drawings, text pages: at least 4000 thin-line pairs making up at least 30 % of the frame's events): the mean asymmetry of the window's measured lines, from 1 sample after their rising edge. Elsewhere thin lines are left alone.

A template never reaches the next feature's measured region: it stops 12 samples before the next step, and 1.5 samples before the next thin line on line-art frames (lettering strokes, close parallel lines; elsewhere 12, as thin lines there are left alone). A feature with more than 3 events in the 24 samples after it — stipple, hatching — is left alone, so fills are not mottled; the edge of a fill onto clean paper is still corrected. Falling-edge and line gains are scaled per frame by the frame's own measured excess (lines at most 1). Nothing else changes: flat areas, blanking, sync, burst and VBI lines are untouched, and a frame in no certified window passes **bit-identical**. Only PAL composite sources are processed; any other source passes through with a warning.

## Parameters

### ring_track (string)
The certified corrections: first `levels:` and the source's rising templates (2 classes × 15 level bins, `/` between), then one item per window, `first-end:f|w:rise,fall,line,fill:kappa_rise,kappa_fall:fall:line` — the gains of isolated rising steps, falling steps, thin lines and fill-edge rising steps; the ratios of a full step to an event's own step; and the falling and line templates. `f` scales the falling and line gains per frame, `w` keeps them fixed; templates are comma lists in units of 1e-5 of the step. Set it with Edge Ring Calibration, which ties the windows to this input's frame numbers. Default: empty (no correction).

## Tools

**Edge Ring Calibration** (`edge_ring_calibration`) tiles the input with windows of about 1024 frames and splits each window's frames by frame number mod 3 into a fit third (24 frames read), a certifying third and a reporting third (18 each): of every 8th frame of a third, those with the most measurable features, at least 6 frames apart. It measures isolated events only (nothing within 16 samples before or 22 after; for a feature measured on the vertical comb, nothing else on the two lines it mixes in either), and nothing across a dropout. It first reads every window's fit third and pools the rising templates per class and level bin (a bin with fewer than 200 edges takes its class's template). Each window's falling and line templates come from its own fit third (30 falling steps, or 40 lines on 8 line-art frames). Candidates — per-frame then fixed gains, every subset of the fitted components, full then half gain — are tried in turn on the certifying third, paired before and after, with a 300-draw bootstrap over frames and each field's per-frame gain estimated on the other field of its frame. The first is kept whose 97.5 % bound of the change is within 0.1 % of the step on every edge measure (each component's ring RMS, pre-edge RMS and tail; the line halo RMS and halo at +3 samples), within 0.05 dB on flat-area noise and 0.1 IRE on level, and whose summed edge measures improve by at least 0.5 % of the step and on at least 60 % of the certifying frames; a component must have 30 measured features there. A window with no certified candidate is left out of the track. The report lists every window with the reporting third's measures before and after. **Apply to Stage** sets `ring_track`. From the command line: `orc-cli tools run disc.orcprj edge_ring_calibration --node <node> --apply`.
