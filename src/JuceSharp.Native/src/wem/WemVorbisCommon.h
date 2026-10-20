// Shared by the .wem decoder and encoder: Vorbis bit I/O (LSB-first), the packed
// codebook library Wwise names codebooks by, and ww2ogg's codebook rebuild.
// Moved verbatim out of WemVorbisDecoder.cpp (ww2ogg port, BSD-style licence).
#pragma once

#include "wem_codebooks.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace dcwem::detail
{

struct ParseError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

[[noreturn]] inline void fail(const char* what) { throw ParseError(what); }

inline int ilog(uint32_t v)
{
    int r = 0;
    while (v != 0) { ++r; v >>= 1; }
    return r;
}

// libvorbis's _book_maptype1_quantvals, as ww2ogg carries it.
inline uint32_t maptype1Quantvals(uint32_t entries, uint32_t dimensions)
{
    if (dimensions == 0) fail("codebook with zero dimensions");
    const int bits = ilog(entries);
    int64_t vals = (int64_t) (entries >> ((bits - 1) * (int) (dimensions - 1) / (int) dimensions));
    for (int guard = 0; guard < 1 << 20; ++guard)
    {
        uint64_t acc = 1, acc1 = 1;
        for (uint32_t i = 0; i < dimensions; ++i)
        {
            acc  *= (uint64_t) vals;
            acc1 *= (uint64_t) (vals + 1);
        }
        if (acc <= entries && acc1 > entries) return (uint32_t) vals;
        if (acc > entries) --vals; else ++vals;
        if (vals < 0) fail("codebook quantvals underflow");
    }
    fail("codebook quantvals did not converge");
}

// LSB-first bit reader (Vorbis/oggpack order, ww2ogg's bit_stream).
class BitReader
{
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    uint32_t read(int bits)
    {
        uint32_t v = 0;
        for (int i = 0; i < bits; ++i)
        {
            if (bitPos_ >= size_ * 8) fail("read past the end of the stream");
            const uint32_t bit = (data_[bitPos_ >> 3] >> (bitPos_ & 7)) & 1u;
            v |= bit << i;
            ++bitPos_;
        }
        return v;
    }

    size_t bitsRead() const noexcept { return bitPos_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t bitPos_ = 0;
};

// LSB-first bit writer.
class BitWriter
{
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) { out_.clear(); }

    void write(uint32_t value, int bits)
    {
        for (int i = 0; i < bits; ++i)
        {
            if ((bitPos_ & 7) == 0) out_.push_back(0);
            if ((value >> i) & 1u) out_.back() |= (uint8_t) (1u << (bitPos_ & 7));
            ++bitPos_;
        }
    }

    // Fast path: whole bytes, at any bit alignment.
    void writeBytes(const uint8_t* p, size_t n)
    {
        const int shift = (int) (bitPos_ & 7);
        if (shift == 0)
        {
            out_.insert(out_.end(), p, p + n);
        }
        else
        {
            uint8_t& last = out_.back();
            if (n == 0) return;
            last |= (uint8_t) (p[0] << shift);
            const size_t base = out_.size();
            out_.resize(base + n);
            for (size_t i = 0; i < n; ++i)
            {
                uint8_t v = (uint8_t) (p[i] >> (8 - shift));
                if (i + 1 < n) v |= (uint8_t) (p[i + 1] << shift);
                out_[base + i] = v;
            }
        }
        bitPos_ += n * 8;
    }

    size_t bitsWritten() const noexcept { return bitPos_; }

    void writeHeader(int type)
    {
        write((uint32_t) type, 8);
        for (const char c : { 'v', 'o', 'r', 'b', 'i', 's' }) write((uint32_t) (uint8_t) c, 8);
    }

private:
    std::vector<uint8_t>& out_;
    size_t bitPos_ = 0;
};

// ww2ogg codebook_library::rebuild(bitstream&, cb_size, ostream&).
inline void rebuildCodebook(BitReader& in, size_t cbSize, BitWriter& out)
{
    const uint32_t dimensions = in.read(4);
    const uint32_t entries    = in.read(14);

    out.write(0x564342, 24);
    out.write(dimensions, 16);
    out.write(entries, 24);

    const uint32_t ordered = in.read(1);
    out.write(ordered, 1);
    if (ordered)
    {
        out.write(in.read(5), 5);   // initial length
        uint32_t current = 0;
        while (current < entries)
        {
            const int bits = ilog(entries - current);
            const uint32_t number = in.read(bits);
            out.write(number, bits);
            current += number;
            if (number == 0 && bits == 0) break;
        }
        if (current > entries) fail("current_entry out of range");
    }
    else
    {
        const uint32_t lengthLength = in.read(3);
        const uint32_t sparse       = in.read(1);
        if (lengthLength == 0 || lengthLength > 5) fail("nonsense codeword length");
        out.write(sparse, 1);
        for (uint32_t i = 0; i < entries; ++i)
        {
            bool present = true;
            if (sparse)
            {
                const uint32_t p = in.read(1);
                out.write(p, 1);
                present = p != 0;
            }
            if (present)
                out.write(in.read((int) lengthLength), 5);
        }
    }

    const uint32_t lookupType = in.read(1);
    out.write(lookupType, 4);
    if (lookupType == 1)
    {
        out.write(in.read(32), 32);   // min
        out.write(in.read(32), 32);   // max
        const uint32_t valueLength = in.read(4);
        out.write(valueLength, 4);
        out.write(in.read(1), 1);     // sequence flag
        const uint32_t quantvals = maptype1Quantvals(entries, dimensions);
        for (uint32_t i = 0; i < quantvals; ++i)
            out.write(in.read((int) valueLength + 1), (int) valueLength + 1);
    }

    // "if all bits are used in the last byte there will be one extra 0 byte"
    if (cbSize != 0 && in.bitsRead() / 8 + 1 != cbSize) fail("codebook size mismatch");
}

class CodebookLibrary
{
public:
    CodebookLibrary(const unsigned char* data, size_t size)
    {
        if (size < 4) return;
        const uint32_t offsetOffset = (uint32_t) data[size - 4] | ((uint32_t) data[size - 3] << 8)
                                    | ((uint32_t) data[size - 2] << 16) | ((uint32_t) data[size - 1] << 24);
        if (offsetOffset >= size) return;
        data_  = data;
        count_ = (int) ((size - offsetOffset) / 4);
        table_ = data + offsetOffset;
    }

    int count() const noexcept { return count_; }   // ww2ogg's codebook_count; count()-1 are usable

    void rebuild(int id, BitWriter& out) const
    {
        if (data_ == nullptr) fail("codebook library not loaded");
        if (id < 0 || id >= count_ - 1) fail("invalid codebook id");
        const uint32_t a = offset(id), b = offset(id + 1);
        if (b < a || b > (uint32_t) (table_ - data_)) fail("corrupt codebook library");
        BitReader in(data_ + a, b - a);
        rebuildCodebook(in, b - a, out);
    }

private:
    uint32_t offset(int i) const
    {
        const unsigned char* p = table_ + (size_t) i * 4;
        return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
    }

    const unsigned char* data_ = nullptr;
    const unsigned char* table_ = nullptr;
    int count_ = 0;
};

inline const CodebookLibrary& library(bool aoTuV)
{
    static const CodebookLibrary def(kPackedCodebooksDefault, kPackedCodebooksDefaultSize);
    static const CodebookLibrary aot(kPackedCodebooksAoTuV603, kPackedCodebooksAoTuV603Size);
    return aoTuV ? aot : def;
}

} // namespace dcwem::detail
