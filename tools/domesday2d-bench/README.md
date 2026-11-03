# Domesday2D bench tools

Research tools for the Domesday2D still-frame decoder investigation
([`docs-tech/domesday2d-decoder-investigation.md`](../../docs-tech/domesday2d-decoder-investigation.md)). They are outside
the build and are not run in CI.

| File | What it is |
|---|---|
| `cvbs.py` | Reads a PAL `.cvbs` (`CVBS_U10_4FSC`) or ld-decode `.tbc` capture line by line, in IRE, without loading it |
| `vits.py` | Measures the chain from the VITS (CCIR 17, 18, 330, 331 on lines 19 and 20 of each field): the 20T pulse's chroma/luma gain and delay, the multiburst, the chroma bars, differential gain and phase, burst jitter, the 20T's sideband symmetry, and the edge smear fitted to the long tails of the bar and 2T pulse. `separate_20t()` splits a 20T delay into the smear's share and a residual IEC 60856 group delay |
| `picture_chroma.py` | Decodes each picture line against its own burst and fits the line-alternating (Hanover) error on flat colour as a 2×2 matrix |
| `registration.py` | Registers field 2 against field 1 of a still frame, horizontally (samples) and vertically (field lines) |
| `phase2_vits.py` | Phase 2 steps 1–4 from the VITS, per capture at the inner, middle and outer radius |
| `phase2_picture.py` | Phase 2 steps 4 and 5 on the picture, over frames spread across a capture |
| `bench.py` | The Phase 1 still bench in Python: the stills, their truth and masks as `DecoderStillTruth` builds them, decode-orc's raw YUV decodes, and a port of the still metrics (`measure`) plus the "\|\|\|\|" dot amplitude (`edge_dots`). The `channel` case is the clean bench through the Phase 2 replay response |
| `proto.py` | The carrier lattice of the woven frame (fitted to the bursts), per-row demodulation and remodulation, and PalColour's kernels sampled on field or frame rows |
| `transform.py` | Transform PAL: decode-orc's `transform2d` on a field, and on the frame raster with separate U and V reflections; amplitude matching, band-restricted testing and a comb fallback as options |
| `encoder.py` | A composite encoder equivalent to videosynth's on the truth grid (`check_encoder` compares the two) |
| `training.py` | Procedural, licence-clean training pictures (maps, text, natural texture, graphics) |
| `lslcd.py` | Least-squares-trained separators: one kernel per carrier class, on field or frame windows |
| `phase3_lattice.py` | Phase 3 step 1: the lattice measured on the bench, and on a capture's bursts |
| `phase3_lslcd.py` | Phase 3 step 4: trains the separators and reports the generalisation gap |
| `phase3_run.py` | Phase 3 steps 2, 3 and 5: scores every prototype on the clean and noisy bench |
| `adaptive.py` | The Phase 4 candidate after separation: an edge-preserving vertical chroma fit steered by the local luma detail, re-encode passes, and the residual handed back to luma against the blanked lines' noise |
| `hanover.py` | The Phase 2 per-line chroma errors put on the bench (phase, alternating crosstalk, line jitter shared with the burst or not), and the anti-Hanover measures: a per-frame 2×2 correction, per-line burst referencing, masked field-line averaging |
| `phase4_run.py` | Phase 4 steps 1, 3 and 4: the selection, refinement and bandwidth variants on the clean and noisy bench, their prototype time, and a check on the procedural training pictures |
| `phase4_hanover.py` | Phase 4 step 2 on the impaired bench: Hanover residue on flat colour, the green edges' vertical rise |
| `phase4_jitter.py` | Phase 4 step 2 on a capture: whether the picture's chroma follows its burst's line-to-line jitter |
| `equalise.py` | The Phase 5 channel equaliser: the inverse of the one-pole edge smear on the composite, a burst-referenced chroma gain, an inverse replay response (tried and dropped), the smear measured once per source from the VITS with its guards, and a capture read through the inverse |
| `phase5_bench.py` | Writes the Phase 5 videosynth projects: `pal_stills` with the four LaserDisc VITS lines, without and with the Phase 2 channel, clean and noisy |
| `phase5_run.py` | Phase 5 steps 1 and 2 on the channel bench: the Phase 4 decoder with each equaliser variant and parameter source |
| `phase5_vits.py` | Phase 5 steps 1 and 2 on captures: per-source and per-frame smear fits, and the VITS measured through the inverse |
| `phase5_decoders.py` | Phase 5 step 4: the equaliser as a stage of its own, in front of `pal2d`, `transform2d` and the Phase 4 decoder, its chroma gain on U/V or on the composite (the Stacker's shelf) |
| `phase5_upstream.py` | Phase 5 step 3: the equaliser behind `picture_band_limit`, `fm_beat_cancel` and the stacker |
| `phase6_compare.py` | Phase 6: decode-orc's `domesday2d` decoder and `channel_equalise` stage against these prototypes, in output codes |
| `phase7_real.py` | Phase 7 on Community South: calibrates the discs and writes GUI projects (`held.orcprj`, `cross.orcprj`) that decode held and single-frame stills with `pal2d`, `transform2d` and `domesday2d`; scores the decodes against a reference built from each held run without a decoder, and against each other across pressings; renders side-by-side crops |
| `phase7_rotation.py` | Phase 7: the still bench with its stills moved 0–3 frames along the PAL sequence, so each is decoded at every carrier phase, and every decoder scored at each |

The tools need numpy and scipy (and Pillow for `training.py`):

```sh
nix shell --impure --expr \
  'with import <nixpkgs> {}; python3.withPackages (p: [p.numpy p.scipy])' \
  -c python3 -I tools/domesday2d-bench/phase2_vits.py --frames 48 CAPTURE.cvbs
```

The Phase 3 tools read the still bench that the videosynth suite renders into `build/videosynth-output/` (configure with
`BUILD_VIDEOSYNTH_TESTS=ON` and run `ctest -R DecoderStillTruth` once). To score decode-orc's own decoders beside the
prototypes, decode the bench with the video sink first:

```sh
for case in pal_stills_clean pal_stills; do
  for d in pal2d transform2d; do
    ./build/bin/orc-cli --source PAL_CVBS_Source=input_path=$PWD/build/videosynth-output/$case/*.cvbs \
      --sink video_sink=output_path=DIR/${case}_$d.yuv:output_mode=raw:raw_format=yuv:decoder_type=$d:output_padding=1
  done
done
python3 -I tools/domesday2d-bench/phase3_lslcd.py --fonts DEJAVU_TTF_DIR --out LSDIR
DOMESDAY2D_LS_DIR=LSDIR python3 -I tools/domesday2d-bench/phase3_run.py --yuv-dir DIR --json results.json
```

Phase 4 (Pillow is needed only for `--fonts`):

```sh
DOMESDAY2D_LS_DIR=LSDIR python3 -I tools/domesday2d-bench/phase4_run.py --json p4.json \
  --fonts DEJAVU_TTF_DIR --training p4_training.json
python3 -I tools/domesday2d-bench/phase4_hanover.py --json p4_hanover.json
python3 -I tools/domesday2d-bench/phase4_jitter.py --frames 150 CAPTURE.cvbs...
```

Phase 5 (PyYAML for `phase5_bench.py`). The renders go to a scratch directory, `P5`:

```sh
python3 -I tools/domesday2d-bench/phase5_bench.py P5
for p in vits_clean vits_noisy chan_clean chan_noisy; do videosynth --project P5/$p.yaml; done
DOMESDAY2D_P5_DIR=P5 python3 -I tools/domesday2d-bench/phase5_run.py --json p5.json
DOMESDAY2D_P5_DIR=P5 python3 -I tools/domesday2d-bench/phase5_decoders.py --json p5_decoders.json
python3 -I tools/domesday2d-bench/phase5_vits.py --frames 48 --json p5_vits.json CAPTURE.cvbs...
```

For step 3, two more renders of `chan_noisy.yaml` are made by hand:

- `chan_noisy2`: every section's capture-layer `noise_seed` + 1000.
- `chan_beat_noisy`: `laserdisc.channel.fm_beat: {carrier_black_hz: 7088000, level_db: -24.0}`.

Then run the stages and score them:

```sh
orc-cli -i "PAL_CVBS_Source=input_path=P5/chan_noisy.cvbs" -f picture_band_limit -o "CVBSSink=output_path=P5/st_pbl.cvbs"
orc-cli -i "PAL_CVBS_Source=input_path=P5/chan_beat_noisy.cvbs" -f "fm_beat_cancel=carrier_black_mhz=7.088" \
  -o "CVBSSink=output_path=P5/st_fbc.cvbs"
orc-cli -i "PAL_CVBS_Source=input_path=P5/chan_noisy.cvbs[a]; PAL_CVBS_Source=input_path=P5/chan_noisy2.cvbs[b]" \
  -f "[a][b] stacker" -o "CVBSSink=output_path=P5/st_stack.cvbs"
DOMESDAY2D_P5_DIR=P5 python3 -I tools/domesday2d-bench/phase5_upstream.py --json p5_upstream.json
```

Phase 6. Decode the still bench with `domesday2d` and compare it with the prototype, and the same for the
equaliser on a Phase 5 render (at the smear Channel Calibration reads on it):

```sh
for case in pal_stills_clean pal_stills; do
  ./build/bin/orc-cli --source PAL_CVBS_Source=input_path=$PWD/build/videosynth-output/$case/videosynth_$case.cvbs \
    --sink video_sink=output_path=DIR/${case}_domesday2d.yuv:output_mode=raw:raw_format=yuv:decoder_type=domesday2d:output_padding=1
done
python3 -I tools/domesday2d-bench/phase6_compare.py decoder DIR
orc-cli -i "PAL_CVBS_Source=input_path=P5/chan_noisy.cvbs" \
  -f "channel_equalise=smear_fraction=0.1842:smear_tau_samples=5.7872" -o "CVBSSink=output_path=P5/st_eq.cvbs"
DOMESDAY2D_P5_DIR=P5 python3 -I tools/domesday2d-bench/phase6_compare.py equalise chan_noisy 0.1842 5.7872 P5/st_eq.cvbs
```

Phase 7. On a capture, with the Community South side's stack project for the disc maps (the projects, the decodes and
the crops go to OUT, which can be beside the captures; the crops are of BBC pictures and stay out of the repository):

```sh
python3 -I tools/domesday2d-bench/phase7_real.py setup STACK.orcprj OUT
(cd OUT && orc-cli held.orcprj --process && orc-cli cross.orcprj --process)
python3 -I tools/domesday2d-bench/phase7_real.py held OUT --json OUT/held.json
python3 -I tools/domesday2d-bench/phase7_real.py cross OUT --json OUT/cross.json
python3 -I tools/domesday2d-bench/phase7_real.py panel OUT OUT/panels
python3 -I tools/domesday2d-bench/phase7_real.py panel-bench YUV_DIR OUT/panels
```

`panel-bench` reads the still bench's decodes as Phase 6 makes them (`YUV_DIR/pal_stills[_clean]_<decoder>.yuv`, for
`pal2d`, `transform2d` and `domesday2d`) and puts the picture truth beside them.

The bench at every phase of the PAL sequence (`ROT` a scratch directory):

```sh
python3 -I tools/domesday2d-bench/phase7_rotation.py write ROT
for n in rot0 rot1 rot2 rot3 rot0_clean rot1_clean rot2_clean rot3_clean; do
  videosynth --project ROT/$n.yaml --output-root ROT
  for d in pal2d transform2d domesday2d; do
    orc-cli --source PAL_CVBS_Source=input_path=ROT/$n.cvbs \
      --sink video_sink=output_path=ROT/${n}_$d.yuv:output_mode=raw:raw_format=yuv:decoder_type=$d:output_padding=1
  done
done
python3 -I tools/domesday2d-bench/phase7_rotation.py score ROT --json rot.json
```

## Validation

Every measurement was checked on videosynth captures with known impairments before it was used on a real disc. The
projects are short: a 75 % bars or `OSM_TOWN` still with the VITS on the LaserDisc lines
(`line_injections: {placement: laserdisc, vits: [uk-national 19, vits20 20, itu-composite 332, itu-combination 333]}`)
and one impairment each.

| Impairment | Read as |
|---|---|
| `laserdisc.channel.group_delay: {preset: iec60856_nominal, scale: 1.0}` | 20T delay −139 ns; fitted scale 0.999 |
| … `scale: 0.5` | 20T delay −68 ns; fitted scale 0.499 |
| `channel.edge_smear: {fraction: 0.15, tau_samples: 9}` | tail fit 0.154, τ 8.4 samples; 20T delay −47.5 ns, all of it the smear's |
| `laserdisc.channel.response_db: [[0,0],[4.4,-2],[5,-6],[5.5,-20]]` | sideband asymmetry −1.6 / −3.2 / −4.8 dB at ±0.2 / 0.4 / 0.6 MHz (table: −1.6 / −3.0 / −5.1) |
| `chroma_error.phase_deg: 3.0` | Hanover 2.95° |
| `chroma_error.crosstalk: {v_to_u: 0.1, alternate_with_switch: true}` | alternating matrix M[U,V] = 0.100 |
| `chroma_error.crosstalk: {v_to_u: 0.1}` (not alternating) | M = 0 |
| `laserdisc.channel.field2_offset: {x_samples: 0.25}` | dx 0.244 samples |
| `laserdisc.channel.field2_offset: {y_lines: 0.5}` | dy 0.28 field lines (0.25 expected) |

The 20T pulse alone cannot tell a group delay from a smear: a one-pole smear delays the pulse's luma half and barely
touches its chroma, so it reads as a chroma lead. The smear is therefore fitted first, from tails more than 0.4 µs
after each edge (where neither the generator's edge shaping nor a group delay confined to the chroma band reaches),
and its share of the 20T delay is simulated and taken off.

The VITS chroma on Domesday pressings is not locked to the burst (its phase against the burst repeats only every six
frames), so the line-alternating error is measured on the picture's flat colour, not on the VITS.
