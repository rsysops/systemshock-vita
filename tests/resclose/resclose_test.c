// ResCloseFile must say when writing a resource file failed: a save puts the
// game's file in its slot only if it was written whole. The card running out
// of room is played by the file size limit (RLIMIT_FSIZE): past it, writes
// fail as on a full card.
// Usage: resclose_test <a folder to work in>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include "log.h"
#include "res.h"
#include "res_.h"
#include "resformat.h"
#include "restypes.h"

#define FIRST_ID 100
#define RESOURCES 40
#define SIZE 3000 // bytes each: some 120 KB in all

static uint8_t bytes[RESOURCES + 1][50000];

static void fill(int k, size_t size) {
    size_t i;

    for (i = 0; i < size; i++)
        bytes[k][i] = (uint8_t)(i * 7 + k * 13 + (i >> 5));
}

static void write_one(int k, size_t size, int32_t fn) {
    Id id = (Id)(FIRST_ID + k);

    ResMake(id, bytes[k], size, RTYPE_APP, fn, 0, FORMAT_RAW);
    ResWrite(id);
    ResUnmake(id);
}

// Writes the resources into a new file; returns what closing it says.
static int32_t create(const char *file) {
    int32_t fn = ResCreateFile(file);
    int k;

    if (fn < 0)
        return 99;
    for (k = 0; k < RESOURCES; k++)
        write_one(k, SIZE, fn);
    return ResCloseFile(fn);
}

// From now on no file may grow past `bytes`, or with -1 as it was.
static struct rlimit was;
static void limit(long bytes) {
    struct rlimit small = was;

    if (bytes < 0) {
        setrlimit(RLIMIT_FSIZE, &was);
        return;
    }
    small.rlim_cur = (rlim_t)bytes;
    setrlimit(RLIMIT_FSIZE, &small);
}

static long size_of(const char *file) {
    FILE *f = fopen(file, "rb");
    long n = -1;

    if (f != NULL && fseek(f, 0, SEEK_END) == 0)
        n = ftell(f);
    if (f != NULL)
        fclose(f);
    return n;
}

static int expect(int good, const char *what) {
    if (!good)
        printf("  %s\n", what);
    return !good;
}

int main(int argc, char **argv) {
    char file[600], full[600];
    int32_t fn, rv;
    int bad = 0, k;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <a folder to work in>\n", argv[0]);
        return 2;
    }
    snprintf(file, sizeof(file), "%s/game.res", argv[1]);
    snprintf(full, sizeof(full), "%s/full.res", argv[1]);
    log_set_quiet(1);
    ResInit();
    signal(SIGXFSZ, SIG_IGN);
    getrlimit(RLIMIT_FSIZE, &was);
    for (k = 0; k <= RESOURCES; k++)
        fill(k, sizeof(bytes[k]));

    // written whole: 0, and it reads back
    rv = create(file);
    bad += expect(rv == 0, "a file written whole: its closing said it failed");
    fn = ResOpenFile(file);
    if (fn >= 0) {
        uint8_t got[SIZE];
        Id id = (Id)(FIRST_ID + RESOURCES / 2);

        bad += expect(ResInUse(id) && ResExtract(id, FORMAT_RAW, got) != NULL && memcmp(got, bytes[RESOURCES / 2], SIZE) == 0,
                      "a file written whole: a resource doesn't read back");
        // opened only to read: 0
        bad += expect(ResCloseFile(fn) == 0, "a file read: its closing said it failed");
    } else
        bad += expect(0, "a file written whole: it doesn't open");

    // no room past 20 KB, while writing a new one: -1
    limit(20000);
    rv = create(full);
    limit(-1);
    bad += expect(rv == -1, "a new file that ran out of room: its closing said it was written");

    // no room past what it holds, while adding to one: -1
    limit(size_of(file));
    fn = ResEditFile(file, FALSE);
    if (fn >= 0) {
        write_one(RESOURCES, sizeof(bytes[RESOURCES]), fn);
        rv = ResCloseFile(fn);
    } else
        rv = 99;
    limit(-1);
    bad += expect(rv == -1, "a file added to that ran out of room: its closing said it was written");

    remove(file);
    remove(full);
    printf("4 closings: %d wrong\n", bad);
    return bad != 0;
}
