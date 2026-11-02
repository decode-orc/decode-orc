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

The tools need numpy and scipy:

```sh
nix shell --impure --expr \
  'with import <nixpkgs> {}; python3.withPackages (p: [p.numpy p.scipy])' \
  -c python3 -I tools/domesday2d-bench/phase2_vits.py --frames 48 CAPTURE.cvbs
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
