// dcvoice_codec.h — the LOSSLESS sample codec of a version-2 voice pack (#408).
// Plain C++17, no JUCE.
//
// Each zone's int16 samples are one bit stream, written by
// tools/voice/build_voice_pack.py (encode_zone; the stream layout is documented
// there under "ZONE CODEC"): per block of 4096 samples an LPC predictor (order
// 0..32, int16 coefficients and a shift), per partition of 256 samples a Rice
// parameter, then the zig-zagged prediction residuals. Decoding reproduces the
// encoder's int16 samples EXACTLY — the sound of a v2 pack is its v1 pack's.
//
// THREADS: decodeZone is allocation-free but it is run only at PACK LOAD
// (parsePack, control thread). Never on the audio thread: the sampler plays the
// decoded int16 buffer, exactly as it plays a version-1 pack.
#pragma once

#include <cstddef>
#include <cstdint>

namespace dcvoice::codec {

inline constexpr uint32_t kBlock = 4096;
inline constexpr uint32_t kPartition = 256;
inline constexpr int kMaxOrder = 32;
inline constexpr int kMaxRice = 24;
/// The longest unary prefix (u >> k) a well-formed stream holds; the encoder
/// picks k so it stays below this.
inline constexpr uint32_t kMaxQuotient = 65535;

/// MSB-first reader over [p, p + n). Every read is bounds-checked: a read past
/// the end sets `bad` and returns 0, so a truncated stream fails, never reads on.
/// Reads go through a 64-bit window (bytes past the end read as zero, and any
/// read that lands past the end is caught by the position check).
class BitReader
{
public:
    BitReader(const uint8_t* p, size_t n) noexcept : p_(p), n_(n) {}

    /// `count` (0..32) bits as an unsigned number.
    uint32_t bits(int count) noexcept
    {
        if (count == 0)
            return 0;
        const uint64_t w = window();
        if (! advance((size_t) count))
            return 0;
        return (uint32_t) (w >> (64 - count));
    }

    /// Zeros up to the next 1 (consumed). Fails past kMaxQuotient.
    uint32_t unary() noexcept
    {
        uint32_t q = 0;
        for (;;)
        {
            // The window's top 56 bits are always stream (or the zero padding
            // past its end): a 1 among them ends the run, else take all 56.
            const uint64_t w = window();
            if ((w >> 8) != 0)
            {
                const int z = leadingZeros(w);
                q += (uint32_t) z;
                if (q > kMaxQuotient || ! advance((size_t) z + 1))
                    break;
                return q;
            }
            q += 56;
            if (q > kMaxQuotient || ! advance(56))
                break;
        }
        bad = true;
        return 0;
    }

    /// True when the stream ended in its LAST byte and the padding bits after
    /// it are zero, as the encoder writes them: a damaged stream almost never
    /// decodes to exactly its own length.
    bool endsCleanly() const noexcept
    {
        if (bad || n_ == 0 || pos_ <= (n_ - 1) * 8 || pos_ > n_ * 8)
            return false;
        const int pad = (int) (n_ * 8 - pos_);
        return pad == 0 || (p_[n_ - 1] & ((1u << pad) - 1u)) == 0;
    }

    bool bad = false;

private:
    uint64_t window() const noexcept
    {
        const size_t byte = pos_ >> 3;
        uint64_t w = 0;
        if (byte + 8 <= n_)
            for (int i = 0; i < 8; ++i)
                w = (w << 8) | p_[byte + (size_t) i];
        else
            for (int i = 0; i < 8; ++i)
                w = (w << 8) | (byte + (size_t) i < n_ ? p_[byte + (size_t) i] : 0u);
        return w << (pos_ & 7);
    }

    bool advance(size_t count) noexcept
    {
        pos_ += count;
        if (pos_ <= n_ * 8)
            return true;
        bad = true;
        return false;
    }

    static int leadingZeros(uint64_t w) noexcept
    {
        int z = 0;
        if ((w >> 32) == 0) { z += 32; w <<= 32; }
        if ((w >> 48) == 0) { z += 16; w <<= 16; }
        if ((w >> 56) == 0) { z += 8; w <<= 8; }
        if ((w >> 60) == 0) { z += 4; w <<= 4; }
        if ((w >> 62) == 0) { z += 2; w <<= 2; }
        if ((w >> 63) == 0) z += 1;
        return z;
    }

    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
};

/// Decodes one zone's stream ([in, in + n)) into exactly `length` samples at
/// `out`. False on anything malformed: a read past the end, an order above 32,
/// a Rice parameter above 24, a quotient above 65535, a sample outside int16,
/// or a stream that does not end in its last byte with zero padding.
/// Allocation-free; the history before the zone's first sample is zero.
inline bool decodeZone(const uint8_t* in, size_t n, int16_t* out, uint32_t length) noexcept
{
    if (in == nullptr || out == nullptr)
        return false;
    BitReader r(in, n);
    int32_t coef[kMaxOrder] {};
    for (uint32_t s = 0; s < length;)
    {
        const uint32_t blockEnd = s + (length - s < kBlock ? length - s : kBlock);
        const int order = (int) r.bits(6);
        int shift = 0;
        if (order > kMaxOrder)
            return false;
        if (order > 0)
        {
            shift = (int) r.bits(4);
            for (int j = 0; j < order; ++j)
                coef[j] = (int32_t) (int16_t) (uint16_t) r.bits(16);
        }
        if (r.bad)
            return false;
        while (s < blockEnd)
        {
            const uint32_t partEnd = s + (blockEnd - s < kPartition ? blockEnd - s : kPartition);
            const int k = (int) r.bits(5);
            if (k > kMaxRice || r.bad)
                return false;
            for (; s < partEnd; ++s)
            {
                const uint32_t q = r.unary();
                const uint64_t u = ((uint64_t) q << k) | r.bits(k);
                if (r.bad)
                    return false;
                const int64_t e = (int64_t) (u >> 1) ^ -(int64_t) (u & 1);
                int64_t acc = 0;
                const int avail = s < (uint32_t) order ? (int) s : order;
                for (int j = 0; j < avail; ++j)
                    acc += (int64_t) coef[j] * out[s - 1 - (uint32_t) j];
                // >> on a negative int64 is an arithmetic shift on every
                // compiler this builds with (and defined so from C++20); the
                // encoder (numpy) floors the same way.
                const int64_t x = e + (acc >> shift);
                if (x < -32768 || x > 32767)
                    return false;
                out[s] = (int16_t) x;
            }
        }
    }
    return r.endsCleanly();
}

} // namespace dcvoice::codec
