#!/usr/bin/env python3
"""Calibrates the chart voice's pickup conversion (src/dsp/voice/dcvoice_pickup.h,
dcvoice_models.h; DotCharter #50) against the recordings that EXIST, and prints
the evidence the model table cites.

  1. GEOMETRY. EGFxSet recorded every string and fret of one Stratocaster in all
     five pickup positions. Each position is predicted from every other one
     through the conversion's own maths (target comb / regularised source
     comb), and the Strat's pickup distances are FITTED to minimise the error
     against the real recordings. Reported: the fitted distances and the error
     per position pair, against "no conversion".
  2. REGULARISATION. The same prediction for the source inverse's g and the
     target comb's leak, and for one- and two-pickup sources.
  3. PROVENANCE. Where the chromatic Karoryfer sets were recorded (their readmes
     say little): the comb left in each recording's harmonics is correlated
     against candidate placements. Checked first on the Strat packs, where the
     answer is known.

  python calibrate_pickups.py --zip <EGFxSet Clean.zip> --packs <dir with the .dcvp packs>

Needs numpy and scipy (scipy only for the geometry fit). Read-only; writes nothing.
"""
import argparse
import io
import struct
import wave
import zipfile

import numpy as np

POS = ["Neck", "Middle-Neck", "Middle", "Bridge-Middle", "Bridge"]
OPEN = {1: 329.63, 2: 246.94, 3: 196.0, 4: 146.83, 5: 110.0, 6: 82.41}   # EGFxSet string 1 = high E
NH = 40
SCALE = 648.0


def decode24(b):
    w = wave.open(io.BytesIO(b))
    a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.uint8).reshape(-1, 3)
    x = a[:, 0].astype(np.int32) | (a[:, 1].astype(np.int32) << 8) | (a[:, 2].astype(np.int32) << 16)
    return np.where(x >= 1 << 23, x - (1 << 24), x) / float(1 << 23), w.getframerate()


def harmonics(zip_path):
    """{(position, string, fret): dB amplitudes of harmonics 1..NH}, f0 per note."""
    z = zipfile.ZipFile(zip_path)
    amps, f0s = {}, {}
    for name in z.namelist():
        if not name.endswith(".wav"):
            continue
        pos = name.split("/")[1]
        s, k = map(int, name.split("/")[-1][:-4].split("-"))
        x, fs = decode24(z.read(name))
        on = int(np.argmax(np.abs(x) > 0.05 * np.max(np.abs(x))))
        seg = x[on + int(0.08 * fs):on + int(0.58 * fs)]
        nfft = 1 << 19
        mag = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), nfft))
        df = fs / nfft
        nominal = OPEN[s] * 2 ** (k / 12)
        cands = nominal * 2 ** (np.linspace(-40, 40, 161) / 1200)
        f0 = cands[int(np.argmax([sum(np.log(mag[int(round(c * n / df))] + 1e-9) for n in range(1, 6)) for c in cands]))]
        a = np.zeros(NH)
        for n in range(1, NH + 1):
            fc = n * f0
            if fc > 16000:
                break
            w = max(3, int(0.03 * fc / df))
            i0 = int(fc / df)
            a[n - 1] = mag[max(1, i0 - w):min(len(mag) - 1, i0 + w)].max()
        amps[(pos, s, k)] = 20 * np.log10(a + 1e-9)
        f0s[(pos, s, k)] = f0
    return amps, f0s


class Egfx:
    def __init__(self, amps, f0s):
        self.keys = sorted(set((s, k) for (_, s, k) in amps))
        self.S = np.array([s for s, _ in self.keys])
        self.L = SCALE * 2.0 ** (-np.array([k for _, k in self.keys]) / 12.0)
        self.n = np.arange(1, NH + 1)[None, :]
        self.A = {p: np.array([amps[(p, s, k)] for s, k in self.keys]) for p in POS}
        self.F = np.array([np.arange(1, NH + 1) * f0s[("Middle", s, k)] for s, k in self.keys])

    def geometry(self, prm):
        bhi, blo, dm, dn = prm
        db = bhi + (blo - bhi) * (self.S - 1) / 5.0     # S 1 = high E (nearest the bridge)
        return {"Neck": [dn], "Middle-Neck": [dm, dn], "Middle": [dm], "Bridge-Middle": [db, dm], "Bridge": [db]}

    def rho(self, d):
        return (d if np.ndim(d) else np.full(len(self.keys), d)) / self.L

    def target(self, pts, g):
        """|leaky target comb|, delays in periods (a = rho / 2, centred on the farthest)."""
        a = [0.5 * self.rho(d)[:, None] for d in pts]
        A = np.maximum.reduce(a) if len(a) > 1 else a[0]
        om = 2 * np.pi * self.n
        re = np.zeros(self.F.shape)
        im = np.zeros(self.F.shape)
        for ai in a:
            for sign, delay in ((1.0, A - ai), (-g, A + ai)):
                re += 0.5 / len(pts) * sign * np.cos(om * delay)
                im -= 0.5 / len(pts) * sign * np.sin(om * delay)
        return np.hypot(re, im)

    def inverse(self, pts, g):
        """|regularised source inverse|: one pole comb, or sine x cosine factors for two points."""
        r = [self.rho(d)[:, None] for d in pts]
        if len(r) == 1:
            w = 2 * np.pi * self.n * r[0]
            return 2 / np.hypot(1 - g * np.cos(w), g * np.sin(w))
        lo, hi = np.minimum(r[0], r[1]), np.maximum(r[0], r[1])
        w1, w2 = 2 * np.pi * self.n * 0.5 * (lo + hi), 2 * np.pi * self.n * 0.5 * (hi - lo)
        return 2 / np.hypot(1 - g * np.cos(w1), g * np.sin(w1)) * 2 / np.hypot(1 + g * np.cos(w2), g * np.sin(w2))

    def error(self, P, Q, geo, gs=0.5, gt=0.7, convert=True):
        t = 20 * np.log10(self.target(geo[Q], gt) * self.inverse(geo[P], gs) + 1e-9) if convert else 0.0
        pred, act = self.A[P] + t, self.A[Q]
        ok = (self.F < 8000) & (self.A[P] > self.A[P].max(axis=1, keepdims=True) - 45) & (act > act.max(axis=1, keepdims=True) - 45)
        d = np.where(ok, pred - act, np.nan)
        d = d - np.nanmedian(d, axis=1, keepdims=True)    # the level is the builder's, not the pickup's
        return float(np.nanmedian(np.sqrt(np.nanmean(d * d, axis=1))))


def provenance(pack_path, variant, scale, candidates):
    b = open(pack_path, "rb").read()
    count = struct.unpack_from("<I", b, 12)[0]
    fs = struct.unpack_from("<f", b, 16)[0]
    zones_off, data_off, data_samples = struct.unpack_from("<QQQ", b, 24)
    data = np.frombuffer(b, dtype="<i2", count=data_samples, offset=data_off)
    scores = {k: [] for k in candidates}
    for i in range(count):
        o = zones_off + 40 * i
        if b[o] != 0 or b[o + 1] != variant:
            continue
        root = struct.unpack_from("<f", b, o + 4)[0]
        length = struct.unpack_from("<I", b, o + 12)[0]
        start = struct.unpack_from("<Q", b, o + 16)[0]
        fret = max(b[o + 29] - 1, 0)
        seg = data[start:start + length].astype(float)[int(0.08 * fs):int(0.58 * fs)]
        if len(seg) < 4096:
            continue
        nfft = 1 << 18
        mag = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), nfft))
        df = fs / nfft
        h = []
        for n in range(1, 25):
            if n * root > 8000:
                break
            i0 = int(n * root / df)
            w = max(2, int(0.02 * n * root / df))
            h.append(mag[max(1, i0 - w):i0 + w].max())
        if len(h) < 8:
            continue
        h = 20 * np.log10(np.array(h) + 1e-9)
        x = np.log(np.arange(1, len(h) + 1))
        hd = h - np.polyval(np.polyfit(x, h, 2), x)
        L = scale * 2 ** (-fret / 12)
        for name, pts in candidates.items():
            nn = np.arange(1, len(h) + 1)
            c = np.clip(20 * np.log10(np.abs(sum(np.sin(np.pi * nn * d / L) for d in pts) / len(pts)) + 1e-3), -30, 0)
            cd = c - np.polyval(np.polyfit(x, c, 2), x)
            if np.std(cd) > 0:
                scores[name].append(np.corrcoef(hd, cd)[0, 1])
    return {k: float(np.nanmean(v)) for k, v in scores.items() if v}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--zip", required=True, help="EGFxSet Clean.zip")
    ap.add_argument("--packs", help="directory with guitar.dcvp, bass.dcvp and strat-*.dcvp")
    args = ap.parse_args()

    from scipy.optimize import minimize
    e = Egfx(*harmonics(args.zip))
    pairs = [(P, Q) for P in POS for Q in POS if P != Q]
    total = lambda prm: np.mean([e.error(P, Q, e.geometry(prm)) for P, Q in pairs])
    fit = minimize(total, np.array([38.0, 48.0, 98.0, 160.0]), method="Nelder-Mead", options={"maxiter": 800, "xatol": 0.2})
    geo = e.geometry(fit.x)
    print("1. GEOMETRY fitted (mm from the bridge saddle): bridge %.1f (high E) .. %.1f (low E), middle %.1f, neck %.1f"
          % tuple(fit.x))
    print("   %-14s %-14s %8s %8s" % ("from", "to", "as is", "convert"))
    for P, Q in pairs:
        print("   %-14s %-14s %8.2f %8.2f" % (P, Q, e.error(P, Q, geo, convert=False), e.error(P, Q, geo)))
    print("   mean: as is %.2f dB, converted %.2f dB RMS per harmonic"
          % (np.mean([e.error(P, Q, geo, convert=False) for P, Q in pairs]), np.mean([e.error(P, Q, geo) for P, Q in pairs])))

    print("2. REGULARISATION (mean error over every pair):")
    for gs in (0.3, 0.5, 0.7, 0.85):
        for gt in (0.7, 1.0):
            print("   source inverse g %.2f, target leak %.2f: %.2f dB" % (gs, gt, np.mean([e.error(P, Q, geo, gs, gt) for P, Q in pairs])))

    if args.packs:
        print("3. PROVENANCE (correlation of each recording's comb with a candidate placement):")
        p = args.packs.rstrip("/\\")
        for pack, cands in (("strat-neck", {"neck 158.8": [158.8], "middle 99": [99.0], "bridge 41": [41.0]}),
                            ("strat-bridge", {"neck 158.8": [158.8], "middle 99": [99.0], "bridge 41": [41.0]})):
            print("   %-12s (known)" % pack, provenance(p + "/" + pack + ".dcvp", 0, SCALE, cands))
        print("   Gretsch      ", provenance(p + "/guitar.dcvp", 0, 625.0,
                                            {"neck 158": [158.0], "bridge 45": [45.0], "both": [158.0, 45.0]}))
        print("   AB-95 fingers", provenance(p + "/bass.dcvp", 0, 864.0,
                                            {"neck 150": [150.0], "bridge 50": [50.0], "both": [150.0, 50.0]}))
        print("   Growly pick  ", provenance(p + "/bass.dcvp", 1, 864.0,
                                            {"neck 150": [150.0], "bridge 60": [60.0], "both 150+60": [150.0, 60.0],
                                             "single 110": [110.0]}))


if __name__ == "__main__":
    main()
