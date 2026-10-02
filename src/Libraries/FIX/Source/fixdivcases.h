#ifndef __FIXDIVCASES_H
#define __FIXDIVCASES_H

// Test inputs for the bit-exactness checks of fix_div's FPU path, shared by
// the on-device self-check (fix.c, profile builds) and tests/fix_div/.

#include <stdint.h>

#define FIXDIV_SEED 0x9E3779B97F4A7C15ULL

static const int32_t fixdiv_edges[] = {
    0,           1,           -1,          2,          -2,
    0x7FFF,      -0x7FFF,     0x8000,      -0x8000,    0xFFFF,
    -0xFFFF,     0x10000,     -0x10000,    0x10001,    -0x10001,
    0x7FFFFFFF,  0x7FFFFFFE,  -0x7FFFFFFF, -0x7FFFFFFE, INT32_MIN,
};
#define FIXDIV_EDGE_COUNT (sizeof(fixdiv_edges) / sizeof(fixdiv_edges[0]))

static uint64_t fixdiv_next(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

// Random sign and a random magnitude of 0 to 31 significant bits.
static int32_t fixdiv_varied(uint64_t *s) {
    uint64_t r = fixdiv_next(s);
    uint32_t m = (uint32_t)(r >> 33) >> (r & 31);
    return (r & 32) ? -(int32_t)m : (int32_t)m;
}

// a for which a * 65536 / b lands within +-3 of q * b / 65536, or a varied
// value if that doesn't fit in 32 bits.
static int32_t fixdiv_near(uint64_t *s, int64_t q, int32_t b, uint64_t r) {
    int64_t a = q * b / 65536 + (int64_t)(r % 7) - 3;
    if (a > INT32_MAX || a < INT32_MIN) {
        return fixdiv_varied(s);
    }
    return (int32_t)a;
}

static void fixdiv_case(uint64_t *s, int32_t *a, int32_t *b) {
    uint64_t r = fixdiv_next(s);

    switch (r & 3) {
    case 0:
    case 1:
        *a = fixdiv_varied(s);
        *b = fixdiv_varied(s);
        return;
    case 2: {
        // Close to an exact quotient, where rounding would bite if it could.
        int32_t d = fixdiv_varied(s);
        if (d == 0) {
            d = 1;
        }
        *a = fixdiv_near(s, fixdiv_varied(s), d, r >> 8);
        *b = d;
        return;
    }
    default: {
        // Quotient at the edge of the fast path's range (+-2^31 - 5 .. +2).
        int32_t d = (int32_t)((r >> 8) & 0xFFFF) + 1;
        int64_t q = 2147483643 + (int64_t)((r >> 24) % 8);
        if (r & 64) {
            d = -d;
        }
        if (r & 128) {
            q = -q;
        }
        *a = fixdiv_near(s, q, d, r >> 32);
        *b = d;
        return;
    }
    }
}

#endif // __FIXDIVCASES_H
