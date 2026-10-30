#!/usr/bin/env python3
#
# File:        measure_noise.py
# Module:      tools/stack-benchmark
# Purpose:     Measure the capture and pressing noise of a multi-capture set
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 decode-orc contributors
#
# A research tool, outside the build and not run in CI. It measures, per
# frequency band, the noise each capture adds on its own and the noise every
# capture of one pressing shares, from PAL or NTSC captures matched frame by
# frame (--system).
# The same measurement runs on real captures and on a videosynth set, so the
# two can be compared.
#
# Model: capture c of pressing p is x_c = s + P_p + I_c, with s the picture,
# P_p the pressing's noise and I_c the capture's own. On active picture lines:
#   - a pair of one pressing differs by I_i - I_j, so its difference power is
#     2 I_p (I_p taken as the same for every capture of the pressing);
#   - a pair of two pressings differs by I_i - I_j + P_p - P_q, so its power
#     less I_p and I_q is P_p + P_q.
# Two pressings therefore give the mean pressing noise (P_p + P_q) / 2. A
# third, even one lone capture, gives each pressing's own by the three-cornered
# hat: P_p = (D_pq + D_pr - D_qr) / 2 - I_p, with D the cross-pair powers.
#
# A difference keeps a residue of the picture wherever the captures' gains or
# sample grids differ, so each band is measured only on lines whose two
# neighbours in the field carry little of that band in the mean of the
# captures. The neighbours, not the line itself, decide, so the selection does
# not depend on the line's own noise. A pair's line is skipped as a dropout
# when its difference power is over three times the pair's median for the
# frame.
#
# Above 5.5 MHz the capture noise grows with the picture level and the
# pressing noise falls with it, so that band is also measured on two classes
# of line: black, and 35-75 IRE, where nearly every flat line of a real disc
# lies. A line's level is the rms, above black,
# of the mean of the captures low-passed below 3 MHz. The other bands do not
# grow with level and are measured on every flat line.
#
# Inputs:
#   --project/--compare: an orc project whose source_align node stacks the
#     captures (each optionally through a frame_map), and the JSON of
#     `orc-cli tools run <project> source_alignment --node N --json`, which
#     gives each capture's sample grid and pressing (stamper group).
#   --capture PATH,PRESSING[,GRID]: captures whose frames already line up, such
#     as a videosynth set. GRID is the capture's sample grid against the first
#     (source_alignment's convention: capture[n + GRID] matches first[n]).
#
# Run with numpy and PyYAML, for example:
#   nix shell --impure --expr \
#     'with import <nixpkgs> {}; python3.withPackages (p: [p.numpy p.pyyaml])' \
#     -c python3 -I tools/stack-benchmark/measure_noise.py ...

import argparse
import json
import os
import sys

import numpy as np

# PAL composite at 4fsc, in 10-bit codes: 1135 samples a line, a frame of two
# fields (313 + 312 lines) with field 2 starting two samples late. configure()
# switches these to NTSC: 910 samples a line, 263 + 262 lines, no offset.
SPL = 1135
FRAME = 709379
FIELD1_LINES = 313
FRAME_LINES = 625
FIELD2_OFFSET = 2
FS = 4 * (283.75 * 15625 + 25)
CODES_PER_IRE = (844 - 256) / 100.0
BLACK_IRE = 256 / CODES_PER_IRE

# Active picture used: field lines 23-308 of each field, samples 200-1079.
ACTIVE_FIELD_LINES = range(23, 309)
SEG = slice(200, 1080)
SEG_LEN = SEG.stop - SEG.start

# Power is gathered in groups of 0.25 MHz. The first group is left out of the
# bands: below 0.25 MHz the captures of different pressings differ by black
# level and line tilt, which a stacker's level match removes, and which is not
# noise.
GROUP_MHZ = 0.25
BANDS = [(GROUP_MHZ, 2.0), (2.0, 3.5), (3.5, 5.5), (5.5, FS / 2e6)]
BAND_NAMES = ['0.25-2', '2-3.5', '3.5-5.5', '5.5+']

# NTSC's band edges are PAL's scaled by the ratio of the subcarriers, as the
# NTSC Domesday model's spectra were, so the measured layers compare with it band for band.
NTSC_FSC = 315e6 / 88
PAL_FSC = 283.75 * 15625 + 25

# Neighbour content above this many times the band's floor excludes a line.
CONTENT_FACTOR = 2.5
# A pair's line whose difference is above this many times the pair's median
# for the frame is a dropout.
DROPOUT_FACTOR = 3.0

# Line level classes for the band above 5.5 MHz, in IRE above black.
LEVEL_LOWPASS_MHZ = 3.0
LEVEL_CLASSES = {'black': (-np.inf, 15.0), '35-75': (35.0, 75.0)}
HIGH_BAND = len(BANDS) - 1


def configure(system):
    """Switch the geometry and levels to |system| ('pal' or 'ntsc')."""
    global SPL, FRAME, FIELD1_LINES, FRAME_LINES, FIELD2_OFFSET, FS
    global CODES_PER_IRE, BLACK_IRE, ACTIVE_FIELD_LINES, SEG, SEG_LEN, BANDS
    global BAND_NAMES
    if system == 'pal':
        return
    # NTSC at 4fsc in 10-bit codes: blanking 240, white 800, and a LaserDisc's
    # black at the 7.5 IRE setup. Active picture: field lines 22-259, samples
    # 160-879.
    SPL, FIELD1_LINES, FRAME_LINES, FIELD2_OFFSET = 910, 263, 525, 0
    FRAME = SPL * FRAME_LINES
    FS = 4 * NTSC_FSC
    CODES_PER_IRE = (800 - 240) / 100.0
    BLACK_IRE = 282 / CODES_PER_IRE
    ACTIVE_FIELD_LINES = range(22, 260)
    SEG = slice(160, 880)
    SEG_LEN = SEG.stop - SEG.start
    k = NTSC_FSC / PAL_FSC
    edges = [GROUP_MHZ, 2.0 * k, 3.5 * k, 5.5 * k, FS / 2e6]
    BANDS = list(zip(edges[:-1], edges[1:]))
    BAND_NAMES = ['%.2f-%.1f' % BANDS[0], '%.1f-%.1f' % BANDS[1],
                  '%.1f-%.1f' % BANDS[2], '%.1f+' % BANDS[3][0]]


def sample_scale(path):
    """Codes per 10-bit code in |path|: CVBS_U16_4FSC holds the 10-bit value
    times 64, read from the capture's .meta beside it."""
    import sqlite3
    meta = path.rsplit('.', 1)[0] + '.meta'
    try:
        with sqlite3.connect('file:%s?mode=ro' % meta, uri=True) as db:
            row = db.execute('select sample_encoding_preset from cvbs_file').fetchone()
    except sqlite3.Error:
        return 1.0
    return 64.0 if row and row[0] == 'CVBS_U16_4FSC' else 1.0


def line_offset(line):
    return line * SPL + (FIELD2_OFFSET if line >= FIELD1_LINES else 0)


def expand_ranges(ranges):
    out = []
    for tok in ranges.replace(' ', '').split(','):
        if not tok:
            continue
        if tok.startswith('PAD_'):
            out += [None] * int(tok[4:])
        elif '-' in tok:
            a, b = map(int, tok.split('-'))
            out += list(range(a, b + 1))
        else:
            out.append(int(tok))
    return out


def captures_from_project(project, compare_json):
    import yaml
    p = yaml.safe_load(open(project))
    nodes = {n['id']: n for n in p['dag']['nodes']}
    edges = p['dag']['edges']
    align = next(n for n in nodes.values() if n['stage'] == 'source_align')
    inputs = [e['from'] for e in edges if e['to'] == align['id']]
    pads = {}
    for tok in align['parameters']['alignmentMap']['value'].split(','):
        k, pad = tok.strip().split('+')
        pads[int(k)] = int(pad)
    text = open(compare_json).read()
    report = json.loads(text[text.index('{'):])['nodes'][0]
    per_source = {}
    for item in report['items']:
        data = item.get('data') or {}
        if 'source' in data:
            per_source[data['source']] = data
    caps = []
    for k, fm in enumerate(inputs, 1):
        src = next((e['from'] for e in edges if e['to'] == fm), fm)
        sn = nodes[src]
        mapped = None
        if nodes[fm]['stage'] == 'frame_map':
            mapped = expand_ranges(nodes[fm]['parameters']['ranges']['value'])
        data = per_source.get(k, {})
        path = sn['parameters']['input_path']['value']
        if not os.path.isabs(path):
            path = os.path.join(os.path.dirname(os.path.abspath(project)), path)
        caps.append(dict(label=sn.get('user_label', str(src)),
                         path=path,
                         mapped=mapped, pad=pads.get(k, 0),
                         grid=int(data.get('sample_grid') or 0),
                         pressing=data.get('stamper_group', '?')))
    return caps


def captures_from_args(specs):
    caps = []
    for spec in specs:
        parts = spec.split(',')
        caps.append(dict(label=os.path.basename(parts[0]).rsplit('.', 1)[0],
                         path=parts[0], mapped=None, pad=0,
                         grid=int(parts[2]) if len(parts) > 2 else 0,
                         pressing=parts[1]))
    return caps


def frame_count(c):
    stored = os.path.getsize(c['path']) // (FRAME * 2)
    return c['pad'] + (len(c['mapped']) if c['mapped'] is not None else stored)


def source_frame(c, f):
    i = f - c['pad']
    if i < 0:
        return None
    if c['mapped'] is None:
        return i
    return c['mapped'][i] if i < len(c['mapped']) else None


def read_frame(c, sf):
    with open(c['path'], 'rb') as fh:
        fh.seek(sf * FRAME * 2)
        a = np.frombuffer(fh.read(FRAME * 2), dtype='<u2').astype(np.float64)
    if len(a) < FRAME:
        return None
    lines = np.stack([a[line_offset(l):line_offset(l) + SPL]
                      for l in range(FRAME_LINES)])
    if c['grid']:
        lines = np.roll(lines, -c['grid'], axis=1)
    if 'scale' not in c:
        c['scale'] = sample_scale(c['path'])
    return lines / (CODES_PER_IRE * c['scale'])


def active_lines():
    """Frame lines measured, as (line, upper neighbour, lower neighbour)."""
    out = []
    for base in (0, FIELD1_LINES):
        for fl in ACTIVE_FIELD_LINES:
            out.append((base + fl, base + fl - 1, base + fl + 1))
    return out


def group_edges():
    freqs = np.fft.rfftfreq(SEG_LEN, 1 / FS) / 1e6
    n_groups = int(np.ceil(freqs[-1] / GROUP_MHZ))
    gid = np.minimum((freqs / GROUP_MHZ).astype(int), n_groups - 1)
    return freqs, gid, n_groups


def band_of_groups(n_groups):
    """The band of each group, or -1 for a group below the first band."""
    centres = (np.arange(n_groups) + 0.5) * GROUP_MHZ
    return np.array([-1 if c < BANDS[0][0] else
                     next(i for i, (lo, hi) in enumerate(BANDS) if c < hi or i == len(BANDS) - 1)
                     for c in centres])


def measure(caps, probes, first_frame, last_frame):
    """Per probed frame: each pair's difference power and the mean's power, per
    line and group, and each line's level."""
    window = np.hanning(SEG_LEN)
    scale = 2.0 / (SEG_LEN * np.sum(window ** 2))
    _, gid, n_groups = group_edges()
    lowpass = np.fft.rfftfreq(SEG_LEN, 1 / FS) < LEVEL_LOWPASS_MHZ * 1e6
    lines = active_lines()
    rows = [l for l, _, _ in lines]
    n = len(caps)
    pairs = [(i, j) for i in range(n) for j in range(i + 1, n)]

    total = min(frame_count(c) for c in caps)
    last = total - 1 if last_frame is None else min(last_frame, total - 1)
    candidates = np.unique(np.linspace(first_frame, last, probes * 2).astype(int))

    diff_pow, mean_pow, levels, used = [], [], [], []
    for f in candidates:
        if len(used) >= probes:
            break
        sfs = [source_frame(c, f) for c in caps]
        if any(s is None for s in sfs):
            continue
        frames = [read_frame(c, s) for c, s in zip(caps, sfs)]
        if any(fr is None for fr in frames):
            continue
        segs = np.stack([fr[:, SEG] for fr in frames])          # caps x lines x SEG
        spec = np.fft.rfft(segs * window, axis=-1)              # caps x lines x bins
        mean_spec = spec.mean(axis=0)
        mp = np.zeros((FRAME_LINES, n_groups))
        np.add.at(mp.T, gid, (scale * np.abs(mean_spec) ** 2).T)
        dp = np.zeros((len(pairs), len(rows), n_groups), dtype=np.float32)
        for k, (i, j) in enumerate(pairs):
            d = scale * np.abs(spec[i, rows] - spec[j, rows]) ** 2
            acc = np.zeros((len(rows), n_groups))
            np.add.at(acc.T, gid, d.T)
            dp[k] = acc
        diff_pow.append(dp)
        mean_pow.append(np.stack([np.maximum(mp[up], mp[down]) for _, up, down in lines]))
        mean_line = np.fft.rfft(segs.mean(axis=0)[rows] - BLACK_IRE, axis=-1)
        luma = np.fft.irfft(np.where(lowpass, mean_line, 0), n=SEG_LEN, axis=-1)
        levels.append(np.sqrt((luma ** 2).mean(axis=-1)))
        used.append(int(f))
    if not used:
        sys.exit('no frame has every capture')

    return dict(pairs=pairs, n_groups=n_groups, frames=used,
                diff_pow=np.stack(diff_pow),     # frames x pairs x lines x groups
                mean_pow=np.stack(mean_pow),     # frames x lines x groups
                levels=np.stack(levels))         # frames x lines


def select(raw, level_window=None):
    """Mean difference power per pair and group over the clean, flat lines; with
    level_window, only over the lines whose level is in it."""
    diff_pow, mean_pow, levels = raw['diff_pow'], raw['mean_pow'], raw['levels']
    gband = band_of_groups(raw['n_groups'])

    # Dropouts: a pair's line far above the pair's median for the frame.
    broadband = diff_pow.sum(axis=-1)
    median = np.median(broadband, axis=2, keepdims=True)
    clean = broadband <= DROPOUT_FACTOR * median           # frames x pairs x lines

    # Content: the neighbours' band power in the mean, against the band's floor.
    band_mean = np.stack([mean_pow[..., gband == b].sum(axis=-1)
                          for b in range(len(BANDS))], axis=-1)   # frames x lines x bands
    floor = np.percentile(band_mean.reshape(-1, len(BANDS)), 5, axis=0)
    flat = band_mean <= CONTENT_FACTOR * floor              # frames x lines x bands
    if level_window:
        flat = flat & ((levels >= level_window[0]) & (levels < level_window[1]))[..., None]
    flat_groups = flat[..., np.maximum(gband, 0)]          # frames x lines x groups

    sel = clean[..., None] & flat_groups[:, None, :, :]    # frames x pairs x lines x groups
    pair_pow = np.where(sel, diff_pow, 0).sum(axis=(0, 2)) / np.maximum(sel.sum(axis=(0, 2)), 1)
    lines_used = sel.sum(axis=(0, 2))                      # pairs x groups
    high = flat[..., HIGH_BAND]
    level_sq = float((levels[high] ** 2).mean()) if high.any() else float('nan')

    return dict(pairs=raw['pairs'], pair_pow=pair_pow, lines_used=lines_used, gband=gband,
                n_groups=raw['n_groups'], frames=raw['frames'], floor=floor,
                flat_fraction=(band_mean <= CONTENT_FACTOR * floor).mean(axis=(0, 1)),
                high_band_level_sq=level_sq)


def solve(caps, m):
    """Per pressing: capture noise I_p, and pressing noise P from the cross pairs."""
    pressings = sorted({c['pressing'] for c in caps})
    member = {p: [i for i, c in enumerate(caps) if c['pressing'] == p] for p in pressings}
    pair_pow = dict(zip(m['pairs'], m['pair_pow']))

    def pw(i, j):
        return pair_pow[(min(i, j), max(i, j))]

    own = {}
    for p in pressings:
        within = [pw(i, j) for i in member[p] for j in member[p] if i < j]
        own[p] = np.mean(within, axis=0) / 2 if within else None
    cross = {}
    for a in pressings:
        for b in pressings:
            if a < b:
                cross[(a, b)] = np.mean([pw(i, j) for i in member[a] for j in member[b]], axis=0)

    multi = [p for p in pressings if own[p] is not None]
    out = dict(pressings=pressings, member=member, own=own, cross=cross, shared={}, mean_shared=None)
    # Pressing-pair sums, where both pressings have their own capture noise.
    sums = [cross[(a, b)] - own[a] - own[b] for a in multi for b in multi if a < b]
    if sums:
        out['mean_shared'] = np.mean(sums, axis=0) / 2
    # Three-cornered hat for each pressing with a capture noise of its own.
    for p in multi:
        others = [q for q in pressings if q != p]
        for q in others:
            for r in others:
                if q < r:
                    d_pq = cross[tuple(sorted((p, q)))]
                    d_pr = cross[tuple(sorted((p, r)))]
                    d_qr = cross[(q, r)]
                    out['shared'].setdefault(p, []).append((d_pq + d_pr - d_qr) / 2 - own[p])
        if p in out['shared']:
            out['shared'][p] = np.mean(out['shared'][p], axis=0)
    return out


def bands(x, gband):
    return [float(x[gband == b].sum()) for b in range(len(BANDS))]


def summarise(caps, m, s):
    gband = m['gband']
    multi = [p for p in s['pressings'] if s['own'][p] is not None]
    own = np.mean([s['own'][p] for p in multi], axis=0)
    result = dict(frames=m['frames'], captures=[dict(label=c['label'], pressing=c['pressing'],
                                                     grid=c['grid']) for c in caps],
                  bands=BAND_NAMES, group_mhz=GROUP_MHZ,
                  flat_fraction=[float(x) for x in m['flat_fraction']],
                  own_sigma_ire={p: [float(np.sqrt(max(v, 0))) for v in bands(s['own'][p], gband)]
                                 for p in multi},
                  own_power_by_pressing={p: [float(v) for v in s['own'][p]] for p in multi})
    if s['mean_shared'] is not None:
        shared = s['mean_shared']
        own_b, shared_b = bands(own, gband), bands(shared, gband)
        result['shared_sigma_ire'] = [float(np.sqrt(max(v, 0))) for v in shared_b]
        result['total_sigma_ire'] = [float(np.sqrt(max(a + b, 0))) for a, b in zip(own_b, shared_b)]
        result['shared_fraction'] = [float(b / (a + b)) if a + b > 0 else float('nan')
                                     for a, b in zip(own_b, shared_b)]
        result['shared_power'] = [float(v) for v in shared]
    result['shared_sigma_ire_by_pressing'] = {
        p: [float(np.sqrt(max(v, 0))) for v in bands(x, gband)] for p, x in s['shared'].items()}
    result['lines_per_band'] = [int(np.median(m['lines_used'][:, gband == b]))
                                for b in range(len(BANDS))]
    return result


def summarise_high_band(caps, raw):
    """The band above 5.5 MHz, measured separately on each class of line level."""
    out = {}
    for name, window in LEVEL_CLASSES.items():
        m = select(raw, window)
        r = summarise(caps, m, solve(caps, m))
        out[name] = dict(window_ire=[float(x) for x in window],
                         mean_level_sq=m['high_band_level_sq'],
                         lines=r['lines_per_band'][HIGH_BAND],
                         own_sigma_ire={p: v[HIGH_BAND] for p, v in r['own_sigma_ire'].items()})
        for key in ('shared_sigma_ire', 'total_sigma_ire', 'shared_fraction'):
            if key in r:
                out[name][key] = r[key][HIGH_BAND]
    return out


def spectrum_table(power, tolerance_db=1.0, floor_db=-40.0):
    """A videosynth `spectrum` table for a layer of per-group noise power.

    One breakpoint per group centre, in dB relative to the strongest group and
    floored at |floor_db|; the first group takes the second's level. Points are
    then dropped, the least needed first, while the straight lines between the
    rest stay within |tolerance_db| of every dropped point.
    """
    p = np.maximum(np.array(power, dtype=float), 1e-12)
    p[0] = p[1]
    db = np.maximum(10 * np.log10(p / p.max()), floor_db)
    mhz = (np.arange(len(p)) + 0.5) * GROUP_MHZ
    keep = list(range(len(p)))

    def worst(kept):
        return max(abs(np.interp(mhz[i], mhz[kept], db[kept]) - db[i]) for i in range(len(p)))

    while len(keep) > 2:
        trials = [(worst(keep[:k] + keep[k + 1:]), k) for k in range(1, len(keep) - 1)]
        err, k = min(trials)
        if err > tolerance_db:
            break
        keep.pop(k)
    return [[round(float(mhz[i]), 3), round(float(db[i]), 1)] for i in keep]


def layer_noise_db(power):
    """videosynth's noise_db for a layer of this per-group power: 100 IRE over
    its broadband sigma, the first group taken at the second's level."""
    p = np.maximum(np.array(power, dtype=float), 0)
    p[0] = p[1]
    return float(20 * np.log10(100 / np.sqrt(p.sum())))


def capture_model(r, pressing, level_gain=1.0):
    """videosynth capture-layer settings for one pressing: a floor whose band
    above 5.5 MHz is the black lines' noise, and a level term, shaped to that
    band alone, through the 35-75 IRE lines' noise. level_gain scales the
    level term's power, for the share of it that videosynth's level-modulated
    noise puts outside the band."""
    power = np.array(r['own_power_by_pressing'][pressing], dtype=float)
    gband = band_of_groups(len(power))
    high = gband == HIGH_BAND
    black, mid = (r['high_band_by_level'][k] for k in LEVEL_CLASSES)
    floor_high = black['own_sigma_ire'][pressing] ** 2
    a = level_gain * (mid['own_sigma_ire'][pressing] ** 2 - floor_high) / mid['mean_level_sq']
    floor = power.copy()
    floor[high] *= floor_high / power[high].sum()
    floor_db = layer_noise_db(floor)
    first_high = (np.argmax(high) + 0.5) * GROUP_MHZ
    return dict(noise_db=floor_db,
                noise_spread_db=float(10 * np.log10(1 + max(a, 0) * 100 ** 2
                                                    / (100 / 10 ** (floor_db / 20)) ** 2)),
                spectrum=spectrum_table(floor),
                spread_spectrum=[[first_high - GROUP_MHZ, -40.0]]
                + [pt for pt in spectrum_table(np.where(high, power, 0)) if pt[0] >= first_high])


def pressing_model(r):
    """videosynth shared-layer settings: a constant component below 5.5 MHz
    and a level term above it, at the black lines' pressing noise, whose gain
    falls to the 35-75 IRE lines' noise. The level_db points are in IRE above
    black, at each class's rms level; a project converts them to mV.

    videosynth joins spectrum points by straight lines in dB, so each component
    holds its level to the 5.5 MHz edge and drops to -40 dB a half group past
    it, on the other component's side; a drop between group centres would take
    power that noise_db then puts back in the other groups."""
    power = np.array(r['shared_power'], dtype=float)
    high = band_of_groups(len(power)) == HIGH_BAND
    edge = BANDS[HIGH_BAND][0]
    low_table = [pt for pt in spectrum_table(np.where(high, 0, power)) if pt[0] < edge]
    high_table = [pt for pt in spectrum_table(np.where(high, power, 0)) if pt[0] > edge]
    classes = [r['high_band_by_level'][k] for k in LEVEL_CLASSES]
    black_sigma = classes[0]['shared_sigma_ire']
    return dict(noise_db=layer_noise_db(np.where(high, 0, power)),
                spectrum=low_table + [[edge, low_table[-1][1]], [edge + GROUP_MHZ / 2, -40.0]],
                level_term=dict(
                    noise_db=float(20 * np.log10(100 / black_sigma)),
                    level_db_ire=[[round(float(np.sqrt(c['mean_level_sq'])), 1),
                                   round(float(20 * np.log10(c['shared_sigma_ire'] / black_sigma)), 2)]
                                  for c in classes],
                    spectrum=[[edge - GROUP_MHZ / 2, -40.0], [edge, high_table[0][1]]] + high_table))


def print_noise_model(r):
    """The measured layers as videosynth noise settings."""
    for p in r['own_power_by_pressing']:
        if 'high_band_by_level' in r:
            m = capture_model(r, p)
            print('capture layer, pressing %s: noise_db %.1f, noise_spread_db %.2f (level term '
                  'before calibration)' % (p, m['noise_db'], m['noise_spread_db']))
            print('  spectrum: %s' % json.dumps(m['spectrum']))
            print('  spread_spectrum: %s' % json.dumps(m['spread_spectrum']))
        else:
            print('capture layer, pressing %s: noise_db %.1f at the lines measured' % (
                p, layer_noise_db(r['own_power_by_pressing'][p])))
            print('  spectrum: %s' % json.dumps(spectrum_table(r['own_power_by_pressing'][p])))
    if 'shared_power' in r and 'high_band_by_level' in r:
        m = pressing_model(r)
        print('pressing layer, mean: noise_db %.1f' % m['noise_db'])
        print('  spectrum: %s' % json.dumps(m['spectrum']))
        t = m['level_term']
        print('  level_term: noise_db %.1f, level_db (IRE above black) %s' % (
            t['noise_db'], json.dumps(t['level_db_ire'])))
        print('    spectrum: %s' % json.dumps(t['spectrum']))
    elif 'shared_power' in r:
        print('pressing layer, mean: noise_db %.1f' % layer_noise_db(r['shared_power']))
        print('  spectrum: %s' % json.dumps(spectrum_table(r['shared_power'])))


def print_summary(r):
    print('%d frames, %s' % (len(r['frames']),
                             ', '.join('%s:%s' % (c['label'], c['pressing']) for c in r['captures'])))
    print('%-28s' % 'band (MHz)' + ''.join('%10s' % b for b in r['bands']))
    print('%-28s' % 'lines used per pair' + ''.join('%10d' % v for v in r['lines_per_band']))
    print('%-28s' % 'flat line fraction' + ''.join('%10.2f' % v for v in r['flat_fraction']))
    for p, v in r['own_sigma_ire'].items():
        print('%-28s' % ('capture sigma, pressing %s' % p) + ''.join('%10.3f' % x for x in v))
    for p, v in r['shared_sigma_ire_by_pressing'].items():
        print('%-28s' % ('pressing sigma, %s (hat)' % p) + ''.join('%10.3f' % x for x in v))
    if 'shared_sigma_ire' in r:
        print('%-28s' % 'pressing sigma, mean' + ''.join('%10.3f' % x for x in r['shared_sigma_ire']))
        print('%-28s' % 'total sigma per capture' + ''.join('%10.3f' % x for x in r['total_sigma_ire']))
        print('%-28s' % 'shared fraction' + ''.join('%10.3f' % x for x in r['shared_fraction']))
    if 'high_band_by_level' in r:
        classes = r['high_band_by_level']
        print('%-28s' % ('%s MHz by line level' % r['bands'][HIGH_BAND])
              + ''.join('%10s' % k for k in classes))
        print('%-28s' % 'mean level (IRE rms)'
              + ''.join('%10.1f' % np.sqrt(v['mean_level_sq']) for v in classes.values()))
        print('%-28s' % 'lines used per pair' + ''.join('%10d' % v['lines'] for v in classes.values()))
        for p in r['own_sigma_ire']:
            print('%-28s' % ('capture sigma, pressing %s' % p)
                  + ''.join('%10.3f' % v['own_sigma_ire'][p] for v in classes.values()))
        for key, label in (('shared_sigma_ire', 'pressing sigma, mean'),
                           ('total_sigma_ire', 'total sigma per capture'),
                           ('shared_fraction', 'shared fraction')):
            if all(key in v for v in classes.values()):
                print('%-28s' % label + ''.join('%10.3f' % v[key] for v in classes.values()))


def compare(measured, reference, tolerance):
    """The measured set against a reference measurement, each layer where a
    videosynth model defines it. Below 5.5 MHz neither layer depends on the
    level: every flat line. Above it both layers are compared per class of
    line level, since sets of different content differ in how many of their
    flat lines are bright. Pressings are paired in order among
    those with a capture noise of their own. A set of one pressing has no
    pressing layer, so the pressing rows are left out when either side lacks
    one."""
    ok = True
    both_pressed = 'total_sigma_ire' in measured and 'total_sigma_ire' in reference

    def table(title, columns, rows):
        nonlocal ok
        print('%-28s' % title + ''.join('%10s' % c for c in columns))
        for label, a, b in rows:
            ratios = [None if x is None else x / y for x, y in zip(a, b)]
            print('%-28s' % label + ''.join('%10s' % '' if x is None else '%10.3f' % x
                                            for x in ratios))
            ok &= all(abs(x - 1) <= tolerance for x in ratios if x is not None)

    pressings = list(zip(measured['own_sigma_ire'], reference['own_sigma_ire']))
    low = slice(0, HIGH_BAND)
    rows = []
    if both_pressed:
        rows += [('total sigma ratio', measured['total_sigma_ire'][low],
                  reference['total_sigma_ire'][low]),
                 ('shared fraction ratio', measured['shared_fraction'][low],
                  reference['shared_fraction'][low])]
    for pm, pr in pressings:
        rows.append(('capture sigma ratio, %s/%s' % (pm, pr), measured['own_sigma_ire'][pm][low],
                     reference['own_sigma_ire'][pr][low]))
    table('against reference', measured['bands'][low], rows)

    mc, rc = measured['high_band_by_level'], reference['high_band_by_level']
    classes = list(rc)
    rows = []
    if both_pressed:
        rows.append(('pressing sigma ratio', [mc[k]['shared_sigma_ire'] for k in classes],
                     [rc[k]['shared_sigma_ire'] for k in classes]))
    for pm, pr in pressings:
        rows.append(('capture sigma ratio, %s/%s' % (pm, pr),
                     [mc[k]['own_sigma_ire'][pm] for k in classes],
                     [rc[k]['own_sigma_ire'][pr] for k in classes]))
    table('%s MHz against reference' % measured['bands'][HIGH_BAND], classes, rows)
    print('within %.0f %%: %s' % (100 * tolerance, 'yes' if ok else 'NO'))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--system', choices=['pal', 'ntsc'], default='pal',
                    help='the captures\' video system (NTSC band edges are '
                    'PAL\'s scaled by the subcarrier ratio)')
    ap.add_argument('--project', help='orc project stacking the captures')
    ap.add_argument('--compare', help='source_alignment tool JSON for --project')
    ap.add_argument('--capture', action='append', default=[],
                    help='PATH,PRESSING[,GRID] of a capture whose frames line up')
    ap.add_argument('--use', help='comma-separated capture labels to keep')
    ap.add_argument('--probes', type=int, default=40, help='frames to measure')
    ap.add_argument('--first', type=int, default=0, help='first frame to probe')
    ap.add_argument('--last', type=int, help='last frame to probe')
    ap.add_argument('--levels', help='LO,HI: measure only lines whose level is in '
                    'this range of IRE above black')
    ap.add_argument('--model', action='store_true',
                    help='print the layers as videosynth noise settings')
    ap.add_argument('--json', help='write the result here')
    ap.add_argument('--reference', help='a result JSON to compare against')
    ap.add_argument('--tolerance', type=float, default=0.10)
    args = ap.parse_args()
    configure(args.system)

    if args.project:
        caps = captures_from_project(args.project, args.compare)
    else:
        caps = captures_from_args(args.capture)
    if args.use:
        keep = args.use.split(',')
        caps = [c for c in caps if c['label'] in keep]
    if len(caps) < 3:
        sys.exit('need at least three captures')

    levels = [float(x) for x in args.levels.split(',')] if args.levels else None
    raw = measure(caps, args.probes, args.first, args.last)
    m = select(raw, levels)
    r = summarise(caps, m, solve(caps, m))
    r['levels_ire'] = levels
    if levels is None:
        r['high_band_by_level'] = summarise_high_band(caps, raw)
    print_summary(r)
    if args.model:
        print_noise_model(r)
    if args.json:
        with open(args.json, 'w') as fh:
            json.dump(r, fh, indent=1)
    if args.reference:
        ok = compare(r, json.load(open(args.reference)), args.tolerance)
        sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
