# Stacking multiple discs

Several captures of the same LaserDisc title can be stacked into one output
with fewer dropouts and less noise than any single capture. Each disc first
needs its frame list worked out by the Disc Mapper, which removes player skips
and repeats, and then the discs need lining up with each other by Source
Alignment. This page shows how to do that from a script, so a batch of new
captures goes from decode to stacked output in one command.

The approach is:

1. Build a **template project** once, in the GUI.
2. For each batch, copy the template, point its sources at the new captures
   with `orc-cli nodes set`, and run `orc-cli tools refresh --process`.

Everything here works for both capture formats: `.tbc` decodes from ld-decode
(read by **TBC Source**) and `.cvbs` captures (read by the **CVBS Source**
stages). A stack takes all its discs in one format, never a mix.

## Building the template

Build the project in the GUI with the captures of one batch, so you can check
every stage works before relying on it. For three discs the graph is:

```
Disc 1 ─ Disc 1 map ─┐
Disc 2 ─ Disc 2 map ─┼─ Align ─ Stacker ─ Video Sink
Disc 3 ─ Disc 3 map ─┘
```

- **Sources**: one TBC Source or CVBS Source per disc.
- **Frame Map** after each source.
- **Source Align** taking every Frame Map.
- **Stacker** after Source Align, then the sinks you want (Video Sink, Audio
  Sink, EFM Sink and so on).

Then:

1. **Name the stages** you will script against, with **Rename Stage...**: for
   example "Disc 1", "Disc 1 map", ..., "Align" and "Video Out". Names are
   unique within a project, so a script can address each stage by name. Stage
   ids work too, but names survive edits to the graph.
2. **Choose the refresh tools.** On each Frame Map, open **Stage Tools >
   Disc Mapper**, set its parameters, tick **Use this tool when refreshing
   from the CLI**, then run it and press **Apply to Stage** (or press **OK**
   without running it). Do the same on Source Align with **Source Alignment
   Analysis**.
3. **Save** the template.

Keep one template per capture format and per composite / Y/C combination: a
template's sources take `input_path` for a composite capture or `y_path` and
`c_path` for a Y/C one, and a project is all one or the other.

You can also choose the refresh tools from the command line:

```bash
for n in 1 2 3; do
  orc-cli tools refresh-config template.orcprj "Disc $n map" field_mapping
done
orc-cli tools refresh-config template.orcprj Align source_alignment
```

Check what the template records with `orc-cli nodes list template.orcprj`:
each Frame Map and the Source Align show a "Refreshed by" line.

If the template has a Video Parameters stage whose values should follow each
batch's captures, run `orc-cli nodes reset <project> "<its name>"` after
pointing the sources at the new captures. It takes the values the stage's
input reports, as the GUI's **Reset to Metadata Values** does. See
[Resetting a stage's parameters](overview.md#resetting-a-stages-parameters).

## `.tbc` captures

Each ld-decode capture is a `.tbc` file (or a `.tbcy` and `.tbcc` pair for
Y/C), with its metadata beside it as `.tbc.db` (current ld-decode) or
`.tbc.json` (older versions). **The metadata must be there**: without it the
source cannot run. The audio and data sidecars are optional:

| Key | File |
|---|---|
| `input_path` | Composite `.tbc` |
| `y_path`, `c_path` | Y/C `.tbcy` and `.tbcc` |
| `pcm_path` | Analogue audio `.pcm` |
| `efm_path` | EFM `.efm` |
| `ac3rf_path` | AC3 RF symbols `.ac3sym` |

A player that skipped or repeated during the decode leaves gaps and repeats in
the capture. The Disc Mapper finds them from the picture numbers (CAV) or time
codes (CLV) and pads each gap with blank frames, so every disc lines up frame
for frame before stacking. The padding shows as `PAD_n` entries in the Frame
Map's `ranges` and as `paddingFrames` in the refresh report.

```bash
#!/usr/bin/env bash
# Stack three composite PAL .tbc decodes of one title.
set -uo pipefail

template=templates/pal-tbc-3disc.orcprj
work=work/title.orcprj
mkdir -p work out
cp "$template" "$work"

for n in 1 2 3; do
  tbc="caps/disc$n.tbc"
  if [[ ! -f "$tbc.db" && ! -f "$tbc.json" ]]; then
    echo "No metadata for $tbc" >&2
    exit 1
  fi
  orc-cli nodes set "$work" "Disc $n" input_path="$tbc" \
      pcm_path="caps/disc$n.pcm" efm_path="caps/disc$n.efm" || exit 1
done
orc-cli nodes set "$work" "Video Out" output_path=out/title.mkv || exit 1

orc-cli tools refresh "$work" --json --process > work/title.report.json
status=$?
case $status in
  0)   echo "Stacked: out/title.mkv" ;;
  5)   echo "The template or a capture was refused; see the message above" >&2 ;;
  6)   echo "A disc could not be mapped or aligned; see work/title.report.json" >&2 ;;
  130) echo "Cancelled" >&2 ;;
  *)   echo "Failed (exit $status)" >&2 ;;
esac
exit $status
```

## `.cvbs` captures

A CVBS capture is a `.cvbs` file (or a `.cvbsy` and `.cvbsc` pair for Y/C)
with a `.meta` sidecar describing it. Audio, EFM and AC3 sidecars are found
beside the capture, so the only keys are the capture paths:

| Key | File |
|---|---|
| `input_path` | Composite `.cvbs` |
| `y_path`, `c_path` | Y/C `.cvbsy` and `.cvbsc` |

Leave the source's sample encoding at **From metadata**, so the `.meta`
sidecar is read. The source accepts the `STANDARD_STABLE_LOCKED` and
`STANDARD_STABLE_UNLOCKED` signal states and refuses any other.

The sidecar also says whether the capture is one unbroken sequence
(`sequence_continuous`). `false` means the capture has at least one
discontinuity, usually a player skip, which is what the Disc Mapper repairs.
`nodes list --json` shows both values for each CVBS source, so a script can
check the captures before running anything:

```bash
orc-cli nodes list work/title.orcprj --json \
  | jq '.[] | select(.stage | endswith("_CVBS_Source"))
        | {label, signal_state, sequence_continuous}'
```

The refresh report carries the same values under each node's `sources`. A
refresh runs the Disc Mapper on every disc whatever `sequence_continuous`
says; the template decides which tools run.

```bash
#!/usr/bin/env bash
# Stack three Y/C NTSC .cvbs captures of one title.
set -uo pipefail

work=work/title.orcprj
mkdir -p work out
cp templates/ntsc-cvbs-yc-3disc.orcprj "$work"

for n in 1 2 3; do
  orc-cli nodes set "$work" "Disc $n" \
      y_path="caps/disc$n.cvbsy" c_path="caps/disc$n.cvbsc" || exit 1
done
orc-cli nodes set "$work" "Video Out" output_path=out/title.mkv || exit 1

orc-cli tools refresh "$work" --process
```

## Checking a batch before processing it

Processing a stack takes much longer than mapping it. To look at the mapping
and alignment first, refresh without `--process`, or with `--dry-run` to save
nothing:

```bash
orc-cli tools refresh work/title.orcprj --dry-run --json \
  | jq '.nodes[] | {node, status, parameter_changes}'
```

A refresh stops at the first disc that cannot be mapped, for example one with
no readable picture numbers, and leaves the project unchanged (exit code `6`).
The report names the disc. Drop it from the template, or replace the capture,
and refresh again.

The analysis sinks can help choose which captures to stack. For example, run
the SNR analysis on each disc in a separate single-disc project and leave out
a capture whose mean SNR is well below the others':

```bash
orc-cli tools run disc1.orcprj snr_analysis --node "SNR Analysis Sink" --json \
  | jq '.nodes[0].statistics'
```

## Rules to remember

- **One format per stack.** All `.tbc` or all `.cvbs`; a mixed stack is
  refused (exit code `5`) and the message names the sources of each kind.
- **Files, not streams.** The stage tools read their input more than once, so
  they cannot run on a project fed from a stream source. Decode to files first.
- **Configure the sources before refreshing.** A refresh refuses a source with
  no capture set (exit code `5`).
- **All or nothing.** A refresh saves only when every tool succeeded.

See [Refreshing a project](overview.md#refreshing-a-project) for every option
and [Exit Codes](overview.md#exit-codes) for the full list of codes.
