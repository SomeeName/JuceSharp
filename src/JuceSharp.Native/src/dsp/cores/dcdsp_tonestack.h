// dcdsp_tonestack.h — the passive Treble / Middle / Bass ("TMB", "FMV") tone
// stack of the Fender / Marshall family, modelled from its COMPONENT VALUES.
//
// SOURCE (the whole transfer function is theirs, not ours):
//   D. T. Yeh and J. O. Smith, "Discretization of the '59 Fender Bassman Tone
//   Stack", Proc. DAFx-06, Montreal 2006, eqs. (1)-(2), Fig. 1.
//   https://ccrma.stanford.edu/~dtyeh/papers/yeh06_dafx.pdf
// The circuit is a third-order passive RC network, so its response is exact in
// closed form: H(s) = (b1 s + b2 s^2 + b3 s^3) / (1 + a1 s + a2 s^2 + a3 s^3),
// with every coefficient a polynomial in the component values and the three
// pot positions t (treble), m (middle), l (bass). Discretised by the bilinear
// transform with c = 2 fs (the paper's choice: "ideal for frequencies close to
// DC"). Yeh & Smith verified (1) against SPICE and found it unaffected by the
// 1 kOhm cathode-follower source and the 1 MOhm phase-inverter load that the
// Bassman and the Marshalls put either side of it — which is why this header
// models no source or load impedance, and why only CATHODE-FOLLOWER-FED stacks
// may use it (a plate-fed stack such as Fender's AB763 sees ~38 kOhm and is a
// different response; robrobinette.com, "How the TMB Tone Stack Works").
//
// COMPONENT SETS (kTmbStacks) — each row names where its values come from:
//   1 Fender 5F6-A Bassman, the published SCHEMATIC values (Yeh & Smith Fig. 1).
//     Production Bassmans are reported to have left the factory with a 100 k
//     slope resistor and a 0.1 uF bass cap instead (robrobinette.com, as
//     above); the schematic values are the ones a source states, so they are
//     the ones used.
//   2 Marshall JMP50 Model 1987 "Plexi": the 5F6-A with C1 = 500 pF,
//     R4 = 33 k and C2 = C3 = 22 nF (R. Kuehnel, Amp Books, "Fender Bassman
//     Tone Stack": "The JMP50 Model 1987 'Plexi' increases the value of C1 to
//     500pF, decreases the value of R1 [the slope resistor] to 33kOhm, and
//     increases the values of C2 and C3 to 0.022uF").
//
// POT TAPERS. Yeh & Smith: "The Treble and Middle controls use linear
// potentiometers, while the Bass control uses a logarithmic taper". An audio
// ("log") pot is 10 % of its resistance at 50 % rotation (Kuehnel, Amp Books,
// "Circuit Analysis of the Orange AD15": "20dB attenuation at 50-percent
// rotation, 10-percent resistance"); `audioTaper` is the exponential through
// exactly that point, r(x) = (81^x - 1) / 80.
//
// LEVEL. The stack is passive and loses 10-25 dB in the midband — that loss is
// real, and a real amp's gain staging is built around it. The `amp` core's
// stage gains were calibrated around a 0 dB tone stack, so a circuit voicing
// divides the stack by its own log-frequency RMS gain at noon (80 Hz - 5 kHz,
// `normalisationGain`). That moves LEVEL only — every frequency-response shape
// stays the paper's — and is computed from the components, never typed in.
//
// Pure header, no allocation. `tmbAnalog`/`bilinear3` are cheap enough to run
// from AmpCore::setParams (the ParamMailbox applies it at a block boundary on
// the audio thread): a few dozen multiplies.
#pragma once

#include <cmath>
#include <complex>

namespace dcdsp {

enum class PotTaper { Linear, Audio };

/// The component values of one TMB stack, in ohms and farads, named as in
/// Yeh & Smith Fig. 1: C1 treble cap, C2 bass cap, C3 mid cap, R1 treble pot,
/// R2 bass pot, R3 mid pot, R4 slope resistor.
struct TmbComponents {
    double C1, C2, C3;
    double R1, R2, R3, R4;
    PotTaper treble, bass, mid;
};

/// Continuous-time H(s) = (b[1]s + b[2]s^2 + b[3]s^3) / (a[0] + a[1]s + a[2]s^2 + a[3]s^3).
struct TmbAnalog {
    double b[4];
    double a[4];
};

inline double clampUnit(double v) {
    if (!(v > 0.0)) return 0.0;          // NaN lands here
    return v > 1.0 ? 1.0 : v;
}

/// Fraction of the track resistance between the wiper and the low end at
/// rotation `knob` (0..1).
inline double potFraction(double knob, PotTaper taper) {
    const double x = clampUnit(knob);
    if (taper == PotTaper::Linear) return x;
    return (std::pow(81.0, x) - 1.0) / 80.0;   // 10 % at 50 % rotation
}

/// Yeh & Smith (2006) eq. (1), transcribed term by term. t, m, l are the pot
/// FRACTIONS (after the taper), each in [0, 1].
inline TmbAnalog tmbAnalogFractions(const TmbComponents& k, double t, double m, double l) {
    const double C1 = k.C1, C2 = k.C2, C3 = k.C3;
    const double R1 = k.R1, R2 = k.R2, R3 = k.R3, R4 = k.R4;
    const double m2 = m * m;
    const double R3s = R3 * R3;
    TmbAnalog h{};
    h.b[0] = 0.0;
    h.b[1] = t * C1 * R1 + m * C3 * R3 + l * (C1 * R2 + C2 * R2) + (C1 * R3 + C2 * R3);
    h.b[2] = t * (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4)
           - m2 * (C1 * C3 * R3s + C2 * C3 * R3s)
           + m * (C1 * C3 * R1 * R3 + C1 * C3 * R3s + C2 * C3 * R3s)
           + l * (C1 * C2 * R1 * R2 + C1 * C2 * R2 * R4 + C1 * C3 * R2 * R4)
           + l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3)
           + (C1 * C2 * R1 * R3 + C1 * C2 * R3 * R4 + C1 * C3 * R3 * R4);
    const double C123 = C1 * C2 * C3;
    h.b[3] = l * m * (C123 * R1 * R2 * R3 + C123 * R2 * R3 * R4)
           - m2 * (C123 * R1 * R3s + C123 * R3s * R4)
           + m * (C123 * R1 * R3s + C123 * R3s * R4)
           + t * C123 * R1 * R3 * R4 - t * m * C123 * R1 * R3 * R4
           + t * l * C123 * R1 * R2 * R4;
    h.a[0] = 1.0;
    h.a[1] = (C1 * R1 + C1 * R3 + C2 * R3 + C2 * R4 + C3 * R4)
           + m * C3 * R3 + l * (C1 * R2 + C2 * R2);
    h.a[2] = m * (C1 * C3 * R1 * R3 - C2 * C3 * R3 * R4 + C1 * C3 * R3s + C2 * C3 * R3s)
           + l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3)
           - m2 * (C1 * C3 * R3s + C2 * C3 * R3s)
           + l * (C1 * C2 * R2 * R4 + C1 * C2 * R1 * R2 + C1 * C3 * R2 * R4 + C2 * C3 * R2 * R4)
           + (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4 + C1 * C2 * R3 * R4
              + C1 * C2 * R1 * R3 + C1 * C3 * R3 * R4 + C2 * C3 * R3 * R4);
    h.a[3] = l * m * (C123 * R1 * R2 * R3 + C123 * R2 * R3 * R4)
           - m2 * (C123 * R1 * R3s + C123 * R3s * R4)
           + m * (C123 * R3s * R4 + C123 * R1 * R3s - C123 * R1 * R3 * R4)
           + l * C123 * R1 * R2 * R4 + C123 * R1 * R3 * R4;
    return h;
}

/// The same from KNOB positions (0..1), through each pot's taper.
inline TmbAnalog tmbAnalog(const TmbComponents& k, double treble, double mid, double bass) {
    return tmbAnalogFractions(k,
                              potFraction(treble, k.treble),
                              potFraction(mid, k.mid),
                              potFraction(bass, k.bass));
}

/// |H(j 2 pi hz)| of the continuous-time stack.
inline double tmbMagnitude(const TmbAnalog& h, double hz) {
    const std::complex<double> s(0.0, 6.283185307179586 * hz);
    const std::complex<double> num = h.b[1] * s + h.b[2] * s * s + h.b[3] * s * s * s;
    const std::complex<double> den = h.a[0] + h.a[1] * s + h.a[2] * s * s + h.a[3] * s * s * s;
    return std::abs(num / den);
}

/// Yeh & Smith eq. (2): the bilinear transform s = c (1 - z^-1)/(1 + z^-1),
/// c = 2 fs, normalised so A[0] = 1. Returns false (and leaves a wire) when
/// the result is not finite.
inline bool bilinear3(const TmbAnalog& h, double fs, double B[4], double A[4]) {
    const double c = 2.0 * fs, c2 = c * c, c3 = c2 * c;
    const double b1 = h.b[1], b2 = h.b[2], b3 = h.b[3];
    const double a0 = h.a[0], a1 = h.a[1], a2 = h.a[2], a3 = h.a[3];
    const double B0 = -b1 * c - b2 * c2 - b3 * c3;
    const double B1 = -b1 * c + b2 * c2 + 3.0 * b3 * c3;
    const double B2 =  b1 * c + b2 * c2 - 3.0 * b3 * c3;
    const double B3 =  b1 * c - b2 * c2 + b3 * c3;
    const double A0 = -a0 - a1 * c - a2 * c2 - a3 * c3;
    const double A1 = -3.0 * a0 - a1 * c + a2 * c2 + 3.0 * a3 * c3;
    const double A2 = -3.0 * a0 + a1 * c + a2 * c2 - 3.0 * a3 * c3;
    const double A3 = -a0 + a1 * c - a2 * c2 + a3 * c3;
    const double inv = 1.0 / A0;
    B[0] = B0 * inv; B[1] = B1 * inv; B[2] = B2 * inv; B[3] = B3 * inv;
    A[0] = 1.0;      A[1] = A1 * inv; A[2] = A2 * inv; A[3] = A3 * inv;
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(B[i]) || !std::isfinite(A[i])) {
            B[0] = 1.0; B[1] = B[2] = B[3] = 0.0;
            A[1] = A[2] = A[3] = 0.0;
            return false;
        }
    return true;
}

/// The reciprocal of the stack's log-frequency RMS gain over 80 Hz - 5 kHz
/// with every knob at noon: the one LEVEL constant a circuit voicing applies
/// (see LEVEL above). 64 log-spaced points.
inline double normalisationGain(const TmbComponents& k) {
    const TmbAnalog h = tmbAnalog(k, 0.5, 0.5, 0.5);
    const int n = 64;
    double acc = 0.0;
    for (int i = 0; i < n; ++i) {
        const double hz = 80.0 * std::pow(5000.0 / 80.0, (double) i / (double) (n - 1));
        const double g = tmbMagnitude(h, hz);
        acc += g * g;
    }
    const double rms = std::sqrt(acc / n);
    return rms > 1e-9 ? 1.0 / rms : 1.0;
}

/// Third-order IIR, transposed direct form II, DOUBLE coefficients and state —
/// it runs inside the amp's 4x oversampled cascade, where float state would
/// lose the precision poles at z ~= 1 need (see BiquadD).
class ThirdOrderD {
public:
    void setCoeffs(const double B[4], const double A[4]) {
        b0_ = B[0]; b1_ = B[1]; b2_ = B[2]; b3_ = B[3];
        a1_ = A[1]; a2_ = A[2]; a3_ = A[3];
    }
    inline double processD(double x) {
        const double y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y + z3_;
        z3_ = b3_ * x - a3_ * y;
        return y;
    }
    void reset() { z1_ = z2_ = z3_ = 0.0; }

private:
    double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, b3_ = 0.0;
    double a1_ = 0.0, a2_ = 0.0, a3_ = 0.0;
    double z1_ = 0.0, z2_ = 0.0, z3_ = 0.0;
};

/// The component sets a circuit voicing may name (index 1-based in AmpCore's
/// `toneStack` field; 0 there means the legacy shelf trio). See the header
/// comment for each row's source.
inline constexpr int kNumTmbStacks = 2;
inline constexpr TmbComponents kTmbStacks[kNumTmbStacks] = {
    // 1 Fender 5F6-A Bassman (schematic) — Yeh & Smith 2006, Fig. 1.
    { 250e-12, 20e-9, 20e-9,  250e3, 1e6, 25e3, 56e3,
      PotTaper::Linear, PotTaper::Audio, PotTaper::Linear },
    // 2 Marshall JMP50 Model 1987 — Kuehnel: 5F6-A with C1 500 pF, R4 33 k,
    //   C2 = C3 = 22 nF. Pots and tapers as the 5F6-A (the only changes the
    //   source lists are those four parts).
    { 500e-12, 22e-9, 22e-9,  250e3, 1e6, 25e3, 33e3,
      PotTaper::Linear, PotTaper::Audio, PotTaper::Linear },
};

} // namespace dcdsp
