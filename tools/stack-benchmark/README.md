# Stacking benchmark tools

Research tools for the known-truth stacking benchmark. They are outside the build and are not run in CI.

The benchmark itself runs in the local videosynth suite, as the `StackerTruth.*_DomesdayModel` cases, on the
Domesday-model pressings in [`orc-tests/videosynth/projects/domesday/`](../../orc-tests/videosynth/projects/domesday/).
This directory keeps only the measurement those pressings were built from.

| File | What it is |
|---|---|
| `measure_noise.py` | Measures, per band, each capture's own noise and the noise every capture of one pressing shares, from PAL captures matched frame by frame, or NTSC ones with `--system ntsc` (whose band edges are PAL's scaled by the ratio of the subcarriers). It runs on real captures through an orc project and its Source Alignment report, or on a videosynth set; a `CVBS_U16_4FSC` capture is read as its 10-bit value, from the `.meta` beside it. Above 5.5 MHz, where the capture noise grows with the picture level and the pressing noise falls with it, it also measures black lines and lines at 35–75 IRE separately. With `--model` it prints the layers as videosynth `noise:` settings, each as a level-independent part and a level term. With `--reference` it compares a set against an earlier measurement. |
| `community-south-noise.json` | The measurement the Domesday-model pressings were built from: Community South's pressing A (DS1, DS2, DS4, DS8, DS12) and pressing C (DS5, DS11), with DS3, the lone capture of pressing B, as the third corner, over 40 frames from picture 500. |

The tool needs numpy and PyYAML:

```sh
nix shell --impure --expr \
  'with import <nixpkgs> {}; python3.withPackages (p: [p.numpy p.pyyaml])' \
  -c python3 -I tools/stack-benchmark/measure_noise.py --help
```

To check the PAL Domesday-model set against the real measurement, after the suite has generated it
(`ctest -R PAL_DomesdayModel` in a build with `BUILD_VIDEOSYNTH_TESTS=ON`):

```sh
O=build/videosynth-output
args=()
for c in A_cap1 A_cap2 A_cap3 A_cap4 B_cap1 B_cap2; do
  grid=0; [ "$c" = A_cap3 ] && grid=-1   # capture 3 reads one sample early, as DS4 does
  args+=(--capture "$O/pal_domesday_$c/videosynth_pal_domesday_$c.cvbs,${c%%_*},$grid")
done
python3 -I tools/stack-benchmark/measure_noise.py --probes 16 "${args[@]}" \
  --reference tools/stack-benchmark/community-south-noise.json
```

It exits non-zero when any comparison is more than 10 % from the reference. Below 5.5 MHz it compares the total
noise, the shared fraction and each pressing's capture noise. Above 5.5 MHz it compares both layers on black lines
and on lines at 35–75 IRE, since the test content's flat lines in that band are mostly black and Domesday's mostly
mid-grey.

The NTSC model's capture layer is checked the same way against `dragons-lair-noise.json`, Dragon's Lair's four
captures of one pressing. A set of one pressing has no pressing layer, so only the capture noise is compared; give
it pressing A's four captures alone:

```sh
O=build/videosynth-output
args=()
for c in cap1 cap2 cap3 cap4; do
  grid=0; [ "$c" = cap3 ] && grid=-1
  args+=(--capture "$O/ntsc_domesday_A_$c/videosynth_ntsc_domesday_A_$c.cvbs,A,$grid")
done
python3 -I tools/stack-benchmark/measure_noise.py --system ntsc --probes 16 "${args[@]}" \
  --reference tools/stack-benchmark/dragons-lair-noise.json
```
