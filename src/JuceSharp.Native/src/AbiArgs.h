#pragma once

// Argument validation helpers for the ABI boundary.
//
// A NaN or an infinity crossing this ABI is a caller bug, and every one of them
// is UNRECOVERABLE once it is past the boundary:
//
//   * a non-finite master / monitor / input gain is stored as the ramp's
//     "current" value, so `rampOrScale` multiplies the whole block by NaN and
//     the output scrub then zeroes the master bus — for good, because the next
//     block ramps FROM the stored NaN;
//   * a non-finite EQ / filter frequency, Q or gain produces NaN biquad
//     coefficients, and a `juce::dsp::IIR` feeds its own output back, so the
//     band is silent until an off->on transition resets it;
//   * a non-finite pitch or tempo reaches Rubber Band through `jmax(0.05, x)`,
//     which is NaN-transparent in exactly the wrong direction.
//
// `DspChain::setParam` has rejected NaN since the rig shipped for precisely
// this reason (every core's clamp is NaN-transparent: `v < lo` and `v > hi` are
// both false for NaN). The engine setters did not. They do now, through this
// header, so there is one predicate rather than 30 hand-written `isfinite`s.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace dcabi {

/// True when `n` fits an int32_t out-param or a JUCE `int` size. Check BEFORE
/// the cast: a size_t over INT32_MAX narrows to a negative int32, and a negative
/// count handed to memcpy or a JUCE API becomes a huge size_t again.
inline bool fitsInt32(std::size_t n) noexcept
{
    return n <= (std::size_t) std::numeric_limits<int32_t>::max();
}

/// The tail of a "size query + copy" export (dca_plugin_save_state): reports the
/// whole size in *out and copies at most `capacity` bytes of it. A block that
/// does not fit int32 is refused (false, *out = 0) before any cast or copy; a
/// null buffer or a capacity <= 0 copies nothing.
inline bool copyBlockOut(const void* data, std::size_t size, uint8_t* buffer, int32_t capacity,
                         int32_t* out) noexcept
{
    *out = 0;
    if (! fitsInt32(size)) return false;
    *out = (int32_t) size;
    if (buffer != nullptr && capacity > 0 && size > 0 && data != nullptr)
        std::memcpy(buffer, data, std::min((std::size_t) capacity, size));
    return true;
}

/// True when every argument is a finite real number (not NaN, not +/-Inf).
inline bool finite(float v) noexcept  { return std::isfinite(v); }
inline bool finite(double v) noexcept { return std::isfinite(v); }

template <typename First, typename... Rest>
inline bool finite(First first, Rest... rest) noexcept
{
    return finite(first) && finite(rest...);
}

} // namespace dcabi
