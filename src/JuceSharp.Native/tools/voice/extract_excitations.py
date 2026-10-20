#!/usr/bin/env python3
"""Builds src/dsp/voice/dcvoice_excitation_data.h from CC0 DI recordings.

What it does, per recording (one plucked note):
  1. decode with ffmpeg (mono, 44.1 kHz float);
  2. find the onset and the TRUE fundamental (the file name is only a hint:
     an odd-harmonic test decides between it and an octave below);
  3. fit a string loop to the recording: inharmonicity B from the stretch of
     its partials, the loss filter from the decay of the fundamental and of a
     high partial, the dispersion allpass from B — the same formulas as
     dcvoice_string.h (designLoss, fitDispersion);
  4. inverse-filter: e[n] = y[n] - Loop(y)[n]. What is left is everything the
     string loop could NOT explain — the pick or finger, the pickup and the
     pick-position comb — and it dies away within tens of ms;
  5. trim (-40 dB envelope), fade, make its sum exactly zero (a DC component
     would be integrated by a 0.9999-gain loop), level-normalise per kind
     keeping the velocity layers' relation;
  6. band-limit and decimate per kind (BAND_DIV: 22.05 kHz for picks and the
     guitar's dead notes, which carry -51 / -40 dB above 10 kHz; 11.025 kHz
     for bass fingers and ghosts, -57 / -72 dB above 5 kHz), aligned so the
     sample addClip starts on is the same instant at either rate;
  7. quantise to 12 bits per clip (peak = 2047, the integer sum nudged to
     exactly zero) and write each sample as two base-64 digits in a string
     literal per clip (dcvoice_excitation.h, clipSample). The size budget is
     a test (guitar_voice_test, testBankIsShipped).

Unpitched sources (the guitar's dead-string "chk", the bass ghost notes) are
kept as trimmed raw attacks instead of inverse-filtered.

Sources (all CC0 1.0, verified 2026-10-04 at each repository's LICENSE file):
  --emily   github.com/sfzinstruments/karoryfer.emilyguitar        (guitar pick, dead)
  --growly  github.com/sfzinstruments/karoryfer.growlybass         (bass pick)
  --babb    a subset of github.com/sfzinstruments/karoryfer.black-and-blue-basses
            Samples/darkblack/{reg,ghost} laid out as <dir>/reg, <dir>/ghost
            (bass fingerstyle, ghost)

Usage:
  python extract_excitations.py --emily E --growly G --babb B --out dcvoice_excitation_data.h
Requires numpy and ffmpeg on PATH. Deterministic: same inputs, same header.
"""
import argparse
import math
import os
import re
import subprocess
import sys

import numpy as np

FS = 44100.0

KIND_PICK, KIND_FINGER, KIND_GHOST, KIND_DEAD = 0, 1, 2, 3
# Decimation factor per kind, from each kind's measured spectrum (see step 6).
BAND_DIV = {KIND_PICK: 2, KIND_FINGER: 4, KIND_GHOST: 4, KIND_DEAD: 2}
SAMPLE_BITS = 12
SAMPLE_MAX = (1 << (SAMPLE_BITS - 1)) - 1   # 2047
NOTE = {"c": 0, "db": 1, "d": 2, "eb": 3, "e": 4, "f": 5, "gb": 6, "g": 7, "ab": 8, "a": 9, "bb": 10, "b": 11}


def name_to_hz(name):
    m = re.match(r"([a-g]b?)(\d)", name)
    midi = 12 * (int(m.group(2)) + 1) + NOTE[m.group(1)]
    return 440.0 * 2.0 ** ((midi - 69) / 12.0)


def load(path):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(int(FS)), "-f", "f64le", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float64).copy()


def onset_of(x):
    peak = np.max(np.abs(x))
    idx = int(np.argmax(np.abs(x) > 0.02 * peak))
    return max(0, idx - int(0.001 * FS))


# ---- the same filter maths as dcvoice_string.h -----------------------------

def allpass_pd(a, w):
    return (1 - a) / (1 + a) if w < 1e-9 else 1 - 2 * math.atan2(a * math.sin(w), 1 + a * math.cos(w)) / w


class Loss:
    """g * (b0 + b1 z^-1) / (1 + a1 z^-1): the shelf loss of dcvoice_string.h."""

    def __init__(self, g=1.0, b0=1.0, b1=0.0, a1=0.0):
        self.g, self.b0, self.b1, self.a1 = g, b0, b1, a1


def loss_pd(c, w):
    if w < 1e-9:
        return c.b1 / (c.b0 + c.b1) - c.a1 / (1 + c.a1)
    num = math.atan2(-c.b1 * math.sin(w), c.b0 + c.b1 * math.cos(w))
    den = math.atan2(-c.a1 * math.sin(w), 1 + c.a1 * math.cos(w))
    return -(num - den) / w


def loss_shape(c, w):
    nr, ni = c.b0 + c.b1 * math.cos(w), -c.b1 * math.sin(w)
    dr, di = 1 + c.a1 * math.cos(w), -c.a1 * math.sin(w)
    return math.sqrt((nr * nr + ni * ni) / (dr * dr + di * di))


def design_loss(f0, fs, t60lo, t60hi, corner):
    t60lo = max(t60lo, 1e-3)
    t60hi = min(max(t60hi, 1e-3), t60lo)
    g0 = 10 ** (-3 / (f0 * t60lo))
    gh = 10 ** (-3 / (f0 * t60hi))
    k = min(max(gh / g0, 1e-4), 1.0)
    c = Loss()
    if k < 0.999999:
        sq = math.sqrt(k)
        top = 0.45 * fs
        fc = min(max(corner, 1.0), top)
        fz = fc / sq
        if fz > top:
            fz = top
            fc = fz * sq
        fp = fc * sq
        cc = 2 * fs
        wp = cc * math.tan(math.pi * fp / fs)
        wz = cc * math.tan(math.pi * fz / fs)
        kk = wp / wz
        c.b0 = kk * (cc + wz) / (cc + wp)
        c.b1 = kk * (wz - cc) / (cc + wp)
        c.a1 = (wp - cc) / (cc + wp)
    c.g = min(g0 / loss_shape(c, 2 * math.pi * f0 / fs), 0.999995)
    return c


def model_t60(c, f0, f, fs):
    """T60 the loop gives a component at f (per round trip of f0)."""
    gain = c.g * loss_shape(c, 2 * math.pi * f / fs)
    return -3.0 / (f0 * math.log10(max(min(gain, 0.9999999), 1e-9)))


def fit_profile(f0, t_lo, partial_t60s):
    """Grid fit of the shelf (corner, plateau) to measured partial decays."""
    best = (1e18, 400.0, 1.0)
    pts = [(f, t) for (f, t) in partial_t60s if t is not None and t < 29]
    if len(pts) < 3:
        return max(4 * f0, 400.0), min(1.0, t_lo)
    for corner in np.geomspace(max(1.2 * f0, 80.0), 6000.0, 36):
        for th in np.geomspace(0.15, min(8.0, t_lo), 36):
            c = design_loss(f0, FS, t_lo, th, corner)
            err = 0.0
            for f, t in pts:
                d = math.log(model_t60(c, f0, f, FS)) - math.log(t)
                err += d * d
            if err < best[0]:
                best = (err, float(corner), float(th))
    return best[1], best[2]


def fit_dispersion(f0, fs, B, stages, loss, maxdelay):
    """Mirror of dcvoice_string.h fitDispersion (first 12 partials, 1/n^2)."""
    if B <= 0 or stages <= 0:
        return 0.0
    period = fs / f0
    fn, tt = [], []
    for n in range(1, 13):
        f = n * f0 * math.sqrt((1 + B * n * n) / (1 + B))
        if f > 0.4 * fs:
            break
        fn.append(f)
        tt.append(n * fs / f)
    if len(fn) < 3:
        return 0.0

    def cost(a):
        w1 = 2 * math.pi * fn[0] / fs
        d = period - stages * allpass_pd(a, w1) - loss_pd(loss, w1)
        err = 0.0
        for i in range(1, len(fn)):
            w = 2 * math.pi * fn[i] / fs
            tau = d + stages * allpass_pd(a, w) + loss_pd(loss, w)
            rel = (tau - tt[i]) / tt[i]
            err += rel * rel / ((i + 1) * (i + 1))
        return err

    per = max(maxdelay / stages, 1.0)
    lo0 = max(-0.98, (1 - per) / (1 + per))
    if lo0 >= 0.0:
        return 0.0
    grid = 64
    best_k, best_c = grid, cost(0.0)
    for k in range(grid):
        c = cost(lo0 * (1 - k / grid))
        if c < best_c:
            best_c, best_k = c, k
    lo = lo0 * (1 - max(best_k - 1, 0) / grid)
    hi = min(0.0, lo0 * (1 - (best_k + 1) / grid))
    gr = 0.6180339887498949
    x1, x2 = hi - gr * (hi - lo), lo + gr * (hi - lo)
    c1, c2 = cost(x1), cost(x2)
    for _ in range(40):
        if c1 < c2:
            hi, x2, c2 = x2, x1, c1
            x1 = hi - gr * (hi - lo)
            c1 = cost(x1)
        else:
            lo, x1, c1 = x1, x2, c2
            x2 = lo + gr * (hi - lo)
            c2 = cost(x2)
    a = 0.5 * (lo + hi)
    return a if cost(a) < cost(0.0) else 0.0


# ---- analysis ----------------------------------------------------------------

NFFT = 1 << 21


class Spectrum:
    """Zero-padded magnitude spectrum of one windowed segment (0.02 Hz bins)."""

    def __init__(self, seg):
        self.mag = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), NFFT))
        self.df = FS / NFFT


def peak_near(sp, f, rel=0.02):
    lo = max(1, int(f * (1 - rel) / sp.df))
    hi = min(len(sp.mag) - 2, int(f * (1 + rel) / sp.df) + 1)
    k = lo + int(np.argmax(sp.mag[lo:hi + 1]))
    a, b, c = (math.log(sp.mag[k - 1] + 1e-30), math.log(sp.mag[k] + 1e-30), math.log(sp.mag[k + 1] + 1e-30))
    den = a - 2 * b + c
    off = 0.5 * (a - c) / den if den < 0 else 0.0
    return (k + off) * sp.df, sp.mag[k]


def true_f0(sp, nominal):
    """The name is a hint; the octave below wins if its odd harmonics are there."""
    cand = nominal
    for _ in range(2):
        half = cand / 2
        if half < 25:
            break
        odd = sum(peak_near(sp, h * half, 0.015)[1] for h in (1, 3, 5, 7))
        even = sum(peak_near(sp, h * half, 0.015)[1] for h in (2, 4, 6, 8))
        if odd > 0.2 * even:
            cand = half
        else:
            break
    f1, m1 = peak_near(sp, cand, 0.03)
    f2, m2 = peak_near(sp, 2 * f1, 0.01)
    if m1 < 0.05 * m2:   # a buried fundamental: trust the octave
        f1 = f2 / 2
    return f1


def partials(sp, f0, nmax, fmax):
    out = []
    B = 0.0
    for n in range(1, nmax + 1):
        fe = n * f0 * math.sqrt((1 + B * n * n) / (1 + B))
        if fe > fmax:
            break
        f, m = peak_near(sp, fe, min(0.01, 0.4 / n))
        out.append((n, f, m))
        if n >= 4:
            ests = []
            for (k, fk, mk) in out[2:]:
                r = fk / (k * out[0][1])
                if k * k - r * r > 0:
                    ests.append((r * r - 1) / (k * k - r * r))
            if ests:
                B = min(max(float(np.median(ests)), 0.0), 6e-4)
    return out, B


def partial_t60(x, start, f, maxdur):
    frame = 4096
    hop = 1024
    win = np.hanning(frame)
    n = np.arange(frame)
    ph = np.exp(-2j * np.pi * f * n / FS)
    ts, db = [], []
    pos = start
    end = min(len(x) - frame, start + int(maxdur * FS))
    while pos < end:
        m = abs(np.dot(x[pos:pos + frame] * win, ph))
        ts.append(pos / FS)
        db.append(20 * math.log10(m + 1e-12))
        pos += hop
    if len(ts) < 6:
        return None
    db = np.array(db)
    ts = np.array(ts)
    keep = db > db.max() - 50          # above the noise floor
    if keep.sum() < 6:
        return None
    slope = np.polyfit(ts[keep], db[keep], 1)[0]
    if slope >= -0.5:
        return 30.0
    return min(max(-60.0 / slope, 0.04), 30.0)


def lagrange_delay(y, D):
    di = int(D)
    d = 1 + (D - di)
    h = [-(d - 1) * (d - 2) * (d - 3) / 6, d * (d - 2) * (d - 3) / 2, -d * (d - 1) * (d - 3) / 2, d * (d - 1) * (d - 2) / 6]
    out = np.zeros_like(y)
    for k in range(4):
        sh = di - 1 + k
        out[sh:] += h[k] * y[:len(y) - sh]
    return out


def first_order_allpass(x, a):
    y = np.empty_like(x)
    s = 0.0
    for i in range(len(x)):
        v = a * x[i] + s
        s = x[i] - a * v
        y[i] = v
    return y


def shelf_loss(x, c):
    y = np.empty_like(x)
    s = 0.0
    x1 = 0.0
    for i in range(len(x)):
        s = c.g * (c.b0 * x[i] + c.b1 * x1) - c.a1 * s
        x1 = x[i]
        y[i] = s
    return y


def loop_filter(y, f0, a, stages, loss):
    w = 2 * math.pi * f0 / FS
    D = max(2.0, FS / f0 - stages * allpass_pd(a, w) - loss_pd(loss, w))
    v = lagrange_delay(y, D)
    for _ in range(stages):
        v = first_order_allpass(v, a)
    return shelf_loss(v, loss)


def trim_and_fade(e, min_ms, max_ms, floor_db=-40.0):
    n_max = min(len(e), int(max_ms * 1e-3 * FS))
    e = e[:n_max].copy()
    hop = int(0.002 * FS)
    env = np.array([np.sqrt(np.mean(e[i:i + hop] ** 2)) for i in range(0, len(e) - hop, hop)])
    peak = env.max() + 1e-12
    thr = peak * 10 ** (floor_db / 20)
    last = len(env) - 1
    while last > 0 and env[last] < thr:
        last -= 1
    n = int(min(max((last + 2) * hop, min_ms * 1e-3 * FS), n_max))
    e = e[:n]
    fade = max(int(0.3 * n), 8)
    e[n - fade:] *= 0.5 + 0.5 * np.cos(np.pi * np.arange(fade) / fade)
    rise = int(0.0005 * FS)
    e[:rise] *= 0.5 - 0.5 * np.cos(np.pi * np.arange(rise) / rise)
    w = np.hanning(n + 2)[1:-1]
    e -= np.sum(e) * w / np.sum(w)   # sum exactly zero
    return e


class Clip:
    def __init__(self, data, f0, kind, layer, rr, source, level, B=0.0, t_lo=5.0, t_hi=1.0, corner=400.0):
        self.data, self.f0, self.kind, self.layer, self.rr, self.source, self.level = data, f0, kind, layer, rr, source, level
        self.B, self.t_lo, self.t_hi, self.corner = B, t_lo, t_hi, corner
        self.rate = FS


def extract_pitched(path, nominal, kind, layer, rr, source, stages, high_hz, min_ms, max_ms, log):
    x = load(path)
    on = onset_of(x)
    seg_start = on + int(0.12 * FS)
    seg = x[seg_start:seg_start + int(0.8 * FS)]
    sp = Spectrum(seg)
    f0 = true_f0(sp, nominal)
    parts, B = partials(sp, f0, 24, min(6000.0, 0.4 * FS))
    t_lo = partial_t60(x, on + int(0.1 * FS), parts[0][1], 2.5) or 5.0
    strong = [q for q in parts[1:16] if q[2] > 0.003 * parts[0][2]]
    measured = [(q[1], partial_t60(x, on + int(0.05 * FS), q[1], 1.2)) for q in strong]
    corner, t_hi = fit_profile(f0, t_lo, measured)
    loss = design_loss(f0, FS, t_lo, t_hi, corner)
    a = fit_dispersion(f0, FS, B, stages, loss, 0.45 * FS / f0 - 2)
    y = x[on:on + int(0.6 * FS)]
    e = y - loop_filter(y, f0, a, stages, loss)
    full = np.sqrt(np.mean(e[: int(0.02 * FS)] ** 2)) + 1e-12
    tail = np.sqrt(np.mean(e[int(0.2 * FS): int(0.4 * FS)] ** 2)) + 1e-12
    e = trim_and_fade(e, min_ms, max_ms)
    log.append(f"{os.path.basename(path)}: f0 {f0:.2f} Hz (hint {nominal:.1f}) B {B:.2e} T60 {t_lo:.2f}s, "
               f"plateau {t_hi:.2f}s above {corner:.0f}Hz, a {a:+.3f}, residual tail {20*math.log10(tail/full):+.1f} dB, "
               f"{len(e)} samples")
    return Clip(e, f0, kind, layer, rr, source, float(np.max(np.abs(y))), B, t_lo, t_hi, corner)


def extract_raw(path, f0, kind, layer, rr, source, min_ms, max_ms, log):
    x = load(path)
    on = onset_of(x)
    y = x[on:on + int(max_ms * 1e-3 * FS)].copy()
    e = trim_and_fade(y, min_ms, max_ms, -36.0)
    log.append(f"{os.path.basename(path)}: raw attack, tagged {f0:.2f} Hz, {len(e)} samples")
    return Clip(e, f0, kind, layer, rr, source, float(np.max(np.abs(y))))


def normalise(clips, target=0.5):
    """Per kind: the loudest layer's median source peak maps to `target`, so the
    velocity layers keep their recorded relation."""
    for kind in sorted(set(c.kind for c in clips)):
        ks = [c for c in clips if c.kind == kind]
        top = max(c.layer for c in ks)
        ref = float(np.median([c.level for c in ks if c.layer == top]))
        for c in ks:
            gain = target / ref
            c.data = c.data * gain


def band_limit(clips, taps=161):
    """Kaiser-windowed sinc low-pass at 0.9 x the new Nyquist, then decimate.
    addClip reads from index 1 on, so output k sits on input q*k - (q-1):
    index 1 is the same instant at either rate. The sum is re-zeroed."""
    for c in clips:
        q = BAND_DIV[c.kind]
        if q == 1:
            continue
        fc = 0.45 / q
        n = np.arange(taps) - (taps - 1) / 2
        h = 2 * fc * np.sinc(2 * fc * n) * np.kaiser(taps, 8.0)
        y = np.convolve(c.data, h / h.sum(), mode="same")
        y = np.concatenate([np.zeros(q - 1), y])[::q]
        w = np.hanning(len(y) + 2)[1:-1]
        c.data = y - np.sum(y) * w / np.sum(w)
        c.rate = FS / q


def quantise(data):
    """12-bit integers (peak = SAMPLE_MAX) whose sum is EXACTLY zero, and the scale."""
    scale = (float(np.max(np.abs(data))) + 1e-12) / SAMPLE_MAX
    q = np.clip(np.round(data / scale), -SAMPLE_MAX, SAMPLE_MAX).astype(np.int64)
    s = int(q.sum())
    # Rounding leaves a sum of a few LSB: nudge that many of the quietest
    # samples by one LSB each (never past the range).
    i = 0
    order = np.argsort(np.abs(q), kind="stable")
    while s != 0:
        k = order[i % len(q)]
        step = -1 if s > 0 else 1
        if abs(q[k] + step) <= SAMPLE_MAX:
            q[k] += step
            s += step
        i += 1
    return q, scale


def digit(v):
    """Base-64 digit: '0' + v, skipping '?' and '\' (no escapes, no trigraphs)."""
    return chr(48 + v + (v >= 15) + (v >= 43))


def encode(q):
    u = q + (SAMPLE_MAX + 1)
    return "".join(digit(int(v) >> 6) + digit(int(v) & 63) for v in u)


def emit(path, banks, sources, log):
    lines = []
    w = lines.append
    w("// GENERATED by tools/voice/extract_excitations.py — do not edit by hand.")
    w("//")
    w("// Pick / finger / ghost / dead-string excitation residuals for the chart")
    w("// voice, inverse-filtered from CC0 1.0 recordings by Karoryfer Samples")
    w("// (see voice/CREDITS.md). Each clip is band-limited to its own rate, 12-bit,")
    w("// two base-64 digits per sample in a string literal; clipSample() decodes")
    w("// sample i and `clipSample * scale` is the float excitation.")
    w("//")
    for i, s in enumerate(sources):
        w(f"// source {i}: {s}")
    w("#pragma once")
    w("")
    w('#include "dcvoice_excitation.h"')
    w("")
    w("namespace dcvoice {")
    w("namespace excdata {")
    per = 120   # digits per source line (60 samples)
    for name, clips, layers in banks:
        total = sum(len(c.data) for c in clips)
        w("")
        w(f"// {name}: {len(clips)} clips, {total} samples ({total * 2 / 1024:.0f} KiB of digits)")
        w(f"inline constexpr ExcitationClip k{name}Clips[] = {{")
        for c in clips:
            q, c.scale = quantise(c.data)
            text = encode(q)
            parts = [text[i:i + per] for i in range(0, len(text), per)]
            w("    { \"" + parts[0] + "\"")
            for p in parts[1:]:
                w("      \"" + p + "\"")
            w(f"      , {len(q)}, {c.rate:.1f}f, {c.f0:.4f}f, {c.scale:.9e}f, "
              f"{c.kind}, {c.layer}, {c.rr}, {c.source}, {c.B:.4e}f, {c.t_lo:.4f}f, {c.t_hi:.4f}f, {c.corner:.2f}f }},")
        w("};")
        w(f"inline constexpr int k{name}Layers = {layers};")
    w("")
    w("} // namespace excdata")
    w("")
    for name, clips, layers in banks:
        fn = "guitarExcitations" if name == "Guitar" else "bassExcitations"
        w(f"inline ExcitationBank {fn}()")
        w("{")
        w(f"    return {{ excdata::k{name}Clips, (int) (sizeof(excdata::k{name}Clips) / sizeof(excdata::k{name}Clips[0])), "
          f"excdata::k{name}Layers }};")
        w("}")
        w("")
    w("} // namespace dcvoice")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--emily", required=True)
    ap.add_argument("--growly", required=True)
    ap.add_argument("--babb", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--log")
    args = ap.parse_args()
    log = []
    sources = [
        "Karoryfer Samples, Emilyguitar (CC0 1.0) github.com/sfzinstruments/karoryfer.emilyguitar",
        "Karoryfer Samples, Growlybass (CC0 1.0) github.com/sfzinstruments/karoryfer.growlybass",
        "Karoryfer Samples, Black And Blue Basses / darkblack (CC0 1.0) "
        "github.com/sfzinstruments/karoryfer.black-and-blue-basses",
    ]

    guitar = []
    emily_pitches = ["e2", "gb2", "a2", "c3", "eb3", "gb3", "a3", "c4", "eb4", "gb4", "a4", "c5", "eb5", "gb5", "a5", "c6", "d6"]
    for p in emily_pitches:
        for layer, vel in enumerate(["p", "mf", "f"]):
            for rr in (1, 2):
                path = os.path.join(args.emily, "notes", f"{p}_{vel}_rr{rr}.wav")
                if os.path.exists(path):
                    guitar.append(extract_pitched(path, name_to_hz(p), KIND_PICK, layer, rr - 1, 0, 2, 4000.0, 12, 45, log))
    for k in range(1, 6):
        for rr in (1, 2, 3):
            path = os.path.join(args.emily, "noises", f"muted{k}_rr{rr}.wav")
            if os.path.exists(path):
                guitar.append(extract_raw(path, 150.0, KIND_DEAD, 0, (k - 1) * 3 + rr - 1, 0, 20, 70, log))

    bass = []
    growly_pitches = ["e2", "gb2", "a2", "c3", "eb3", "gb3", "a3", "c4", "eb4", "gb4", "a4", "c5", "eb5"]
    for p in growly_pitches:
        for layer, vel in enumerate(["p", "f", "ff"]):
            path = os.path.join(args.growly, "sustain", f"{p}_{vel}_rr1.wav")
            if os.path.exists(path):
                bass.append(extract_pitched(path, name_to_hz(p), KIND_PICK, layer, 0, 1, 4, 2500.0, 20, 80, log))
    babb_pitches = ["b1", "d2", "f2", "ab2", "b2", "d3", "f3", "ab3", "b3", "d4", "f4", "ab4", "b4", "d5"]
    for p in babb_pitches:
        measured = None
        for layer, vel in enumerate(["p", "mf", "f"]):
            for rr in (1, 2):
                path = os.path.join(args.babb, "reg", f"darkblack_{p}_{vel}_rr{rr}.wav")
                if os.path.exists(path):
                    c = extract_pitched(path, name_to_hz(p), KIND_FINGER, layer, rr - 1, 2, 4, 2500.0, 20, 80, log)
                    bass.append(c)
                    measured = measured or c.f0
        # A ghost note is barely pitched; it carries the fingerstyle note's measured pitch.
        for rr in (1, 2):
            path = os.path.join(args.babb, "ghost", f"darkblack_{p}_ghost_f_rr{rr}.wav")
            if os.path.exists(path) and measured:
                bass.append(extract_raw(path, measured, KIND_GHOST, 0, rr - 1, 2, 25, 70, log))

    normalise(guitar)
    normalise(bass)
    band_limit(guitar)
    band_limit(bass)
    emit(args.out, [("Guitar", guitar, 3), ("Bass", bass, 3)], sources, log)
    if args.log:
        with open(args.log, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(log) + "\n")
    print(f"guitar clips {len(guitar)}, bass clips {len(bass)}; wrote {args.out}")


if __name__ == "__main__":
    sys.exit(main())
