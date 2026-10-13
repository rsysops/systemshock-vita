// A resource file's life as a saved game has it: created with many
// resources, then opened for writing again and again, some resources
// rewritten each time, which leaves the old ones dead in the file until the
// next opening packs it (ResPack).
//
// This writes a copy of the file after every round. The same program is
// built with the resource library as it was and as it is, and the two sets
// of copies must be the same bytes: the packing done in memory gives the
// file the packing done in place gave. It also reads every resource back
// after each round and holds it against what was last written.
// Usage: respack_test <a folder for the copies> [seed]

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "res.h"
#include "res_.h"
#include "resformat.h"
#include "restypes.h"

#define FIRST_ID 100
#define RESOURCES 700 // at the start; a few more are added along the way
#define MAX_IDS 900
#define ROUNDS 14

static struct {
    uint8_t *bytes;
    size_t size;
    uint8_t flags;
} want[MAX_IDS];
static int ids = 0;

static uint64_t rng_state;
static unsigned rnd(unsigned n) {
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(rng_state >> 33) % n;
}

// New content for a resource: a few bytes to tens of kilobytes, such as
// compresses (runs and repeats) when it is to be stored compressed.
static void fill(int k) {
    size_t size = rnd(10) == 0 ? 5000 + rnd(60000) : 1 + rnd(3000), i;

    free(want[k].bytes);
    want[k].bytes = malloc(size);
    want[k].size = size;
    want[k].flags = rnd(3) == 0 ? RDF_LZW : 0;
    for (i = 0; i < size; i++)
        want[k].bytes[i] = want[k].flags ? (uint8_t)((i / (1 + k % 7)) + k) : (uint8_t)rnd(256);
}

static void write_one(int k, int32_t fn) {
    Id id = (Id)(FIRST_ID + k);

    ResMake(id, want[k].bytes, want[k].size, RTYPE_APP, fn, want[k].flags, FORMAT_RAW);
    if (ResWrite(id) == -1) {
        printf("ResWrite failed for $%x\n", id);
        exit(1);
    }
    ResUnmake(id);
}

static void copy_out(const char *file, const char *dir, int round) {
    char name[600], buf[65536];
    FILE *in = fopen(file, "rb"), *out;
    size_t n;

    snprintf(name, sizeof(name), "%s/round-%02d.res", dir, round);
    out = fopen(name, "wb");
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
}

// Every resource read back from the file. Returns how many are wrong.
static int read_back(const char *file) {
    int32_t fn = ResOpenFile(file);
    int k, bad = 0;

    if (fn < 0)
        return ids;
    for (k = 0; k < ids; k++) {
        Id id = (Id)(FIRST_ID + k);
        uint8_t *got;

        if (!ResInUse(id) || RESDESC(id)->fsize != want[k].size) {
            bad++;
            continue;
        }
        got = malloc(want[k].size + 1);
        if (ResExtract(id, FORMAT_RAW, got) == NULL || memcmp(got, want[k].bytes, want[k].size) != 0)
            bad++;
        free(got);
    }
    ResCloseFile(fn);
    return bad;
}

int main(int argc, char **argv) {
    char file[600];
    int32_t fn;
    int round, k, bad = 0, rewritten = 0, read_between = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <a folder for the copies> [seed]\n", argv[0]);
        return 2;
    }
    rng_state = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x2545f4914f6cdd1dull;
    snprintf(file, sizeof(file), "%s/game.res", argv[1]);
    log_set_quiet(1);
    ResInit();

    fn = ResCreateFile(file);
    if (fn < 0) {
        printf("cannot create %s\n", file);
        return 1;
    }
    for (ids = 0; ids < RESOURCES; ids++) {
        fill(ids);
        write_one(ids, fn);
    }
    ResCloseFile(fn);
    copy_out(file, argv[1], 0);
    bad += read_back(file);

    for (round = 1; round <= ROUNDS; round++) {
        // a level's worth rewritten, from a run of ids somewhere in the file
        // as a level's pieces are, or ids here and there; now and then none
        int count = round % 5 == 0 ? 0 : 20 + (int)rnd(50), start = (int)rnd(ids);

        fn = ResEditFile(file, FALSE); // packs what the round before left dead
        if (fn < 0) {
            printf("cannot open %s for writing\n", file);
            return 1;
        }
        for (k = 0; k < count; k++) {
            int which = round % 2 ? (start + k) % ids : (int)rnd(ids);
            fill(which);
            write_one(which, fn);
            rewritten++;
            // Now and then a resource is read from the file between two
            // writes: the next write must find its place again.
            if (round % 3 == 0 && k % 7 == 3) {
                int other = (int)rnd(ids);
                Id id = (Id)(FIRST_ID + other);

                if (ResInUse(id) && RESDESC(id)->fsize == want[other].size) {
                    uint8_t *got = malloc(want[other].size + 1);
                    if (ResExtract(id, FORMAT_RAW, got) == NULL ||
                        memcmp(got, want[other].bytes, want[other].size) != 0)
                        bad++;
                    free(got);
                    read_between++;
                }
            }
        }
        if (round % 4 == 0 && ids < MAX_IDS) { // and a new one
            fill(ids);
            write_one(ids, fn);
            ids++;
        }
        ResCloseFile(fn);
        copy_out(file, argv[1], round);
        bad += read_back(file);
    }
    // a last opening packs what the last round left
    fn = ResEditFile(file, FALSE);
    ResCloseFile(fn);
    copy_out(file, argv[1], ROUNDS + 1);
    bad += read_back(file);

    printf("%d resources, %d rounds, %d rewritten, %d read between two writes: %d read back wrong\n", ids, ROUNDS,
           rewritten, read_between, bad);
    return bad != 0;
}
