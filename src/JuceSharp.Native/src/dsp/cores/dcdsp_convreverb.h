// dcdsp_convreverb.h — uniform-partitioned FFT convolution reverb (pure dcdsp core).
//
// Models nothing: it plays back a real room's impulse response, so the "model"
// is whatever WAV the player loaded. Topology: predelay -> uniformly
// partitioned overlap-save convolution -> bounded low shelf + high cut -> mix.
// Partition size P is the next power of two at or above the block size given at
// prepare(); the FFT is 2P, the frequency-delay line holds one input spectrum
// per partition, and an IR shorter than kDirectMax taps skips the FFT entirely
// and runs as a plain dot product over the same input history.
//
// Easy to get wrong, three ways. (1) The FIFO means the wet is P samples late,
// so the DRY must be delayed by P too and latencySamples() must say P — a
// convolution that reports zero puts every parallel branch a partition out.
// (2) The IR is immutable state the AUDIO thread reads and the OWNER thread
// replaces; it is published as a shared_ptr snapshot and the retired one is
// parked in a RetirementQueue, because whoever drops the last reference runs
// the free, and that must never be the callback. (3) The only allocation-free
// way to change IR length is to rebuild OFF the callback — `lengthPct` is a
// knob that costs an FFT pass per partition on the owner thread, not a knob the
// audio thread can honour.
#pragma once
#ifndef DCDSP_PRIMITIVES_H
#include "dcdsp_primitives.h"
#endif
#include "dcdsp_detail_fir.h"
#include "dcdsp_hallreverb.h"   // revdetail::RampedPredelay, clampNaN
#include "../Retirement.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

namespace dcdsp {
namespace convdetail {

/// Real-input FFT of a power-of-two size N, built on an N/2 complex FFT plus
/// the usual split-radix unpacking. Spectra are stored as (N/2 + 1) interleaved
/// complex floats — the packing that makes a partition cost 2 floats per tap
/// instead of 4, which is the difference between 16 MB and 33 MB of IR storage
/// at the 10-second cap.
///
/// NOT RE-ENTRANT, and not thread-safe against itself: `forward` and `inverse`
/// both stage their input into the member scratch `z_` and transform it in
/// place. One instance therefore belongs to exactly ONE thread. `ConvReverbCore`
/// owns two — `fft_` for the callback's `runHop()` and `buildFft_` for the
/// owner thread's `rebuildAndPublish()` — because sharing one meant every IR
/// load and every `lengthPct` write corrupted whichever transform the audio
/// thread was midway through, for the duration of the rebuild.
class RealFft
{
public:
    void prepare(int n)
    {
        n_ = n;
        m_ = n / 2;
        tw_.assign((std::size_t) std::max(2, m_), 0.f);          // cos/sin, m_/2 pairs
        for (int k = 0; k < m_ / 2; ++k)
        {
            const double a = -2.0 * kPi * (double) k / (double) m_;
            tw_[(std::size_t) (2 * k)]     = (float) std::cos(a);
            tw_[(std::size_t) (2 * k + 1)] = (float) std::sin(a);
        }
        post_.assign((std::size_t) (m_ + 1) * 2u, 0.f);
        for (int k = 0; k <= m_; ++k)
        {
            const double a = -2.0 * kPi * (double) k / (double) n_;
            post_[(std::size_t) (2 * k)]     = (float) std::cos(a);
            post_[(std::size_t) (2 * k + 1)] = (float) std::sin(a);
        }
        z_.assign((std::size_t) m_ * 2u, 0.f);
    }

    int size() const noexcept { return n_; }
    /// Floats in one spectrum: (N/2 + 1) interleaved complex.
    int specFloats() const noexcept { return (m_ + 1) * 2; }

    /// `time` holds N real samples; `spec` receives specFloats() floats.
    void forward(const float* time, float* spec) noexcept
    {
        for (int k = 0; k < m_; ++k)
        {
            z_[(std::size_t) (2 * k)]     = time[2 * k];
            z_[(std::size_t) (2 * k + 1)] = time[2 * k + 1];
        }
        fft(z_.data(), m_, false);
        for (int k = 0; k <= m_; ++k)
        {
            const int kk = (m_ - k) % m_;
            const float zr = z_[(std::size_t) (2 * k % (2 * m_))];
            const float zi = z_[(std::size_t) ((2 * k + 1) % (2 * m_))];
            const float cr =  z_[(std::size_t) (2 * kk)];
            const float ci = -z_[(std::size_t) (2 * kk + 1)];
            const float er = 0.5f * (zr + cr), ei = 0.5f * (zi + ci);
            const float dr = zr - cr,          di = zi - ci;
            const float or_ = 0.5f * di, oi = -0.5f * dr;      // (Z - conj)/2i
            const float wr = post_[(std::size_t) (2 * k)], wi = post_[(std::size_t) (2 * k + 1)];
            spec[2 * k]     = er + (or_ * wr - oi * wi);
            spec[2 * k + 1] = ei + (or_ * wi + oi * wr);
        }
    }

    /// `spec` holds specFloats() floats; `time` receives N real samples.
    void inverse(const float* spec, float* time) noexcept
    {
        for (int k = 0; k < m_; ++k)
        {
            const int kk = m_ - k;
            const float xr = spec[2 * k],  xi = spec[2 * k + 1];
            const float cr = spec[2 * kk], ci = -spec[2 * kk + 1];
            const float er = 0.5f * (xr + cr), ei = 0.5f * (xi + ci);
            const float dr = 0.5f * (xr - cr), di = 0.5f * (xi - ci);
            // * conj(post) = e^{+i 2 pi k / N}
            const float wr = post_[(std::size_t) (2 * k)], wi = -post_[(std::size_t) (2 * k + 1)];
            const float or_ = dr * wr - di * wi;
            const float oi  = dr * wi + di * wr;
            z_[(std::size_t) (2 * k)]     = er - oi;      // Xe + i*Xo
            z_[(std::size_t) (2 * k + 1)] = ei + or_;
        }
        fft(z_.data(), m_, true);
        const float s = 1.f / (float) m_;
        for (int k = 0; k < m_; ++k)
        {
            time[2 * k]     = z_[(std::size_t) (2 * k)] * s;
            time[2 * k + 1] = z_[(std::size_t) (2 * k + 1)] * s;
        }
    }

    /// Multiply-accumulate two packed spectra: acc += a * b.
    static void mulAdd(const float* a, const float* b, float* acc, int floats) noexcept
    {
        for (int i = 0; i < floats; i += 2)
        {
            const float ar = a[i], ai = a[i + 1], br = b[i], bi = b[i + 1];
            acc[i]     += ar * br - ai * bi;
            acc[i + 1] += ar * bi + ai * br;
        }
    }

private:
    void fft(float* z, int m, bool inv) const noexcept
    {
        for (int i = 1, j = 0; i < m; ++i)
        {
            int bit = m >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j)
            {
                std::swap(z[2 * i], z[2 * j]);
                std::swap(z[2 * i + 1], z[2 * j + 1]);
            }
        }
        for (int len = 2; len <= m; len <<= 1)
        {
            const int step = m / len;
            const int half = len >> 1;
            for (int i = 0; i < m; i += len)
                for (int k = 0; k < half; ++k)
                {
                    const int   t  = 2 * (k * step);
                    const float wr = tw_[(std::size_t) t];
                    const float wi = inv ? -tw_[(std::size_t) (t + 1)] : tw_[(std::size_t) (t + 1)];
                    const int   a  = 2 * (i + k);
                    const int   b  = 2 * (i + k + half);
                    const float vr = z[b] * wr - z[b + 1] * wi;
                    const float vi = z[b] * wi + z[b + 1] * wr;
                    z[b]     = z[a] - vr;
                    z[b + 1] = z[a + 1] - vi;
                    z[a]     += vr;
                    z[a + 1] += vi;
                }
        }
    }

    int                n_ = 2, m_ = 1;
    std::vector<float> tw_, post_;
    std::vector<float> z_;          // the per-instance staging scratch; see the class note
};

/// One immutable IR snapshot: what the audio thread convolves with. Built on
/// the owner thread, published as a shared_ptr, retired through a
/// RetirementQueue. Nothing in here is ever mutated after construction.
///
/// It carries its OWN geometry (`part`, `specFloats`) and that is load-bearing:
/// a device change re-derives the partition size from the new block size, and a
/// snapshot partitioned at the old size walked with the new stride is garbage.
/// Nothing could detect the mismatch while the snapshot only described its
/// length.
struct IrSnapshot
{
    std::vector<float> spectra;    // nParts * specFloats, partitioned + transformed
    std::vector<float> direct;     // time-domain taps, when the IR fits in one dot product
    int                taps    = 0;
    int                nParts  = 0;
    int                part    = 0;   // partition size this was built for
    int                specFloats = 0;// floats per spectrum at that partition size
    bool               isDirect = false;
};

} // namespace convdetail

struct ConvReverbParams {
    float predelayMs = 0.f;    // 0 .. 200 ms (ramped tap)
    float lengthPct  = 100.f;  // 10 .. 100 % (OWNER-side rebuild, not read here)
    float lowCutHz   = 40.f;   // 20 .. 500 Hz; 20 = off
    float highCutKHz = 16.f;   // 2 .. 20 kHz; 20 = off
    float mix        = 0.3f;   // 0 .. 1
};

class ConvReverbCore {
public:
    /// The IR length cap. TEN SECONDS, and additionally 480,000 taps per
    /// channel — which is ten seconds at 48 kHz, five at 96 kHz and two and a
    /// half at 192 kHz. The tap cap is not a second opinion about the duration:
    /// it is the memory bound. Every tap costs 2 floats in each live snapshot
    /// and 2 more in the frequency-delay line per channel, so 480,000 taps is
    /// already ~16 MB per instance and a flat ten seconds at 192 kHz would be
    /// four times that for a single pedal on a single seat.
    static constexpr double kMaxIrSeconds = 10.0;
    static constexpr int    kMaxIrTaps    = 480000;
    /// An IR at or under this many taps skips the FFT and runs as a dot product
    /// over the input history the overlap-save path already keeps.
    static constexpr int    kDirectMax    = 256;

    ConvReverbCore() = default;
    ConvReverbCore(const ConvReverbCore&)            = delete;
    ConvReverbCore& operator=(const ConvReverbCore&) = delete;

    // ---- owner thread ------------------------------------------------------

    void prepare(double sampleRate, int maxBlockSize, int /*numChannels*/)
    {
        sr_       = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = maxBlockSize > 0 ? maxBlockSize : 512;

        part_ = 1;
        while (part_ < maxBlock_) part_ <<= 1;
        if (part_ < 64) part_ = 64;
        fft_.prepare(2 * part_);
        // The owner thread transforms with its OWN instance and its own pad
        // buffer, both sized here so `rebuildAndPublish()` never touches a
        // vector the callback is reading and never allocates the pad per call.
        buildFft_.prepare(2 * part_);
        pad_.assign((std::size_t) (2 * part_), 0.f);
        specFloats_ = fft_.specFloats();

        maxTaps_  = std::min(kMaxIrTaps, (int) std::floor(kMaxIrSeconds * sr_));
        maxParts_ = (maxTaps_ + part_ - 1) / part_;

        for (int c = 0; c < kChannels; ++c)
        {
            timeIn_[c].assign((std::size_t) (2 * part_), 0.f);
            fdl_[c].assign((std::size_t) maxParts_ * (std::size_t) specFloats_, 0.f);
            inFifo_[c].assign((std::size_t) (part_ + maxBlock_), 0.f);
            outFifo_[c].assign((std::size_t) (2 * part_ + maxBlock_), 0.f);
            dryLine_[c].assign((std::size_t) (part_ + maxBlock_ + 1), 0.f);
            dryTmp_[c].assign((std::size_t) maxBlock_, 0.f);
        }
        for (int s = 0; s < 2; ++s)
        {
            acc_[s].assign((std::size_t) specFloats_, 0.f);
            wet_[s].assign((std::size_t) part_, 0.f);
        }
        scratch_.assign((std::size_t) (2 * part_), 0.f);

        for (int c = 0; c < kChannels; ++c) pre_[c].prepare(sr_, kMaxPredelayMs);
        mixS_.reset(sr_, kGainRampSeconds);
        dryS_.reset(sr_, kGainRampSeconds);
        fadeStep_ = 1.f / std::max(1.f, (float) (kIrFadeSeconds * sr_));

        applyAll();
        mixS_.snapToTarget();
        dryS_.snapToTarget();
        for (int c = 0; c < kChannels; ++c) pre_[c].snap();
        reset();

        // The IR is USER CONTENT and survives a device change: if a file was
        // loaded it is re-converted from the ORIGINAL decoded samples to the
        // new rate, never from an already-converted copy.
        if (src_.empty()) buildDefaultSource();
        convDirty_ = true;             // the rate and the tap cap both just moved
        rebuildAndPublish();
    }

    /// STATE only — this is also DspChain's bypass-park clear, so it must never
    /// touch the loaded IR.
    void reset()
    {
        for (int c = 0; c < kChannels; ++c)
        {
            std::fill(timeIn_[c].begin(), timeIn_[c].end(), 0.f);
            std::fill(fdl_[c].begin(), fdl_[c].end(), 0.f);
            std::fill(inFifo_[c].begin(), inFifo_[c].end(), 0.f);
            std::fill(outFifo_[c].begin(), outFifo_[c].end(), 0.f);
            std::fill(dryLine_[c].begin(), dryLine_[c].end(), 0.f);
        }
        for (int c = 0; c < kChannels; ++c) pre_[c].reset();
        shelfL_.reset(); shelfR_.reset(); lpL_.reset(); lpR_.reset();
        inFill_  = 0;
        outFill_ = part_;          // the FIFO's P samples of latency, prefilled
        dryPos_  = 0;
        fdlPos_  = 0;
        fade_    = 1.f;
        next_    = nullptr;
        // `current_` goes with them, and so does the generation the callback
        // has seen. The loaded IR (`src_`, `lengthPct_`) is untouched — this
        // drops only the pointer to a PARTITIONED snapshot, and a prepare() at
        // a new block size has just made that snapshot the wrong geometry.
        // adoptPendingIr() then takes the freshly published one with NO fade,
        // through the `current_ == nullptr` branch it already had; keeping the
        // stale pointer instead crossfaded 20 ms of a snapshot walked with the
        // new stride.
        current_ = nullptr;
        seenGen_ = 0;
    }

    /// Owner thread. `samples`/`count`/`rate` are a decoded mono IR exactly as
    /// the WAV carried it. Returns false and CHANGES NOTHING if it is unusable,
    /// so a bad file leaves the previous IR playing rather than going silent.
    bool setIr(const float* samples, int count, double rate)
    {
        if (samples == nullptr || count <= 0 || !(rate > 0.0)) return false;
        int n = 0;
        for (int i = 0; i < count; ++i) if (std::isfinite(samples[i]) && samples[i] != 0.f) ++n;
        if (n == 0) return false;                       // silence is not an IR
        src_.assign(samples, samples + count);
        for (float& v : src_) if (!std::isfinite(v)) v = 0.f;
        srcRate_   = rate;
        userIr_    = true;
        convDirty_ = true;
        rebuildAndPublish();
        return true;
    }

    /// Owner thread. Back to the built-in default IR.
    void clearIr()
    {
        userIr_    = false;
        buildDefaultSource();
        convDirty_ = true;
        rebuildAndPublish();
    }

    /// OWNER THREAD ONLY. A truncation is an IR rebuild, never a knob the
    /// callback can honour: it re-partitions and re-transforms the whole
    /// snapshot. Everything it touches is owner-thread state (see
    /// `rebuildAndPublish`), and the rate conversion is cached, so a slider
    /// drag costs one partition+transform per tick and not a resample too.
    void setLengthPct(float pct)
    {
        const float v = revdetail::clampNaN(pct, 10.f, 100.f);
        if (v == lengthPct_) return;
        lengthPct_ = v;
        rebuildAndPublish();
    }
    float lengthPct() const { return lengthPct_; }
    bool  hasUserIr() const { return userIr_; }
    /// Test hook: taps in the snapshot the audio thread is currently playing.
    int   activeTapsForTest() const
    { auto p = std::atomic_load(&published_); return p == nullptr ? 0 : p->taps; }
    /// Test hook: how many times the decoded source has been rate-converted and
    /// normalised. Flat across a `lengthPct` sweep; bumps on setIr/clearIr/prepare.
    int   irConversionsForTest() const { return conversions_; }
    /// Test hook: the partition size the published snapshot was built for.
    int   publishedPartitionForTest() const
    { auto p = std::atomic_load(&published_); return p == nullptr ? 0 : p->part; }

    // ---- audio thread ------------------------------------------------------

    /// Reaches the core through the adapter's ParamMailbox. `lengthPct` is
    /// deliberately NOT read here — see setLengthPct().
    void setParams(const ConvReverbParams& p)
    {
        ConvReverbParams q;
        q.predelayMs  = revdetail::clampNaN(p.predelayMs, 0.f, kMaxPredelayMs);
        q.lengthPct   = revdetail::clampNaN(p.lengthPct, 10.f, 100.f);
        q.lowCutHz    = revdetail::clampNaN(p.lowCutHz, 20.f, 500.f);
        q.highCutKHz  = revdetail::clampNaN(p.highCutKHz, 2.f, 20.f);
        q.mix         = revdetail::clampNaN(p.mix, 0.f, 1.f);
        p_ = q;
        applyAll();
    }

    void process(float* const* channels, int numChannels, int numFrames)
    {
        if (numChannels <= 0 || numFrames <= 0) return;
        if (numFrames > maxBlock_) return;               // oversized: skipped
        float* L = channels[0];
        float* R = numChannels > 1 ? channels[1] : nullptr;
        if (L == nullptr) return;

        adoptPendingIr();
        if (current_ == nullptr) return;                 // nothing to convolve with

        const int dsz = (int) dryLine_[0].size();
        for (int n = 0; n < numFrames; ++n)
        {
            const float inL = L[n];
            const float inR = R != nullptr ? R[n] : inL;
            // One predelay per channel: the IR is mono, so keeping L and R
            // separate through the convolution is what preserves the player's
            // own stereo image in the wet.
            inFifo_[0][(std::size_t) (inFill_ + n)] = pre_[0].process(inL);
            inFifo_[1][(std::size_t) (inFill_ + n)] = pre_[1].process(inR);
            // The dry is delayed by exactly one partition so it lines up with
            // the wet the FIFO is holding back.
            dryLine_[0][(std::size_t) dryPos_] = inL;
            dryLine_[1][(std::size_t) dryPos_] = inR;
            int dr = dryPos_ - part_;
            if (dr < 0) dr += dsz;
            dryTmp_[0][(std::size_t) n] = dryLine_[0][(std::size_t) dr];
            dryTmp_[1][(std::size_t) n] = dryLine_[1][(std::size_t) dr];
            if (++dryPos_ >= dsz) dryPos_ = 0;
        }
        inFill_ += numFrames;
        while (inFill_ >= part_)
        {
            runHop();
            for (int c = 0; c < kChannels; ++c)
                std::memmove(inFifo_[c].data(), inFifo_[c].data() + part_,
                             sizeof(float) * (std::size_t) (inFill_ - part_));
            inFill_ -= part_;
        }

        // Drain: `numFrames` wet samples out of the output FIFO, and the dry
        // that belongs with them out of the one-partition delay.
        for (int n = 0; n < numFrames; ++n)
        {
            float wL = outFifo_[0][(std::size_t) n];
            float wR = outFifo_[1][(std::size_t) n];
            if (useShelf_) { wL = shelfL_.process(wL); wR = shelfR_.process(wR); }
            if (useLp_)    { wL = lpL_.process(wL);    wR = lpR_.process(wR); }

            const float m = mixS_.next(), d = dryS_.next();
            const float outL = wL * m + dryTmp_[0][(std::size_t) n] * d;
            const float outR = wR * m + dryTmp_[1][(std::size_t) n] * d;
            if (R != nullptr) { L[n] = outL; R[n] = outR; }
            else              L[n] = 0.5f * (outL + outR);
        }
        for (int c = 0; c < kChannels; ++c)
            std::memmove(outFifo_[c].data(), outFifo_[c].data() + numFrames,
                         sizeof(float) * (std::size_t) (outFill_ - numFrames));
        outFill_ -= numFrames;
    }

    /// The uniform-partition FIFO costs exactly one partition of latency, and a
    /// convolution that reports zero puts every parallel branch a partition out.
    int latencySamples() const { return part_; }

private:
    static constexpr int    kChannels         = 2;
    static constexpr float  kMaxPredelayMs    = 200.f;
    static constexpr float  kGainRampSeconds  = 0.010f;
    static constexpr double kIrFadeSeconds    = 0.020;
    static constexpr double kTruncFadeSeconds = 0.020;
    static constexpr double kLowShelfDb       = -15.0;
    static constexpr double kIrEnergy         = 0.5;

    void applyAll()
    {
        for (int c = 0; c < kChannels; ++c) pre_[c].setDelayMs(p_.predelayMs);
        // Both wet-path filters are OFF at the end of their range, not merely
        // "nearly transparent": at 20 Hz / 20 kHz the module is a pure
        // convolution, which is what makes an exactness test possible at all.
        useShelf_ = p_.lowCutHz > 20.001f;
        useLp_    = p_.highCutKHz < 19.999f;
        if (useShelf_)
            for (Biquad* b : { &shelfL_, &shelfR_ })
                b->setLowShelf(sr_, (double) p_.lowCutHz, 0.707, kLowShelfDb);
        if (useLp_)
            for (Biquad* b : { &lpL_, &lpR_ })
                // nearNyquist (DotCharter #87): the knob runs 2-20 kHz, and a
                // 16 kHz cut (the default) was a 12 kHz cut at 48 kHz.
                b->setLowpass(sr_, (double) p_.highCutKHz * 1000.0, 0.707, true);
        mixS_.setTarget(p_.mix);
        dryS_.setTarget(1.f - p_.mix);
    }

    // ---- IR publication ----------------------------------------------------

    void buildDefaultSource()
    {
        // A 0.5 s synthetic decaying noise burst, so the module makes a sound
        // with no file loaded. Deterministic: same seed, same reverb, every run.
        const int n = std::max(64, std::min(maxTaps_, (int) (0.5 * sr_)));
        src_.assign((std::size_t) n, 0.f);
        uint32_t rng = 0x5EED1234u;
        for (int i = 0; i < n; ++i)
        {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            const float w = ((float) (rng >> 8) / (float) (1u << 24)) * 2.f - 1.f;
            const float env = std::exp(-6.5f * (float) i / (float) n);
            src_[(std::size_t) i] = w * env;
        }
        src_[0] = 1.0f;                     // a direct arrival, so it has a front
        srcRate_ = sr_;
    }

    /// ENERGY normalisation, on the owner thread, and it is not cosmetic. The
    /// shared WAV reader bounds an IR by its L1 norm (up to +30 dB worst-case
    /// convolution gain), which is the right guard for a 2,048-tap CABINET and
    /// a 40 dB ATTENUATION for a two-second room — L1 grows with length, and a
    /// reverb IR is mostly length. Normalising to a fixed energy undoes that
    /// and gives the far more useful property that swapping IRs does not move
    /// the wet level.
    ///
    /// It runs on the CONVERTED taps, not on the decoded source, and that is
    /// load-bearing: `detail::resampleIr` scales by srcRate/dstRate to preserve
    /// the FIR's DC gain, which moves its ENERGY by the same factor — so
    /// normalising the source made the wet jump 6 dB on a 48 -> 96 kHz device
    /// change while every other rate-owned quantity was re-derived correctly.
    static void normalise(float* h, int n)
    {
        double e = 0.0;
        for (int i = 0; i < n; ++i) e += (double) h[i] * (double) h[i];
        if (!(e > 1.0e-20)) return;
        const float g = (float) (kIrEnergy / std::sqrt(e));
        for (int i = 0; i < n; ++i) h[i] *= g;
    }

    /// Owner thread. Rate-converts and normalises the decoded source ONCE into
    /// `conv_`, and only when something the conversion depends on has moved
    /// (`setIr`, `clearIr`, `prepare`). A `lengthPct` drag issues one rebuild
    /// per UI tick and the resample is by far the most expensive step in it —
    /// re-running it to throw away a different amount of the tail was work for
    /// a result that cannot change.
    void refreshConverted()
    {
        if (!convDirty_) return;
        convDirty_ = false;
        convTaps_  = 0;
        conv_.assign((std::size_t) maxTaps_, 0.f);
        int taps = 0;
        if (std::fabs(srcRate_ - sr_) < 1.0)
        {
            taps = std::min((int) src_.size(), maxTaps_);
            std::copy_n(src_.data(), taps, conv_.data());
        }
        else
        {
            taps = detail::resampleIr(src_.data(), (int) src_.size(), srcRate_,
                                      conv_.data(), maxTaps_, sr_);
        }
        if (taps <= 0) return;
        normalise(conv_.data(), taps);
        convTaps_ = taps;
        ++conversions_;
    }

    /// Owner thread. Converts (once), truncates, partitions, transforms and
    /// publishes. Every buffer it touches — `conv_`, `work_`, `pad_`,
    /// `buildFft_` — is owner-thread state; the ONLY thing the audio thread
    /// ever sees is the finished snapshot, handed over atomically.
    void rebuildAndPublish()
    {
        retired_.sweep();
        if (src_.empty() || part_ <= 0) return;

        // 1. rate-convert from the ORIGINAL decoded samples (cached)
        refreshConverted();
        int taps = convTaps_;
        if (taps <= 0) return;
        work_.assign(conv_.begin(), conv_.begin() + taps);

        // 2. truncate with a fade, so a shortened IR ends rather than stops
        const int kept = std::max(8, std::min(taps,
            (int) std::lround((double) taps * (double) lengthPct_ * 0.01)));
        if (kept < taps)
        {
            const int fade = std::min(kept, (int) std::lround(kTruncFadeSeconds * sr_));
            for (int i = 0; i < fade; ++i)
                work_[(std::size_t) (kept - fade + i)] *=
                    1.f - (float) i / (float) std::max(1, fade);
        }
        taps = kept;

        // 3. partition + transform
        auto snap = std::make_shared<convdetail::IrSnapshot>();
        snap->taps       = taps;
        snap->part       = part_;
        snap->specFloats = specFloats_;
        if (taps <= kDirectMax && taps <= part_)
        {
            snap->isDirect = true;
            snap->nParts   = 0;
            snap->direct.assign(work_.data(), work_.data() + taps);
        }
        else
        {
            const int nParts = (taps + part_ - 1) / part_;
            snap->nParts = nParts;
            snap->spectra.assign((std::size_t) nParts * (std::size_t) specFloats_, 0.f);
            if ((int) pad_.size() != 2 * part_) pad_.assign((std::size_t) (2 * part_), 0.f);
            for (int m = 0; m < nParts; ++m)
            {
                std::fill(pad_.begin(), pad_.end(), 0.f);
                const int off = m * part_;
                const int len = std::min(part_, taps - off);
                std::copy_n(work_.data() + off, len, pad_.data());
                buildFft_.forward(pad_.data(), snap->spectra.data() + (std::size_t) m * (std::size_t) specFloats_);
            }
        }

        auto old = std::atomic_load(&published_);
        std::atomic_store(&published_, snap);
        gen_.fetch_add(1, std::memory_order_release);
        // Parked, not dropped: the callback may still be holding `old`, and
        // whoever drops the last reference runs the free.
        retired_.retire(std::move(old));
    }

    void adoptPendingIr() noexcept
    {
        const uint32_t g = gen_.load(std::memory_order_acquire);
        if (g == seenGen_) return;
        seenGen_ = g;
        // The shared_ptr atomics take a process-wide CRT spin lock on the MSVC
        // STL, so this is behind the generation counter: an idle block never
        // touches it at all (namamp's does, and pays for it every block).
        auto p = std::atomic_load(&published_);
        if (p == nullptr || p == current_) return;
        // A snapshot built for a DIFFERENT partition size is not playable here:
        // runHop() walks `spectra` with the CURRENT stride. Drop it rather than
        // read it — the owner thread always republishes at the new geometry
        // from inside prepare(), so this is a skip of at most one generation.
        if (p->part != part_ || p->specFloats != specFloats_) return;
        if (current_ == nullptr) { current_ = p; fade_ = 1.f; next_ = nullptr; return; }
        next_ = p;
        fade_ = 0.f;
    }

    /// One partition of input -> one partition of wet output.
    void runHop() noexcept
    {
        const int nOut = (next_ != nullptr) ? 2 : 1;
        for (int c = 0; c < kChannels; ++c)
        {
            // Overlap-save: the FFT window is the previous partition plus this
            // one, and only the second half of the inverse transform is output.
            std::memmove(timeIn_[c].data(), timeIn_[c].data() + part_,
                         sizeof(float) * (std::size_t) part_);
            std::copy_n(inFifo_[c].data(), part_, timeIn_[c].data() + part_);

            float* slot = fdl_[c].data() + (std::size_t) fdlPos_ * (std::size_t) specFloats_;
            fft_.forward(timeIn_[c].data(), slot);

            for (int s = 0; s < nOut; ++s)
            {
                const auto& set = (s == 0) ? current_ : next_;
                if (set->isDirect)
                {
                    const float* h = set->direct.data();
                    const int    t = set->taps;
                    const float* x = timeIn_[c].data() + part_;   // this hop's samples
                    for (int i = 0; i < part_; ++i)
                    {
                        float a = 0.f;
                        for (int k = 0; k < t; ++k) a += h[k] * x[i - k];
                        wet_[s][(std::size_t) i] = a;
                    }
                }
                else
                {
                    std::fill(acc_[s].begin(), acc_[s].end(), 0.f);
                    const int nParts = std::min(set->nParts, maxParts_);
                    for (int m = 0; m < nParts; ++m)
                    {
                        int idx = fdlPos_ - m;
                        if (idx < 0) idx += maxParts_;
                        convdetail::RealFft::mulAdd(
                            fdl_[c].data() + (std::size_t) idx * (std::size_t) specFloats_,
                            set->spectra.data() + (std::size_t) m * (std::size_t) specFloats_,
                            acc_[s].data(), specFloats_);
                    }
                    fft_.inverse(acc_[s].data(), scratch_.data());
                    std::copy_n(scratch_.data() + part_, part_, wet_[s].data());
                }
            }

            float* dst = outFifo_[c].data() + outFill_;
            if (nOut == 1)
            {
                std::copy_n(wet_[0].data(), part_, dst);
            }
            else
            {
                // Equal-gain crossfade over kIrFadeSeconds. Both channels walk
                // the SAME ramp, so the image cannot tilt while it runs.
                float f = fade_;
                for (int i = 0; i < part_; ++i)
                {
                    dst[i] = wet_[0][(std::size_t) i]
                           + (wet_[1][(std::size_t) i] - wet_[0][(std::size_t) i]) * f;
                    f = std::min(1.f, f + fadeStep_);
                }
            }
        }
        if (nOut == 2)
        {
            fade_ = std::min(1.f, fade_ + fadeStep_ * (float) part_);
            if (fade_ >= 1.f) { current_ = next_; next_ = nullptr; fade_ = 1.f; }
        }
        outFill_ += part_;
        if (++fdlPos_ >= maxParts_) fdlPos_ = 0;
    }

    // audio-thread state
    convdetail::RealFft fft_;
    std::vector<float>  timeIn_[kChannels], fdl_[kChannels];
    std::vector<float>  inFifo_[kChannels], outFifo_[kChannels];
    std::vector<float>  dryLine_[kChannels], dryTmp_[kChannels];
    std::vector<float>  acc_[2], wet_[2], scratch_;
    std::shared_ptr<convdetail::IrSnapshot> current_, next_;
    revdetail::RampedPredelay pre_[kChannels];
    Biquad                    shelfL_, shelfR_, lpL_, lpR_;
    SmoothedValue             mixS_, dryS_;
    ConvReverbParams          p_{};
    uint32_t                  seenGen_ = 0;
    int  part_ = 64, specFloats_ = 2, maxTaps_ = 64, maxParts_ = 1, maxBlock_ = 512;
    int  inFill_ = 0, outFill_ = 0, dryPos_ = 0, fdlPos_ = 0;
    float fade_ = 1.f, fadeStep_ = 1.f;
    bool  useShelf_ = true, useLp_ = true;
    double sr_ = 48000.0;

    // owner-thread state. NOTHING here is read by the callback, which is the
    // whole point of buildFft_ being a second instance: RealFft stages into a
    // member scratch, so the one the callback uses cannot be borrowed.
    convdetail::RealFft buildFft_;
    std::vector<float> src_, conv_, work_, pad_;
    double             srcRate_    = 48000.0;
    float              lengthPct_  = 100.f;
    bool               userIr_     = false;
    bool               convDirty_  = true;
    int                convTaps_   = 0;
    int                conversions_ = 0;

    // shared
    std::shared_ptr<convdetail::IrSnapshot>            published_;
    std::atomic<uint32_t>                              gen_{ 0 };
    RetirementQueue<convdetail::IrSnapshot>            retired_;
};

} // namespace dcdsp
