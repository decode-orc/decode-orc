# Embedded Disc Metadata Format

decode-orc can write a LaserDisc's VBI metadata — picture numbers, programme
time codes, chapter markers, stop codes, lead-in/lead-out markers and the
programme status word — **inside** the exported Matroska file, as an
attachment. A player emulator then needs exactly one file to play a side: no
sidecar, no second thing to keep alongside the video.

This page is the format reference. It documents what the document contains,
how to read it, and the rules a consumer must follow. If you only want to
switch the feature on, see the `embed_disc_metadata` parameter in the
[video sink documentation](../gui-user-guide/stages/sink-core-stages.md).

## Why it exists

Almost all seeking a real LaserDisc player performs is expressed in terms of
picture number or programme time code, and players expose that over a serial
interface. An emulator driving decoded video needs the same information to
behave like a player. The single most important thing the document provides is
therefore the **map between file frame position and disc address**.

## Finding the document

| Property | Value |
| --- | --- |
| Attachment filename | `orc-disc-metadata.yaml` |
| Attachment MIME type | `application/vnd.decode-orc.disc-metadata+yaml` |
| Container | Matroska (`.mkv`) only |

Both are a contract: locate the document by either, and never guess. **Neither
is ever versioned** — they are how the document is *found*; the version inside
says whether you can *read* it.

Matroska only, because MP4 and MOV reject attachment streams outright. FFV1
lives in Matroska anyway, so the restriction costs nothing in practice.

!!! note "The metadata is not in the video stream"
    FFV1 has no user-data field, so nothing can ride inside the codec
    bitstream. The document lives at container level, which is also why it can
    be read without decoding a single frame.

### Reading it with libavformat

The attachment arrives whole in `extradata`. No packet is demuxed and no seek
is performed:

```c
avformat_open_input(&fmt, path, NULL, NULL);
avformat_find_stream_info(fmt, NULL);

for (unsigned i = 0; i < fmt->nb_streams; i++) {
    AVStream *st = fmt->streams[i];
    if (st->codecpar->codec_type != AVMEDIA_TYPE_ATTACHMENT) continue;
    AVDictionaryEntry *fn = av_dict_get(st->metadata, "filename", NULL, 0);
    if (!fn || strcmp(fn->value, "orc-disc-metadata.yaml")) continue;

    parse_disc_metadata(st->codecpar->extradata,
                        st->codecpar->extradata_size);
    break;
}
```

Load it once at file open and the picture-number map is available for the rest
of the session.

### Reading it from the command line

```bash
ffmpeg -dump_attachment:t:0 orc-disc-metadata.yaml -i disc.mkv -f null -
```

Consumers not using FFmpeg can read the same bytes from the Matroska
`Attachments` element with any EBML reader.

## Versioning

Every document opens with the same header:

```yaml
# decode-orc disc metadata
%YAML 1.2
---
format: orc-disc-metadata
format_version: "1.0"
```

- `format` is a constant. It identifies the document *type* independently of
  the filename and MIME type, so the document survives being extracted,
  renamed, or carried in some future container.
- `format_version` is `MAJOR.MINOR`, and is **always a quoted string**.
  Unquoted, YAML reads it as a float and `1.10` silently becomes `1.1`.

### Compatibility rules

| Change | Bump | What a consumer must do |
| --- | --- | --- |
| New optional key or block; a new value in a field documented as extensible | MINOR | **Read the file**, ignoring what it does not recognise |
| Key removed or renamed; a field's type, units or meaning changed; a rule such as the `unnumbered` arithmetic altered | MAJOR | **Refuse the file** and report the version |

The reader's contract, which every consumer should implement identically:

```
if format        != "orc-disc-metadata"  -> not our document; ignore it
if MAJOR         != 1                    -> refuse, report the version
if MINOR          > known minor          -> read it, ignore unknown keys
```

!!! warning "Refuse an unknown MAJOR — do not guess"
    An unknown MINOR means keys this reader has not heard of, which is
    harmless. An unknown MAJOR means a rule the reader relies on may have
    changed underneath it, and continuing would produce a seek map that is
    confidently *wrong*. A player that lands on the wrong frame is worse than
    one that reports it cannot read the metadata.

Keys reserved but not yet emitted (`fm40`, `pulldown`,
`address_map.source: fm40`) do not change meaning within MAJOR 1. Their
absence means "not decoded", never "not applicable".

## Container tags

The disc-level summary is mirrored into Matroska tags, so a file is
self-describing to `ffprobe` and MediaInfo without extracting anything:

```
ORC_DISC_METADATA          orc-disc-metadata.yaml
ORC_DISC_METADATA_VERSION  1.0
ORC_DISC_FORMAT            CAV
ORC_DISC_SIDE              1
ORC_DISC_SIZE_INCHES       12
ORC_VIDEO_SYSTEM           PAL
ORC_FIRST_PICTURE          1
ORC_LAST_PICTURE           54000
ORC_SOUND_MODE             stereo
ORC_CX                     on
```

Tags whose value was never recovered are **omitted**, not written as
`unknown`. `ORC_DISC_METADATA_VERSION` always matches the document's
`format_version`, so a tool can decide whether it can read the attachment
before extracting it — but the document remains the authority.

## Document structure

```yaml
# decode-orc disc metadata
%YAML 1.2
---
format: orc-disc-metadata
format_version: "1.0"

generator:
  application: decode-orc
  version: "1.4.0"

video:
  system: PAL              # PAL | NTSC | PAL_M
  frame_rate: 25           # nominal: the rate CLV timecode counts at
  frame_rate_exact: [25, 1]   # [30000, 1001] for NTSC
  field_order: tff         # tff | bff
  frame_count: 54000       # frames present in this file
  fields_per_frame: 2

source:
  first_frame: 0           # source frame id of file frame 0 (provenance only)
  last_frame: 53999

disc:
  format: CAV              # CAV | CLV
  side: 1
  size_inches: 12
  cx_enabled: true
  teletext: false
  digital_video: false
  sound_mode: stereo
  fm_multiplex: false
  programme_dump: false
  user_code: "8DC00"
  amendment2:
    present: true
    copy_permitted: false
    video_standard: true
    sound_mode: stereo
  confidence:
    fields_total: 108000
    fields_with_status: 106880
    fields_parity_valid: 106834
    agreement: 0.9998

address_map:
  kind: cav_picture
  source: biphase
  runs:
    - {file_frame: 0, count: 54000, picture: 1}
  unnumbered:
    count: 10800
    encoding: bitmap
    bitmap: "8420..."
  undecoded:
    count: 12
    encoding: ranges
    ranges: [[20114, 3], [41022, 9]]
  unmapped:
    count: 24
    encoding: ranges
    ranges: [[0, 24]]

events:
  lead_in: [[0, 24]]
  lead_out: [[53976, 24]]
  stop_codes: [12480, 33001]
  chapters:
    - {file_frame: 0, chapter: 1}
    - {file_frame: 9000, chapter: 2}
```

**File frames are always 0-based and contiguous**, counting the frames actually
present in this file. `source.first_frame` records where they came from and is
provenance only — never use it to offset a lookup.

### `disc`

Every value here is **voted** across each field that carried a status word, not
taken from the first field that had one. A single mis-decoded field must not be
able to flip the disc side. `confidence.agreement` is the fraction of status
fields whose whole word matched the vote — a strict measure, where one
dissenting bit counts the field as disagreeing.

Values never recovered are omitted rather than guessed.

### `events`

- `lead_in` / `lead_out` — `[file_frame, count]` ranges.
- `stop_codes` — file frames carrying a picture stop code.
- `chapters` — only chapter *transitions*, not every frame.

## The address map

This is the part an emulator seeks with.

### Runs

A run asserts an arithmetic relation over a span of consecutive file frames:

```
address(f) = address₀ + (f − file_frame) − |unnumbered ∩ [file_frame, f)|
```

That is: **the address advances once per file frame, except across frames
listed in `unnumbered`, which advance it by nothing.**

`kind` selects the address space and the run's key:

| `kind` | Run key | Meaning |
| --- | --- | --- |
| `cav_picture` | `picture: 1` | CAV picture number of the run's first frame |
| `clv_timecode` | `time: "00:00:00.00"` | CLV programme time code of the run's first frame |
| `none` | — | No address information was recovered at all |

For `clv_timecode`, each subsequent frame advances one picture, rolling over at
the video system's nominal frame rate — 25 for PAL, 30 for NTSC and PAL-M.

!!! warning "CLV timecode is a disc address, not elapsed time"
    NTSC's 30 is nominal: the disc counts 30 pictures per timecode second while
    running at 30000/1001 fps. Never convert a CLV timecode to wall-clock time.

`source` says which channel the map came from. It is currently always
`biphase`; `fm40` is reserved for the NTSC 40-bit channel (see
[Not yet decoded](#not-yet-decoded)).

A run breaks at an address discontinuity and at a change of address kind. Runs
are **not** broken by frames that merely lack a number.

### Frames with no picture number

On NTSC, film-sourced CAV material is mastered so that pulldown frames — video
frames built from fields of two different photographic pictures — carry **no**
picture number. That stops a player pausing or stepping onto a frame that is
not a real picture. On a 2:3 cadence roughly one frame in five is affected, so
the file-frame index and the disc address are *not* in one-to-one
correspondence.

**The picture number does not advance across such a frame.** A frame numbered
*P*, then a pulldown frame, then *P + 1*. The number sequence stays dense — no
value is skipped — so a picture-number range remains contiguous.

There is a second reason a frame can lack a number, and it behaves in exactly
the opposite way:

| Set | On the disc | Effect on the count |
| --- | --- | --- |
| `unnumbered` | no number was ever recorded (pulldown) | does **not** advance |
| `undecoded` | a number exists; decode-orc did not recover it | **does** advance |

Keeping them separate is what lets one arithmetic rule cover both a
fully-numbered video disc and a film-sourced one. It also means the ratio of
`undecoded` to `unnumbered` is a direct measure of how well the VBI read.

!!! info "How the two are told apart"
    decode-orc does not guess. For a gap of *k* frames between numbers *P* and
    *Q*: `Q − P == k + 1` means every hole frame advanced the count
    (`undecoded`); `Q − P == 1` means none did (`unnumbered`). Anything between
    is a mix, resolved only when the per-frame evidence accounts for it exactly
    — a frame whose VBI did not decode at all is a decode failure, and a clean
    frame carrying a chapter code is positively a pulldown frame, since both
    IEC standards place chapter numbers in the fields "which do not have an
    insertion of picture numbers" (§10.1.5). Otherwise the run simply breaks:
    an ambiguous gap is never silently resolved.

**What a consumer must do.** An `unnumbered` frame *has no address*; an
`undecoded` one has an address the file cannot state. Neither may be given a
neighbouring frame's number in a map lookup — a real player holds and
redisplays the last number it saw, which is display behaviour, not an address.
Seeking is unaffected either way: every address the file states resolves to
exactly one file frame.

### Set encoding: `ranges` or `bitmap`

`unnumbered`, `undecoded` and `unmapped` all use the same shape, in whichever
of two encodings is smaller. **A reader must accept both.**

```yaml
unnumbered:
  count: 10800
  encoding: ranges          # a list of [file_frame, count] pairs
  ranges: [[3, 1], [8, 1], [13, 1]]
```

```yaml
unnumbered:
  count: 10800
  encoding: bitmap          # one bit per file frame, LSB-first, 1 = member
  bitmap: "8420108421..."
```

The bitmap is 6.75 KB for a 54,000-frame side regardless of cadence, and
assumes nothing about the pulldown pattern being regular — it is not, across
edits. Ranges win when few frames are affected, which is the usual shape of
`undecoded`.

Decoding a bitmap:

```python
frames = {i * 8 + bit
          for i, byte in enumerate(bytes.fromhex(bitmap))
          for bit in range(8)
          if byte >> bit & 1}
```

`count` is always present, so a consumer that only cares how much of the side
is affected never has to decode either form.

### Worked example

A short PAL CAV export: two lead-in frames, then pictures 1–16 across 18
frames, with two frames the disc never numbered.

```yaml
address_map:
  kind: cav_picture
  source: biphase
  runs:
    - {file_frame: 2, count: 18, picture: 1}
  unnumbered:
    count: 2
    encoding: bitmap
    bitmap: "802000"      # bits 7 and 13 -> file frames 7 and 13
  unmapped:
    count: 2
    encoding: bitmap
    bitmap: "030000"      # bits 0 and 1 -> the lead-in frames
```

Applying the rule:

| File frame | Calculation | Address |
| --- | --- | --- |
| 0, 1 | in `unmapped` | none — lead-in |
| 2 | `1 + (2−2) − 0` | picture 1 |
| 6 | `1 + (6−2) − 0` | picture 5 |
| 7 | in `unnumbered` | none — not a picture |
| 8 | `1 + (8−2) − 1` | picture 6 |
| 19 | `1 + (19−2) − 2` | picture 16 |

Note frame 8: the pulldown frame at 7 held the count, so picture 6 follows
picture 5 with no value skipped.

In Python:

```python
def address(map_, f):
    for run in map_["runs"]:
        if not run["file_frame"] <= f < run["file_frame"] + run["count"]:
            continue
        if f in unnumbered or f in undecoded:
            return None          # no address a consumer may rely on
        held = len([u for u in unnumbered if run["file_frame"] <= u < f])
        return run["picture"] + (f - run["file_frame"]) - held
    return None                  # unmapped
```

### `unmapped`

Frames covered by no run at all — typically lead-in frames before the first
numbered frame, which have nothing preceding them to anchor a run. They are
**never** interpolated: a gap is reported as a gap, because an emulator that
seeks to a fabricated picture number lands somewhere wrong and silently.

## Raw VBI

At `full` detail an extra block carries the biphase words verbatim, so anything
the schema does not model stays recoverable from the file itself:

```yaml
raw_vbi:
  encoding: hex24-per-field
  lines: [16, 17, 18]
  # One line per file frame, six words:
  #   f1_l16 f1_l17 f1_l18 f2_l16 f2_l17 f2_l18
  # "------" means the line was not decoded.
  data: |
    8dc000 88ffff 88ffff 8dc000 88ffff 88ffff
    8dc000 f00001 f00001 8dc000 f00001 f00001
```

That is 42 bytes per frame — roughly 2.3 MB for a full 54,000-frame side,
against tens of gigabytes of FFV1. It is off by default because most consumers
only need the map.

## PAL and NTSC

LaserVision is specified by **two** standards, one per video system:

- **IEC 60856-1986** — LaserVision PAL, 50 Hz/625 lines
- **IEC 60857-1986** — LaserVision NTSC, 60 Hz/525 lines

They are not interchangeable:

| | PAL (IEC 60856) | NTSC (IEC 60857) |
| --- | --- | --- |
| Biphase picture/chapter/time code | lines 17, 18 / 330, 331 | lines 17, 18 / 280, 281 |
| Biphase CLV picture number | line 16 / 329 | line 16 / 279 |
| Biphase picture stop code | lines 16, 17 / 329, 330 | lines 16, 17 / 279, 280 |
| Max CAV picture number (biphase) | 99 999 | 79 999 |
| CLV picture-within-second | X4 = 0–2, X5 = 0–9 (0–24 used at 25 fps) | X4 = 0–2, X5 = 0–9 (0–29 used at 30 fps) |
| 40-bit FM coded channel | absent | lines 10, 273 (§10.2) |
| First-field white flag | absent | line 11 / 274 (§10.2.4) |

Both standards put the biphase codes on the same lines *of each field*, so no
system-specific handling is needed for what decode-orc decodes today.

!!! warning "Known limitation: PAL picture numbers above 79 999"
    The biphase decoder masks the picture number to the NTSC limit of 79 999.
    A PAL disc numbered at or above 80 000 will decode incorrectly. Where that
    produces an address discontinuity, the map reports a run break rather than
    silently repairing it.

## Not yet decoded

IEC 60857 §10.2 defines a **40-bit FM coded channel** on lines 10 and 273 with
no PAL counterpart. It is a better address source than the biphase codes: on
CAV it is always present and runs to 99 999 rather than 79 999 (§10.2.3), and
on CLV its programme time carries a character naming the disc region outright —
`A` lead-in, `B` end-of-lead-in, `D` picture, `C` lead-out (§10.2.5) — where
the biphase codes leave the programme-area boundary to be inferred.

§10.2.4 defines the **first-field white flag** on line 11/274, positioned so it
always marks the first field of the next *photographic* picture. That is the
authoritative pulldown marker, better than inferring cadence from missing
picture numbers.

decode-orc decodes neither. The schema reserves `fm40` and `pulldown` blocks
plus `address_map.source: fm40` so that adding them later is additive, and
their absence today means "not decoded".

## Constraints worth knowing

**The document is built before the first frame is encoded.** Matroska writes
attachments and tags into the file header, so the whole map must exist up
front. Enabling the option therefore adds a pre-scan pass over the export
range — the same cost chapter embedding already pays, and the two share one
pass when both are on.

**The metadata describes the frames actually written.** VBI is decoded from the
sink's own input, after any upstream frame removal or padding, so the file
frame indices in the document always match the frames in the file.
