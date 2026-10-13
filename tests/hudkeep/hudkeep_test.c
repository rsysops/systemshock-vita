// An outlined text copied from its kept picture (hudkeep_outlined) must be
// the pixels of the nine draws it stands for (draw_shadowed_string), for
// the game's own fonts in every screen mode. The picture's outline is grown
// from one draw: this is where that is held against the nine.
// Usage: hudkeep_test <the game's GAMESCR.RES> [texts per font and mode] [seed]

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The keep itself, as the game builds it, with its scratch canvas in reach.
#include "../../src/GameSrc/hudkeep.c"

#include "gamescr.h"
#include "log.h"
#include "res.h"

// What the libraries and gr2ss.c want from the game and this test never
// comes to.
int32_t gScreenRowbytes;
void *_fr;
uchar full_game_3d;
void mouse_constrain_xy(short xl, short yl, short xh, short yh) {}

#define CANVAS_W 1200
#define CANVAS_H 320
#define TEXTS 400     // per font and mode
#define TEXT_LEN 48   // of a random one
#define MARGIN 8      // of a place well inside the canvas

static uchar *canvas_bits, *background, *drawn;
static grs_canvas canvas;
static grs_screen screen;

static uint64_t rng_state = 0x243f6a8885a308d3ull;
static unsigned rnd(unsigned n) {
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(rng_state >> 33) % n;
}

static const Id fonts[] = {RES_smallTechFont,       RES_tinyTechFont,      RES_mediumTechFont,
                           RES_largeTechFont,       RES_citadelFont,       RES_mediumLEDFont,
                           RES_bigLEDFont,          RES_graffitiFont,      RES_mfdFont,
                           RES_cutsceneFont,        RES_readingFont,       RES_coloraliasedFont,
                           RES_doubleTinyTechFont,  RES_megaTinyTechFont,  RES_doubleMediumLEDFont,
                           RES_megaMediumLEDFont,   RES_tallTinyTechFont,  RES_doubleCutsceneFont};
#define FONTS (int)(sizeof(fonts) / sizeof(fonts[0]))

typedef struct {
    char s[TEXT_CHARS + 64];
    uchar color, shadow;
} text;

// The nine draws, as draw_shadowed_string makes them where the keep declines.
static void nine_draws(char *s, short x, short y, uchar shadow, uchar color) {
    gr_set_fcolor(shadow);
    ss_scale_string(s, x - 1, y - 1);
    ss_scale_string(s, x, y - 1);
    ss_scale_string(s, x + 1, y - 1);
    ss_scale_string(s, x, y + 1);
    ss_scale_string(s, x - 1, y + 1);
    ss_scale_string(s, x + 1, y + 1);
    ss_scale_string(s, x - 1, y);
    ss_scale_string(s, x + 1, y);
    gr_set_fcolor(color);
    ss_scale_string(s, x, y);
}

static void make_text(text *t, const grs_font *f, int which) {
    static const char *const fixed[] = {"",
                                        " ",
                                        "A",
                                        "MAP",
                                        "100%",
                                        "i.i.i",
                                        "Hello, hacker.",
                                        "ENERGY  USAGE: 12 JPM",
                                        "line one\nline two",
                                        "soft\001return",
                                        "soft\002space",
                                        "\n\nlow down",
                                        "trailing   ",
                                        "   leading",
                                        "a char the font hasn't: \003 \004"};
    int n = (int)(sizeof(fixed) / sizeof(fixed[0])), i, len;

    t->color = (uchar)(1 + rnd(255));
    t->shadow = (uchar)(1 + rnd(255));
    if (which < n) {
        strcpy(t->s, fixed[which]);
        return;
    }
    // now and then one too long to keep, by its characters or by its pixels
    len = 1 + rnd(TEXT_LEN);
    if (rnd(40) == 0)
        len = TEXT_CHARS - 8 + rnd(16);
    for (i = 0; i < len; i++) {
        unsigned r = rnd(100);
        if (r < 3)
            t->s[i] = '\n';
        else if (r < 5)
            t->s[i] = 1;
        else if (r < 7)
            t->s[i] = 2;
        else if (r < 20)
            t->s[i] = ' ';
        else if (r < 22)
            t->s[i] = (char)(3 + rnd(252)); // any, the font's or not
        else
            t->s[i] = (char)(f->min + rnd(f->max - f->min + 1));
        if (t->s[i] == '\0')
            t->s[i] = ' ';
    }
    t->s[len] = '\0';
}

typedef struct {
    unsigned asked, copied, declined, bad, edge_asked, edge_bad, dirty;
} tally;

// One asking: the text at (x, y) by the keep, against the nine draws.
static void ask(text *t, grs_font *f, short x, short y, int edge, tally *n, const char *what) {
    size_t bytes = (size_t)CANVAS_W * CANVAS_H, i;
    int kept, bad;

    memcpy(canvas_bits, background, bytes);
    gr_set_font(f);
    nine_draws(t->s, x, y, t->shadow, t->color);
    memcpy(drawn, canvas_bits, bytes);

    memcpy(canvas_bits, background, bytes);
    gr_set_font(f);
    gr_set_fcolor(t->color);
    kept = hudkeep_outlined(t->s, x, y, t->shadow);
    // where it declines it must have drawn nothing: the caller then draws
    bad = memcmp(canvas_bits, kept ? drawn : background, bytes) != 0;

    if (edge)
        n->edge_asked++;
    else
        n->asked++;
    if (kept)
        n->copied++;
    else
        n->declined++;
    if (bad) {
        if (edge)
            n->edge_bad++;
        else
            n->bad++;
        if (n->bad + n->edge_bad <= 3) {
            size_t differing = 0, first = 0;
            for (i = 0; i < bytes; i++)
                if (canvas_bits[i] != (kept ? drawn : background)[i] && differing++ == 0)
                    first = i;
            printf("  %s: %s at %d,%d differs in %zu pixels, the first at %zu,%zu (%zu characters)\n", what,
                   kept ? "the copy" : "a text declined", x, y, differing, first % CANVAS_W, first / CANVAS_W,
                   strlen(t->s));
        }
    }
    // the scratch canvas is left all 0 for the next text
    if (text_bits != NULL) {
        for (i = 0; i < (size_t)TEXT_W * TEXT_H && text_bits[i] == 0; i++)
            ;
        if (i < (size_t)TEXT_W * TEXT_H) {
            if (n->dirty++ == 0)
                printf("  %s: scratch canvas left dirty at %zu,%zu\n", what, i % TEXT_W, i / TEXT_W);
            memset(text_bits, 0, TEXT_W * TEXT_H);
        }
    }
}

// Which text gives up its place, once the keep is full. A page's texts are
// only asked for when the page is redrawn, all at once: they must not
// replace one another, and a text that changes all the time in between (a
// counter) must not push them out.
#define PAGE 100
static int test_replacing(void) {
    grs_font *f = (grs_font *)ResLock(RES_tinyTechFont);
    char s[32];
    unsigned before, made;
    int i, round, status = 0;

    convert_use_mode = 4;
    gr_set_font(f);
    gr_set_fcolor(7);
    for (round = 0; round < 2; round++)
        for (i = 0; i < TEXT_KEPT; i++) {
            snprintf(s, sizeof(s), "OLD %d", i);
            hudkeep_outlined(s, 20, 20, 1);
        }
    for (round = 0; round < 4; round++) {
        before = hudkeep_stats.text_misses;
        for (i = 0; i < PAGE; i++) {
            snprintf(s, sizeof(s), "PAGE %d", i);
            hudkeep_outlined(s, 20, 20, 1);
        }
        made = hudkeep_stats.text_misses - before;
        printf("a page of %d texts, the keep full, redraw %d: %u pictures made\n", PAGE, round + 1, made);
        if (made != (round == 0 ? PAGE : 0))
            status = 1;
    }
    for (i = 0; i < 1000; i++) {
        snprintf(s, sizeof(s), "COUNT %d", i);
        hudkeep_outlined(s, 20, 20, 1);
    }
    before = hudkeep_stats.text_misses;
    for (i = 0; i < PAGE; i++) {
        snprintf(s, sizeof(s), "PAGE %d", i);
        hudkeep_outlined(s, 20, 20, 1);
    }
    made = hudkeep_stats.text_misses - before;
    printf("and after 1000 values of a counter: %u pictures made\n", made);
    if (made != 0)
        status = 1;
    ResUnlock(RES_tinyTechFont);
    return status;
}

int main(int argc, char **argv) {
    static const short modes[][2] = {{480, 272}, {640, 400}, {640, 480}, {960, 544}};
    size_t bytes = (size_t)CANVAS_W * CANVAS_H, i;
    int count = argc > 2 ? atoi(argv[2]) : TEXTS, mode, fi, status = 0;
    unsigned fonts_seen = 0;
    text *pool;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <GAMESCR.RES> [texts per font and mode] [seed]\n", argv[0]);
        return 2;
    }
    if (argc > 3)
        rng_state = strtoull(argv[3], NULL, 0);
    log_set_quiet(1);
    ResInit();
    if (ResOpenFile(argv[1]) < 0) {
        printf("%s: cannot open\n", argv[1]);
        return 1;
    }

    canvas_bits = malloc(bytes);
    background = malloc(bytes);
    drawn = malloc(bytes);
    pool = malloc(sizeof(text) * (size_t)count);
    // any colour, 0 among them: the picture's "nothing here" must leave it be
    for (i = 0; i < bytes; i++)
        background[i] = (uchar)rnd(256);
    memset(&screen, 0, sizeof(screen));
    screen.c = &canvas;
    grd_screen = &screen;
    gr_init_canvas(&canvas, canvas_bits, BMT_FLAT8, CANVAS_W, CANVAS_H);
    gr_set_canvas(&canvas);
    gr_set_fill_type(FILL_NORM);

    // the modes of screen.c
    gr2ss_register_init(0, 320, 200);
    for (mode = 0; mode < 4; mode++)
        gr2ss_register_mode(0, modes[mode][0], modes[mode][1]);
    convert_type = 0;

    for (mode = 1; mode <= 4; mode++) {
        tally all = {0};

        convert_use_mode = (char)mode;
        for (fi = 0; fi < FONTS; fi++) {
            grs_font *f;
            tally n = {0};
            char what[64];
            int k, asks;

            snprintf(what, sizeof(what), "mode %d (%dx%d) font $%x", mode, modes[mode - 1][0], modes[mode - 1][1],
                     fonts[fi]);
            if (fonts[fi] > resDescMax || !ResInUse(fonts[fi])) {
                printf("  %s: not in the file\n", what);
                continue;
            }
            f = (grs_font *)ResLock(fonts[fi]);
            gr_set_font(f);
            gr_set_fcolor(1);
            if (f->id == 0xcccc) {
                // a font of colours is never outlined: the keep must decline
                int kept = hudkeep_outlined("colours", 20, 20, 1);
                printf("  %s: a font of colours, %s\n", what, kept ? "KEPT" : "left alone");
                status |= kept;
                ResUnlock(fonts[fi]);
                continue;
            }
            fonts_seen |= 1u << fi;
            for (k = 0; k < count; k++)
                make_text(&pool[k], f, k);
            // each text several times over and in no order, so that pictures
            // are made, copied again, and given up for others
            asks = 3 * count;
            for (k = 0; k < asks; k++) {
                text *t = &pool[k < count ? k : (int)rnd(count)];
                int edge = rnd(6) == 0;
                short x, y;

                if (edge) { // cut by the canvas's sides, or off it
                    x = (short)(rnd(2) ? -40 + (int)rnd(60) : CANVAS_W - 60 + (int)rnd(80));
                    y = (short)(rnd(2) ? -30 + (int)rnd(40) : CANVAS_H - 30 + (int)rnd(40));
                    if (rnd(3) == 0)
                        x = (short)(MARGIN + rnd(200));
                    else if (rnd(2) == 0)
                        y = (short)(MARGIN + rnd(100));
                } else {
                    x = (short)(MARGIN + rnd(40));
                    y = (short)(MARGIN + rnd(40));
                }
                ask(t, f, x, y, edge, &n, what);
            }
            ResUnlock(fonts[fi]);
            printf("  %s: %u asked, %u copied, %u left to the nine draws, %u differ; cut by an edge: %u, %u differ\n",
                   what, n.asked + n.edge_asked, n.copied, n.declined, n.bad, n.edge_asked, n.edge_bad);
            all.asked += n.asked + n.edge_asked;
            all.copied += n.copied;
            all.bad += n.bad + n.edge_bad;
            all.dirty += n.dirty;
        }
        printf("mode %d: %u asked, %u copied, %u differ\n", mode, all.asked, all.copied, all.bad);
        if (all.bad != 0 || all.dirty != 0 || all.copied == 0)
            status = 1;
    }
    printf("pictures made %u, copied again %u\n", hudkeep_stats.text_misses, hudkeep_stats.text_hits);
    status |= test_replacing();
    if (fonts_seen == 0) {
        printf("no font found in %s\n", argv[1]);
        status = 1;
    }
    return status;
}
