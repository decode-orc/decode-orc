#!/usr/bin/env python3
#
# File:        make_long_fixture.py
# Module:      tools/perf-benchmark
# Purpose:     Write a sparse long-capture fixture for memory and index cost
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# A research tool, outside the build and not run in CI. It writes, into one
# directory, a capture of any length (six hours by default) in every form the
# source stages read:
#
#   tape_composite.tbc        + .tbc.db + .pcm      tbc_source, composite
#   tape_yc.tbc + _chroma.tbc + .tbc.db + .pcm      tbc_source, Y/C
#   tape_audio_44k16.wav                            audio_import, valid RIFF
#   long.cvbs + .meta + .dropouts.meta              cvbs_source
#   long_audio_0.wav                                CVBS channel pair 0
#
# Picture and audio files are sparse: they take no disk space and read as
# zeros, so the fixture measures what a stage holds and indexes per frame, not
# picture quality. The metadata is real: one field_record row per field,
# dropout rows at the stated density, and the CVBS frame count.
#
# A six-hour 48 kHz/24-bit stereo WAV is 6.2 GB, past the 4 GiB a RIFF size
# field can state. long_audio_0.wav therefore carries the sizes a 32-bit
# writer would produce (taken modulo 2^32), which is what a reader meets in
# practice today; see cvbs-file-format-specification#16.

import argparse
import os
import random
import sqlite3
import struct
import sys

SYSTEMS = {
    # field_width, field_height, sample_rate, active start/end, burst
    # start/end, white/black 16-bit IRE, CVBS samples per frame, frame rate.
    "PAL": dict(
        field_width=1135,
        field_height=313,
        video_sample_rate=17734475.0,
        active_video_start=185,
        active_video_end=1107,
        colour_burst_start=98,
        colour_burst_end=138,
        white_16b_ire=54016,
        black_16b_ire=16602,
        cvbs_frame_samples=709379,
        fps_num=25,
        fps_den=1,
        phases=8,
    ),
    "NTSC": dict(
        field_width=910,
        field_height=263,
        video_sample_rate=14318181.818181818,
        active_video_start=134,
        active_video_end=894,
        colour_burst_start=74,
        colour_burst_end=112,
        white_16b_ire=51200,
        black_16b_ire=18048,
        cvbs_frame_samples=477750,
        fps_num=30000,
        fps_den=1001,
        phases=4,
    ),
}

# ld-decode / vhs-decode .tbc.db schema (as written by vhs-decode 79e30baf).
TBC_SCHEMA = """
CREATE TABLE capture (capture_id INTEGER PRIMARY KEY, system TEXT NOT NULL
  CHECK (system IN ('NTSC','PAL','PAL_M')), decoder TEXT NOT NULL
  CHECK (decoder IN ('ld-decode','vhs-decode')), git_branch TEXT,
  git_commit TEXT, video_sample_rate REAL, active_video_start INTEGER,
  active_video_end INTEGER, field_width INTEGER, field_height INTEGER,
  number_of_sequential_fields INTEGER, colour_burst_start INTEGER,
  colour_burst_end INTEGER, is_mapped INTEGER CHECK (is_mapped IN (0,1)),
  is_subcarrier_locked INTEGER CHECK (is_subcarrier_locked IN (0,1)),
  is_widescreen INTEGER CHECK (is_widescreen IN (0,1)), white_16b_ire INTEGER,
  black_16b_ire INTEGER, capture_notes TEXT);
CREATE TABLE pcm_audio_parameters (capture_id INTEGER PRIMARY KEY
  REFERENCES capture(capture_id) ON DELETE CASCADE, bits INTEGER,
  is_signed INTEGER CHECK (is_signed IN (0,1)), is_little_endian INTEGER
  CHECK (is_little_endian IN (0,1)), sample_rate REAL);
CREATE TABLE field_record (capture_id INTEGER NOT NULL
  REFERENCES capture(capture_id) ON DELETE CASCADE, field_id INTEGER NOT NULL,
  audio_samples INTEGER, decode_faults INTEGER, disk_loc REAL,
  efm_t_values INTEGER, field_phase_id INTEGER, file_loc INTEGER,
  is_first_field INTEGER CHECK (is_first_field IN (0,1)),
  median_burst_ire REAL, pad INTEGER CHECK (pad IN (0,1)), sync_conf INTEGER,
  ntsc_is_fm_code_data_valid INTEGER CHECK (ntsc_is_fm_code_data_valid IN (0,1)),
  ntsc_fm_code_data INTEGER, ntsc_field_flag INTEGER
  CHECK (ntsc_field_flag IN (0,1)), ntsc_is_video_id_data_valid INTEGER
  CHECK (ntsc_is_video_id_data_valid IN (0,1)), ntsc_video_id_data INTEGER,
  ntsc_white_flag INTEGER CHECK (ntsc_white_flag IN (0,1)),
  PRIMARY KEY (capture_id, field_id));
CREATE TABLE vits_metrics (capture_id INTEGER NOT NULL,
  field_id INTEGER NOT NULL, b_psnr REAL, w_snr REAL,
  FOREIGN KEY (capture_id, field_id) REFERENCES field_record(capture_id, field_id)
  ON DELETE CASCADE, PRIMARY KEY (capture_id, field_id));
CREATE TABLE vbi (capture_id INTEGER NOT NULL, field_id INTEGER NOT NULL,
  vbi0 INTEGER NOT NULL, vbi1 INTEGER NOT NULL, vbi2 INTEGER NOT NULL,
  FOREIGN KEY (capture_id, field_id) REFERENCES field_record(capture_id, field_id)
  ON DELETE CASCADE, PRIMARY KEY (capture_id, field_id));
CREATE TABLE drop_outs (capture_id INTEGER NOT NULL, field_id INTEGER NOT NULL,
  field_line INTEGER NOT NULL, startx INTEGER NOT NULL, endx INTEGER NOT NULL,
  PRIMARY KEY (capture_id, field_id, field_line, startx, endx),
  FOREIGN KEY (capture_id, field_id) REFERENCES field_record(capture_id, field_id)
  ON DELETE CASCADE);
CREATE TABLE vitc (capture_id INTEGER NOT NULL, field_id INTEGER NOT NULL,
  vitc0 INTEGER NOT NULL, vitc1 INTEGER NOT NULL, vitc2 INTEGER NOT NULL,
  vitc3 INTEGER NOT NULL, vitc4 INTEGER NOT NULL, vitc5 INTEGER NOT NULL,
  vitc6 INTEGER NOT NULL, vitc7 INTEGER NOT NULL,
  FOREIGN KEY (capture_id, field_id) REFERENCES field_record(capture_id, field_id)
  ON DELETE CASCADE, PRIMARY KEY (capture_id, field_id));
CREATE TABLE closed_caption (capture_id INTEGER NOT NULL,
  field_id INTEGER NOT NULL, data0 INTEGER, data1 INTEGER,
  FOREIGN KEY (capture_id, field_id) REFERENCES field_record(capture_id, field_id)
  ON DELETE CASCADE, PRIMARY KEY (capture_id, field_id));
"""

# CVBS file format specification v1.6.0 (.meta user_version 11).
CVBS_META_SCHEMA = """
CREATE TABLE cvbs_file (cvbs_file_id INTEGER PRIMARY KEY,
  preset TEXT NOT NULL CHECK (preset IN ('NTSC', 'PAL', 'PAL_M')),
  sample_encoding_preset TEXT NOT NULL CHECK (sample_encoding_preset IN
    ('CVBS_U10_4FSC', 'CVBS_U16_4FSC', 'RAW_S16_28M', 'RAW_S16_40M',
     'CVBS_TPG21_4FSC', 'CVBS_S16_4FSC')),
  signal_state_preset TEXT NOT NULL CHECK (signal_state_preset IN (
    'STANDARD_STABLE_LOCKED', 'STANDARD_STABLE_UNLOCKED', 'STANDARD_RAW',
    'NONSTANDARD_STABLE_LOCKED', 'NONSTANDARD_STABLE_UNLOCKED',
    'NONSTANDARD_RAW')),
  sequence_continuous BOOLEAN,
  signal_type TEXT NOT NULL CHECK (signal_type IN ('composite', 'yc')),
  decoder TEXT NOT NULL, git_branch TEXT, git_commit TEXT,
  number_of_sequential_frames INTEGER CHECK (number_of_sequential_frames IS
    NULL OR number_of_sequential_frames >= 1),
  black_level INTEGER, has_nonstandard_values BOOLEAN, capture_notes TEXT);
CREATE TABLE audio_channel_pair (channel_pair INTEGER PRIMARY KEY
  CHECK (channel_pair BETWEEN 0 AND 7), description TEXT);
"""

CVBS_DROPOUT_SCHEMA = """
CREATE TABLE dropout_run (cvbs_file_id INTEGER NOT NULL,
  frame_id INTEGER NOT NULL CHECK (frame_id >= 0),
  sample_start INTEGER NOT NULL CHECK (sample_start >= 0),
  sample_count INTEGER NOT NULL CHECK (sample_count > 0),
  severity INTEGER NOT NULL CHECK (severity >= 0 AND severity <= 100),
  PRIMARY KEY (cvbs_file_id, frame_id, sample_start));
CREATE INDEX idx_dropout_run_frame ON dropout_run (cvbs_file_id, frame_id);
"""


def sparse_file(path, size):
    """Create (or resize) a sparse file of exactly size bytes."""
    with open(path, "wb") as f:
        f.truncate(size)


def fast_db(path):
    if os.path.exists(path):
        os.remove(path)
    db = sqlite3.connect(path)
    db.execute("PRAGMA journal_mode=OFF")
    db.execute("PRAGMA synchronous=OFF")
    return db


def field_audio_samples(field_index, rate, sysp):
    """PCM samples carried by one field: cumulative-floor split of rate."""
    field_rate_num = 2 * sysp["fps_num"]
    field_rate_den = sysp["fps_den"]

    def upto(i):
        return (i * int(rate) * field_rate_den) // field_rate_num

    return upto(field_index + 1) - upto(field_index)


def write_tbc_db(path, frames, system, sysp, dropouts_per_frame, rng):
    fields = 2 * frames
    db = fast_db(path)
    db.executescript(TBC_SCHEMA)
    db.execute("PRAGMA user_version = 1")
    db.execute(
        "INSERT INTO capture VALUES (1, ?, 'vhs-decode', 'fixture', 'fixture',"
        " ?, ?, ?, ?, ?, ?, ?, ?, 0, 0, 0, ?, ?, ?)",
        (
            system,
            sysp["video_sample_rate"],
            sysp["active_video_start"],
            sysp["active_video_end"],
            sysp["field_width"],
            sysp["field_height"],
            fields,
            sysp["colour_burst_start"],
            sysp["colour_burst_end"],
            sysp["white_16b_ire"],
            sysp["black_16b_ire"],
            "Sparse long-capture fixture (tools/perf-benchmark)",
        ),
    )
    db.execute("INSERT INTO pcm_audio_parameters VALUES (1, 16, 1, 1, 44100.0)")

    field_bytes = sysp["field_width"] * sysp["field_height"] * 2
    total_audio = 0

    def field_rows():
        nonlocal total_audio
        for f in range(fields):
            a = field_audio_samples(f, 44100, sysp)
            total_audio += a
            yield (
                1,
                f,
                a,
                None,
                None,
                None,
                (f % sysp["phases"]) + 1,
                f * field_bytes,
                1 if f % 2 == 0 else 0,
                21.0 if system == "PAL" else 20.0,
                0,
                100,
                None,
                None,
                None,
                None,
                None,
                None,
            )

    db.executemany(
        "INSERT INTO field_record VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        field_rows(),
    )

    # Dropout runs spread over the active picture lines of each field.
    per_field = dropouts_per_frame / 2.0
    width = sysp["field_width"]
    first_line, last_line = 22, sysp["field_height"] - 2

    def dropout_rows():
        carry = 0.0
        for f in range(fields):
            carry += per_field
            n = int(carry)
            carry -= n
            seen = set()
            for _ in range(n):
                line = rng.randint(first_line, last_line)
                start = rng.randint(sysp["active_video_start"],
                                    sysp["active_video_end"] - 40)
                key = (line, start)
                if key in seen:
                    continue
                seen.add(key)
                yield (1, f, line, start, start + rng.randint(4, 40))

    db.executemany("INSERT INTO drop_outs VALUES (?,?,?,?,?)", dropout_rows())
    db.commit()
    db.close()
    return total_audio


def write_wav_header(f, rate, bits, data_bytes):
    """Canonical 44-byte RIFF/WAVE header; sizes taken modulo 2^32."""
    block = 2 * bits // 8
    riff = (36 + data_bytes) & 0xFFFFFFFF
    f.write(b"RIFF" + struct.pack("<I", riff) + b"WAVE")
    f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, rate, rate * block,
                                   block, bits))
    f.write(b"data" + struct.pack("<I", data_bytes & 0xFFFFFFFF))


def sparse_wav(path, rate, bits, pairs):
    data_bytes = pairs * 2 * bits // 8
    with open(path, "wb") as f:
        write_wav_header(f, rate, bits, data_bytes)
        f.truncate(44 + data_bytes)
    return data_bytes


def write_cvbs(outdir, frames, system, sysp, dropouts_per_frame, rng):
    base = os.path.join(outdir, "long")
    sparse_file(base + ".cvbs", frames * sysp["cvbs_frame_samples"] * 2)

    meta = fast_db(base + ".meta")
    meta.executescript(CVBS_META_SCHEMA)
    meta.execute("PRAGMA user_version = 11")
    meta.execute(
        "INSERT INTO cvbs_file VALUES (1, ?, 'CVBS_U10_4FSC',"
        " 'STANDARD_STABLE_UNLOCKED', 1, 'composite', 'vhs-decode', 'fixture',"
        " 'fixture', ?, NULL, 0, ?)",
        (system, frames, "Sparse long-capture fixture (tools/perf-benchmark)"),
    )
    meta.execute("INSERT INTO audio_channel_pair VALUES (0, 'Linear audio')")
    meta.commit()
    meta.close()

    drop = fast_db(base + ".dropouts.meta")
    drop.executescript(CVBS_DROPOUT_SCHEMA)
    drop.execute("PRAGMA user_version = 5")
    samples = sysp["cvbs_frame_samples"]

    def rows():
        carry = 0.0
        for fr in range(frames):
            carry += dropouts_per_frame
            n = int(carry)
            carry -= n
            starts = sorted(set(rng.randrange(0, samples - 64) for _ in range(n)))
            for s in starts:
                yield (1, fr, s, rng.randint(4, 40), 100)

    drop.executemany("INSERT INTO dropout_run VALUES (?,?,?,?,?)", rows())
    drop.commit()
    drop.close()

    # CVBS spec: every pair file carries audio_pair_offset(frames) pairs.
    pairs = frames * 48000 * sysp["fps_den"] // sysp["fps_num"]
    data_bytes = sparse_wav(base + "_audio_0.wav", 48000, 24, pairs)
    return pairs, data_bytes


def main():
    ap = argparse.ArgumentParser(
        description="Write a sparse long-capture fixture (tbc, cvbs, audio).")
    ap.add_argument("outdir")
    ap.add_argument("--system", choices=sorted(SYSTEMS), default="PAL")
    ap.add_argument("--hours", type=float, default=6.0)
    ap.add_argument("--frames", type=int,
                    help="frame count (overrides --hours)")
    ap.add_argument("--tbc-dropouts-per-frame", type=float, default=20.0)
    ap.add_argument("--cvbs-dropouts-per-frame", type=float, default=20.0)
    ap.add_argument("--seed", type=int, default=375)
    ap.add_argument("--only", choices=["tbc", "cvbs"],
                    help="write only one family")
    args = ap.parse_args()

    sysp = SYSTEMS[args.system]
    frames = args.frames or int(round(args.hours * 3600 * sysp["fps_num"] /
                                      sysp["fps_den"]))
    os.makedirs(args.outdir, exist_ok=True)
    rng = random.Random(args.seed)
    summary = {"system": args.system, "frames": frames}

    if args.only in (None, "tbc"):
        frame_bytes = 2 * sysp["field_width"] * sysp["field_height"] * 2
        comp = os.path.join(args.outdir, "tape_composite")
        audio_samples = write_tbc_db(comp + ".tbc.db", frames, args.system,
                                     sysp, args.tbc_dropouts_per_frame, rng)
        sparse_file(comp + ".tbc", frames * frame_bytes)
        sparse_file(comp + ".pcm", audio_samples * 4)

        yc = os.path.join(args.outdir, "tape_yc")
        with open(comp + ".tbc.db", "rb") as src, open(yc + ".tbc.db", "wb") as dst:
            dst.write(src.read())
        sparse_file(yc + ".tbc", frames * frame_bytes)
        sparse_file(yc + "_chroma.tbc", frames * frame_bytes)
        sparse_file(yc + ".pcm", audio_samples * 4)

        wav_bytes = sparse_wav(os.path.join(args.outdir, "tape_audio_44k16.wav"),
                               44100, 16, audio_samples)
        summary.update(tbc_frame_bytes=frame_bytes, pcm_samples=audio_samples,
                       wav44_data_bytes=wav_bytes)

    if args.only in (None, "cvbs"):
        pairs, data_bytes = write_cvbs(args.outdir, frames, args.system, sysp,
                                       args.cvbs_dropouts_per_frame, rng)
        summary.update(cvbs_audio_pairs=pairs, cvbs_audio_data_bytes=data_bytes,
                       cvbs_audio_header_wrapped=data_bytes > 0xFFFFFFFF - 36)

    for k, v in summary.items():
        print(f"{k}: {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
