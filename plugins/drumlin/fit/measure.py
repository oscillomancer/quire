#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
"""Measurements for fitting Drumlin's recipes (README.md, "Measuring a recording").

    measure.py partials <wav> --clock HZ [--count N] [--body S]    a sound's partials, as JSON
    measure.py clock <wav> --guess HZ                              its DAC's clock, from its images
    measure.py spec <partials.json> --clock HZ --length MS --target LINE [...]
                                                                   a draft spec built on the partials
    measure.py compare <recording.wav> <render.wav>                where a render differs, and how
    measure.py listen <spec> <render-dir> <out.wav>                recordings and renders in turn

numpy only. Recordings are read, never copied: what these print are measurements, and only
fitted numbers go into the script.
"""
import argparse
import json
import math
import struct
import sys

import numpy as np

RATE = 44100
MOST = 16  # the script's kModes and kBands: a recipe's modes, and its bands


# --- reading and writing ---------------------------------------------------------------------

def read_wav(path):
    """The first channel of a PCM (16, 24 or 32-bit) or float WAV, as floats; its rate."""
    with open(path, 'rb') as f:
        d = f.read()
    if d[:4] != b'RIFF' or d[8:12] != b'WAVE':
        sys.exit(f'{path}: not a WAV file')
    i, fmt, data = 12, None, None
    while i + 8 <= len(d):
        cid, size = d[i:i + 4], struct.unpack('<I', d[i + 4:i + 8])[0]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', d[i + 8:i + 24])
        elif cid == b'data':
            data = d[i + 8:i + 8 + size]
        i += 8 + size + (size & 1)
    if fmt is None or data is None:
        sys.exit(f'{path}: no fmt or data chunk')
    tag, channels, rate, _, _, bits = fmt
    if tag == 0xFFFE:  # WAVE_FORMAT_EXTENSIBLE: the subformat's tag
        i = d.index(b'fmt ') + 8
        tag = struct.unpack('<H', d[i + 24:i + 26])[0]
    if tag == 3 and bits == 32:
        x = np.frombuffer(data, '<f4').astype(float)
    elif tag == 3 and bits == 64:
        x = np.frombuffer(data, '<f8').astype(float)
    elif tag == 1 and bits == 16:
        x = np.frombuffer(data, '<i2') / 32768.0
    elif tag == 1 and bits == 24:
        b = np.frombuffer(data[:len(data) // 3 * 3], np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = np.where(v >= 1 << 23, v - (1 << 24), v) / float(1 << 23)
    elif tag == 1 and bits == 32:
        x = np.frombuffer(data, '<i4') / 2147483648.0
    else:
        sys.exit(f'{path}: format {tag}, {bits} bits is not read here')
    return x[::channels], rate


def read(path):
    x, rate = read_wav(path)
    if rate != RATE:
        sys.exit(f'{path}: {rate} Hz; the fitter and these measurements work at {RATE}')
    return x


def write_wav(path, x):
    """16-bit mono at RATE."""
    d = (np.clip(x, -1, 1) * 32767).astype('<i2').tobytes()
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(d)) + b'WAVEfmt ' +
                struct.pack('<IHHIIHH', 16, 1, 1, RATE, RATE * 2, 2, 16) + b'data' +
                struct.pack('<I', len(d)) + d)


def hit(x):
    """The hit: the first sample past a quarter of the peak, as the fitter lines sounds up."""
    return int(np.argmax(np.abs(x) > 0.25 * np.abs(x).max()))


def db(p):
    return 10 * np.log10(p + 1e-20)


def floor_of(f, s, pct=20):
    """Each bin's floor: the pct-th percentile of the third of an octave around it. Low, so a dense
    cluster of partials still stands above it."""
    out = s.copy()
    for i, fi in enumerate(f):
        if fi > 80:
            out[i] = np.percentile(s[(f > fi * 2 ** (-1 / 6)) & (f < fi * 2 ** (1 / 6))], pct)
    return out


# --- partials --------------------------------------------------------------------------------

def averaged(x, a, b, n):
    """The power spectrum (dB) averaged over frames of n from a to b seconds."""
    w = np.hanning(n)
    acc, k = 0.0, 0
    for i in range(int(a * RATE), max(int(a * RATE) + 1, int(b * RATE) - n), n // 8):
        seg = x[i:i + n]
        if len(seg) < n:
            seg = np.pad(seg, (0, n - len(seg)))
        acc = acc + np.abs(np.fft.rfft(seg * w)) ** 2
        k += 1
    return np.fft.rfftfreq(n, 1 / RATE), db(acc / k)


def partials(x, clock, count=24, body=0.9):
    """A recording's strongest partials below half its DAC's clock (the images above are the
    engine's to make): frequency, -3 dB width with the window's own taken out, prominence over the
    floor, level (dB re the loudest), when it is loudest, and its decay as time constants over its
    first 10 dB (early) and the 20 dB after (late). Peaks from the body (from 30 ms, or 3 ms for a
    short sound, to `body` seconds) and from the hit alone (its first 100 ms), then the hit's own
    low partials, gone too soon to show in the body (`hit`: true)."""
    x = x[hit(x):]
    nb = 16384 if body > 0.5 else 4096
    start = 0.03 if body > 0.5 else 0.003
    found = []
    for a, b, n in ((start, body, nb), (0.0, 0.1, 4096)):
        f, s = averaged(x, a, b, n)
        fl = floor_of(f, s)
        r = max(2, n // 4096)
        for i in range(r, len(f) - r):
            if 150 < f[i] < clock / 2 - 150 and s[i] - fl[i] > 12 and s[i] == s[i - r:i + r + 1].max():
                found.append((f[i], s[i] - s.max()))
    f, s = averaged(x, start, body, nb)
    prom = s - floor_of(f, s)
    picked = []
    for hz, _ in sorted(found, key=lambda t: -t[1]):
        i = int(np.argmin(abs(f - hz)))
        i = i - 3 + int(np.argmax(s[i - 3:i + 4]))
        if all(abs(f[i] / f[j] - 1) > 0.003 for j in picked):
            picked.append(i)
    picked = picked[:count]
    if not picked:
        return []
    # Each partial's level over time: 4096-point frames every 1024 samples.
    m = 4096
    frames, times = [], []
    for i in range(0, len(x) - m, m // 4):
        frames.append(np.abs(np.fft.rfft(x[i:i + m] * np.hanning(m))) ** 2)
        times.append((i + m / 2) / RATE)
    frames, times = db(np.array(frames)), np.array(times)
    top = max(s[i] for i in picked)
    out = []
    for i in sorted(picked, key=lambda i: f[i]):
        lo, hi = i, i
        while lo > 0 and s[lo] > s[i] - 3:
            lo -= 1
        while hi < len(s) - 1 and s[hi] > s[i] - 3:
            hi += 1
        width = max(0.5, math.sqrt(max((f[hi] - f[lo]) ** 2 - (1.44 * RATE / nb) ** 2, 0.25)))
        j = int(round(f[i] / (RATE / m)))
        env = frames[:, j - 1:j + 2].max(axis=1)
        k = int(np.argmax(env))

        def after(drop, env=env, k=k):
            below = np.where(env[k:] < env[k] - drop)[0]
            return times[k + below[0]] if len(below) else None
        t10, t30 = after(10), after(30)
        out.append(dict(hz=round(float(f[i]), 1), bw=round(width, 1), prom=round(float(prom[i]), 1),
                        level=round(float(s[i] - top), 1), peak_ms=round(float(times[k]) * 1000),
                        tau_early=round((t10 - times[k]) / (10 / 8.686) * 1000) if t10 else None,
                        tau_late=round((t30 - t10) / (20 / 8.686) * 1000) if t10 and t30 else None))
    # The hit's own low partials: the first 30 ms, zero-padded, each decay from a band about it.
    s0 = x[:int(0.03 * RATE)]
    n0 = 16384
    p0 = db(np.abs(np.fft.rfft(s0 * np.hanning(len(s0)), n0)) ** 2)
    f0 = np.fft.rfftfreq(n0, 1 / RATE)
    fl0 = floor_of(f0, p0)
    peaks = [i for i in range(8, len(f0) - 8)
             if 150 < f0[i] < 2000 and p0[i] - fl0[i] > 3 and p0[i] == p0[i - 8:i + 9].max()]
    xs = x[:int(0.5 * RATE)]
    spectrum = np.fft.rfft(xs)
    fx = np.fft.rfftfreq(len(xs), 1 / RATE)
    for i in sorted(sorted(peaks, key=lambda i: -p0[i])[:6], key=lambda i: f0[i]):
        hz = f0[i]
        if any(abs(hz / o['hz'] - 1) < 0.02 for o in out):
            continue
        band = spectrum.copy()
        band[(fx < hz - 20) | (fx > hz + 20)] = 0
        y = np.fft.irfft(band)
        env = np.array([20 * np.log10(np.sqrt(np.mean(y[k:k + 220] ** 2)) + 1e-12)
                        for k in range(0, len(y) - 220, 220)])
        k = int(np.argmax(env[:10]))

        def after(drop, env=env, k=k):
            below = np.where(env[k:] < env[k] - drop)[0]
            return (k + below[0]) * 5.0 if len(below) else None
        t10, t25 = after(10), after(25)
        out.append(dict(hz=round(float(hz), 1), bw=20.0, prom=round(float(p0[i] - fl0[i]), 1),
                        level=round(float(p0[i] - p0.max()), 1), peak_ms=0,
                        tau_early=round((t10 - k * 5.0) / (10 / 8.686)) if t10 else 30,
                        tau_late=round((t25 - t10) / (15 / 8.686)) if t10 and t25 else 150, hit=True))
    return out


# --- the DAC's clock -------------------------------------------------------------------------

def interpolated(f, s, i):
    a, b, c = s[i - 1], s[i], s[i + 1]
    return f[i] + 0.5 * (a - c) / (a - 2 * b + c) * (f[1] - f[0])


def clock_from_images(x, near, parts):
    """Each strong partial's image sits at clock - f: the sums of partials and the images found
    within 0.3 % of `near` (the mirror's clock: searched wider, another partial's image is often
    the loudest), with how far each image stands out."""
    x = x[hit(x):]
    f, s = averaged(x, 0.02, min(len(x) / RATE, 1.2), 32768)
    pairs = []
    for p in sorted(parts, key=lambda p: -p['level'])[:16]:
        i = int(np.argmin(abs(f - p['hz'])))
        i = i - 3 + int(np.argmax(s[i - 3:i + 4]))
        fp = interpolated(f, s, i)
        span = near * 0.003
        window = np.where((f > near - fp - span) & (f < near - fp + span))[0]
        if len(window) == 0 or window[-1] + 200 >= len(s) or window[0] < 200:
            continue
        j = window[np.argmax(s[window])]
        fi = interpolated(f, s, j)
        pairs.append((fp + fi, fp, fi, s[j] - np.median(s[j - 200:j + 200])))
    return pairs


def clock_from_mirror(x, guess):
    """The clock at which the spectrum's fine detail from 30 to 48 % of it best matches its mirror
    image above (S(f) against S(clock - f)), searched 4 % either side of the guess: the correlation
    and the next best well away from it, which says how sure it is."""
    x = x[hit(x):hit(x) + int(0.25 * RATE)]
    n = 65536
    s = db(np.abs(np.fft.rfft(x * np.hanning(len(x)), n)) ** 2)
    df = RATE / n
    k = np.arange(len(s))
    detail = s - np.array([s[max(0, i - int(i * 0.06)):i + int(i * 0.06) + 1].mean() for i in k])
    tried = []
    for c in np.arange(guess * 0.96, guess * 1.04, 0.5):
        lo, hi = int(c * 0.30 / df), int(c * 0.48 / df)
        images = np.round((c - k[lo:hi] * df) / df).astype(int)
        if images.max() >= len(detail):
            continue
        tried.append((np.corrcoef(detail[lo:hi], detail[images])[0, 1], c))
    tried.sort(reverse=True)
    r, c = tried[0]
    return c, r, max(v for v, cc in tried if abs(cc - c) > 30)


# --- a draft spec ----------------------------------------------------------------------------

def rise_for(peak_ms, tau, smallest):
    """The attack a with a * ln(1 + tau / a) = peak_ms: a band rising as 1 - e^(-t/a) and falling
    with tau is loudest when its partial was."""
    if peak_ms <= smallest:
        return smallest / 15.0
    lo, hi = 0.05, 5000.0
    for _ in range(60):
        a = math.sqrt(lo * hi)
        if a * math.log(1 + tau / a) < peak_ms:
            lo = a
        else:
            hi = a
    return a


def draft(parts, clock, length, targets, modes, narrow, broad, apart, hold):
    """A spec on measured partials: the hit's own and the earliest as modes (decaying sines), the
    rest that are loudest as narrow bands of noise rising to their peak, then broad bands for the
    wash and a stick band. Frequencies within 0.5 % (modes) or 1 % (bands) of the measured, and a
    narrow band never narrower than its partial measured: narrower, it rings as a steady tone (the
    hi-hat's and the tambourine's old fault). hold: the partials' shapes held at the measured, for
    a first fit of the gains alone."""
    lines = []

    def free(name, init, lo, hi, log=True):
        if hold:
            return f'set {name} {init}'
        return f'free {name} {init} {lo} {hi}' + (' log' if log else '')
    ranked = sorted(parts, key=lambda p: -p['level'])
    early = [p for p in ranked if p.get('hit')] + \
            [p for p in ranked if p['peak_ms'] <= 100 and not p.get('hit')]
    as_modes = sorted(early[:modes], key=lambda p: p['hz'])
    as_bands = []
    for p in ranked:
        if p in as_modes or len(as_bands) >= narrow:
            continue
        if all(abs(p['hz'] / q['hz'] - 1) > apart for q in as_bands + as_modes):
            as_bands.append(p)
    as_bands.sort(key=lambda p: p['hz'])
    stick = 1 if as_modes else 0
    if len(as_modes) > MOST or broad + stick + len(as_bands) > MOST:
        sys.exit(f'{len(as_modes)} modes and {broad + stick + len(as_bands)} bands: a recipe holds '
                 f'{MOST} of each (kModes, kBands in the script)')
    lines += [f'# partials measured from the recording: {len(as_modes)} as modes, {len(as_bands)} '
              f'as narrow bands; {broad} broad bands for the wash' + (', a stick band' if stick else ''),
              f'set clock {clock}', f'set length {length}', f'set modes {len(as_modes)}',
              f'set bands {broad + stick + len(as_bands)}', 'weights 0.5 1 0.7 0 1.5 1',
              f'window 0.02 {max(0.3, length / 1000 + 0.05):.2f}']
    for j, p in enumerate(as_modes):
        hz, te = p['hz'], p['tau_early'] or 200
        tl = p['tau_late'] or te
        lines += [free(f'mode{j}.hz', hz, f'{hz / 1.005:.2f}', f'{hz * 1.005:.2f}'),
                  f"free mode{j}.gain {0.1 * 10 ** (p['level'] / 20):.4f} 0.0005 1 log",
                  free(f'mode{j}.fast', te, f'{te / 3:.0f}', f'{te * 3:.0f}'),
                  free(f'mode{j}.slow', tl, f'{tl / 3:.0f}', f'{tl * 3:.0f}'),
                  free(f'mode{j}.split', 0.5, 0, 1, False)]
    top = clock / 2
    centres = [top * r for r in np.geomspace(0.04, 0.6, broad)] if broad else []
    for b, hz in enumerate(centres):
        lines += [f'set band{b}.shape 0', f'free band{b}.hz {hz:.0f} {hz / 2.5:.0f} {hz * 2.5:.0f} log',
                  f'free band{b}.q 0.8 0.2 4 log', f'free band{b}.gain 0.05 0.0005 2 log',
                  f'free band{b}.delay 0 -20 20', f'free band{b}.attack 5 0.01 400 log',
                  f'free band{b}.fast 60 1 1000 log', f'free band{b}.slow 400 5 3000 log',
                  f'free band{b}.split 0.5 0 1']
    if stick:
        b = broad
        lines += [f'set band{b}.shape 0', f'free band{b}.hz 700 250 2000 log',
                  f'free band{b}.q 0.7 0.3 3 log', f'free band{b}.gain 0.1 0.0005 4 log',
                  f'set band{b}.split 0', f'free band{b}.fast 30 3 200 log']
    smallest = 60 if length > 500 else 15
    for k, p in enumerate(as_bands):
        b = broad + stick + k
        hz, bw = p['hz'], max(p['bw'], 10.0)
        te = p['tau_early'] or 30
        tl = p['tau_late'] or te
        a = rise_for(p['peak_ms'], tl, smallest)
        lines += [f'set band{b}.shape 0', free(f'band{b}.hz', hz, f'{hz / 1.01:.2f}', f'{hz * 1.01:.2f}'),
                  free(f'band{b}.q', f'{hz / bw:.1f}', f'{hz / (2.5 * bw):.1f}', f'{hz / bw:.1f}'),
                  f"free band{b}.gain {0.5 * 10 ** (p['level'] / 20):.4f} 0.002 20 log",
                  free(f'band{b}.attack', f'{a:.2f}', f'{max(0.05, a / 4):.2f}', f'{a * 4:.2f}'),
                  free(f'band{b}.fast', te, f'{te / 3:.1f}', f'{te * 3:.1f}'),
                  free(f'band{b}.slow', tl, f'{tl / 3:.1f}', f'{tl * 3:.1f}'),
                  free(f'band{b}.split', 0.5, 0, 1, False)]
    lines += ['free hpf 10 1 500 log', 'free level -12 -40 12', 'free peak 1.0 0.5 4 log']
    if as_modes:
        lines += ['free strike 0.3 0.01 20 log']
    lines += [t if t.startswith('target ') else f'target {t}' for t in targets]
    return lines


# --- comparing a render with its recording ---------------------------------------------------

def compare(a, b):
    """Render minus recording in third octaves over time; each band's floor and how far its peaks
    stand above it (a ring stands too proud); the strongest partials, which set a cymbal's pitch;
    and the balance by octave."""
    a, b = a[hit(a):], b[hit(b):]
    end = min(len(a), len(b)) / RATE
    edges = [0.0, 0.03, 0.15, 0.4, 1.0, 1.8, 3.0]
    spans = [(edges[i], edges[i + 1]) for i in range(len(edges) - 1) if edges[i + 1] <= end + 0.05]
    if end < 0.3:
        spans = [(0.0, 0.015), (0.015, 0.04), (0.04, 0.08), (0.08, min(0.15, end))]

    def window(x, t0, t1, n=None):
        seg = x[int(t0 * RATE):int(t1 * RATE)]
        n = n or len(seg)
        buf = np.zeros(n)
        k = min(len(seg), n)
        buf[:k] = seg[:k] * np.hanning(k)
        return np.fft.rfftfreq(n, 1 / RATE), np.abs(np.fft.rfft(buf)) ** 2
    thirds = [500, 1000, 2000, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000]
    print('Render minus recording, dB, by third octave:')
    print('ms         ' + ''.join(f'{c / 1000:>6g}k' for c in thirds))
    for t0, t1 in spans:
        f, pa = window(a, t0, t1)
        _, pb = window(b, t0, t1)
        row = ''
        for c in thirds:
            m = (f >= c * 2 ** (-1 / 6)) & (f < c * 2 ** (1 / 6))
            row += f'{db(pb[m].sum()) - db(pa[m].sum()):7.1f}'
        print(f'{t0 * 1000:5.0f}-{t1 * 1000:<5.0f}' + row)
    octaves = [(1000, 2000), (2000, 4000), (4000, 6000), (6000, 8000), (8000, 11000), (11000, 16000)]
    print('\nFloor (median, dB) / peaks above it (97th percentile less the median), 8192-point:')
    print('ms              ' + ''.join(f'{lo / 1000:>5g}-{hi / 1000:<5g}k  ' for lo, hi in octaves))
    for t0, t1 in spans:
        for tag, x in (('rec', a), ('ren', b)):
            f, p = window(x, t0, t1, 8192)
            s = db(p)
            row = ''
            for lo, hi in octaves:
                m = (f >= lo) & (f < hi)
                med = np.median(s[m])
                row += f'{med:6.1f}/{np.percentile(s[m], 97) - med:4.1f}  '
            print(f'{t0 * 1000:5.0f}-{t1 * 1000:<5.0f} {tag} ' + row)
    print('\nStrongest partials (Hz (dB re the strongest)), 32768-point:')
    for t0, t1 in [(spans[1][0], spans[-1][1])] if len(spans) > 1 else spans:
        for tag, x in (('rec', a), ('ren', b)):
            f, p = window(x, t0, t1, 32768)
            s = db(p)
            peaks = [(s[i], f[i]) for i in range(6, len(s) - 6)
                     if f[i] >= 150 and s[i] == s[i - 6:i + 7].max()]
            peaks.sort(reverse=True)
            peaks = sorted(peaks[:14], key=lambda t: t[1])
            top = max(v for v, _ in peaks)
            print(f'{t0 * 1000:5.0f}-{t1 * 1000:<5.0f} {tag} ' +
                  ' '.join(f'{fr:.0f}({v - top:.1f})' for v, fr in peaks))
    print('\nCentroid (Hz) and share of the energy (dB) below 700 Hz, to 2k, 5k, 10k, and above:')
    for t0, t1 in spans:
        for tag, x in (('rec', a), ('ren', b)):
            f, p = window(x, t0, t1)
            m = f > 100
            total = p[m].sum()
            shares = [db(p[(f >= lo) & (f < hi)].sum() / total)
                      for lo, hi in ((100, 700), (700, 2000), (2000, 5000), (5000, 10000), (10000, 22050))]
            print(f'{t0 * 1000:5.0f}-{t1 * 1000:<5.0f} {tag} {(p[m] * f[m]).sum() / total:6.0f} |' +
                  ''.join(f'{v:6.1f}' for v in shares))


# --- the command line ------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('partials', help="a recording's partials, as JSON")
    p.add_argument('wav')
    p.add_argument('--clock', type=float, required=True, help="the DAC's clock it was played at")
    p.add_argument('--count', type=int, default=24)
    p.add_argument('--body', type=float, default=0.9, help='seconds of body to measure (short sounds: 0.2)')
    p = sub.add_parser('clock', help="the DAC's clock from where its images mirror its partials")
    p.add_argument('wav')
    p.add_argument('--guess', type=float, required=True)
    p.add_argument('--body', type=float, default=0.9, help='as for partials')
    p = sub.add_parser('spec', help='a draft spec on measured partials')
    p.add_argument('partials')
    p.add_argument('--clock', type=float, required=True)
    p.add_argument('--length', type=float, required=True, help="ms until the EPROM ends, at the clock")
    p.add_argument('--target', action='append', required=True,
                   help="a target line's words after 'target' (repeat for several)")
    p.add_argument('--modes', type=int, default=8)
    p.add_argument('--narrow', type=int, default=11)
    p.add_argument('--broad', type=int, default=4)
    p.add_argument('--apart', type=float, default=0.015, help='narrow bands at least this ratio apart')
    p.add_argument('--hold', action='store_true', help="the partials' shapes held: fit the gains first")
    p = sub.add_parser('compare', help='where a render differs from its recording')
    p.add_argument('recording')
    p.add_argument('render')
    p = sub.add_parser('listen', help="a spec's recordings and their renders, one after the other")
    p.add_argument('spec')
    p.add_argument('renders', help="the directory 'drumlin-fit render' wrote render0.wav ... into")
    p.add_argument('out')
    a = ap.parse_args()

    if a.cmd == 'partials':
        print(json.dumps(partials(read(a.wav), a.clock, a.count, a.body), indent=1))
    elif a.cmd == 'clock':
        x = read(a.wav)
        c, r, next_best = clock_from_mirror(x, a.guess)
        print(f'mirror: clock {c:.1f} (r {r:.2f}; the best well away from it r {next_best:.2f}: '
              'sure when the first is high and the second low)')
        pairs = clock_from_images(x, c, partials(x, c, body=a.body))
        for pc, fp, fi, prom in pairs:
            print(f'  partial {fp:8.1f}  image {fi:8.1f}  clock {pc:8.1f}  image stands {prom:5.1f} dB out')
        good = [pc for pc, _, _, prom in pairs if prom > 15]
        if good:
            print(f'images: clock {np.median(good):.1f} from {len(good)} clear pairs, '
                  f'spread {np.std(good):.1f}')
        else:
            print('images: no clear pairs (no partials with images that stand out)')
    elif a.cmd == 'spec':
        with open(a.partials) as f:
            parts = json.load(f)
        print('\n'.join(draft(parts, a.clock, a.length, a.target, a.modes, a.narrow, a.broad,
                              a.apart, a.hold)))
    elif a.cmd == 'compare':
        compare(read(a.recording), read(a.render))
    elif a.cmd == 'listen':
        with open(a.spec) as f:
            refs = [line.split(' clock=')[0][len('target '):] for line in f if line.startswith('target ')]
        gap = np.zeros(int(0.35 * RATE))
        parts = []
        for i, ref in enumerate(refs):
            x, y = read(ref), read(f'{a.renders}/render{i}.wav')
            # each from 30 ms before its hit, so the two land alike
            x = x[max(0, hit(x) - int(0.03 * RATE)):]
            y = y[max(0, hit(y) - int(0.03 * RATE)):]
            n = min(len(y), max(len(x), int(0.25 * RATE)) + int(0.05 * RATE))
            parts += [x, gap, y[:n], gap, gap]
        write_wav(a.out, np.concatenate(parts))


if __name__ == '__main__':
    main()
