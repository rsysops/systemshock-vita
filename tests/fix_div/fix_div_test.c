// Checks that fix_div (fix.c built with -DFIX_DIV_FPU) matches the original
// int64 fix_div bit for bit, return value and gOVResult, on the edge grid and
// a large number of generated cases. Usage: fix_div_test [cases]

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "fix.h"
#include "fixdivcases.h"

extern int ref_ov;
int32_t fix_div_ref(int32_t a, int32_t b);

static unsigned long long g_checked = 0;
static unsigned long long g_mismatches = 0;

static void check(int32_t a, int32_t b) {
    int32_t want = fix_div_ref(a, b);
    // A non-zero sentinel proves the FPU path still leaves gOVResult at 0.
    gOVResult = 7;
    int32_t got = fix_div(a, b);

    g_checked++;
    if (got != want || gOVResult != ref_ov) {
        if (g_mismatches < 10) {
            printf("  mismatch: a=%" PRId32 " b=%" PRId32 ": want %" PRId32 " ov=%d, got %" PRId32 " ov=%d\n", a, b,
                   want, ref_ov, got, gOVResult);
        }
        g_mismatches++;
    }
}

int main(int argc, char **argv) {
    unsigned long long cases = argc > 1 ? strtoull(argv[1], NULL, 10) : 100000000ULL;
    uint64_t state = FIXDIV_SEED;
    unsigned i, j;
    unsigned long long n;

    for (i = 0; i < FIXDIV_EDGE_COUNT; i++) {
        for (j = 0; j < FIXDIV_EDGE_COUNT; j++) {
            check(fixdiv_edges[i], fixdiv_edges[j]);
        }
    }
    for (n = 0; n < cases; n++) {
        int32_t a, b;
        fixdiv_case(&state, &a, &b);
        check(a, b);
    }

    printf("  %llu mismatches in %llu cases\n", g_mismatches, g_checked);
    return g_mismatches != 0;
}
