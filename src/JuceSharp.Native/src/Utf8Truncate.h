#pragma once

// Truncating a string into a fixed ABI buffer without splitting a UTF-8
// sequence.
//
// Every string crossing this ABI is UTF-8 and every managed side reads it with
// `[MarshalAs(UnmanagedType.LPUTF8Str)]` / `Encoding.UTF8`, so a buffer that
// ends in the middle of a multi-byte code point does not come back as a
// slightly-short name — it comes back with U+FFFD where the torn bytes were.
//
// That is not cosmetic here. `dca_device_info` writes the SAME device name into
// `id[128]` and `name[256]`, so a name whose 127th byte lands mid-sequence tears
// in the id and not in the name: the two disagree, and selecting the device by
// the id it was enumerated under silently falls through to the fallback walk.
// It is the same failure the `LPStr` mojibake bug produced, from the other end.
// Non-ASCII device names are the norm on a localized Windows ("Lautsprecher
// (Realtek® Audio)", "マイク配列"), and interface names are long.
//
// Cut at a code-point boundary instead. Both ABI string-out helpers
// (`copyTruncated` over juce::String, `copyStdTruncated` over std::string) go
// through this one function so there is one implementation to be right.

#include <cstddef>

namespace dcabi {

/// Bytes of `s` (length `len`) that fit in `limit` bytes without splitting a
/// UTF-8 sequence. Never returns more than `min(len, limit)`. Invalid UTF-8 is
/// passed through byte-wise rather than rejected — this is a truncator, not a
/// validator, and the ABI's producers are JUCE and std::string.
inline std::size_t utf8TruncatedLength(const char* s, std::size_t len, std::size_t limit) noexcept
{
    if (s == nullptr || limit == 0) return 0;
    if (len <= limit) return len;

    // `limit` is the first byte that would be dropped. If it is a continuation
    // byte (10xxxxxx) the cut lands inside a sequence; walk back to that
    // sequence's lead byte.
    std::size_t i = limit;
    while (i > 0 && ((unsigned char) s[i] & 0xC0u) == 0x80u) --i;

    // `s[i]` is now a lead byte (or i == 0). Keep the sequence only if all of
    // it fits; otherwise stop before it.
    const unsigned char c = (unsigned char) s[i];
    std::size_t need = 1;
    if      ((c & 0x80u) == 0x00u) need = 1;
    else if ((c & 0xE0u) == 0xC0u) need = 2;
    else if ((c & 0xF0u) == 0xE0u) need = 3;
    else if ((c & 0xF8u) == 0xF0u) need = 4;
    else                           need = 1;   // stray continuation / invalid lead

    if (i + need <= limit) i += need;
    return i;
}

} // namespace dcabi
