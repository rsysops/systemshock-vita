// The smooth shadows' table of colours: see ShadeLut.h.

#include <stdlib.h>

#include "ShadeLut.h"

static float brightness(const unsigned char *rgb) { return 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2]; }

static int by_value(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

void shade_lut_ratios(float k[SHADE_ROWS], unsigned char constant[256], const unsigned char raw[768],
                      const unsigned char ltab[SHADE_ROWS * 256]) {
    float found[256];
    int r, t;

    for (t = 0; t < 256; t++) {
        constant[t] = 1;
        for (r = 1; r < SHADE_ROWS; r++)
            if (ltab[r * 256 + t] != ltab[t])
                constant[t] = 0;
    }
    for (r = 0; r < SHADE_ROWS; r++) {
        int n = 0;

        // texels bright enough at row 0 for their ratio to mean something;
        // 0 and 255 are the game's own (transparent, black)
        for (t = 1; t < 255; t++) {
            float base = brightness(&raw[3 * ltab[t]]);
            if (base >= 16.0f && !constant[t])
                found[n++] = brightness(&raw[3 * ltab[r * 256 + t]]) / base;
        }
        if (n == 0) {
            k[r] = 1.0f;
        } else {
            qsort(found, (size_t)n, sizeof(found[0]), by_value);
            k[r] = found[n / 2];
        }
    }
    // The last row goes on from the two before it, not to black; then no row
    // under the floor, and none brighter than the row above.
    if (SHADE_ROWS >= 3)
        k[SHADE_ROWS - 1] = 2.0f * k[SHADE_ROWS - 2] - k[SHADE_ROWS - 3];
    for (r = 0; r < SHADE_ROWS; r++) {
        if (k[r] > 1.0f)
            k[r] = 1.0f;
        if (k[r] < SHADE_DARK_FLOOR)
            k[r] = SHADE_DARK_FLOOR;
        if (r > 0 && k[r] > k[r - 1])
            k[r] = k[r - 1];
    }
}

void shade_lut_fill(unsigned char out[SHADE_ROWS * 256 * 4], const float k[SHADE_ROWS],
                    const unsigned char constant[256], const unsigned char raw[768],
                    const unsigned char ltab[SHADE_ROWS * 256], const unsigned char gamma[256], int black255,
                    const unsigned char *only) {
    int r, t, c;

    for (t = 0; t < 256; t++) {
        int index = ltab[t];

        if (only != NULL && !only[t])
            continue;
        for (r = 0; r < SHADE_ROWS; r++) {
            unsigned char *o = &out[(r * 256 + t) * 4];
            float share = constant[t] ? 1.0f : k[r];

            for (c = 0; c < 3; c++) {
                int v = (int)((float)raw[3 * index + c] * share + 0.5f);
                o[c] = black255 && index == 255 ? 0 : gamma[v > 255 ? 255 : v];
            }
            o[3] = 255;
        }
    }
}

int shade_lut_affected(unsigned char only[256], const unsigned char changed[256],
                       const unsigned char ltab[SHADE_ROWS * 256]) {
    int t, n = 0;

    for (t = 0; t < 256; t++) {
        only[t] = changed[ltab[t]] != 0;
        n += only[t];
    }
    return n;
}
