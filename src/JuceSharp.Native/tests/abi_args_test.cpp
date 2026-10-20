// dcabi::copyBlockOut — the tail of dca_plugin_save_state.
//
// A plugin state past INT32_MAX used to be cast to a negative int32_t, and the
// copy length min(capacity, negative) then reached memcpy as a huge size_t. No
// test can make a real plugin produce 2 GiB of state, so the decision lives in
// one pure function and is pinned here: an oversized block is refused before
// any cast or copy, and the ordinary cases still report and copy as before.
//
// Not vacuous: the oversized case passes a data pointer that must never be
// read; a copy would crash the test.

#include "AbiArgs.h"

#include <cstdint>
#include <cstdio>
#include <limits>

namespace
{
int failures = 0;

void check(bool ok, const char* what)
{
    if (! ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
} // namespace

int main()
{
    const std::size_t int32Max = (std::size_t) std::numeric_limits<int32_t>::max();

    check(dcabi::fitsInt32(0), "0 fits");
    check(dcabi::fitsInt32(int32Max), "INT32_MAX fits");
    check(! dcabi::fitsInt32(int32Max + 1), "INT32_MAX + 1 does not fit");

    {
        // Never dereferenced: the block is refused before the copy.
        const auto* poison = reinterpret_cast<const void*>(std::uintptr_t{ 0x10 });
        uint8_t buffer[8] = {};
        int32_t out = 1234;
        check(! dcabi::copyBlockOut(poison, int32Max + 1, buffer, (int32_t) sizeof(buffer), &out),
              "a block over INT32_MAX is refused");
        check(out == 0, "a refused block reports size 0");
    }
    {
        const uint8_t data[5] = { 1, 2, 3, 4, 5 };
        uint8_t buffer[3] = {};
        int32_t out = -1;
        check(dcabi::copyBlockOut(data, sizeof(data), buffer, (int32_t) sizeof(buffer), &out),
              "a small block is accepted");
        check(out == 5, "the whole size is reported even when the buffer is smaller");
        check(buffer[0] == 1 && buffer[2] == 3, "the first `capacity` bytes are copied");
    }
    {
        const uint8_t data[4] = { 9, 9, 9, 9 };
        uint8_t buffer[4] = {};
        int32_t out = 0;
        check(dcabi::copyBlockOut(data, sizeof(data), buffer, -7, &out) && out == 4 && buffer[0] == 0,
              "a negative capacity copies nothing and still reports the size");
        check(dcabi::copyBlockOut(data, sizeof(data), nullptr, 4, &out) && out == 4,
              "a size query (null buffer) reports the size");
    }

    if (failures == 0) std::puts("PASS: oversized plugin state is refused before the int32 cast");
    return failures == 0 ? 0 : 1;
}
