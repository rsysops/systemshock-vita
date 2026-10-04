// The original fix_div from src/Libraries/FIX/Source/fix.c, verbatim apart
// from its name and where it stores its overflow code. It lives in its own
// translation unit and is never inlined: a reference inlined differently
// disagrees with the real function at INT32_MIN under -Ofast.

#include <stdint.h>

int ref_ov;

__attribute__((noinline)) int32_t fix_div_ref(int32_t a, int32_t b) {
    if (b == 0) {
        ref_ov = 2;
        int32_t r = 0x7FFFFFFF;
        if (a >= 0) {
            return r;
        }
        return -r;
    }
    ref_ov = 0;
    int64_t r64 = ((int64_t)(a) << 16) / (int64_t)(b);
    int32_t r32 = (int32_t)(r64 & 0xFFFFFFFF);
    if (r64 != (int64_t)r32) {
        ref_ov = 1;
        int32_t r = 0x7FFFFFFF;
        if (a >= 0) {
            return r;
        }
        return -r;
    }
    return r32;
}
