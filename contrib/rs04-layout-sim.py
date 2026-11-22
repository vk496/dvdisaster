#!/usr/bin/env python3
"""
rs04-layout-sim.py - damage simulator for choosing the RS04 on-disc layout.

dvdisaster only sees whole unreadable 2048-byte sectors (erasures). This
script maps parameterised damage models onto LBAs using CLV disc geometry and
counts erasures per codeword for

  * RS03: n = 255 contiguous layers of L = capacity // 255 sectors,
          codeword i = sector i of every layer;
  * RS04 candidates: n layers of L = ceil(capacity / n) sectors, again one
          symbol per layer, optionally with a per-layer permutation of the
          32-sector blocks inside the layer (offset inside a block is kept,
          the L % 32 tail sectors stay in place):
            shift32  : block b -> (b + c_j) mod B            (a_j = 1)
            affine32 : block b -> (a_j * b + c_j) mod B,  gcd(a_j, B) = 1
            random32 : uniformly random permutation per layer (reference)

A codeword with more than m erasures fails and loses its erased data sectors.
Every damage model emits an ordered sequence of lost sectors (damage grows
monotonically), so one trial yields the exact loss fraction at which the
first codeword fails, for every layout and redundancy at once.

Damage models: outer-edge band (single layer, and dual-layer opposite track
path), scattered ECC-unit loss, band + decaying fringe, radial scratches,
contiguous runs, ~1 cm^2 spots. Also prints the I/O cost of a RAM-limited
repair and tries to reproduce dvdisaster issue #43 (RS03 on CD-R).

Usage:  python3 rs04-layout-sim.py [--trials 200] [--quick] > results.md
Needs Python 3 and numpy. Runs on all cores; --quick is a ~1 min smoke run.
"""
import argparse
import math
import os
from multiprocessing import Pool
from typing import NamedTuple

import numpy as np

SECTOR = 2048
RATES = [32 / 255, 44 / 255, 64 / 255]  # parity fraction m/n (RS03 32, 44, 64 roots)
LEVELS = [0.75, 0.9]               # loss levels, as fraction of the MDS limit m/n
CAP = 0.28                              # simulate damage up to 28 % of the disc
NS = [255, 1023, 4095, 16383, 65535]
PERMS = ['none', 'shift32', 'affine32', 'random32']


class Disc(NamedTuple):
    name: str
    sectors: int      # capacity C in 2048-byte sectors
    unit: int         # sectors lost together when the drive ECC fails
    r_in: float       # data zone radii, mm
    r_out: float
    pitch: float      # track pitch, mm


DISCS = {
    'CD':   Disc('CD-R 80 min', 359847, 1, 25.0, 58.0, 1.6e-3),
    'DVD5': Disc('DVD-5', 2295104, 16, 24.0, 58.0, 0.74e-3),
    'DVD9': Disc('DVD-9 (dual layer, OTP)', 4171712, 16, 24.0, 58.0, 0.74e-3),
    'BD25': Disc('BD-25 (no DM)', 12219392, 32, 24.0, 58.0, 0.32e-3),
}


# ---------------------------------------------------------------- geometry
# CLV spiral: track length up to radius r is pi*(r^2 - r_in^2)/pitch, and
# the capacity fills the data zone exactly, so the sector length on the
# track is s = pi*(r_out^2 - r_in^2) / (pitch * C).

def radius(d, x):
    return np.sqrt(d.r_in ** 2 + (x + 0.5) / d.sectors * (d.r_out ** 2 - d.r_in ** 2))


def lba_at(d, r):
    return int(d.sectors * (r * r - d.r_in ** 2) / (d.r_out ** 2 - d.r_in ** 2))


def sector_len(d):
    return math.pi * (d.r_out ** 2 - d.r_in ** 2) / (d.pitch * d.sectors)


def per_rev(d, r):
    """Sectors per revolution at radius r."""
    return 2 * math.pi * r / sector_len(d)


# ----------------------------------------------------------- damage models
# Each returns an int64 array of distinct LBAs in the order they get lost.

def expand(units, d):
    s = (np.asarray(units)[:, None] * d.unit + np.arange(d.unit)).ravel()
    return s[s < d.sectors]


def dmg_band(d, rng, cap):
    """Outer-edge band on a single-layer disc: highest LBAs go first."""
    return np.arange(d.sectors - 1, d.sectors - 1 - cap, -1)


def dmg_band_dual(d, rng, cap):
    """Opposite track path dual layer: L0 ends and L1 starts at the outer edge,
    so the band grows from the middle LBA outwards in both directions."""
    h, t = d.sectors // 2, np.arange(cap // 2 + 1)
    return np.column_stack([h - 1 - t, h + t]).ravel()[:cap]


def dmg_scattered(d, rng, cap):
    """Independent loss of whole ECC units (CD 1, DVD 16, BD 32 sectors)."""
    nu = -(-d.sectors // d.unit)
    return expand(rng.permutation(nu)[:cap // d.unit + 1], d)


def dmg_mixed(d, rng, cap, band=0.7, lam=0.05):
    """At total loss x: band of 0.7x at the outer end plus a fringe of 0.3x
    whose density decays as exp(-dist/lam) inwards from the band edge
    (dist and lam as fractions of the LBA range). Monotone in x, so each
    unit gets the level x at which it dies; units are lost in that order."""
    nu = -(-d.sectors // d.unit)
    z = (d.sectors - (np.arange(nu) + 0.5) * d.unit) / d.sectors  # distance from outer end
    u = rng.random(nu)
    lo, hi = np.zeros(nu), z / band                                 # hi: swallowed by the band
    for _ in range(40):                                             # bisect fringe density == u
        mid = (lo + hi) / 2
        dead = (1 - band) * mid / lam * np.exp(-(z - band * mid) / lam) >= u
        hi, lo = np.where(dead, mid, hi), np.where(dead, lo, mid)
    return expand(np.argsort(hi)[:cap // d.unit + 1], d)


def patch(d, r0, r1, theta, width):
    """ECC units hit by a patch spanning radii [r0, r1) and an arc of `width` mm
    starting at angle theta (in revolutions): one hit per revolution."""
    x = np.arange(lba_at(d, r0), lba_at(d, r1))
    r = radius(d, x)
    phase = ((r - d.r_in) / d.pitch - theta) % 1.0   # sector start relative to patch
    sec = sector_len(d) / (2 * np.pi * r)            # sector length in revolutions
    hit = (phase < width / (2 * np.pi * r)) | (phase > 1 - sec)
    return expand(np.unique(x[hit] // d.unit), d)


def grow(d, cap, pieces):
    """Concatenate damage pieces (in order, without duplicates) until cap."""
    seen, out, total = np.zeros(d.sectors, bool), [], 0
    while total < cap:
        s = next(pieces)
        s = s[~seen[s]]
        seen[s] = True
        out.append(s)
        total += len(s)
    return np.concatenate(out)


def dmg_patches(d, rng, cap, width, length):
    """Radial scratches (narrow, long) or spots (width ~ length), added one
    by one at random radius and angle."""
    def pieces():
        while True:
            r0 = rng.uniform(d.r_in, d.r_out - length)
            yield patch(d, r0, r0 + length, rng.random(), width)
    return grow(d, cap, pieces())


def dmg_runs(d, rng, cap, run):
    """Contiguous runs of `run` sectors at random places."""
    def pieces():
        while True:
            x0 = rng.integers(0, d.sectors)
            yield np.arange(x0, min(x0 + run, d.sectors))
    return grow(d, cap, pieces())


MODELS = {
    'band':      (dmg_band, {}),
    'band_dual': (dmg_band_dual, {}),
    'scattered': (dmg_scattered, {}),
    'mixed':     (dmg_mixed, {}),
    'scratch':   (dmg_patches, dict(width=0.5, length=3.0)),
    'runs':      (dmg_runs, dict(run=4096)),
    'spots':     (dmg_patches, dict(width=10.0, length=10.0)),
}


# ----------------------------------------------------------------- layouts

class Layout(NamedTuple):
    n: int
    L: int
    kind: str
    a: np.ndarray = None     # per-layer multiplier (affine/shift)
    c: np.ndarray = None     # per-layer offset
    table: np.ndarray = None  # per-layer inverse block permutation (random32)


def layout_specs(d):
    """(label, n, perm) for every layout that fits: RS04 needs B = L//32 >= 2."""
    specs = [('RS03', 255, 'rs03')]
    for n in NS:
        for p in PERMS:
            if (n == 255 and p == 'none') or -(-d.sectors // n) < 64:
                continue  # n=255/none is RS03 up to rounding of L
            specs.append((f'n={n} {p}', n, p))
    return specs


def make_layout(d, n, kind, rng):
    if kind == 'rs03':
        return Layout(n, d.sectors // n, 'none')
    L = -(-d.sectors // n)
    B = L // 32
    if kind == 'none':
        return Layout(n, L, kind)
    if kind == 'random32':
        return Layout(n, L, kind, table=np.argsort(rng.random((n, B)), axis=1))
    # Physical -> codeword block map is affine too, so sample it directly.
    a = np.ones(n, np.int64)
    if kind == 'affine32':
        a = rng.integers(1, B, n) if B > 2 else a
        while (bad := np.gcd(a, B) != 1).any():
            a[bad] = rng.integers(1, B, bad.sum())
    return Layout(n, L, kind, a=a, c=rng.integers(0, B, n))


def codeword_of(lay, lba):
    """Codeword index of each LBA (-1: outside the layout, e.g. RS03 tail)."""
    j = lba // lay.L
    cw = lba - j * lay.L
    if lay.kind != 'none':
        b, o = cw >> 5, cw & 31
        inb = b < lay.L // 32
        jb, bb = j[inb], b[inb]
        if lay.kind == 'random32':
            nb = lay.table[jb, bb]
        else:
            nb = lay.a[jb] * (bb - lay.c[jb]) % (lay.L // 32)
        cw[inb] = nb * 32 + o[inb]
    cw[j >= lay.n] = -1
    return cw


def first_failure(cw, ms):
    """Per m: number of lost sectors (prefix length of the damage sequence)
    at which the first codeword collects m+1 erasures; len(cw)+1 if never."""
    valid = np.flatnonzero(cw >= 0)
    c = cw[valid]
    c = c.astype(np.uint16) if c.max() < 65536 else c   # radix sort
    order = np.argsort(c, kind='stable')
    counts = np.bincount(c)
    starts = np.cumsum(counts) - counts
    out = []
    for m in ms:
        sel = counts > m
        out.append(valid[order[starts[sel] + m]].min() + 1 if sel.any() else len(cw) + 1)
    return out


def lost_data(cw, layer, q, m, k):
    """Unrecoverable data sectors after the first q lost sectors."""
    cq, lq = cw[:q], layer[:q]
    ok = cq >= 0
    failed = np.bincount(cq[ok]) > m
    data = cq[ok & (lq < k)]
    return int(failed[data].sum())


def rates_for(n):
    """(m, k) per parity fraction (for n = 255 these are RS03's root counts)."""
    return [(round(r * n), n - round(r * n)) for r in RATES]


# ------------------------------------------------------------------ trials

def run_trial(job):
    dkey, model, seed = job
    d = DISCS[dkey]
    gen, kw = MODELS[model]
    rng = np.random.default_rng(seed)
    cap = int(CAP * d.sectors)
    seq = gen(d, rng, cap, **kw)[:cap]
    specs = layout_specs(d)
    crit = np.zeros((len(specs), len(RATES)))
    lost = np.zeros((len(specs), len(RATES), len(LEVELS)))
    for li, (_, n, kind) in enumerate(specs):
        lay = make_layout(d, n, kind, rng)
        cw = codeword_of(lay, seq)
        layer = seq // lay.L
        mk = rates_for(n)
        fail = first_failure(cw, [m for m, _ in mk])
        for ri, ((m, k), p) in enumerate(zip(mk, fail)):
            crit[li, ri] = p / d.sectors
            for vi, f in enumerate(LEVELS):
                q = int(f * RATES[ri] * d.sectors)
                if p <= q:
                    lost[li, ri, vi] = lost_data(cw, layer, q, m, k)
    return crit, lost


def simulate(pool, dkey, model, trials, seed):
    jobs = [(dkey, model, seed + t) for t in range(trials)]
    res = pool.map(run_trial, jobs, chunksize=1)
    return np.array([r[0] for r in res]), np.array([r[1] for r in res])


def pct(x):
    return f'>{CAP * 100:.0f}' if x >= CAP else f'{x * 100:.2f}'


def report(dkey, model, trials, crit, lost):
    d = DISCS[dkey]
    print(f'\n### {d.name} - {model} ({trials} trials)\n')
    lim = ' / '.join(f'{r * 100:.1f}%' for r in RATES)
    print(f'99%-level = loss (% of sectors) where P(full recovery) falls below 99%. '
          f'MDS limits {lim}. lost@x = mean unrecoverable data sectors at x * limit.\n')
    hdr = [f'99%-lvl {r * 255:.0f}r' for r in RATES]
    hdr += [f'lost@{f} {r * 255:.0f}r' for r in RATES for f in LEVELS]
    print('| layout | ' + ' | '.join(hdr) + ' |')
    print('|---' * (len(hdr) + 1) + '|')
    q01 = np.quantile(crit, 0.01, axis=0, method='lower')
    ml = lost.mean(axis=0)
    for li, (label, _, _) in enumerate(layout_specs(d)):
        cells = [pct(q01[li, ri]) for ri in range(len(RATES))]
        cells += [f'{ml[li, ri, vi]:.0f}' for ri in range(len(RATES))
                  for vi in range(len(LEVELS))]
        print(f'| {label} | ' + ' | '.join(cells) + ' |')


# ------------------------------------------------------------- I/O model

FFTS = [1024, 4096, 16384, 65536]


def next_pow2(x):
    return 1 << (x - 1).bit_length()


def fft_size(n, r):
    """Leopard / reed-solomon-simd decoder work size for k + m = n shards:
    next_pow2(next_pow2(m) + k); must be <= 65536."""
    m = round(r * n)
    return next_pow2(next_pow2(m) + n - m)


def largest_n(fft, r):
    return max(n for n in range(255, fft + 1) if fft_size(n, r) <= fft)


def leopard_limits():
    print('\n## Leopard / reed-solomon-simd shard counts\n')
    print('Decoder work = next_pow2(next_pow2(m) + k) shards. Largest n per FFT size '
          '(n = 2^p - 1 often needs the next FFT size up):\n')
    print('| m/n | ' + ' | '.join(f'FFT {f}' for f in FFTS) + ' | FFT size of n=4095 / 16383 / 65535 |')
    print('|---' * (len(FFTS) + 2) + '|')
    for r in RATES:
        cells = [f'{largest_n(f, r)} (m={round(r * largest_n(f, r))})' for f in FFTS]
        cells.append(' / '.join(str(fft_size(n, r)) for n in (4095, 16383, 65535)))
        print(f'| {r * 255:.0f}/255 | ' + ' | '.join(cells) + ' |')


def fmt_t(s):
    return f'{s / 3600:.1f} h' if s >= 3600 else f'{s / 60:.0f} min' if s >= 60 else f'{s:.0f} s'


def io_table(r=RATES[0]):
    """RAM-limited repair: W codewords (= sectors per layer) per pass need
    W * fft * 2048 bytes; per layer a pass reads W sectors with one seek
    (identity / shift layouts) or ceil(W/32) seeks (block-permuted layouts),
    or the whole layer sequentially if that is cheaper."""
    disks = {'HDD': (10e-3, 150e6), 'SSD': (0.1e-3, 500e6)}
    rams = {'256M': 256 << 20, '1G': 1 << 30, '4G': 4 << 30}
    print(f'\n## Repair I/O cost, m/n = {r * 255:.0f}/255 (image file on HDD 10 ms + 150 MB/s, '
          f'SSD 0.1 ms + 500 MB/s)\n')
    print('W = sectors per layer buffered per pass, contig = identity layout, '
          'blocks = affine32/random32. Same pattern applies to encoding.\n')
    print('| image | n (FFT) | L | RAM | W | passes | seeks contig | seeks blocks | '
          'HDD contig | HDD blocks | SSD contig | SSD blocks |')
    print('|---' * 12 + '|')
    for name, C in [('BD-25', 12219392), ('BDXL 100G', 48878592)]:
        for n, fft in [(255, 256)] + [(largest_n(f, r), f) for f in FFTS]:
            L = -(-C // n)
            for rn, ram in rams.items():
                W = min(L, ram // (fft * SECTOR))
                passes = -(-L // W)
                runs_b = -(-W // 32)
                row = [name, f'{n} ({fft})', L, rn, W, passes, passes * n, passes * n * runs_b]
                for seek, bw in disks.values():
                    full_layer = seek + L * SECTOR / bw
                    t_c = min(seek + W * SECTOR / bw, full_layer)
                    t_b = min(runs_b * seek + W * SECTOR / bw, full_layer)
                    row += [fmt_t(passes * n * t_c), fmt_t(passes * n * t_b)]
                print('| ' + ' | '.join(str(v) for v in row) + ' |')


def aliasing_table():
    """RS03 stride aliasing needs the per-layer phase step L/P (revolutions
    between a codeword's members) to stay near an integer over many layers.
    Under CLV it changes by (L/P)^2 * pitch / r revolutions per layer."""
    print('\n## RS03 stride/revolution aliasing check\n')
    print('| disc | L | P inner..outer (sectors/rev) | L/P inner..outer | '
          'drift of phase step per layer, inner..outer (rev) |')
    print('|---|---|---|---|---|')
    for d in DISCS.values():
        L = d.sectors // 255
        p = [per_rev(d, r) for r in (d.r_in, d.r_out)]
        lp = [L / x for x in p]
        dr = [q * q * d.pitch / r for q, r in zip(lp, (d.r_in, d.r_out))]
        print(f'| {d.name} | {L} | {p[0]:.1f}..{p[1]:.1f} | {lp[0]:.0f}..{lp[1]:.0f} | '
              f'{dr[0]:.2f}..{dr[1]:.2f} |')


# --------------------------------------------------------- issue #43 (RS03)

def read_skip(bad, skip):
    """dvdisaster reader: after a read error mark `skip` sectors unread, go on."""
    lost = bad.copy()
    if skip > 1:
        e, x = np.flatnonzero(bad), 0
        while (i := np.searchsorted(e, x)) < len(e):
            lost[e[i]:e[i] + skip] = True
            x = e[i] + skip
    return lost


def issue43_trial(job):
    """CD-R, RS03 with 47 roots (L = 1409, 359,295 sectors). Marker-pen defects
    of width (along track) x length (radial) mm, each sector inside unreadable
    with prob. `dens`, are added until the reader (with its skip rule) reports
    19,393 lost sectors (5.4 %); the radial length of the last defect is
    trimmed to hit that count."""
    seed, width, length, dens, skip, n, kind = job
    d = DISCS['CD']
    rng = np.random.default_rng(seed)
    img, target = 255 * 1409, 19393
    weak = rng.random(d.sectors) < dens
    bad = np.zeros(d.sectors, bool)

    def with_defect(r0, theta, ln):
        b = bad.copy()
        s = patch(d, r0, r0 + ln, theta, width)
        b[s[weak[s]]] = True
        return b

    while True:
        r0, theta = rng.uniform(d.r_in, d.r_out - length), rng.random()
        if read_skip(with_defect(r0, theta, length), skip)[:img].sum() < target:
            bad = with_defect(r0, theta, length)
            continue
        lo, hi = 0.0, length
        for _ in range(20):
            mid = (lo + hi) / 2
            if read_skip(with_defect(r0, theta, mid), skip)[:img].sum() >= target:
                hi = mid
            else:
                lo = mid
        lost = np.flatnonzero(read_skip(with_defect(r0, theta, hi), skip)[:img])
        break
    lay = make_layout(Disc('CD image', img, 1, 0, 0, 0), n, kind, rng)
    m = 47 if kind == 'rs03' else round(47 / 255 * n)
    cw = codeword_of(lay, lost)
    per_cw = np.bincount(cw, minlength=lay.L)
    failed = per_cw > m
    unrec = int(failed[cw[lost // lay.L < n - m]].sum())
    # periodic spikes: strongest autocorrelation of per-codeword erasures at
    # lags 17..200 (shorter lags are dominated by the reader's skip runs)
    v = per_cw - per_cw.mean()
    ac = np.array([np.dot(v[:-k], v[k:]) for k in range(17, min(201, lay.L // 2))]) / np.dot(v, v)
    return unrec, int(failed.sum()), int(per_cw.max()), int(np.argmax(ac)) + 17, ac.max(), len(lost)


def issue43(pool, trials):
    print('\n## Issue #43 reproduction (CD-R, RS03 47 roots, 5.4 % erased by spots)\n')
    print('max cw = most erasures in one codeword (47 tolerated); AC = strongest '
          'autocorrelation of erasures per codeword index, lag 17..200 (periodic spikes; only RS03/n=255 rows).\n')
    print('| defect (along x radial) | density | skip | layout | P(unrec>0) | mean unrec. | '
          'P(unrec>=1449) | mean failed cw | mean max cw | median AC lag | mean AC peak |')
    print('|---' * 11 + '|')
    for width, length, dens, skip in [(10, 10, 1.0, 1), (10, 10, 0.3, 1), (10, 10, 1.0, 16),
                                      (10, 10, 0.3, 16), (5, 5, 0.3, 16), (1, 20, 1.0, 16),
                                      (1, 20, 1.0, 1)]:
        for label, n, kind in [('RS03', 255, 'rs03'), ('n=255 random32', 255, 'random32'),
                               ('n=255 affine32', 255, 'affine32'), ('n=1023 none', 1023, 'none')]:
            res = np.array(pool.map(issue43_trial,
                                    [(s, width, length, dens, skip, n, kind) for s in range(trials)]))
            print(f'| {width}x{length} mm | {dens} | {skip} | {label} | {np.mean(res[:, 0] > 0):.2f} | '
                  f'{res[:, 0].mean():.0f} | {np.mean(res[:, 0] >= 1449):.2f} | '
                  f'{res[:, 1].mean():.1f} | {res[:, 2].mean():.1f} | '
                  f'{np.median(res[:, 3]):.0f} | {res[:, 4].mean():.2f} |')


# --------------------------------------------------------------------- main

def self_check():
    d = DISCS['DVD5']
    rng = np.random.default_rng(1)
    lba = np.arange(d.sectors)
    for n, kind in [(255, 'rs03'), (1023, 'affine32'), (4095, 'random32'), (255, 'shift32')]:
        lay = make_layout(d, n, kind, rng)
        cw = codeword_of(lay, lba)
        j = lba // lay.L
        # one symbol per layer: (layer, codeword) pairs are unique, all L codewords used
        key = j[cw >= 0] * lay.L + cw[cw >= 0]
        assert len(np.unique(key)) == len(key) and cw.max() == lay.L - 1, (n, kind)
    # band at the MDS limit: RS03 fails exactly when m+1 layers are gone
    lay = make_layout(d, 255, 'rs03', rng)
    seq = dmg_band(d, rng, int(0.2 * d.sectors))
    p = first_failure(codeword_of(lay, seq), [32])[0]
    tail = d.sectors - 255 * lay.L
    assert p == tail + 32 * lay.L + 1, p


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('--trials', type=int, default=200)
    ap.add_argument('--quick', action='store_true', help='few trials, CD/DVD only')
    ap.add_argument('--seed', type=int, default=4)
    args = ap.parse_args()
    self_check()
    t = 10 if args.quick else args.trials
    tb = 3 if args.quick else 20                      # band models are near-deterministic
    plan = [('DVD9', 'band_dual', tb)]
    for dk in (['CD', 'DVD5'] if args.quick else ['CD', 'DVD5', 'BD25']):
        plan += [(dk, 'band', tb)] + [(dk, m, t) for m in
                                     ['scattered', 'mixed', 'scratch', 'runs', 'spots']]
    print('# RS04 layout simulation results\n')
    print(f'Models: scratch = 0.5 mm wide x 3 mm radial; runs = 4096 sectors; spots = 10x10 mm; '
          f'mixed = 70 % band + 30 % fringe decaying over 5 % of the LBA range. '
          f'Seed {args.seed}.')
    with Pool(min(os.cpu_count(), 12)) as pool:
        for dk, model, n in plan:
            crit, lost = simulate(pool, dk, model, n, args.seed * 100000)
            report(dk, model, n, crit, lost)
        aliasing_table()
        leopard_limits()
        io_table()
        issue43(pool, 20 if args.quick else 200)


if __name__ == '__main__':
    main()
