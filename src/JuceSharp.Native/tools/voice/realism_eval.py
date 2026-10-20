#!/usr/bin/env python3
"""Objective realism evaluation of the chart voice (DotCharter #52).

Renders the SAME notes the held-out real recordings play, through the voice the
engine runs (juce_sharp_voice_render --events: dcsynth::GuitarChartVoice with
the default instrument model), measures both with the same feature code, and
reports, per articulation, how far the render is from the real thing.

HELD-OUT REFERENCES (never sampled into a pack; local evaluation only):
  * Guitar-TECHS, player 1 (CC BY 4.0, Zenodo 14963133): an Ibanez Les Paul
    type, bridge humbucker, DI. Single notes, palm mutes, natural and pinch
    harmonics, vibrato. Its own single notes normalise every RELATIVE metric
    (a palm mute is measured against the same guitar's open note on the same
    string and fret), so the guitar's own colour cancels out.
  * Karoryfer Emilyguitar (CC0): muted strings (dead notes) and round robins.
  * Karoryfer Swagbass / Fashionbass (CC0): bass notes and muted plucks.
  * Freesound CC0 slap-bass phrases (Charvel, P bass, Aria Pro II), decoded
    from the HQ previews with ffmpeg (eval only).

  python realism_eval.py refs    --refs <dir> --out <dir>
  python realism_eval.py render  --exe <voice_render.exe> --packs <dir> --out <dir> --tag before
  python realism_eval.py measure --out <dir> --tag before
  python realism_eval.py report  --out <dir> --tags before,after

Needs numpy + scipy (ffmpeg only for the freesound previews).
"""
import argparse
import glob
import json
import math
import os
import subprocess
import sys

import numpy as np
from scipy import signal
from scipy.io import wavfile

FS = 48000
OPEN_G = [40, 45, 50, 55, 59, 64]
OPEN_B = [28, 33, 38, 43]


def hz(m):
    return 440.0 * 2.0 ** ((m - 69.0) / 12.0)


# ---- audio io ---------------------------------------------------------------

def load(path, channel=0):
    import warnings
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        fs, x = wavfile.read(path)
    if x.ndim > 1:
        x = x[:, channel]
    if x.dtype == np.int16:
        x = x.astype(np.float32) / 32768.0
    elif x.dtype == np.int32:
        x = x.astype(np.float32) / 2147483648.0
    else:
        x = x.astype(np.float32)
    if fs != FS:
        g = math.gcd(int(fs), FS)
        x = signal.resample_poly(x, FS // g, int(fs) // g).astype(np.float32)
    return x


def env_db(x, hop=240):
    n = len(x) // hop
    if n == 0:
        return np.array([-120.0])
    e = np.sqrt(np.mean(x[: n * hop].reshape(n, hop) ** 2, axis=1) + 1e-20)
    return 20 * np.log10(e + 1e-12)


def find_onset(x, start, stop, rel_db=-24.0):
    """The attack inside [start, stop): the 1 ms frame with the steepest 10 ms
    rise of the high-passed (> 150 Hz) level among frames that reach within
    `rel_db` of the window's loudest, walked back to where the rise began. A
    previous note still ringing into the window does not fool it."""
    lo = max(0, start - 4800)
    seg = x[lo:stop + 4800]
    if len(seg) < 4800:
        return start
    b, a = signal.butter(2, 150.0 / (FS / 2), "high")
    e = env_db(signal.lfilter(b, a, seg), 48)
    i0 = (start - lo) // 48
    i1 = min(len(e) - 10, (stop - lo) // 48)
    if i1 <= i0:
        return start
    pk = e[i0:i1 + 10].max()
    rise = np.full(len(e), -1e9)
    for i in range(i0, i1):
        if e[i + 10] > pk + rel_db:
            rise[i] = e[i + 10] - e[i]
    i = int(np.argmax(rise))
    if rise[i] < 6.0:
        i = i0 + int(np.argmax(e[i0:i1] > pk + rel_db))
    j = i
    base = e[i]
    while j > i0 and e[j - 1] <= e[j] + 0.5 and e[j - 1] > base - 3.0 and i - j < 20:
        j -= 1
    while j < i + 10 and e[j + 1] - e[j] < 1.0:
        j += 1
    return lo + j * 48


# ---- features ---------------------------------------------------------------

def centroid(x):
    if len(x) < 64:
        return 0.0
    w = np.hanning(len(x))
    n = 1 << int(math.ceil(math.log2(max(len(x), 4096))))
    s = np.abs(np.fft.rfft(x * w, n)) ** 2
    f = np.fft.rfftfreq(n, 1.0 / FS)
    m = (f > 40) & (f < 12000)
    return float(np.sum(s[m] * f[m]) / (np.sum(s[m]) + 1e-30))


def band_ratio_db(x, lo=2000.0):
    if len(x) < 64:
        return -60.0
    n = 1 << int(math.ceil(math.log2(max(len(x), 4096))))
    s = np.abs(np.fft.rfft(x * np.hanning(len(x)), n)) ** 2
    f = np.fft.rfftfreq(n, 1.0 / FS)
    tot = np.sum(s[(f > 40) & (f < 12000)]) + 1e-30
    return float(10 * np.log10(np.sum(s[(f >= lo) & (f < 12000)]) / tot + 1e-12))


def partials(x, f0, count=12):
    """Partial amplitudes (dB re the strongest) of a 100-500 ms window."""
    seg = x[int(0.1 * FS): int(0.5 * FS)]
    if len(seg) < 4096:
        seg = np.pad(seg, (0, 4096 - len(seg)))
    n = 1 << 16
    s = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), n))
    f = np.fft.rfftfreq(n, 1.0 / FS)
    out = []
    for j in range(1, count + 1):
        c = j * f0
        if c > 15000:
            out.append(-120.0)
            continue
        m = (f > c * 0.975) & (f < c * 1.03)
        out.append(20 * np.log10(s[m].max() + 1e-12) if np.any(m) else -120.0)
    out = np.array(out)
    return out - out.max()


def f0_track(x, fmin, fmax, hop=240, win=2048):
    """Normalised-autocorrelation f0 (YIN-like) every `hop` samples. NaN when unvoiced."""
    out = []
    lag_min = int(FS / fmax)
    lag_max = int(FS / fmin)
    for i in range(0, len(x) - win - lag_max, hop):
        a = x[i: i + win + lag_max].astype(np.float64)
        if np.sqrt(np.mean(a ** 2)) < 1e-4:
            out.append(np.nan)
            continue
        w = a[:win]
        d = np.array([np.sum((w - a[l: l + win]) ** 2) for l in range(lag_min, lag_max)])
        cum = np.cumsum(d) / (np.arange(1, len(d) + 1))
        dn = d / (cum + 1e-20)
        k = int(np.argmin(dn))
        thr = np.where(dn < 0.15)[0]
        if len(thr):
            k = int(thr[0])
            while k + 1 < len(dn) and dn[k + 1] < dn[k]:
                k += 1
        if dn[k] > 0.35:
            out.append(np.nan)
            continue
        if 0 < k < len(dn) - 1:
            p = 0.5 * (dn[k - 1] - dn[k + 1]) / (dn[k - 1] - 2 * dn[k] + dn[k + 1] + 1e-20)
        else:
            p = 0.0
        out.append(FS / (lag_min + k + p))
    return np.array(out)


def note_features(x, f0):
    """x starts at the onset (5 ms before it). Shape metrics, level-independent."""
    e = env_db(x, 240)                       # 5 ms frames
    ip = int(np.argmax(e[:80]))              # the peak inside 400 ms
    pk = e[ip]
    after = e[ip:]
    def t_below(db):
        idx = np.where(after < pk - db)[0]
        return float(idx[0] * 5.0) if len(idx) else float(len(after) * 5.0)
    t20 = t_below(20.0)
    t40 = t_below(40.0)
    a = ip + 20
    b = min(len(e), ip + max(40, int(min(t40, 1500.0) / 5.0)))
    if b - a >= 8:
        slope = float(np.polyfit(np.arange(a, b) * 0.005, e[a:b], 1)[0])
    else:
        slope = float((e[min(len(e) - 1, ip + 40)] - pk) / 0.2)
    c0 = centroid(x[0: int(0.03 * FS)])
    c1 = centroid(x[int(0.03 * FS): int(0.12 * FS)])
    c2 = centroid(x[int(0.12 * FS): int(0.4 * FS)])
    c3 = centroid(x[int(0.4 * FS): int(1.2 * FS)])
    hf0 = band_ratio_db(x[0: int(0.03 * FS)])
    # transient vs body: peak of the first 15 ms vs RMS of 50-150 ms
    tr = 20 * np.log10(np.max(np.abs(x[: int(0.02 * FS)])) + 1e-9) - 20 * np.log10(
        np.sqrt(np.mean(x[int(0.05 * FS): int(0.15 * FS)] ** 2)) + 1e-9)
    p = partials(x, f0) if f0 > 0 else np.zeros(12)
    return {
        "peak_db": float(pk), "t_peak_ms": float(ip * 5.0), "t20_ms": t20, "t40_ms": t40,
        "decay_db_s": slope, "c_0_30": c0, "c_30_120": c1, "c_120_400": c2, "c_400_1200": c3,
        "hf_early_db": hf0, "transient_db": float(tr), "partials": p.tolist(),
    }


def oct_ratio(a, b):
    return float(math.log2(max(a, 1.0) / max(b, 1.0)))


# ---- references ---------------------------------------------------------------

def gt_dir(refs, player, part):
    return os.path.join(refs, "guitar-techs", f"{player}_{part}")


def gt_grid(refs, player, part, name, string_fret):
    """Guitar-TECHS 4-second grid: slot i plays string_fret(i). Returns
    [(string, fret, excerpt)] with excerpts starting 5 ms before the onset."""
    x = load(os.path.join(gt_dir(refs, player, part), "audio", "directinput", f"directinput_{name}.wav"))
    out = []
    slots = len(x) // (4 * FS)
    for i in range(slots):
        sf = string_fret(i)
        if sf is None:
            continue
        s0 = i * 4 * FS
        on = find_onset(x, max(0, s0 - int(0.05 * FS)), s0 + int(0.6 * FS))
        seg = x[max(0, on - 240): on - 240 + int(3.6 * FS)]
        if len(seg) < int(3.0 * FS) or np.max(np.abs(seg)) < 1e-3:
            continue
        out.append((sf[0], sf[1], seg))
    return out


def chromatic(i, frets=23):
    s = i // frets
    return (s, i % frets) if s < 6 else None


def estimate_f0(seg, lo=60.0, hi=1400.0):
    t = f0_track(seg[int(0.15 * FS): int(0.9 * FS)], lo, hi, hop=960)
    t = t[np.isfinite(t)]
    return float(np.median(t)) if len(t) else 0.0


def gt_labels(refs, player, part, name):
    """slot -> (string, fret) from the dataset's per-string MIDI (the first
    note in each 4 s slot)."""
    import midiread  # noqa: E402  (local helper next to this file)
    notes, _ = midiread.read(os.path.join(gt_dir(refs, player, part), "midi", f"midi_{name}.mid"))
    bys = {}
    for tr, t0, t1, p, v in notes:
        if v > 5 and 1 <= tr <= 6:
            bys.setdefault(int((t0 + 0.25) // 4), []).append((t0, tr, p))
    out = {}
    for i, ev in bys.items():
        t0, tr, p = min(ev)
        s = 6 - tr
        f = p - OPEN_G[s]
        if 0 <= f <= 22:
            out[i] = (s, f)
    return out


# The held-out player: Guitar-TECHS player 1's techniques are PACKED
# (guitar-arts.dcvp), player 2's never are. Every relative metric compares a
# player-2 articulation with player 2's own ordinary note on the same string.
REF_PLAYER = "P2"


def refs_cmd(a):
    refs, out = a.refs, a.out
    os.makedirs(out, exist_ok=True)
    R = {}
    P = REF_PLAYER
    keep = lambda s, f: f in (0, 1, 3, 5, 7, 10, 12)
    sus = {(s, f): seg for s, f, seg in gt_grid(refs, P, "singlenotes", "allsinglenotes", chromatic) if keep(s, f)}
    # The ordinary note's reference: player 1's single notes (a bridge
    # humbucker like the default model's; player 2's neck pickup is 10-30 dB
    # darker above 1 kHz). Player 1's single notes are never packed.
    sus1 = {(s, f): seg for s, f, seg in gt_grid(refs, "P1", "singlenotes", "allsinglenotes", chromatic) if keep(s, f)}
    R["g_sustain"] = [dict(string=s, fret=f, **note_features(seg, hz(OPEN_G[s] + f))) for (s, f), seg in sus1.items()]
    # LEVEL BALANCE (an articulation against the same player's open note) needs
    # one recording session: player 2's single notes were recorded at another
    # input gain than its techniques, so the balance comes from player 1's own
    # sessions — a calibration of the mix, not of the timbre.
    R["level"] = {}
    for name, key in (("PalmMute", "g_palm"), ("PinchHarmonics", "g_pinch")):
        lab = gt_labels(refs, "P1", "techniques", name)
        vals = []
        for s, f, seg in gt_grid(refs, "P1", "techniques", name, lab.get):
            base = sus1.get((s, f))
            if base is None:
                base = next((sus1[k] for k in sorted(sus1, key=lambda k: abs(k[1] - f)) if k[0] == s), None)
            if base is not None:
                vals.append(float(env_db(seg[: int(0.4 * FS)]).max() - env_db(base[: int(0.4 * FS)]).max()))
        R["level"][key] = vals

    def base_for(s, f):
        if (s, f) in sus:
            return sus[(s, f)]
        cand = [k for k in sus if k[0] == s]
        return sus[min(cand, key=lambda k: abs(k[1] - f))] if cand else None

    palm = []
    lab = gt_labels(refs, P, "techniques", "PalmMute")
    for s, f, seg in gt_grid(refs, P, "techniques", "PalmMute", lab.get):
        if keep(s, f) and (s, f) in sus:
            palm.append(dict(string=s, fret=f, **note_features(seg, hz(OPEN_G[s] + f)),
                             base=note_features(sus[(s, f)], hz(OPEN_G[s] + f))))
    R["g_palm"] = palm
    pinch = []
    lab = gt_labels(refs, P, "techniques", "PinchHarmonics")
    for s, f, seg in gt_grid(refs, P, "techniques", "PinchHarmonics", lab.get):
        if f % 3 != 1:
            continue
        base = base_for(s, f)
        if base is None:
            continue
        pinch.append(dict(string=s, fret=f, **note_features(seg, hz(OPEN_G[s] + f)),
                          base=note_features(base, hz(OPEN_G[s] + f))))
    R["g_pinch"] = pinch
    # Natural harmonics: a few slots per string, low E first; k from the pitch.
    xh = load(os.path.join(gt_dir(refs, P, "techniques"), "audio", "directinput", "directinput_Harmonics.wav"))
    per = {"P1": 4, "P2": 5}[P]
    harm = []
    for i in range(len(xh) // (4 * FS)):
        s = i // per
        if s > 5:
            continue
        s0 = i * 4 * FS
        on = find_onset(xh, max(0, s0 - int(0.05 * FS)), s0 + int(2.0 * FS))
        seg = xh[max(0, on - 240): on - 240 + int(3.6 * FS)]
        if len(seg) < int(3.0 * FS) or np.max(np.abs(seg)) < 1e-3:
            continue
        f = estimate_f0(seg, 150.0, 2600.0)
        if f <= 0:
            continue
        semis = 12 * math.log2(f / hz(OPEN_G[s]))
        k = {12: 2, 19: 3, 24: 4, 28: 5, 31: 6}.get(int(round(semis)))
        if k is None:
            continue
        node = {2: 12, 3: 7, 4: 5, 5: 4, 6: 3}[k]
        base = sus.get((s, 0))
        harm.append(dict(string=s, fret=node, k=k, **harmonic_features(seg, hz(OPEN_G[s]), k),
                         base=note_features(base, hz(OPEN_G[s])) if base is not None else None))
    R["g_harmonic"] = harm
    x1 = load(os.path.join(gt_dir(refs, "P1", "techniques"), "audio", "directinput", "directinput_Harmonics.wav"))
    vals = []
    for i in range(len(x1) // (4 * FS)):
        s = i // 4
        if s > 5 or (s, 0) not in sus1:
            continue
        on = find_onset(x1, max(0, i * 4 * FS - int(0.05 * FS)), i * 4 * FS + int(2.0 * FS))
        seg = x1[max(0, on - 240): on + int(0.4 * FS)]
        if np.max(np.abs(seg)) > 1e-3:
            vals.append(float(env_db(seg).max() - env_db(sus1[(s, 0)][: int(0.4 * FS)]).max()))
    R["level"]["g_harmonic"] = vals
    # Vibrato: never packed from either player, so both are references.
    vib = []
    for pl in ("P1", "P2"):
        lab = gt_labels(refs, pl, "techniques", "Vibrato")
        for s, f, seg in gt_grid(refs, pl, "techniques", "Vibrato", lab.get):
            if f not in (3, 5, 7, 10, 12) or s < 2:
                continue
            v = vibrato_features(seg, hz(OPEN_G[s] + f))
            if v:
                vib.append(dict(player=pl, string=s, fret=f, **v))
    R["g_vibrato"] = vib
    # CROSS-CHECK (not held out): player 1's palm mutes and pinch harmonics
    # against player 1's own open notes. Player 1 IS the packed source, so this
    # is how close the conversion keeps the recordings' own relation to an open
    # note; player 2 above is the independent reference.
    for name, key in (("PalmMute", "g_palm_p1x"), ("PinchHarmonics", "g_pinch_p1x")):
        lab = gt_labels(refs, "P1", "techniques", name)
        ev = []
        for s, f, seg in gt_grid(refs, "P1", "techniques", name, lab.get):
            base = sus1.get((s, f))
            if base is None or (key == "g_palm_p1x" and not keep(s, f)):
                continue
            ev.append(dict(string=s, fret=f, **note_features(seg, hz(OPEN_G[s] + f)), base=note_features(base, hz(OPEN_G[s] + f))))
        R[key] = ev
    # Emilyguitar (CC0): dead notes (muted1..5) and round robins.
    emily = os.path.join(refs, "karoryfer", "emilyguitar")
    dead = []
    for p in sorted(glob.glob(os.path.join(emily, "noises", "muted*_rr*.wav"))):
        x = load(p)
        on = find_onset(x, 0, min(len(x), FS))
        seg = np.pad(x[max(0, on - 240):], (0, int(1.2 * FS)))[: int(1.2 * FS)]
        dead.append(dict(file=os.path.basename(p), **dead_features(seg)))
    R["g_dead"] = dead
    notes_pk = [float(env_db(load(p)).max()) for p in sorted(glob.glob(os.path.join(emily, "notes", "*_mf_rr1.wav")))]
    R["level"]["g_dead"] = [float(env_db(load(p)).max()) - float(np.median(notes_pk))
                            for p in sorted(glob.glob(os.path.join(emily, "noises", "muted*_rr*.wav")))]
    rr = []
    for p in sorted(glob.glob(os.path.join(emily, "notes", "*_mf_rr1.wav"))):
        group = []
        for k in (1, 2, 3):
            q = p.replace("_rr1", f"_rr{k}")
            if os.path.exists(q):
                x = load(q)
                on = find_onset(x, 0, min(len(x), FS))
                seg = np.pad(x[max(0, on - 240):], (0, int(1.0 * FS)))[: int(1.0 * FS)]
                group.append((20 * np.log10(np.max(np.abs(seg)) + 1e-9), centroid(seg[: int(0.12 * FS)])))
        if len(group) >= 3:
            g = np.array(group)
            rr.append(dict(file=os.path.basename(p), level_sd_db=float(np.std(g[:, 0])),
                           centroid_sd_oct=float(np.std(np.log2(g[:, 1])))))
    R["g_repeat"] = rr
    # Bass: Swagbass / Fashionbass notes (sustain) and muted plucks (dead / ghost).
    bsus = []
    for lib in ("swagbass", "fashionbass"):
        for p in sorted(glob.glob(os.path.join(refs, "karoryfer", lib, "notes", "*.wav"))):
            name = os.path.basename(p).split("_")[0]
            m = note_midi(name)
            if m is None or m < 26 or m > 50 or ("_f" not in os.path.basename(p) and "_mf" not in os.path.basename(p)):
                continue
            x = load(p)
            on = find_onset(x, 0, min(len(x), FS))
            seg = np.pad(x[max(0, on - 240):], (0, int(3.6 * FS)))[: int(3.6 * FS)]
            bsus.append(dict(file=lib + "/" + os.path.basename(p), midi=m, **note_features(seg, hz(m))))
    R["b_sustain"] = bsus
    bdead = []
    for lib in ("swagbass", "fashionbass"):
        for p in sorted(glob.glob(os.path.join(refs, "karoryfer", lib, "noises", "*muted[a-g]*.wav"))):
            x = load(p)
            on = find_onset(x, 0, min(len(x), FS))
            seg = np.pad(x[max(0, on - 240):], (0, int(1.2 * FS)))[: int(1.2 * FS)]
            bdead.append(dict(file=lib + "/" + os.path.basename(p), **dead_features(seg)))
    R["b_dead"] = bdead
    lv = []
    for lib in ("swagbass", "fashionbass"):
        n_pk = [float(env_db(load(p)).max()) for p in sorted(glob.glob(os.path.join(refs, "karoryfer", lib, "notes", "*.wav")))]
        lv += [float(env_db(load(p)).max()) - float(np.median(n_pk))
               for p in sorted(glob.glob(os.path.join(refs, "karoryfer", lib, "noises", "*muted[a-g]*.wav")))]
    R["level"]["b_dead"] = lv
    # Slap / pop: onsets in the CC0 phrases; the brighter 40 % are pops.
    ev = []
    # Real slap phrases only (two synthesised "slap" uploads and the tapping
    # riffs were looked at and left out). Pops: the brightest 35 % per phrase.
    # 645177 and 625428 were dropped after a look at them: flat, compressed
    # envelopes whose "onsets" are mostly inside notes.
    for p, ch in (("485087.wav", 0), ("553610_L.wav", 0), ("799091.wav", 0), ("26207.wav", 0)):
        path = os.path.join(refs, "freesound", p)
        if not os.path.exists(path):
            continue
        x = load(path, ch)
        for on in phrase_onsets(x, min_gap=0.12, jump_db=14.0):
            seg = x[max(0, on - 240): on - 240 + int(0.6 * FS)]
            if len(seg) < int(0.6 * FS):
                continue
            f = estimate_f0(np.pad(seg, (0, FS)), 35.0, 400.0)
            ev.append(dict(file=p, onset_s=on / FS, f0=f, **slap_features(seg)))
    # ONE reference value per recording (the median of its events): the
    # phrases differ in bass, player and chain far more than events within one
    # phrase do, and one long phrase must not outvote the others.
    R["b_slap"], R["b_pop"] = [], []
    for p in sorted({e["file"] for e in ev}):
        mine = [e for e in ev if e["file"] == p]
        cut = np.percentile([e["hf_early_db"] for e in mine], 65)
        for key, part in (("b_slap", [e for e in mine if e["hf_early_db"] < cut]),
                          ("b_pop", [e for e in mine if e["hf_early_db"] >= cut])):
            if part:
                agg = {k: float(np.median([e[k] for e in part])) for k in part[0] if isinstance(part[0][k], float)}
                R[key].append(dict(file=p, events=len(part), **agg))
    with open(os.path.join(out, "refs.json"), "w") as f:
        json.dump(R, f, indent=1)
    for k, v in R.items():
        print(f"{k:12s} {len(v)} reference events")


def note_midi(name):
    names = {"c": 0, "db": 1, "c#": 1, "d": 2, "eb": 3, "d#": 3, "e": 4, "f": 5, "gb": 6, "f#": 6, "g": 7, "ab": 8,
             "g#": 8, "a": 9, "bb": 10, "a#": 10, "b": 11}
    name = name.lower()
    for k in sorted(names, key=len, reverse=True):
        if name.startswith(k) and name[len(k):].isdigit():
            return 12 * (int(name[len(k):]) + 1) + names[k]
    return None


def phrase_onsets(x, min_gap=0.09, jump_db=9.0):
    """Spectral-flux onsets (high-passed), at least `min_gap` s apart."""
    b, a = signal.butter(2, 300.0 / (FS / 2), "high")
    y = signal.lfilter(b, a, x)
    e = env_db(y, 96)  # 2 ms
    d = np.diff(e)
    pk = e.max()
    out = []
    last = -1e9
    for i in range(2, len(d)):
        if d[i] > jump_db and e[i + 1] > pk - 30.0 and (i - last) * 0.002 > min_gap:
            out.append(i * 96)
            last = i
    return out


def harmonic_features(x, f_open, k):
    """A natural harmonic: off-harmonic suppression (partials of the open string
    that are NOT multiples of k, vs those that are), the slope of its own
    partials, its decay and its brightness in units of its own pitch."""
    seg = x[int(0.08 * FS): int(0.6 * FS)]
    n = 1 << 16
    s = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), n)) ** 2
    f = np.fft.rfftfreq(n, 1.0 / FS)
    def pw(c):
        m = (f > c * 0.985) & (f < c * 1.02)
        return float(s[m].max()) if np.any(m) else 0.0
    on = [pw(j * f_open) for j in range(k, 8 * k + 1, k) if j * f_open < 14000]
    off = [pw(j * f_open) for j in range(1, 8 * k) if j % k != 0 and j * f_open < 14000]
    offharm = 10 * math.log10((sum(off) + 1e-20) / (sum(on) + 1e-20))
    own = np.array([10 * math.log10(v + 1e-20) for v in on[:6]])
    own -= own.max()
    slope = float(np.polyfit(np.arange(len(own)), own, 1)[0]) if len(own) >= 3 else 0.0
    nf = note_features(x, f_open * k)
    h = f_open * k
    return {"offharm_db": float(offharm), "own_slope_db": slope, "t20_ms": nf["t20_ms"], "t40_ms": nf["t40_ms"],
            "decay_db_s": nf["decay_db_s"], "c_rel_h": float(nf["c_120_400"] / h), "peak_db": nf["peak_db"],
            "transient_db": nf["transient_db"]}


def vibrato_features(x, f_nom):
    t = f0_track(x[int(0.05 * FS): int(3.2 * FS)], f_nom * 0.7, f_nom * 1.5, hop=240)
    if np.sum(np.isfinite(t)) < 200:
        return None
    t = np.where(np.isfinite(t), t, np.nan)
    c = 1200 * np.log2(t / f_nom)
    good = np.isfinite(c)
    idx = np.arange(len(c))
    c = np.interp(idx, idx[good], c[good])
    base = np.percentile(c[: 40], 50)  # the first 200 ms
    c = c - base
    # Rate from the autocorrelation of the detrended contour after 0.4 s.
    y = c[80:]
    y = y - signal.savgol_filter(y, 101, 2) if len(y) > 120 else y - y.mean()
    if len(y) < 120:
        return None
    ac = np.correlate(y, y, "full")[len(y) - 1:]
    ac /= ac[0] + 1e-12
    lags = np.arange(len(ac)) * 0.005
    m = (lags > 1 / 12.0) & (lags < 1 / 2.5)
    if not np.any(m):
        return None
    li = np.argmax(np.where(m, ac, -9))
    rate = 1.0 / lags[li]
    depth = float(np.percentile(c[80:], 95) - np.percentile(c[80:], 5))
    mean = float(np.mean(c[80:]))
    # One-sided (pitch only goes up, bend-vibrato) -> mean ~ depth/2.
    asym = mean / max(depth, 1.0)
    # Onset: first time the contour leaves +-8 cents of the base.
    lv = np.where(np.abs(c) > 8.0)[0]
    onset = float(lv[0] * 0.005) if len(lv) else 3.0
    reg = float(ac[li])  # how periodic (1 = a perfect LFO)
    return {"rate_hz": float(rate), "depth_cents": depth, "asym": float(asym), "onset_s": onset, "periodicity": reg}


def dead_features(x):
    nf = note_features(x, 0.0)
    # Pitchedness: the strongest normalised autocorrelation in 60..800 Hz over 10-80 ms.
    seg = x[int(0.01 * FS): int(0.08 * FS)].astype(np.float64)
    seg = seg - seg.mean()
    ac = np.correlate(seg, seg, "full")[len(seg) - 1:]
    ac /= ac[0] + 1e-12
    lo, hi = int(FS / 800), int(FS / 60)
    pitched = float(np.max(ac[lo:hi])) if hi < len(ac) else 0.0
    return {"peak_db": nf["peak_db"], "t20_ms": nf["t20_ms"], "t40_ms": nf["t40_ms"], "c_0_30": nf["c_0_30"], "c_30_120": nf["c_30_120"],
            "hf_early_db": nf["hf_early_db"], "pitchedness": pitched, "transient_db": nf["transient_db"]}


def slap_features(x):
    nf = note_features(np.pad(x, (0, FS)), 0.0)
    w = lambda a, b: x[int(a * FS): int(b * FS)]
    return {"hf_early_db": nf["hf_early_db"], "c_0_30": nf["c_0_30"], "c_30_120": nf["c_30_120"],
            "c_120_400": nf["c_120_400"], "bright_decay_oct": oct_ratio(nf["c_0_30"], nf["c_120_400"]),
            "transient_db": nf["transient_db"], "t_peak_ms": nf["t_peak_ms"],
            "hf_30_120_db": band_ratio_db(w(0.03, 0.12)), "hf_120_400_db": band_ratio_db(w(0.12, 0.4)),
            "hf5k_0_30_db": band_ratio_db(w(0.0, 0.03), 5000.0), "hf5k_30_120_db": band_ratio_db(w(0.03, 0.12), 5000.0),
            "hf5k_120_400_db": band_ratio_db(w(0.12, 0.4), 5000.0),
            "hf_attack_excess_db": band_ratio_db(w(0.0, 0.03)) - band_ratio_db(w(0.12, 0.4)),
            "hf5k_attack_excess_db": band_ratio_db(w(0.0, 0.03), 5000.0) - band_ratio_db(w(0.12, 0.4), 5000.0)}


# ---- renders --------------------------------------------------------------------

def render_cmd(a):
    out = os.path.join(a.out, a.tag)
    os.makedirs(out, exist_ok=True)
    R = json.load(open(os.path.join(a.out, "refs.json")))
    jobs = []

    def notes_file(name, lines, bass=False):
        p = os.path.join(out, name + ".txt")
        with open(p, "w") as f:
            f.write("\n".join(lines) + "\n")
        jobs.append((name, p, bass))

    gs = sorted({(e["string"], e["fret"]) for e in R["g_sustain"]})
    notes_file("g_sustain", [f"{0.2 + 4 * i:.2f} 3.6 {s} {f}" for i, (s, f) in enumerate(gs)])
    gp = sorted({(e["string"], e["fret"]) for e in R["g_palm"]})
    notes_file("g_palm", [f"{0.2 + 4 * i:.2f} 1.0 {s} {f} palm" for i, (s, f) in enumerate(gp)])
    gpi = sorted({(e["string"], e["fret"]) for e in R["g_pinch"]})
    notes_file("g_pinch", [f"{0.2 + 4 * i:.2f} 3.6 {s} {f} pinch" for i, (s, f) in enumerate(gpi)])
    gh = sorted({(e["string"], e["fret"]) for e in R["g_harmonic"]})
    notes_file("g_harmonic", [f"{0.2 + 4 * i:.2f} 3.6 {s} {f} harm" for i, (s, f) in enumerate(gh)])
    gv = sorted({(e["string"], e["fret"]) for e in R["g_vibrato"]})
    notes_file("g_vibrato", [f"{0.2 + 4 * i:.2f} 3.4 {s} {f} vib" for i, (s, f) in enumerate(gv)])
    notes_file("g_dead", [f"{0.2 + 1.5 * i:.2f} 0.25 {s} {f} dead" for i, (s, f) in
                          enumerate([(s, f) for s in range(6) for f in (0, 5)])])
    notes_file("g_repeat", [f"{0.2 + 1.2 * i:.2f} 1.0 {s} {f}" for s, f in ((1, 5), (3, 7), (5, 3)) for i in range(6)])
    # A six-string down strum and an up strum (strum spread).
    chord = [(0, 0), (1, 2), (2, 2), (3, 1), (4, 0), (5, 0)]
    notes_file("g_strum", [f"0.2 2.0 {s} {f} chord=1/6/{i} strum=down" for i, (s, f) in enumerate(chord)]
               + [f"3.2 2.0 {s} {f} chord=2/6/{i} strum=up" for i, (s, f) in enumerate(chord)])
    # Bass (Jazz, both pickups, fingers).
    bm = sorted({e["midi"] for e in R["b_sustain"]})
    def bsf(m):
        for s in (3, 2, 1, 0):
            if m - OPEN_B[s] >= 0:
                return s, m - OPEN_B[s]
        return 0, 0
    notes_file("b_sustain", [f"{0.2 + 4 * i:.2f} 3.6 {bsf(m)[0]} {bsf(m)[1]}" for i, m in enumerate(bm)], True)
    notes_file("b_slap", [f"{0.2 + 1.0 * i:.2f} 0.6 {s} {f} slap" for i, (s, f) in
                          enumerate([(0, 0), (0, 3), (0, 5), (1, 0), (1, 2), (1, 5), (0, 7), (1, 7)])], True)
    notes_file("b_pop", [f"{0.2 + 1.0 * i:.2f} 0.6 {s} {f} pop" for i, (s, f) in
                         enumerate([(2, 2), (2, 5), (3, 2), (3, 4), (2, 7), (3, 7), (2, 0), (3, 0)])], True)
    notes_file("b_dead", [f"{0.2 + 1.2 * i:.2f} 0.25 {s} {f} dead" for i, (s, f) in
                          enumerate([(s, f) for s in range(4) for f in (0, 3)])], True)
    for name, p, bass in jobs:
        cmd = [a.exe, "--events", p, "--out", os.path.join(out, name + ".wav"), "--pack-dir", a.packs]
        if bass:
            cmd.append("--bass")
        r = subprocess.run(cmd, capture_output=True, text=True)
        print(r.stdout.strip() or r.stderr.strip())


def events_of(path):
    ev = []
    for line in open(path):
        line = line.split("#")[0].split()
        if len(line) >= 4:
            ev.append((float(line[0]), float(line[1]), int(line[2]), int(line[3]), line[4:]))
    return ev


def measure_cmd(a):
    d = os.path.join(a.out, a.tag)
    R = json.load(open(os.path.join(a.out, "refs.json")))
    M = {}

    def segs(name, length):
        x = load(os.path.join(d, name + ".wav"))
        for t, dur, s, f, tok in events_of(os.path.join(d, name + ".txt")):
            i = int(t * FS)
            on = find_onset(x, max(0, i - 240), i + int(0.1 * FS))
            yield s, f, tok, np.pad(x[max(0, on - 240):], (0, length))[:length]

    sus = {(s, f): seg for s, f, _, seg in segs("g_sustain", int(3.6 * FS))}
    M["g_sustain"] = [dict(string=s, fret=f, **note_features(seg, hz(OPEN_G[s] + f))) for (s, f), seg in sus.items()]
    def base_for(s, f):
        if (s, f) in sus:
            return sus[(s, f)]
        cand = [k for k in sus if k[0] == s]
        return sus[min(cand, key=lambda k: abs(k[1] - f))] if cand else None
    M["g_palm"] = [dict(string=s, fret=f, **note_features(seg, hz(OPEN_G[s] + f)),
                        base=note_features(base_for(s, f), hz(OPEN_G[s] + f)))
                   for s, f, _, seg in segs("g_palm", int(3.6 * FS))]
    M["g_pinch"] = [dict(string=s, fret=f, **note_features(seg, hz(OPEN_G[s] + f)),
                         base=note_features(base_for(s, f), hz(OPEN_G[s] + f)))
                    for s, f, _, seg in segs("g_pinch", int(3.6 * FS))]
    harm = []
    for s, f, _, seg in segs("g_harmonic", int(3.6 * FS)):
        k = {12: 2, 7: 3, 5: 4, 4: 5, 3: 6}[f]
        harm.append(dict(string=s, fret=f, k=k, **harmonic_features(seg, hz(OPEN_G[s]), k),
                         base=note_features(base_for(s, 0), hz(OPEN_G[s]))))
    M["g_harmonic"] = harm
    M["g_vibrato"] = [dict(string=s, fret=f, **(vibrato_features(seg, hz(OPEN_G[s] + f)) or {}))
                      for s, f, _, seg in segs("g_vibrato", int(3.6 * FS))]
    sus_pk = float(np.median([e["peak_db"] for e in M["g_sustain"]]))
    M["g_dead"] = [dict(string=s, fret=f, **dead_features(seg)) for s, f, _, seg in segs("g_dead", int(1.2 * FS))]
    for e in M["g_dead"]:
        e["level_vs_open_db"] = e["peak_db"] - sus_pk
    rep = {}
    for s, f, _, seg in segs("g_repeat", int(1.0 * FS)):
        rep.setdefault((s, f), []).append((20 * np.log10(np.max(np.abs(seg)) + 1e-9), centroid(seg[: int(0.12 * FS)])))
    M["g_repeat"] = [dict(string=s, fret=f, level_sd_db=float(np.std(np.array(v)[:, 0])),
                          centroid_sd_oct=float(np.std(np.log2(np.array(v)[:, 1])))) for (s, f), v in rep.items()]
    # Strum spread: per-string onset times from the render (each string's own
    # onset is found against the summed signal with a high-passed flux).
    x = load(os.path.join(d, "g_strum.wav"))
    st = []
    for t0 in (0.2, 3.2):
        seg = x[int(t0 * FS) - 480: int((t0 + 0.2) * FS)]
        ons = phrase_onsets(seg, min_gap=0.003)
        if len(ons) >= 2:
            st.append({"spread_ms": float((ons[-1] - ons[0]) / FS * 1000.0), "onsets": len(ons)})
    M["g_strum"] = st
    bsus = []
    for s, f, _, seg in segs("b_sustain", int(3.6 * FS)):
        bsus.append(dict(midi=OPEN_B[s] + f, **note_features(seg, hz(OPEN_B[s] + f))))
    M["b_sustain"] = bsus
    M["b_slap"] = [dict(string=s, fret=f, **slap_features(seg)) for s, f, _, seg in segs("b_slap", int(0.6 * FS))]
    M["b_pop"] = [dict(string=s, fret=f, **slap_features(seg)) for s, f, _, seg in segs("b_pop", int(0.6 * FS))]
    M["b_dead"] = [dict(string=s, fret=f, **dead_features(seg)) for s, f, _, seg in segs("b_dead", int(1.2 * FS))]
    bpk = float(np.median([e["peak_db"] for e in M["b_sustain"]]))
    for e in M["b_dead"]:
        e["level_vs_open_db"] = e["peak_db"] - bpk
    with open(os.path.join(a.out, f"metrics_{a.tag}.json"), "w") as f:
        json.dump(M, f, indent=1)
    print(f"measured {a.tag}")


# ---- comparison -------------------------------------------------------------------
# Each articulation: metric name -> (extractor over one event, floor for the
# reference spread). Relative metrics are against the same instrument's
# ordinary note on the same string and fret.

def rel(field):
    return lambda e: e[field] - e["base"][field] if e.get("base") else None


def rel_oct(field):
    return lambda e: oct_ratio(e[field], e["base"][field]) if e.get("base") else None


METRICS = {
    "g_sustain": {
        "t20_ms": (lambda e: e["t20_ms"], 150.0),
        "decay_db_s": (lambda e: e["decay_db_s"], 2.0),
        "bright_decay_oct": (lambda e: oct_ratio(e["c_0_30"], e["c_400_1200"]), 0.15),
        "transient_db": (lambda e: e["transient_db"], 1.0),
    },
    "g_palm": {
        "level_vs_open_db": (rel("peak_db"), 1.5),
        "t20_ms": (lambda e: e["t20_ms"], 25.0),
        "t40_ms": (lambda e: e["t40_ms"], 50.0),
        "centroid_30_120_vs_open_oct": (rel_oct("c_30_120"), 0.15),
        "hf_early_vs_open_db": (rel("hf_early_db"), 1.5),
        "transient_db": (lambda e: e["transient_db"], 1.0),
    },
    "g_pinch": {
        "level_vs_open_db": (rel("peak_db"), 1.5),
        "centroid_120_400_vs_open_oct": (rel_oct("c_120_400"), 0.15),
        "centroid_400_1200_vs_open_oct": (rel_oct("c_400_1200"), 0.15),
        "squeal_db": (lambda e: float(np.clip(max(e["partials"][2:8]) - max(e["partials"][0:2]), -30.0, 30.0)), 3.0),
        "t20_ms": (lambda e: e["t20_ms"], 100.0),
    },
    "g_harmonic": {
        "offharm_db": (lambda e: e["offharm_db"], 3.0),
        "own_slope_db": (lambda e: e["own_slope_db"], 1.5),
        "c_rel_h": (lambda e: e["c_rel_h"], 0.2),
        "t20_ms": (lambda e: e["t20_ms"], 150.0),
        "level_vs_open_db": (lambda e: e["peak_db"] - e["base"]["peak_db"] if e.get("base") else None, 1.5),
        "transient_db": (lambda e: e["transient_db"], 1.0),
    },
    "g_vibrato": {
        "rate_hz": (lambda e: e.get("rate_hz"), 0.3),
        "depth_cents": (lambda e: e.get("depth_cents"), 8.0),
        "asym": (lambda e: e.get("asym"), 0.05),
        "onset_s": (lambda e: e.get("onset_s"), 0.05),
        "periodicity": (lambda e: e.get("periodicity"), 0.05),
    },
    "g_dead": {
        "level_vs_open_db": (lambda e: e.get("level_vs_open_db"), 1.5),
        "t20_ms": (lambda e: e["t20_ms"], 10.0),
        "t40_ms": (lambda e: e["t40_ms"], 20.0),
        "pitchedness": (lambda e: e["pitchedness"], 0.05),
        "hf_early_db": (lambda e: e["hf_early_db"], 1.5),
        "transient_db": (lambda e: e["transient_db"], 1.0),
    },
    "g_repeat": {
        "level_sd_db": (lambda e: e["level_sd_db"], 0.2),
        "centroid_sd_oct": (lambda e: e["centroid_sd_oct"], 0.01),
    },
    "b_sustain": {
        "t20_ms": (lambda e: e["t20_ms"], 150.0),
        "decay_db_s": (lambda e: e["decay_db_s"], 2.0),
        "bright_decay_oct": (lambda e: oct_ratio(e["c_0_30"], e["c_400_1200"]), 0.15),
        "transient_db": (lambda e: e["transient_db"], 1.0),
    },
    "b_slap": {
        "hf_attack_excess_db": (lambda e: e.get("hf_attack_excess_db"), 2.0),
        "hf5k_attack_excess_db": (lambda e: e.get("hf5k_attack_excess_db"), 2.0),
        "hf_early_db": (lambda e: e["hf_early_db"], 1.5),
        "hf_30_120_db": (lambda e: e.get("hf_30_120_db"), 1.5),
        "hf_120_400_db": (lambda e: e["hf_120_400_db"], 1.5),
        "hf5k_0_30_db": (lambda e: e.get("hf5k_0_30_db"), 1.5),
        "hf5k_120_400_db": (lambda e: e.get("hf5k_120_400_db"), 1.5),
        "bright_decay_oct": (lambda e: e["bright_decay_oct"], 0.15),
        "transient_db": (lambda e: e["transient_db"], 1.0),
    },
    "b_pop": {
        "hf_attack_excess_db": (lambda e: e.get("hf_attack_excess_db"), 2.0),
        "hf5k_attack_excess_db": (lambda e: e.get("hf5k_attack_excess_db"), 2.0),
        "hf_early_db": (lambda e: e["hf_early_db"], 1.5),
        "hf_30_120_db": (lambda e: e.get("hf_30_120_db"), 1.5),
        "hf_120_400_db": (lambda e: e["hf_120_400_db"], 1.5),
        "hf5k_0_30_db": (lambda e: e.get("hf5k_0_30_db"), 1.5),
        "hf5k_120_400_db": (lambda e: e.get("hf5k_120_400_db"), 1.5),
        "bright_decay_oct": (lambda e: e["bright_decay_oct"], 0.15),
        "transient_db": (lambda e: e["transient_db"], 1.0),
    },
    "b_dead": {
        "level_vs_open_db": (lambda e: e.get("level_vs_open_db"), 1.5),
        "t20_ms": (lambda e: e["t20_ms"], 10.0),
        "t40_ms": (lambda e: e["t40_ms"], 20.0),
        "pitchedness": (lambda e: e["pitchedness"], 0.05),
        "hf_early_db": (lambda e: e["hf_early_db"], 1.5),
    },
}

# Strum spread has no audio reference set in the default download: the
# Guitar-TECHS chord MIDI (per-string Triple Play onsets) is read with HTTP range
# requests by `refs --strums` when available; otherwise a published range.
STRUM_REF = {"spread_ms": (30.0, 12.0)}


def stats(events, fn):
    """Robust: median and 1.4826 x MAD (one bad onset must not move the reference)."""
    v = [fn(e) for e in events]
    v = np.array([x for x in v if x is not None and np.isfinite(x)], dtype=float)
    if len(v) == 0:
        return float("nan"), float("nan"), 0
    med = float(np.median(v))
    return med, float(1.4826 * np.median(np.abs(v - med))), len(v)


def compare(R, M):
    rows = []
    M = dict(M)
    for x in ("g_palm", "g_pinch"):
        if x in M:
            M[x + "_p1x"] = M[x]
    for art, mets in list(METRICS.items()) + [(k + "_p1x", METRICS[k]) for k in ("g_palm", "g_pinch")]:
        if art not in R or art not in M or not R[art] or not M[art]:
            continue
        zs = []
        for name, (fn, floor) in mets.items():
            rm, rs, rn = stats(R[art], fn)
            if name == "level_vs_open_db" and art in R.get("level", {}):
                rm, rs, rn = stats(R["level"][art], lambda v: v)
            om, os_, on = stats(M[art], fn)
            if rn == 0 or on == 0:
                continue
            z = (om - rm) / max(rs, floor)
            zs.append(z)
            rows.append((art, name, rm, rs, rn, om, on, z))
        rows.append((art, "DISTANCE", float("nan"), float("nan"), 0, float(np.sqrt(np.mean(np.square(zs)))) if zs else float("nan"), 0, float("nan")))
    return rows


def report_cmd(a):
    R = json.load(open(os.path.join(a.out, "refs.json")))
    tags = a.tags.split(",")
    res = {t: compare(R, json.load(open(os.path.join(a.out, f"metrics_{t}.json")))) for t in tags}
    keys = []
    for t in tags:
        for r in res[t]:
            if (r[0], r[1]) not in keys:
                keys.append((r[0], r[1]))
    look = {t: {(r[0], r[1]): r for r in res[t]} for t in tags}
    lines = ["| articulation | metric | real (median ± 1.48 MAD, n) | " + " | ".join(f"{t}" for t in tags) + " | " +
             " | ".join(f"z {t}" for t in tags) + " |",
             "|---|---|---|" + "---:|" * (2 * len(tags))]
    for art, name in keys:
        cells = []
        zc = []
        ref = ""
        for t in tags:
            r = look[t].get((art, name))
            if r is None:
                cells.append("")
                zc.append("")
                continue
            if name == "DISTANCE":
                cells.append(f"**{r[5]:.2f}**")
                zc.append("")
            else:
                ref = f"{r[2]:.2f} ± {r[3]:.2f} ({r[4]})"
                cells.append(f"{r[5]:.2f}")
                zc.append(f"{r[7]:+.1f}")
        lines.append(f"| {art} | {name} | {ref} | " + " | ".join(cells) + " | " + " | ".join(zc) + " |")
    text = "\n".join(lines)
    path = os.path.join(a.out, "report_" + "_".join(tags) + ".md")
    open(path, "w", encoding="utf-8", newline="\n").write(text + "\n")
    print(text)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("refs"); p.add_argument("--refs", required=True); p.add_argument("--out", required=True)
    p = sub.add_parser("render"); p.add_argument("--exe", required=True); p.add_argument("--packs", required=True)
    p.add_argument("--out", required=True); p.add_argument("--tag", required=True)
    p = sub.add_parser("measure"); p.add_argument("--out", required=True); p.add_argument("--tag", required=True)
    p = sub.add_parser("report"); p.add_argument("--out", required=True); p.add_argument("--tags", required=True)
    a = ap.parse_args()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    {"refs": refs_cmd, "render": render_cmd, "measure": measure_cmd, "report": report_cmd}[a.cmd](a)


if __name__ == "__main__":
    main()
