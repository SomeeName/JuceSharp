#!/usr/bin/env python3
"""Generator for the cab IR bank's models 12 and up (dcdsp_cab_irs.h).

The bank's recipe, as its header states it: an AUTHORED MAGNITUDE TARGET,
folded to minimum phase by cepstral reconstruction, 2048 taps at 48 kHz,
loudness-normalised over 200-4000 Hz. Models 0-11 were authored by a scratch
harness that was never committed; their rows are copied VERBATIM from the
existing header (persisted `model` ints keep their sound, byte for byte) and
this script only appends.

What is new is where the magnitude target comes from. Models 12+ are not
hand-shaped curves: each one starts from a MEASURED loudspeaker response
(driver_curves.json: Jensen's published 1 W / 1 m anechoic sweeps on an IEC
baffle) and changes only what the measurement does not contain, each change by
a published formula:

  * the BOX. The IEC baffle is an open baffle, so the driver's own free-air
    resonance (Fs, Qts) shapes the measured low end. A closed box replaces
    that second-order high-pass with the box's: fc = Fs*sqrt(1 + Vas/Vb),
    Qtc = Qts*sqrt(1 + Vas/Vb) (R. H. Small, "Closed-Box Loudspeaker
    Systems", JAES 20(10), 1972). A vented bass box uses Thiele's QB3/B4
    family through Keele's closed forms, F3 = 0.26*Fs*Qts^-1.4 (D. B. Keele,
    "Sensitivity of Thiele's Vented Loudspeaker Enclosure Alignments to
    Parameter Variations", JAES 21(4), 1973), rendered as a 4th-order
    Butterworth high-pass at F3. An open back keeps the baffle response and
    adds the rear wave (below).
  * the NEIGHBOURS. The close mic (1 inch off the grille, on the dust cap)
    hears the other drivers of the cab at their path difference, attenuated
    by spherical spreading r/d and by each cone's own off-axis directivity,
    the rigid circular piston |2 J1(ka sin t)/(ka sin t)| (L. L. Beranek,
    "Acoustics", ch. 4), with a = sqrt(Sd/pi) from the datasheet.
  * an OPEN BACK's rear wave: inverted, spread over its path round the cab.
  * a 2-way PA box: Jensen's recommended 2.5 kHz crossover for the V-11
    compression driver as a 4th-order Linkwitz-Riley pair, the horn padded
    down by the two published sensitivities' difference.

No layout, box or crossover number here is a voicing choice; each is stated
with its source or its assumption beside it. The output is checked here
(reconstruction error, minimum-phase front, level) and in the native
`cab_irs` test.

Usage:  python gen_cab_irs.py            (rewrites ../../src/dsp/cores/dcdsp_cab_irs.h)
        python gen_cab_irs.py --check    (prints the signature table, writes nothing)
Requires numpy + scipy.
"""
from __future__ import annotations

import json
import math
import os
import re
import sys

import numpy as np
from scipy.special import j1

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.normpath(os.path.join(HERE, "..", "..", "src", "dsp", "cores", "dcdsp_cab_irs.h"))
CURVES = os.path.join(HERE, "driver_curves.json")

SR = 48000.0
TAPS = 2048
NFFT = 1 << 16
C = 343.0            # speed of sound, m/s (20 C)
MIC_R = 0.0254       # close mic 1 inch off the cone's dust cap
INCH = 0.0254

# --------------------------------------------------------------------- drivers
# Thiele-Small parameters, verbatim from each driver's Jensen product page (the
# 8-ohm column where several are listed). Sd in m^2.
DRIVERS = {
    "P10R":      dict(fs=97.0,  qts=1.62, vas=27.4e-3,  sd=330.1e-4, nominal=10),
    "P15N":      dict(fs=77.9,  qts=1.22, vas=62.6e-3,  sd=754.8e-4, nominal=15),
    "BP10150":   dict(fs=55.0,  qts=0.33, vas=42.6e-3,  sd=330.1e-4, nominal=10),
    "BP12250":   dict(fs=50.2,  qts=0.32, vas=73.5e-3,  sd=490.9e-4, nominal=12),
    "BP15250":   dict(fs=47.0,  qts=0.51, vas=133.1e-3, sd=754.8e-4, nominal=15),
    "BS15N350A": dict(fs=45.0,  qts=0.46, vas=163.1e-3, sd=855.3e-4, nominal=15, sens=98.7),
    "V11":       dict(sens=105.8, xover=2500.0),
    "MOD5-30":   dict(fs=125.0, qts=0.84, vas=3.3e-3,   sd=78.5e-4,  nominal=5),
}


def load_curves():
    with open(CURVES, encoding="utf-8") as fh:
        return json.load(fh)["drivers"]


def interp_db(curve, f):
    """The published curve, log-frequency interpolated; outside 20 Hz-20 kHz
    it continues at its end slopes (and keeps falling to Nyquist)."""
    x = np.log(np.asarray(curve["hz"], float))
    y = np.asarray(curve["db_spl"], float)
    lf = np.log(np.maximum(f, 1.0))
    out = np.interp(lf, x, y)
    lo_slope = (y[3] - y[0]) / (x[3] - x[0])
    hi_slope = min((y[-1] - y[-4]) / (x[-1] - x[-4]), -12.0 / math.log(2.0))
    out = np.where(lf < x[0], y[0] + lo_slope * (lf - x[0]), out)
    out = np.where(lf > x[-1], y[-1] + hi_slope * (lf - x[-1]), out)
    return out


# ------------------------------------------------------------ analytic pieces
def hp2(f, f0, q):
    s = 1j * np.asarray(f, float) / f0
    return s * s / (s * s + s / q + 1.0)


def butter_hp4(f, f0):
    s = 1j * np.asarray(f, float) / f0
    # Butterworth 4th order: two sections, Q = 0.5412 and 1.3066
    return (s * s / (s * s + s / 0.541196 + 1.0)) * (s * s / (s * s + s / 1.306563 + 1.0))


def lr4(f, f0):
    s = 1j * np.asarray(f, float) / f0
    lp2 = 1.0 / (s * s + s / 0.70710678 + 1.0)
    hp2_ = s * s / (s * s + s / 0.70710678 + 1.0)
    return lp2 * lp2, hp2_ * hp2_


def piston(f, a, theta):
    x = 2.0 * math.pi * np.asarray(f, float) / C * a * math.sin(theta)
    x = np.where(x < 1e-9, 1e-9, x)
    return np.abs(2.0 * j1(x) / x)


def db(x):
    return 20.0 * np.log10(np.maximum(np.abs(x), 1e-12))


# --------------------------------------------------------------- the box layer
# The IEC baffle is itself an OPEN baffle, so below a few hundred Hz the
# measurement also carries its dipole loss: the rear wave, round the baffle
# edge, cancels the front. IEC 60268-5's baffle is 1.65 x 1.35 m; with the
# driver near its centre and the microphone at 1 m on axis the front-to-back
# path difference is about 0.9-1.1 m (to the near and far edge), 1.0 m mean.
# A dipole |1 - exp(-j w D / c)| rises at 6 dB/oct up to where it averages out,
# so its smoothed loss is a FIRST-order high-pass at sqrt(2) c / (2 pi D) =
# 77 Hz. A boxed driver, and any driver heard by a mic 1 inch off its cone,
# has no such loss, so it is divided back out. (Checked against the curves: a
# BP15250's free-air (Fs 47 Hz, Qts 0.51) shape is -7.1 dB at 41 Hz, this term
# -6.6 dB; the published curve reads -14.6 dB re its 200-4000 Hz mean.)
IEC_PATH = 1.0
IEC_DIPOLE_HZ = C * math.sqrt(2.0) / (2.0 * math.pi * IEC_PATH)


def iec_dipole(f):
    s = 1j * np.asarray(f, float) / IEC_DIPOLE_HZ
    return s / (s + 1.0)


def box_response(f, drv, box):
    """Complex LF response of the driver IN its box, divided by the shape the
    measurement carries (its free-air second-order resonance on the IEC
    baffle, and that baffle's dipole loss), so multiplying the measured curve
    by it swaps one for the other. An `open` back keeps the free-air
    resonance (nothing loads the cone) and loses only the IEC dipole term:
    the close mic hears the cab's own rear wave separately (model_target)."""
    d = DRIVERS[drv]
    free = hp2(f, d["fs"], d["qts"]) * iec_dipole(f)
    kind = box["kind"]
    if kind == "open":
        return 1.0 / iec_dipole(f)
    if kind == "closed":
        vb = box["vb"]
        alpha = d["vas"] / vb
        fc = d["fs"] * math.sqrt(1.0 + alpha)
        qtc = d["qts"] * math.sqrt(1.0 + alpha)
        return hp2(f, fc, qtc) / free
    if kind == "closed-butterworth":
        # Vb chosen for Qtc = 0.707 (Small 1972): only defined for Qts < 0.707.
        qtc = 0.70710678
        fc = d["fs"] * qtc / d["qts"]
        return hp2(f, fc, qtc) / free
    if kind == "vented-qb3":
        f3 = 0.26 * d["fs"] * d["qts"] ** -1.4
        return butter_hp4(f, f3) / free
    raise ValueError(kind)


# ------------------------------------------------------------------ the models
# layout: list of (dx, dy) neighbour offsets in nominal-diameter units from the
# miked driver; frame spacing = 1.1 x nominal diameter (frames touching plus a
# baffle web — an ASSUMPTION, stated once here; the terms it scales sit 20 dB
# under the direct sound). `rear` = open-back rear-wave path in metres.
GRID2x2 = [(1, 0), (0, 1), (1, 1)]
MODELS = [
    dict(index=12, label="4x10 Tweed Open", driver="P10R", box=dict(kind="open"),
         neighbours=GRID2x2, rear=0.40,
         note="Jensen P10R (the tweed Bassman / Super Reverb 10-inch) x4, open back, as on its baffle"),
    dict(index=13, label="1x10 Combo", driver="P10R", box=dict(kind="open"),
         neighbours=[], rear=0.30,
         note="Jensen P10R x1, open-back combo"),
    dict(index=14, label="2x15 Guitar", driver="P15N", box=dict(kind="closed", vb=0.1028),
         neighbours=[(0, 1)], rear=None,
         note="Jensen P15N x2, closed back; Vb = 102.8 L per driver: a 1967 Fender Dual Showman 2x15 enclosure, "
              "45.5 x 30 x 11.5 in outside (Vintage & Rare listing 82596), 3/4 in walls"),
    dict(index=15, label="PA 15 + Horn", driver="BS15N350A", box=dict(kind="closed-butterworth"),
         neighbours=[], rear=None, horn="V11", far=True,
         note="Jensen BS-15N350A woofer, Qtc 0.707 closed box, + V-11 compression driver, LR4 at Jensen's "
              "recommended 2.5 kHz, horn padded by 105.8 - 98.7 dB; heard on axis (no close mic)"),
    dict(index=16, label="PA Column", driver="MOD5-30", box=dict(kind="open"),
         neighbours=[], rear=None, far=True,
         note="Jensen MOD 5-30 full-range 5-inch, as on its baffle; a column's drivers add in phase on axis"),
    dict(index=17, label="Hi-Fi Tower", spec=dict(f3=48.0, f6=33.0, top3=28000.0), far=True,
         note="KEF R7 Meta published response: +/-3 dB 48 Hz-28 kHz, -6 dB at 33 Hz (bass reflex); flat between"),
    dict(index=18, label="2x10 Bass", driver="BP10150", box=dict(kind="vented-qb3"),
         neighbours=[(1, 0)], rear=None, note="Jensen BP10150 x2, vented QB3"),
    dict(index=19, label="3x10 Bass", driver="BP10150", box=dict(kind="vented-qb3"),
         neighbours=[(1, 0), (0.5, 0.87)], rear=None, note="Jensen BP10150 x3, vented QB3"),
    dict(index=20, label="6x10 Bass", driver="BP10150", box=dict(kind="vented-qb3"),
         neighbours=[(1, 0), (0, 1), (1, 1), (0, 2), (1, 2)], rear=None, note="Jensen BP10150 x6 (2 x 3), vented QB3"),
    dict(index=21, label="1x12 Bass", driver="BP12250", box=dict(kind="vented-qb3"),
         neighbours=[], rear=None, note="Jensen BP12250 x1, vented QB3"),
    dict(index=22, label="2x12 Bass", driver="BP12250", box=dict(kind="vented-qb3"),
         neighbours=[(0, 1)], rear=None, note="Jensen BP12250 x2, vented QB3"),
    dict(index=23, label="2x15 Bass", driver="BP15250", box=dict(kind="closed-butterworth"),
         neighbours=[(1, 0)], rear=None, note="Jensen BP15250 x2, Qtc 0.707 closed box"),
    dict(index=24, label="4x15 Bass", driver="BP15250", box=dict(kind="closed-butterworth"),
         neighbours=GRID2x2, rear=None, note="Jensen BP15250 x4 (2 x 2), Qtc 0.707 closed box"),
    dict(index=25, label="1x15 + 1x10 Bass", driver="BP15250", box=dict(kind="closed-butterworth"),
         neighbours=[], rear=None, second=dict(driver="BP10150", box=dict(kind="vented-qb3"), offset=(0, 1.0)),
         note="mic on a Jensen BP15250 (Qtc 0.707), a BP10150 above it (vented QB3) heard as the neighbour"),
]


def model_target(m, f, curves):
    """|H(f)| in dB, before level normalisation."""
    if "spec" in m:
        sp = m["spec"]
        # A 2nd-order high-pass through both published points: -3 dB at f3 and
        # -6 dB at f6 fixes (f0, Q) exactly; a 1st-order top at top3.
        # Solve |hp2(f3)| = -3 dB and |hp2(f6)| = -6 dB for (f0, Q).
        from scipy.optimize import least_squares

        def resid(p):
            f0, q = p
            return [db(hp2(np.array([sp["f3"]]), f0, q))[0] + 3.0,
                    db(hp2(np.array([sp["f6"]]), f0, q))[0] + 6.0]

        sol = least_squares(resid, [sp["f3"], 0.6], bounds=([10.0, 0.2], [200.0, 3.0]))
        f0, q = sol.x
        if max(abs(r) for r in sol.fun) > 0.05:
            raise SystemExit("hi-fi spec fit missed: %r" % (sol.fun,))
        top = 1.0 / (1.0 + 1j * f / sp["top3"])
        h = hp2(f, f0, q) * top
        m["_spec_fit"] = (f0, q)
        return db(h)

    d = DRIVERS[m["driver"]]
    main = interp_db(curves[m["driver"]], f)
    h = 10.0 ** (main / 20.0) * box_response(f, m["driver"], m["box"])

    if m.get("horn"):
        horn = DRIVERS[m["horn"]]
        pad = horn["sens"] - d["sens"]
        hd = 10.0 ** ((interp_db(curves[m["horn"]], f) - pad) / 20.0)
        lp, hp = lr4(f, horn["xover"])
        h = h * lp + hd * hp

    if m.get("far"):
        return db(h)

    a = math.sqrt(d["sd"] / math.pi)
    spacing = 1.1 * d["nominal"] * INCH
    k = 2.0 * math.pi * f / C
    total = h.copy()
    for (dx, dy) in m["neighbours"]:
        s = spacing * math.hypot(dx, dy)
        path = math.hypot(s, MIC_R)
        theta = math.atan2(s, MIC_R)
        g = MIC_R / path
        total = total + h * g * piston(f, a, theta) * np.exp(-1j * k * (path - MIC_R))
    if m.get("second"):
        sec = m["second"]
        sd2 = DRIVERS[sec["driver"]]
        h2 = 10.0 ** (interp_db(curves[sec["driver"]], f) / 20.0) * box_response(f, sec["driver"], sec["box"])
        # level-match the two published sensitivities (the curves are dB SPL)
        s = 0.5 * (d["nominal"] + sd2["nominal"]) * INCH * 1.1
        path = math.hypot(s, MIC_R)
        theta = math.atan2(s, MIC_R)
        a2 = math.sqrt(sd2["sd"] / math.pi)
        total = total + h2 * (MIC_R / path) * piston(f, a2, theta) * np.exp(-1j * k * (path - MIC_R))
    if m.get("rear"):
        L = m["rear"]
        total = total - h * (MIC_R / L) * np.exp(-1j * k * (L - MIC_R))
    return db(total)


# --------------------------------------------------------------- min-phase IR
def min_phase_ir(mag_db_full):
    """Cepstral (homomorphic) minimum-phase reconstruction of a magnitude
    given on the rfft grid of NFFT; returns TAPS samples with a half-Hann
    fade over the last 256."""
    logmag = np.log(10.0 ** (mag_db_full / 20.0))
    full = np.concatenate([logmag, logmag[-2:0:-1]])
    cep = np.fft.ifft(full).real
    n = len(cep)
    w = np.zeros(n)
    w[0] = 1.0
    w[1:n // 2] = 2.0
    w[n // 2] = 1.0
    spec = np.exp(np.fft.fft(cep * w))
    h = np.fft.ifft(spec).real[:TAPS]
    fade = 256
    h[-fade:] *= 0.5 * (1.0 + np.cos(np.linspace(0.0, math.pi, fade)))
    return h


def mag_of(ir, n=NFFT):
    H = np.fft.rfft(ir, n)
    f = np.fft.rfftfreq(n, 1.0 / SR)
    return f, db(H)


def log_mean(ir, lo=200.0, hi=4000.0):
    f, m = mag_of(ir)
    fl = np.geomspace(lo, hi, 200)
    return float(np.mean(np.interp(fl, f, m)))


# ------------------------------------------------------------------- header IO
def parse_existing(text):
    start = text.index("alignas(16) static const float kIr48k")
    body = text[text.index("{", start) + 1:]
    rows = []
    depth = 0
    cur = None
    i = 0
    while i < len(body):
        ch = body[i]
        if ch == "{":
            depth += 1
            if depth == 1:
                cur = i
        elif ch == "}":
            if depth == 0:
                break
            depth -= 1
            if depth == 0:
                rows.append(body[cur:i + 1])
        i += 1
    return rows


def row_values(row):
    return np.array([float(v.rstrip("f")) for v in re.findall(r"[-+0-9.eE]+f", row)])


def fmt_row(ir):
    def lit(v):
        s = format(float(np.float32(v)), ".7g")
        if s in ("0", "-0"):
            s = "0.0"
        elif "." not in s and "e" not in s:
            s += ".0"
        return s + "f"
    vals = [lit(v) for v in ir]
    lines = []
    for i in range(0, len(vals), 8):
        lines.append("      " + ", ".join(vals[i:i + 8]) + ",")
    lines[-1] = lines[-1].rstrip(",")
    return "{\n" + "\n".join(lines) + "\n    }"


HEADER_PREAMBLE = """// dcdsp_cab_irs.h — GENERATED cabinet impulse responses. Do not hand-edit.
//
// {count} cab IRs, minimum-phase, authored at 48 kHz, 2048 taps (42.7 ms).
// Models 0-11 are synthetic close-mic IRs. Their recipe (offline generator,
// cepstral min-phase reconstruction of an authored magnitude target): a box LF rolloff, a
// steeper cone-breakup HF rolloff, log-Gaussian body/presence/modal ripple
// bumps and dips, and 2-3 short reflection combs (baffle-edge diffraction
// 0.4-1.1 ms, cab depth bounce 1.5-2.5 ms, a = 0.20-0.42) — the dense notch
// structure a biquad cascade cannot produce. Each IR's log-mean level over
// 200-4000 Hz is normalized to the original bank's, so model-to-model
// loudness (and existing presets) carry over. Minimum-phase => all energy at
// the front, zero reported latency.
//
// Model order matches the catalogue (0-3 BYTE-IDENTICAL to the original bank
// so persisted `model` ints keep their sound; 4-11 appended, 0.16; 12-{last}
// appended 2026-10, DotCharter #74):
//   0 = 1x12 Combo        1 = 2x12 Combo       2 = 4x12 Scooped
//   3 = Vintage 4x12      4 = 1x8 Tweed        5 = 2x12 AC Open
//   6 = 2x12 Boutique Closed  7 = 4x12 Greenback   8 = 4x12 V30 Modern
//   9 = 4x10 Bass         10 = 8x10 Bass Fridge 11 = 1x15 Vintage Bass
{new_index}
// Models 0-11: generator + verified signature table lived in the batch's
// scratch harness and were never committed; their rows are kept verbatim.
// Models 12+ are generated by tools/cab_irs/gen_cab_irs.py from MEASURED
// driver responses (tools/cab_irs/driver_curves.json: Jensen's published
// 1 W / 1 m anechoic IEC-baffle sweeps) through published box / crossover /
// directivity formulas — every source is in that script, and its targets are
// re-checked by the native `cab_irs` test. Re-run it to change them; it copies
// rows 0-11 back untouched.
#ifndef DCDSP_CAB_IRS_H
#define DCDSP_CAB_IRS_H

namespace dcdsp {{
namespace cabir {{

constexpr int    kIrLen     = 2048;
constexpr double kIrRate    = 48000.0;
constexpr int    kNumModels = {count};

alignas(16) static const float kIr48k[kNumModels][kIrLen] = {{
"""

HEADER_TAIL = """
}};

}} // namespace cabir
}} // namespace dcdsp

#endif // DCDSP_CAB_IRS_H
"""


def main():
    check = "--check" in sys.argv
    curves = load_curves()
    text = open(HEADER, encoding="utf-8").read()
    rows = parse_existing(text)
    if len(rows) < 12:
        raise SystemExit("expected at least the 12 original rows, found %d" % len(rows))
    legacy = rows[:12]
    bank = [row_values(r) for r in legacy]
    target_level = float(np.mean([log_mean(ir) for ir in bank]))

    f = np.fft.rfftfreq(NFFT, 1.0 / SR)
    f[0] = f[1] * 0.5
    new_rows, table = [], []
    for m in MODELS:
        tgt = model_target(m, f, curves)
        ir = min_phase_ir(tgt)
        ir *= 10.0 ** ((target_level - log_mean(ir)) / 20.0)
        ir = ir.astype(np.float32).astype(float)
        # reconstruction check against the (level-shifted) target
        ff, mm = mag_of(ir)
        shift = target_level - float(np.mean(np.interp(np.geomspace(200, 4000, 200), f, tgt)))
        band = (ff >= 60.0) & (ff <= 16000.0)
        oct6 = np.geomspace(60.0, 16000.0, 200)
        err = np.interp(oct6, ff, mm) - (np.interp(oct6, f, tgt) + shift)
        peak = int(np.argmax(np.abs(ir)))
        sig = {hz: round(float(np.interp(hz, ff, mm) - target_level), 1)
               for hz in (41, 63, 82, 100, 125, 250, 500, 1000, 2000, 4000, 6000, 8000, 12000)}
        table.append((m["index"], m["label"], peak, round(float(np.max(np.abs(err))), 2), sig))
        new_rows.append((m, ir))

    for idx, label, peak, err, sig in table:
        print(f"{idx:2d} {label:18s} peak@{peak:3d} recon<= {err:4.2f} dB  {sig}")
    if check:
        return

    names = "\n".join(
        "//  {:2d} = {}".format(m["index"], m["label"]) + "  — " + m["note"] for m, _ in new_rows)
    pre = HEADER_PREAMBLE.format(count=12 + len(new_rows), last=11 + len(new_rows), new_index=names)
    body = ",\n    ".join(legacy + [fmt_row(ir) for _, ir in new_rows])
    out = pre + "    " + body + HEADER_TAIL.format()
    with open(HEADER, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(out)
    print("wrote", HEADER)


if __name__ == "__main__":
    main()
