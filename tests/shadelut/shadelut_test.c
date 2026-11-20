// The smooth shadows' table of colours (src/MacSrc/ShadeLut.c), on made-up
// tables and, given the game's DATA folder, on its own palette, light table
// and infrared table. Usage: shadelut_test [DATA folder]

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ShadeLut.h"

static int failures;

static void fail(const char *what, int r, int t, const char *detail) {
    if (failures++ < 20)
        printf("  FAIL %s: row %d, texel %d: %s\n", what, r, t, detail);
}

// Shock.c's gamma row for a setting from 10 to 100 (the default is 29)
static void gamma_row(unsigned char row[256], int setting) {
    double gamma = (double)setting / 100;
    int j;

    gamma = 1 - gamma;
    gamma *= gamma;
    gamma = 1 - gamma;
    gamma = 1 / (gamma * 2.2);
    for (j = 0; j < 256; j++)
        row[j] = (unsigned char)(pow((double)j / 255, gamma) * 255);
}

static const unsigned char *at(const unsigned char *out, int r, int t) { return &out[(r * 256 + t) * 4]; }

// After some palette entries change, making again only the texels they
// affect gives the table made whole from the new palette.
static void partial(const char *name, const unsigned char raw[768], const unsigned char ltab[SHADE_ROWS * 256],
                    const float k[SHADE_ROWS], const unsigned char constant[256], const unsigned char gamma[256],
                    const unsigned char before[SHADE_ROWS * 256 * 4]) {
    static unsigned char patched[SHADE_ROWS * 256 * 4], whole[SHADE_ROWS * 256 * 4], newer[768];
    unsigned char changed[256], only[256];
    int i, t, n, want = 0;

    memcpy(newer, raw, sizeof(newer));
    memset(changed, 0, sizeof(changed));
    // a cycling range, as the game's: a few entries shifted
    for (i = 0; i < 6; i++) {
        int e = 3 + 7 * i;
        newer[3 * e] = (unsigned char)(newer[3 * e] + 40);
        newer[3 * e + 2] = (unsigned char)(newer[3 * e + 2] ^ 0x5A);
        changed[e] = 1;
    }
    for (t = 0; t < 256; t++)
        want += changed[ltab[t]];
    n = shade_lut_affected(only, changed, ltab);
    if (n != want)
        fail("affected count", -1, n, "not what the changed entries give");
    memcpy(patched, before, sizeof(patched));
    shade_lut_fill(patched, k, constant, newer, ltab, gamma, 1, only);
    shade_lut_fill(whole, k, constant, newer, ltab, gamma, 1, NULL);
    if (memcmp(patched, whole, sizeof(whole)) != 0)
        fail("partial fill", -1, -1, "differs from the whole table");
    printf("%s: %d texels made again after 6 palette entries changed\n", name, n);
}

// What every table must be. `infrared`: all its rows are the same.
static void check(const char *name, const unsigned char raw[768], const unsigned char ltab[SHADE_ROWS * 256],
                  int infrared) {
    static unsigned char fitted[SHADE_ROWS * 256 * 4], gamma[256];
    unsigned char constant[256];
    float k[SHADE_ROWS];
    int r, t, c, before = failures;

    gamma_row(gamma, 29);
    shade_lut_ratios(k, constant, raw, ltab);
    shade_lut_fill(fitted, k, constant, raw, ltab, gamma, 1, NULL);
    printf("%s: ratios", name);
    for (r = 0; r < SHADE_ROWS; r++)
        printf(" %.3f", k[r]);
    printf("\n");

    for (r = 0; r < SHADE_ROWS; r++) {
        if (k[r] < SHADE_DARK_FLOOR || k[r] > 1.0f || (r > 0 && k[r] > k[r - 1]))
            fail("ratio out of order or under the floor", r, -1, "");
    }
    for (t = 0; t < 256; t++) {
        int index = ltab[t], black = index == 255;

        for (r = 0; r < SHADE_ROWS; r++) {
            const unsigned char *f = at(fitted, r, t);

            if (f[3] != 255)
                fail("alpha", r, t, "not 255");
            for (c = 0; c < 3; c++) {
                // never brighter than the row above
                if (r > 0 && f[c] > at(fitted, r - 1, t)[c])
                    fail("fitted brighter than the row above", r, t, "");
                // row 0 is the palette's colour, through the gamma
                if (r == 0 && f[c] != (black ? 0 : gamma[raw[3 * index + c]]))
                    fail("fitted row 0", r, t, "not gamma(palette)");
                // a texel the same in every row keeps its colour
                if (constant[t] && f[c] != at(fitted, 0, t)[c])
                    fail("constant texel", r, t, "changed");
                if (black && f[c] != 0)
                    fail("index 255", r, t, "not black");
            }
        }
        // the darkest row keeps at least the floor's share of the colour
        if (!constant[t] && !black) {
            for (c = 0; c < 3; c++) {
                int want = (int)(raw[3 * index + c] * SHADE_DARK_FLOOR + 0.5f);
                if (at(fitted, SHADE_ROWS - 1, t)[c] < gamma[want])
                    fail("darkest row under the floor", SHADE_ROWS - 1, t, "");
            }
        }
        if (infrared && !constant[t])
            fail("infrared", -1, t, "a texel not the same in every row");
    }
    partial(name, raw, ltab, k, constant, gamma, fitted);
    printf("%s: %s\n", name, failures == before ? "ok" : "FAILED");
}

// Made up: a grey ramp, each row darker by a fifteenth, a few fullbright
// texels, texel 200 on index 255
static void made_up(void) {
    static unsigned char raw[768], ltab[SHADE_ROWS * 256], flat[SHADE_ROWS * 256];
    int r, t;

    for (t = 0; t < 256; t++)
        raw[3 * t] = raw[3 * t + 1] = raw[3 * t + 2] = (unsigned char)t;
    for (r = 0; r < SHADE_ROWS; r++) {
        for (t = 0; t < 256; t++) {
            int v = t * (SHADE_ROWS - 1 - r) / (SHADE_ROWS - 1);
            ltab[r * 256 + t] = (unsigned char)(t % 37 == 0 ? t : v);
            flat[r * 256 + t] = (unsigned char)t;
        }
        ltab[r * 256 + 200] = 255;
    }
    check("made up", raw, ltab, 0);
    check("made up, every row the same", raw, flat, 1);
}

static int read_file(const char *dir, const char *name, long offset, unsigned char *to, size_t bytes) {
    char path[1024];
    FILE *f;
    size_t got;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (f == NULL) {
        printf("no %s\n", path);
        return 0;
    }
    fseek(f, offset, SEEK_SET);
    got = fread(to, 1, bytes, f);
    fclose(f);
    return got == bytes;
}

static void game(const char *dir) {
    static unsigned char raw[768], ltab[SHADE_ROWS * 256], bw[SHADE_ROWS * 256];
    int r;

    // GAMEPAL.RES: the palette is its first resource, after the header
    if (!read_file(dir, "GAMEPAL.RES", 128, raw, sizeof(raw)) || !read_file(dir, "SHADTABL.DAT", 0, ltab, sizeof(ltab)) ||
        !read_file(dir, "BWTABL.DAT", 0, bw, sizeof(bw))) {
        failures++;
        return;
    }
    // as Init_Lighting (textmaps.c) loads them
    for (r = 0; r < SHADE_ROWS; r++) {
        ltab[r * 256] = 0xFF;
        bw[r * 256] = 0;
    }
    check("the game's light table", raw, ltab, 0);
    check("the game's infrared table", raw, bw, 1);
}

int main(int argc, char **argv) {
    made_up();
    if (argc > 1)
        game(argv[1]);
    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures != 0;
}
