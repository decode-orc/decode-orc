# Preview and Observer Rendering Performance — Qt RHI Acceleration Plan

## Purpose

[Issue #293](https://github.com/decode-orc/decode-orc/issues/293) reports that
playback becomes sluggish when several observer and scope dialogs are open,
and proposes GPU-accelerated drawing. This plan records where the per-frame
time actually goes, which of it a GPU can take, and the order in which to do
the work so that each phase delivers a measurable improvement on its own.

The investigation reached one conclusion that shapes the whole plan: the
sluggishness the issue describes is dominated by **work that is not drawing**
— full chroma decodes run on the GUI thread, duplicated requests, unbounded
worker queues and a whole-node background sweep that observer dialogs arm.
None of that is helped by a GPU, and a GPU canvas placed on top of it would
still stall. Phases 1 and 2 therefore remove those costs first, with no GPU
involvement; Phases 3 to 6 then move the genuinely raster-bound work
(the preview blit, the vectorscope and waveform phosphor simulations, the
YUV→RGB conversion) onto Qt's RHI, largest payoff first.

## Why Qt RHI, and the platform floor

The issue thread raises the cross-platform hazard of GPU code (OpenGL
deprecated on macOS). Qt's Rendering Hardware Interface (RHI) is the answer to
exactly that: the application writes one shader in Vulkan-flavoured GLSL,
`qsb` compiles it offline to SPIR-V, HLSL, MSL and GLSL, and at run time Qt
picks Metal on macOS, Direct3D 11 on Windows and OpenGL or Vulkan on Linux.
The application never names a graphics API.

Every shipping target already clears the version floor the public widget
integration needs (`QRhiWidget`, Qt 6.7):

| Target | Qt | Source |
|---|---|---|
| Nix dev shell / CI | 6.11 | `flake.nix` (`qt6.qtbase`) |
| Windows MSI | 6.9 | [`package-windows.yml`](../.github/workflows/package-windows.yml#L148-L159) |
| Flatpak | KDE runtime 6.9 | [`io.github.decode_orc.decode-orc.yml`](../io.github.decode_orc.decode-orc.yml#L2-L4) |
| macOS DMG | Homebrew `qt@6` (rolling, ≥ 6.9) | [`package-macos.yml`](../.github/workflows/package-macos.yml#L56) |

Other facts that bound the design:

- The GUI links only `Qt6::Core`, `Qt6::Widgets` and optionally
  `Qt6::Multimedia` ([`orc/gui/CMakeLists.txt`](../orc/gui/CMakeLists.txt#L276-L283)).
  RHI needs an explicit `Qt6::Gui` link (headers under `<rhi/qrhi.h>`) and the
  `Qt6::ShaderTools` module at build time for `qt6_add_shaders()`. The Nix
  dev shell does not currently include `qt6.qtshadertools`; the KDE Flatpak
  SDK, Homebrew `qt` and the Qt online installer all ship it.
- The Flatpak manifest already grants `--device=dri`.
- macOS deployment target is 14.0, so Metal is always present.
- Qt's RHI classes carry a *limited* compatibility promise (source-compatible,
  may change between minor releases). All RHI use is therefore confined to a
  small `orc/gui/gpu/` directory behind a widget-level seam.
- Nothing in the tree uses OpenGL, QRhi, QQuick or Qt private headers today.
  There is no SIMD anywhere in the render path, and no per-frame timing
  instrumentation exists.
- MVP boundaries: all GPU code lives in `orc/gui`. Core stays Qt-free; what
  changes on the core/presenter side is only *which payload* a render
  returns (planes instead of, or alongside, RGB), never how it is drawn.
- Unit tests must not touch a GPU. Every phase keeps the existing CPU raster
  path as the fallback, and the offscreen GUI test tiers run against that
  path. Geometry, vertex generation and uniform packing are written as
  Qt-Core-only helpers so they get Tier 1 `gui-logic` coverage.

## Where the time goes today

Per displayed frame, PAL colour preview (1135×625, ~710 k pixels), with the
vectorscope and histogram open. Citations are to the current tree.

### Work on the GUI thread

1. **A second and third full chroma decode.** After every completed render,
   `MainWindow::onPreviewReady` calls
   [`refreshVectorscopeForCurrentCoordinate()` and `refreshHistogramForCurrentCoordinate()`](../orc/gui/mainwindow_coordinator_callbacks.cpp#L63-L69).
   Each ends in a **synchronous**
   [`RenderCoordinator::requestPreviewViewData()`](../orc/gui/render_coordinator.cpp#L576-L586),
   which holds `queue_mutex_` — the same mutex the render worker needs to
   dequeue its next job — while calling
   [`get_colour_preview_carrier()`](../orc/core/preview_view_registry.cpp#L190-L191)
   ([histogram](../orc/core/preview_view_registry.cpp#L500-L501)). That
   function ([`video_sink_stage.cpp`](../orc/plugins/stages/sinks/common/video_sink_stage.cpp#L2389-L2677))
   has no per-frame result cache: it rebuilds the look-around field set, runs
   `decodeFrames()` (FFT-based for `transform2d/3d`), extracts the
   vectorscope unconditionally, and then
   [copies three `double` planes element by element](../orc/plugins/stages/sinks/common/video_sink_stage.cpp#L2661-L2670)
   (~17 MB allocated and written) and returns them by value.
   The render worker already decoded the same frame and already attached
   `vectorscope_data` to the result
   ([`preview_renderer.cpp:1115`](../orc/core/preview_renderer.cpp#L1115),
   [`orc_rendering.h`](../orc/sdk/include/orc/stage/preview/orc_rendering.h#L59-L88)) —
   and no GUI code reads it.
2. **The vectorscope raster.** [`renderVectorscope()`](../orc/gui/preview/vectorscope_dialog.cpp#L750-L1300)
   rebuilds a 1024×1024 `QImage` on the GUI thread per frame: two
   megapixel count buffers, Bresenham linking of ~700 k samples, a separable
   Gaussian "spot" over the trace box, a dwell histogram, a per-pixel colourise
   loop with `double` inverse-matrix maths, then `QPixmap::fromImage` and a
   `QPixmap::scaled(SmoothTransformation)` on every new pixmap
   ([`AspectRatioLabel`](../orc/gui/preview/vectorscope_dialog.cpp#L295-L320)).
   This is textbook GPU work: point accumulation with additive blending, a
   blur pass, a colour-mapping pass.
3. **The waveform monitor raster.** [`accumulate()`](../orc/gui/waveformmonitorwidget.cpp#L154-L192)
   reallocates a vector-of-vectors (~900 heap allocations) and scans ~520 k
   samples with a `double` mV conversion each; [`rebuildImage()`](../orc/gui/waveformmonitorwidget.cpp#L285-L365)
   then does an area-max reduction, a blur and a `setPixel()` loop into a new
   `QImage`. Same GPU shape as the vectorscope.
4. **The preview blit.** [`FieldPreviewWidget::paintEvent`](../orc/gui/fieldpreviewwidget.cpp#L112-L132)
   and [`FrameViewportWidget::paintEvent`](../orc/gui/frame_viewport_widget.cpp#L82-L95)
   call `drawImage(targetRect, image)` with `SmoothPixmapTransform` on every
   paint, resampling the full frame from source size with no cached scaled
   copy; `event->rect()` is ignored. Repaints fire on every mouse move over
   the widget ([`:219`](../orc/gui/fieldpreviewwidget.cpp#L219)) and, in the
   dropout editor, on every hover and drag step at up to 8× zoom where the
   widget is ~6356×5000 logical pixels.
5. **Throwaway presenters.** [`onFrameTimingDataReady`](../orc/gui/mainwindow.cpp#L5026-L5038)
   and [`onWaveformMonitorDataReady`](../orc/gui/mainwindow.cpp#L5165-L5170)
   construct a `RenderPresenter` and call `setDAG()` on the GUI thread per
   frame. `setDAG()` fingerprints every node (SHA-256 plus a `stat()` per
   source file, [`frame_provenance.cpp`](../orc/core/frame_provenance.cpp#L210-L225)).
6. **Histogram plot churn.** [`populatePlot()`](../orc/gui/preview/histogram_dialog.cpp#L250-L262)
   deletes and re-creates every `QGraphicsItem` in up to three plots and calls
   `replot()` three times per frame; [`PlotWidget::replot()`](../orc/gui/plotwidget.cpp#L362-L414)
   rebuilds every `QPainterPath` from every point with antialiasing on and no
   decimation.
7. **Payload copies.** `previewReady` is a queued signal whose slot takes
   `PreviewRenderResult` by value ([`render_coordinator.h:1018`](../orc/gui/render_coordinator.h#L1018),
   [`mainwindow.h:158`](../orc/gui/mainwindow.h#L158)); Qt copies the ~2 MB
   RGB buffer into the event and again into the slot argument, and
   [`previewImageToQImage`](../orc/gui/preview_image_qt.cpp#L18-L47) copies it
   a third time. Frame timing and waveform payloads (six `int16` vectors
   covering the whole frame) travel the same way, and both dialogs request the
   same extraction independently
   ([`render_coordinator.cpp:1466`](../orc/gui/render_coordinator.cpp#L1466)).

### Work on the render worker

8. **DAG re-execution with artifact caching disabled** for every preview frame
   ([`ensure_node_executed`](../orc/core/preview_renderer.cpp#L1004-L1017)).
   Not GPU work and not in scope here, but it bounds what any drawing change
   can achieve on deep graphs.
9. **The chroma decode itself** (`decodeFrames()`), single-threaded per
   frame, plus look-ahead/look-behind frames. Porting the decoders to the GPU
   is out of scope.
10. **The YUV→sRGB conversion** ([`colour_preview_conversion.cpp:221-248`](../orc/sdk/src/colour_preview_conversion.cpp#L221-L248)):
    one `double`-precision pass over every pixel with a transfer LUT, into a
    freshly resized RGB buffer. Directly expressible as a fragment shader.
11. **The greyscale conversion** ([`preview_helpers.cpp:234-272`](../orc/sdk/src/preview_helpers.cpp#L234-L272))
    with an integer divide per pixel, an optional second full pass for the
    inactive-area mask ([`:361-400`](../orc/sdk/src/preview_helpers.cpp#L361-L400))
    and a third for dropout burn-in ([`render_dropouts`](../orc/core/preview_renderer.cpp#L681-L718)).

### Fan-out and scheduling

12. The playback `QTimer` (40 ms PAL / 33 ms NTSC,
    [`previewdialog.cpp:910-937`](../orc/gui/previewdialog.cpp#L910-L937))
    drives [`updateAllPreviewComponents()`](../orc/gui/mainwindow.cpp#L5285-L5338),
    which fans out serially on the GUI thread to every open dialog and pushes
    roughly 9–11 requests per frame onto a **single FIFO worker**
    ([`workerLoop`](../orc/gui/render_coordinator.cpp#L609-L646)). Only
    preview requests are ever discarded as superseded
    ([`discardQueuedPreviewsLocked`](../orc/gui/render_coordinator.cpp#L333-L342));
    VBI, observation, closed-caption, frame-timing and waveform requests queue
    up in front of the next render.
13. [`updateVideoParameterObserverDialog()` and `updateNtscObserverDialog()`](../orc/gui/mainwindow.cpp#L5459-L5470)
    are the same function, and both are called per frame
    ([`:5298-5299`](../orc/gui/mainwindow.cpp#L5298-L5299)), so two of the
    four observation requests per frame are always discarded as stale after
    the worker has done the work.
14. Opening the Video Parameter, NTSC or Closed Caption observer arms a
    **whole-node background sweep**
    ([`render_presenter.cpp:1730-1734`](../orc/presenters/src/render_presenter.cpp#L1730-L1734),
    [`sweepNodeForObservation`](../orc/presenters/src/render_presenter.cpp#L775-L806))
    on `hardware_concurrency()/2` worker threads
    ([`core_init.cpp:44-54`](../orc/presenters/src/core_init.cpp#L44-L54)),
    each with its own DAG frame renderer, competing with playback for cores.
    Plain playback also enqueues prefetch observation work on every render
    ([`render_presenter.cpp:1832`](../orc/presenters/src/render_presenter.cpp#L1832)).
15. Every preview render is followed by an `ObservationCache::get_field()`
    call for each field of the displayed frame
    ([`render_presenter.cpp:1806-1817`](../orc/presenters/src/render_presenter.cpp#L1806-L1817)),
    whose only purpose is to fill the shared store for the observer dialogs —
    nothing reads `ObservationCache::get_observation_context()` anywhere in
    the tree. This is **not** a second DAG execution: the cache's renderer
    keeps artifact caching on
    ([`dag_frame_renderer.cpp:207`](../orc/core/dag_frame_renderer.cpp#L207)),
    and a stage's `execute()` is called once per renderer, not once per frame
    (measured: one execution across four frames through one cache). What it
    does cost per frame is a second lazy frame materialisation and a second
    observer pass, on the render worker, in series with the preview.
    Separately, the preview's own execution runs with the artifact cache
    **disabled**
    ([`preview_renderer.cpp:1004-1017`](../orc/core/preview_renderer.cpp#L1004-L1017)),
    so every preview render re-runs every upstream stage's `execute()`. That
    is the larger of the two and is measured by Task 1.1 before anything is
    changed.

The observer arithmetic itself is not where the time goes. Records are keyed
by node provenance and persisted in the sidecar, with the in-memory store a
read-through cache over it
([`observation_store.h:96-99`](../orc/core/include/observation_store.h#L96-L99)),
so an observation is computed once per (node fingerprint, frame, observer
version) and only recomputed when an upstream parameter edit changes the
fingerprint. The pass runs only at the node being rendered, not at every
stage, and passthrough aliasing back-fills upstream nodes whose output is
byte-identical
([`dag_frame_renderer.cpp:476-486`](../orc/core/dag_frame_renderer.cpp#L476-L486)).
Per frame the observers touch a handful of lines each: three VBI lines per
field for biphase
([`biphase_observer.cpp:527`](../orc/core/observers/biphase_observer.cpp#L527)),
one line each for closed captions, white flag and FM code, three sampled
lines' burst windows for burst level
([`burst_level_observer.cpp:73-75`](../orc/core/observers/burst_level_observer.cpp#L73-L75)),
one line slice each for white SNR and black PSNR, a burst window on every
other line for colour frame phase
([`colour_frame_phase_observer.cpp:194`](../orc/core/observers/colour_frame_phase_observer.cpp#L194)),
and no pixel work at all for field quality
([`field_quality_observer.cpp:66-90`](../orc/core/observers/field_quality_observer.cpp#L66-L90)).
That is well under a millisecond of scalar work per frame, dwarfed by the DAG
execution that produces the frame it reads. Moving it to the GPU would mean
uploading a full field to measure a few hundred samples. The observer-related
wins are elsewhere: stop sweeping while playing (Task 2.2), stop duplicating
the request pair (Task 1.3), and measure the two frame materialisations in
finding 15 before touching them (Task 2.6).

The analysis charts (dropout, SNR, burst level) are the one family that
already behaves: they move only a marker, throttled at 16 ms
([`analysisdialogbase.cpp:96-108`](../orc/gui/analysisdialogbase.cpp#L96-L108)),
with a marker `boundingRect()` that avoids repainting the plot
([`plotwidget.cpp:836-857`](../orc/gui/plotwidget.cpp#L836-L857)). The node
graph editor does no per-frame work.

## Candidates ranked

| Rank | Work | Where it runs today | GPU? | Phase |
|---|---|---|---|---|
| 1 | Second/third chroma decode + 17 MB plane copy for scopes, under `queue_mutex_` | GUI thread | No — reuse the render's carrier | 1 |
| 2 | Whole-node sweep on N/2 threads while playing | background pool | No — pause during playback | 2 |
| 3 | Uncoalesced observer requests on one FIFO worker; duplicated observation pair | worker | No — supersede per kind | 1, 2 |
| 4 | Vectorscope megapixel raster + pixmap scale | GUI thread | **Yes** | 4 |
| 5 | Waveform monitor raster | GUI thread | **Yes** | 4 |
| 6 | Preview smooth rescale per paint (incl. mouse moves, dropout editor zoom) | GUI thread | **Yes** | 3 |
| 7 | Throwaway `RenderPresenter::setDAG()` per frame; duplicated timing/waveform extraction | GUI thread + worker | No | 1 |
| 8 | 3× copies of the RGB payload; queued copies of timing vectors | worker→GUI | No — `shared_ptr` | 1 |
| 9 | Histogram item churn; `PlotWidget` full path rebuild without decimation | GUI thread | Mostly no | 2, 6 |
| 10 | YUV→sRGB and greyscale conversion passes; mask and dropout burn-in passes | worker | **Yes** | 5 |
| 11 | Per-pixel integer divide in greyscale scaling; `push_back` plane copy | worker | No — LUT / memcpy | 2 |
| 12 | Preview execution with the artifact cache disabled; second frame materialisation and observer pass per render | worker | No — measure first | 1, 2 |

Not candidates: the observer arithmetic (finding 15: a few lines per field,
already computed once per provenance and persisted), the chroma decoders (a
decoder port, not a drawing change), DAG execution with caching disabled, the
observation scheduler's own work,
the closed-caption assembler, the node graph editor, and `FrameTimingWidget`,
which already decimates to pixel columns.

---

## Phase 1 — Measure, then remove the GUI-thread stalls

No GPU code. Every task here is a prerequisite for a GPU canvas to help at
all, and together they are expected to be the largest single improvement to
the symptom in #293.

### Task 1.1 — Per-frame timing instrumentation

Add `QElapsedTimer`-based counters, off by default, enabled through the
existing logging settings: render request→ready latency, worker queue depth
at enqueue, time spent in `updateAllPreviewComponents()`, time spent in each
scope refresh, and paint time per preview widget. Emit one line per displayed
frame, and a rolling summary every second.

The GUI has one logger, not a tree of named ones, so the switch is its own
setting in Tools > Logging rather than a `gui.frame_timing` logger name.
Records are written at info level because the setting is already opt-in;
making them debug would mean two unrelated settings had to agree before
anything appeared.

Acceptance:
- With the logger off there is no measurable overhead (no allocation, no
  string formatting on the hot path).
- A playback session with vectorscope + histogram + both observers open
  yields a per-frame breakdown that identifies the largest consumer.
- Tier 1 `gui-logic` test for the summary formatter.

### Task 1.2 — Scopes read the render's own carrier

Make the render worker the only place a colour carrier is decoded for the
preview. Extend `RenderPreviewRequest` with the scope selections the GUI
currently sends through `PreviewCoordinate` (vectorscope active-area /
line range, histogram on/off) and have `PreviewRenderer::render_output`
produce the vectorscope and histogram payloads from the carrier it already
holds in [`render_colour_carrier_preview`](../orc/core/preview_renderer.cpp#L1079-L1136)
(the active-picture vectorscope is already attached; add the narrowed
extraction and the histogram extraction from
[`preview_view_registry.cpp`](../orc/core/preview_view_registry.cpp#L344-L455)
as functions over a carrier reference). Deliver them with the render.

Implemented as a `PreviewScopeRequest` carried on the render request and a
`PreviewScopePayloads` carried back on a shared `PreviewRenderDelivery`, so
neither the SDK's `PreviewRenderResult` nor the view registry's request
contract had to change. The extraction itself moved out of the registry into
`carrier_scope_extraction` in core, which both the registry views and the new
`RenderPresenter::getPreviewScopes()` now call, so the two paths cannot
diverge. A signal-domain vectorscope has no carrier to share and no
histogram, so it still goes through the registry — but on the worker, inside
the same call.

`requestPreviewViewData()` stays synchronous. It is now reached only by the
export path and by explicit scope clicks, neither of which is per-frame, and
converting it would add a request kind for no measured gain. Revisit if the
Task 1.1 counters show a click stalling playback.

Acceptance:
- `get_colour_preview_carrier()` is never called on the GUI thread during
  playback.
- One carrier fetch per displayed frame with both scopes open
  (`PreviewDelivery_CarriesScopePayloadsFromOneCall`).
- No carrier fetch at all when both scope dialogues are closed
  (`PreviewDelivery_NoScopesRequestedFetchesNoCarrier`).
- The line selection reaches the worker unchanged, so the plot is of the
  same lines the synchronous path would have used.

### Task 1.3 — One observation request pair per frame

Delete the duplicate call: `updateAllPreviewComponents()` calls
`refreshObserverDialogs()` once. Remove the two forwarding wrappers.

Acceptance:
- Exactly two `requestObservations()` calls per frame in frame mode, one in
  field mode, with either observer dialog open. `refreshObserverDialogs()` is
  now the only entry point; the two wrappers that both forwarded to it are
  gone.

### Task 1.4 — Shared frame-sample extraction; no presenters on the GUI thread

Introduce one `FrameSamples` request kind that both the frame-timing and
waveform-monitor dialogs consume, delivered once per frame as
`std::shared_ptr<const FrameSampleSet>`. Include the `VideoParameters` the
dialogs need in the response so
[`onFrameTimingDataReady`](../orc/gui/mainwindow.cpp#L5026-L5038) and
[`onWaveformMonitorDataReady`](../orc/gui/mainwindow.cpp#L5165-L5170) no
longer construct a `RenderPresenter` or call `setDAG()`. Remove the
`composite_samples = y_samples` duplicate copy in
[`getFieldSamplesForTiming`](../orc/presenters/src/render_presenter.cpp#L2467-L2578)
in favour of a shared view.

Acceptance:
- With both dialogues open, one extraction per frame
  (`FrameSamples_OneExtractionServesBothDialogues`).
- No `RenderPresenter` construction on the GUI thread during playback: the
  video parameters travel with the samples
  (`FrameSamples_VideoParametersTravelWithTheSamples`).
- Frame modes still report both field indices, flat field modes one
  (`FrameSamples_FrameModeReportsBothFieldIndices`).
- The composite duplicate is gone: `LineSampleData::composite()` substitutes
  the luma buffer for a Y/C source instead of copying it.

### Task 1.5 — Large payloads travel by shared pointer

Change the coordinator signals that carry variable-size buffers to
`std::shared_ptr<const T>`: `previewReady`, `lineSamplesReady` and the new
`frameSamplesReady`. Keep the by-value signatures out of the public presenter
surface; only the coordinator's signals change.

The VBI, observation and closed-caption signals keep their by-value view
models. Those are small fixed-size structs of scalars and short optionals; a
`make_shared` per emission would cost an allocation to avoid copying a couple
of hundred bytes, which is a pessimisation, not an optimisation. The rule
applied is payload size, not signal count.

Acceptance:
- One `PreviewRenderDelivery` per render, shared by every connected consumer
  rather than copied per connection
  (`PreviewDelivery_IsSharedNotCopiedPerConnection`, and the same assertion
  for line samples).
- `previewImageToQImage` remains the only buffer copy on the GUI side until
  Phase 3 replaces it with a texture upload.

---

## Phase 2 — Queue discipline and CPU hot loops

Still no GPU code. Removes the remaining scheduling causes and the cheap CPU
wins so that the GPU phases are measured against a clean baseline.

### Task 2.1 — Supersede queued requests per kind and node

Generalise [`discardQueuedPreviewsLocked`](../orc/gui/render_coordinator.cpp#L333-L342):
a new request of kind K for node N discards queued, not-yet-started requests
of the same kind for the same node (VBI, observations, line samples, frame
samples). Closed-caption batches keep their pacing but are bounded to one
in-flight batch. Preview requests keep their current behaviour.

Implemented as a `RequestCoalesceKey` on the base `RenderRequest`; a request
carrying one sweeps the queue of anything of its own type and key at enqueue.
The key is the node **and** which half of the frame the request is for, not the
node alone: the VBI and observation dialogues ask for both fields of a frame
and combine the two answers, so a node-only key would have let the second
field's request discard the first's and left those dialogues waiting for an
answer that was never asked.

Closed-caption reads are deliberately exempt. Each covers a frame no other
read covers — the dialogue assembles a trailing window — and
`issueClosedCaptionRequests()` already bounds the work by skipping frames it
has a request in flight for.

Acceptance:
- Coordinator unit tests: a burst of N navigations with every dialog open
  leaves at most one queued request per kind before the next render.
- Stale-response suppression still holds for each kind.

### Task 2.2 — Pause whole-node sweeps during playback

Add a playback-active flag from `PreviewDialog` through `RenderCoordinator`
and `IRenderPresenter` to the `ObservationScheduler`. While set, `kSweep`
work is not dequeued (prefetch and interactive priorities continue). Resume
on stop/pause.

`ObservationScheduler::set_sweep_paused()` gates the dequeue, not the work: an
item already in flight runs to completion, and queued sweep work stays at the
head of its queue. The workload snapshot gains `sweep_deferred` (set only while
the flag is on *and* sweep work is actually queued), which reaches the status
line as "Observations paused during playback… N%" — a percentage that has
stopped moving needs a reason, or it reads as a hang. `PreviewDialog` gained a
single `setPlaying()` through which every playback transition now passes, so
the signal cannot go out of step with the button.

Acceptance:
- Scheduler unit test: with playback active, only `kInteractive` and
  `kPrefetch` items run; sweep items run to completion after the flag
  clears.
- The status-bar progress message reflects the paused state.

### Task 2.3 — Histogram and plot series update in place

`HistogramDialog::populatePlot()` keeps its series and marker items and calls
`setData()` + one `replot()` per visible plot. In `PlotSeries::updatePath()`
decimate to at most two points per pixel column (min/max) when the series is
longer than the plot width, in the same style as
[`FrameTimingWidget`](../orc/gui/frametimingwidget.cpp#L848-L914).

The histogram keeps a per-plot record of what its zones, guide lines and trace
item were built for — channel kind, video system and theme, none of which
change between displayed frames — and rebuilds them only when one of those
does. Decimation is a free function in `plot_series_decimation`, called from
`PlotSeries::updatePath()`, so every `PlotWidget` consumer gets it. Points
outside the axis range are grouped apart rather than folded into the edge
columns, so an off-plot value cannot decide what the first or last visible
column looks like.

Acceptance:
- No `QGraphicsItem` construction per frame in the histogram (item count
  stable across 100 updates, Tier 3 offscreen test).
- Tier 1 test for the decimation helper: identical output for series shorter
  than the plot width, ≤ 2·width points otherwise, extrema preserved.

### Task 2.4 — Waveform monitor accumulation buffer

Replace the vector-of-vectors with one flat `std::vector<uint32_t>` sized
`x_samples × y_bins`, retained across frames and zeroed with `assign`.
Replace `setPixel()` with scanline writes as the vectorscope already does.

The buffer is a `WaveformCountGrid`: a flat column-major cell array that
re-uses its allocation whenever the shape is unchanged, which is every frame of
a playing preview. `rebuildImage()` reduces each output pixel's cells through
the grid and writes the row directly, and the intermediate brightness plane is
gone with it — the two passes had nothing between them.

Acceptance:
- One allocation on first use and none per frame (the Tier 1 test asserts the
  cells keep their address and capacity across 100 frames; a shrink keeps the
  capacity too).
- Output image identical before and after for a fixed sample set (a Tier 3
  test renders flat levels and checks each draws one band, at the right
  height, reproducibly).

### Task 2.5 — Core render loop micro-fixes

- 1024-entry lookup table for `scale_10bit_to_8bit` rebuilt only when the
  levels change, removing the per-pixel integer divide.
- `resize` + row `memcpy` (or a `std::transform` to `float`, see Phase 5)
  in place of the `push_back` plane copy in `get_colour_preview_carrier()`.
- `paintEvent` in both preview widgets clips to `event->rect()` and sets
  `WA_OpaquePaintEvent`; drop the second background fill.

The table covers the 1024 samples the 10-bit domain holds and remembers the
levels it was built for, so a playing preview builds it once; a sample outside
the domain — which a malformed source can produce — still goes through the
formula, so the table is an optimisation and never a change of result. The
plane copy moved into `carrier_plane_copy.h` so it could be unit-tested against
the per-sample read it replaced.

Acceptance:
- Greyscale preview output byte-identical (existing preview helper tests
  extended with a fixed-frame comparison).
- Carrier planes byte-identical (stage unit test).

### Task 2.6 — Measure and then remove the duplicate frame materialisation

Driven by the Task 1.1 numbers, not by inspection: finding 15 describes two
costs on the render worker per preview frame, and neither has been measured.

First report, from the Task 1.1 counters, the per-frame cost of the preview's
cache-disabled execution and of the `ObservationCache::get_field()` calls that
follow it. Then, only where the numbers justify it:

- If the cache-disabled execution dominates, narrow it. The cache is disabled
  so that a stage's `cached_output_` is populated for preview capability
  queries; establish which stages actually need that and whether the
  capability handle can be fetched without forcing re-execution of the whole
  upstream chain.
- If the `get_field()` pass dominates, have the preview's own execution
  produce the observation records rather than materialising the frame a
  second time through a separate renderer. Nothing reads the cache's
  observation context, so the store is the only output that must be
  preserved.

The measurement is in place and the change is not: both paths are now timed on
the worker and carried back with the frame as `PreviewRenderCostView`, which
the profiler reports as the `dag-exec` and `obs-fill` segments. Both are
excluded from the GUI-thread total and from the dominant-segment report,
because they are parts of the render wait rather than additions to it. What to
do next depends on what a real playback session says, which is the point of the
task.

Acceptance:
- The measured per-frame cost of both paths is recorded in the PR, before and
  after, on the same playback session.
- Observation records for a played frame are byte-identical to today's, and
  an observer dialog opened on that frame reports the same values.
- No change to the number of DAG executions per node (one per renderer, as
  measured today).

---

## Phase 3 — RHI foundation and the preview surface

First GPU phase. Establishes the build, fallback and test conventions every
later phase reuses, and replaces the per-paint smooth rescale with a textured
quad. The preview surface is the simplest shader in the plan, which is why it
carries the infrastructure.

### Task 3.1 — Build integration

- `find_package(Qt6 COMPONENTS Gui ShaderTools)` under a new
  `ORC_GUI_GPU_RENDER` option (default ON, auto-OFF when Qt < 6.7 or
  ShaderTools is absent, with a configure-time warning).
- `qt6_add_shaders()` for `orc/gui/gpu/shaders/*.vert|*.frag` into a Qt
  resource prefix; shaders written in GLSL 440 (Vulkan flavour).
- `flake.nix`: add `qt6.qtshadertools` to the dev shell and CI shell.
- Verify the Windows `install-qt-action`, Homebrew and Flatpak KDE SDK builds
  find `Qt6ShaderTools`; note in the PR what could not be verified locally.
- `ORC_GUI_GPU_RENDER` compiles in both paths; a run-time policy chooses.

Acceptance:
- All four CI workflows build with the option on.
- Building with `-DORC_GUI_GPU_RENDER=OFF` produces a GUI identical to
  today's.

### Task 3.2 — Run-time availability policy and fallback seam

`orc/gui/gpu/gpu_surface_policy.{h,cpp}`: decides whether a GPU surface is
used, from (a) the build option, (b) `ORC_GUI_GPU_RENDER=0` in the
environment, (c) a persisted setting in the logging/settings dialog, and
(d) a `QRhiWidget::renderFailed` signal at run time, which switches that
window to the raster widget for the rest of the session and logs once.
Expose the active backend name (from `QRhiWidget::rhi()->backendName()`) in
the About dialog.

Acceptance:
- Tier 1 test for the policy decision table (no Qt GUI).
- Forcing failure (env var pointing at an unavailable API) shows the raster
  preview with no visual difference apart from the About text.

### Task 3.3 — `FramePreviewSurface` (QRhiWidget)

One widget, in `orc/gui/gpu/`, used by both `FieldPreviewWidget` and
`FrameViewportWidget` through a small `IFrameSurface` interface that both
already effectively implement (`setImage`, `setGeometry`, overlay hooks).

- Upload: `PreviewImage` RGB888 → `QRhiTexture::RGBA8` (RHI has no packed
  RGB format). The RGBA expansion runs on the coordinator worker before the
  `previewReady` emit, so the GUI thread does one `QRhiResourceUpdateBatch::uploadTexture`
  per frame and no pixel loop. Texture is reallocated only on size change.
- Draw: one quad at `FrameViewGeometry::targetRect()`; linear filtering
  (matching the current smooth transform), nearest available as an option for
  1:1 inspection. Frame-flat and sequential layouts are the same quad — the
  layout is baked into the image as today.
- Overlays (dropout bands, crosshairs, line marker, editor bands and handles)
  are drawn in the same render pass as line-list and quad primitives from a
  Qt-Core-only `OverlayPrimitiveBuilder` that consumes the same image-space
  data the current `paintEvent` code uses and the same `FrameViewGeometry`
  mapping. Antialiasing stays off for crosshairs as it is now.
- HiDPI: quad and primitives are built in device pixels from
  `devicePixelRatio()`.

Acceptance:
- Pixel-exact geometry: Tier 1 tests that the quad and primitive coordinates
  equal what `FrameViewGeometry` and the existing overlay code produce for
  the same inputs.
- Mouse-move repaint cost is one draw call with no upload (verified via 1.1
  counters).
- Existing `FieldPreviewWidget` and dropout-editor Tier 3 tests run under
  `QT_QPA_PLATFORM=offscreen` with the policy forcing the raster path and
  pass unchanged.
- A `QRhiWidget::Api::Null` smoke test (pipeline creation, one upload, one
  frame) labelled `gui-widget`, skipped when the widget cannot initialise
  on the test host.

### Task 3.4 — Dropout editor viewport on the surface

`FrameViewportWidget` currently grows to `displaySize()` and lets the
`QScrollArea` pan. A GPU surface must stay viewport-sized, so add a pan
origin to `FrameViewGeometry` (`visibleOrigin`, `imageFromWidget` and
`widgetFromImage` include it) and keep the scroll bars as controls that
drive the origin. Zoom-at-cursor uses the existing `scrollAfterZoom` maths on
the origin instead of the scroll bar values.

Acceptance:
- `FrameViewGeometry` Tier 1 tests extended for the origin: round-trip
  `imageFromWidget(widgetFromImage(p)) == p` at every zoom level in the
  0.25–8.0 range, and `scrollAfterZoom` keeps the point under the cursor
  fixed.
- Dropout editor hit-testing (`hitTest` mirrors of `paintOverlay`) gives the
  same results before and after (existing dialog tests via the
  `IRenderPresenter` seam).

---

## Phase 4 — Vectorscope and waveform monitor on the GPU

The largest GUI-thread raster cost after Phase 1, and the best fit for the
hardware: both instruments are "accumulate many points with additive
blending, blur, colour-map".

### Task 4.1 — Scope canvas and shared passes

`ScopeCanvas` (QRhiWidget) in `orc/gui/gpu/` with three reusable passes:

1. **Accumulate** — a vertex buffer of points (and, when trace lines are on,
   a line strip) rendered with additive blending into an offscreen
   `R32F`/`RGBA16F` render target. This replaces `hit_count` / `transit_count`
   and the Bresenham linking, which the rasteriser performs.
2. **Spread** — two full-screen passes with a separable Gaussian whose radius
   comes from the same `spot_sigma` formula as
   [`vectorscope_dialog.cpp`](../orc/gui/preview/vectorscope_dialog.cpp#L969-L975);
   weights uploaded as a small uniform array.
3. **Map** — a full-screen fragment pass that applies the dwell / gain knee and
   the colour mapping, composited over an underlay texture.

Graticules, zones, axes and ticks keep their existing `QPainter` code: they
are painted into a `QImage` only when system, levels, mode or size change,
and uploaded as the underlay (vectorscope composite mode) or overlay
(decoded mode, waveform grid) texture.

Acceptance:
- Vertex generation (`VectorscopeData::samples` → canvas-space points;
  waveform samples → column/mV points) and uniform packing are Tier 1
  tested against the CPU renderer's own mapping functions.
- The CPU renderers are retained untouched as the fallback path.

### Task 4.2 — Vectorscope on the canvas

Replace `AspectRatioLabel` + `QPixmap` with `ScopeCanvas` when the policy
allows. Field selection, line range, blend-colour, defocus and gain map to
uniforms; the colourise maths from
[`vectorscope_dialog.cpp:1200-1240`](../orc/gui/preview/vectorscope_dialog.cpp#L1200-L1240)
moves verbatim into the map shader. `d_->last_data` copy becomes a
`shared_ptr` (from Phase 1.5).

Acceptance:
- Side-by-side visual check against the CPU renderer on colour bars and a
  real capture, in both acquisition modes, at three gains (manual checklist
  in the PR, all three platforms).
- Per-frame GUI-thread time for the dialog is the upload plus draw only
  (1.1 counters).
- Dialog Tier 3 tests pass with the raster fallback forced.

### Task 4.3 — Waveform monitor on the canvas

Points at (column, mV→bin) accumulate at output resolution, so the area-max
reduction that avoids bin aliasing is no longer needed; use the `Max` blend
operation for the accumulate pass to keep the "brightest cell wins" reading
of the existing renderer, then spread and map with the background→trace
colour ramp. Tick marks, labels and level lines stay as the overlay texture.

Acceptance:
- Visual parity check as in 4.2.
- `accumulate()` (Phase 2.4 form) and `rebuildImage()` remain as the
  fallback and are not called when the canvas is active.

---

## Phase 5 — Colour and greyscale conversion in the fragment shader

Moves the per-pixel conversion off the render worker and stops producing an
RGB buffer for display. This is a throughput win (frames per second the
worker can deliver) rather than a responsiveness win, which is why it follows
Phase 4.

### Task 5.1 — Plane payload in the render result

Add a `PreviewPlanes` payload to `PreviewRenderResult`: for the colour domain
Y, U, V as `float` (converted at the copy in `get_colour_preview_carrier()`,
halving the bytes moved today) with `cvbs_black`, `y_range`, `uv_range`,
matrix coefficients and the active-area rectangle; for the signal domain the
10-bit samples as `uint16` with black/white/sync/peak levels and the field
weave already applied. RGB rendering stays available on request (PNG export,
raster fallback, tests) and is skipped when the request says a GPU surface
will consume the planes.

Acceptance:
- Contract test that a request flagged for planes produces planes and no RGB,
  and vice versa.
- Export path unchanged (existing PNG tests).

### Task 5.2 — Conversion shaders on `FramePreviewSurface`

- Colour: three `R32F` textures; fragment shader performs the
  [`colour_preview_conversion.cpp`](../orc/sdk/src/colour_preview_conversion.cpp#L221-L248)
  maths with the transfer function as a 4096-entry 1-D texture (same nodes
  as `TransferLut`).
- Greyscale: one `R16` texture; per-pixel level scaling in the shader.
- The inactive-area mask and dropout burn-in become shader/overlay work
  ([`preview_helpers.cpp:361-400`](../orc/sdk/src/preview_helpers.cpp#L361-L400),
  [`render_dropouts`](../orc/core/preview_renderer.cpp#L681-L718)), so those
  passes are skipped on the worker when planes are requested.

Acceptance:
- Golden comparison: a `QRhiWidget::Api::Null` cannot produce pixels, so
  parity is established by a manual A/B against the CPU path on all three
  platforms (checklist in the PR), plus a Tier 1 test that the uniform
  values equal the constants the CPU path uses.
- Worker-side time per colour frame excludes the conversion pass (1.1
  counters).

---

## Phase 6 — Plot series on the GPU (conditional)

Only if the Phase 1.1 measurements after Phase 2 still show `PlotWidget`
path rebuilds as a significant per-frame cost for the line scope, histogram
or analysis charts. Otherwise this phase is dropped.

### Task 6.1 — `SeriesCanvas`

A `QRhiWidget` that draws series as line strips (and bars as quads) from
decimated data, with grid, axes, legend and markers painted by the existing
`QPainter` code into an overlay texture refreshed only when they change.
`PlotWidget` gains a canvas mode selected by the Phase 3.2 policy; the
`QGraphicsView` mode stays as the fallback and for tests.

Acceptance:
- Public `PlotWidget` API unchanged; every dialog using it needs no edits.
- Marker-only updates (analysis charts) upload nothing and redraw one quad.
- All existing `PlotWidget` and dialog tests pass with the fallback forced.

---

## Verification across phases

- Every phase re-runs the Phase 1.1 measurement on the same three scenarios
  (playback alone; playback with vectorscope + histogram; playback with all
  observers and scopes open) and records the per-frame breakdown in the PR.
- Every GPU phase is validated on Linux (Nix, Vulkan or OpenGL), Windows
  (Direct3D 11) and macOS (Metal) before merge; what could not be validated
  locally is stated in the PR for reviewers, per `AGENTS.md` §11.
- The raster path remains the reference. No GPU phase deletes CPU rendering
  code; each replaces its use when the policy allows.
- `ctest -R MVPArchitectureCheck` and `-L sdk` gates after every phase: no
  Qt in core, no private headers, no plugin changes beyond payload shape.
