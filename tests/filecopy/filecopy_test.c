// file_copy must leave at the destination exactly the source's bytes, for
// files of any size, whatever was there before; and a source that can't be
// read must leave the destination alone. file_replace must do the same for a
// slot, which must keep its old bytes or get all of the new ones wherever it
// stops; and file_replace_finish must settle whatever it left.
// Usage: filecopy_test <a folder to work in> [a real file to copy too]

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "filecopy.h"

static char dir[512];

static const char *path(const char *name) {
    static char p[4][600];
    static int at;
    at = (at + 1) % 4;
    snprintf(p[at], sizeof(p[at]), "%s/%s", dir, name);
    return p[at];
}

// rename as the Vita's C library does it (vitasdk's _rename_r): the
// destination removed first, then the rename. With fail_rename at n, the nth
// call from then on fails after the removal, as a stop there would leave it.
// (run.sh links with --wrap=rename, so filecopy.c calls this one.)
int __real_rename(const char *from, const char *to);
static int fail_rename;

int __wrap_rename(const char *from, const char *to) {
    remove(to);
    if (fail_rename > 0 && --fail_rename == 0)
        return -1;
    return __real_rename(from, to);
}

static unsigned char *read_all(const char *name, long *size) {
    FILE *f = fopen(name, "rb");
    unsigned char *bytes;

    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    bytes = malloc(*size + 1);
    if (fread(bytes, 1, (size_t)*size, f) != (size_t)*size) {
        free(bytes);
        bytes = NULL;
    }
    fclose(f);
    return bytes;
}

static void write_all(const char *name, const unsigned char *bytes, long size) {
    FILE *f = fopen(name, "wb");
    fwrite(bytes, 1, (size_t)size, f);
    fclose(f);
}

// `size` bytes of something else at `name`; none (-1): no file there
static void put_junk(const char *name, long size) {
    unsigned char *junk;

    if (size < 0) {
        remove(name);
        return;
    }
    junk = malloc(size + 1);
    memset(junk, 0xa5, (size_t)size);
    write_all(name, junk, size);
    free(junk);
}

// 1 unless `name` holds exactly `size` bytes of `bytes` (-1: isn't there)
static int differs(const char *name, const unsigned char *bytes, long size) {
    long got = -1;
    unsigned char *b = read_all(name, &got);
    int bad = size < 0 ? b != NULL : b == NULL || got != size || memcmp(b, bytes, (size_t)size) != 0;

    free(b);
    return bad;
}

static const char *beside(const char *slot, const char *ext) {
    static char p[2][700];
    static int at;
    at = (at + 1) % 2;
    snprintf(p[at], sizeof(p[at]), "%s.%s", slot, ext);
    return p[at];
}

// 1 if file_replace left a name beside `slot`
static int leftovers(const char *slot) {
    return access(beside(slot, "tmp"), F_OK) == 0 || access(beside(slot, "new"), F_OK) == 0;
}

// Copies `from` over a destination that already holds `before` bytes of
// something else. Returns 1 if the destination isn't the source afterwards.
static int check(const char *from, long before) {
    const char *to = path("copy.out");
    long from_size, to_size = -1, reported = -1;
    unsigned char *a = read_all(from, &from_size), *b;
    int rv, bad;

    put_junk(to, before);
    rv = file_copy(from, to, &reported);
    b = read_all(to, &to_size);
    bad = a == NULL || b == NULL || rv != 0 || reported != from_size || to_size != from_size ||
          memcmp(a, b, (size_t)from_size) != 0;
    if (bad)
        printf("  %s (%ld bytes) over %ld bytes: returned %d, said %ld bytes, left %ld\n", from, from_size, before, rv,
               reported, to_size);
    free(a);
    free(b);
    return bad;
}

// The same through file_replace, into a slot holding `before` bytes (-1: no
// slot yet); nothing may be left beside it either.
static int check_replace(const char *from, long before) {
    const char *to = path("slot.dat");
    long from_size, reported = -1;
    unsigned char *a = read_all(from, &from_size);
    int rv, bad;

    put_junk(to, before);
    rv = file_replace(from, to, &reported);
    bad = a == NULL || rv != 0 || reported != from_size || differs(to, a, from_size) || leftovers(to);
    if (bad)
        printf("  replacing %ld bytes with %s (%ld bytes): returned %d, said %ld bytes, %s\n", before, from, from_size,
               rv, reported, leftovers(to) ? "a name left beside it" : "nothing left beside it");
    free(a);
    return bad;
}

static int expect(int good, const char *what) {
    if (!good)
        printf("  %s\n", what);
    return !good;
}

// A save failing or stopped at each of its points, and what a start-up
// settles after it. Returns how many cases went wrong; *cases, how many ran.
static int replace_failures(int *cases) {
    static const unsigned char old_save[] = "the save there before";
    static const unsigned char new_save[] = "the save being made";
    const long old_n = sizeof(old_save), new_n = sizeof(new_save);
    char slot[600], from[600], tmp[700], fresh[700], inside[800];
    int bad = 0, rv;

    snprintf(slot, sizeof(slot), "%s", path("slot.dat"));
    snprintf(from, sizeof(from), "%s", path("current.dat"));
    snprintf(tmp, sizeof(tmp), "%s", beside(slot, "tmp"));
    snprintf(fresh, sizeof(fresh), "%s", beside(slot, "new"));
    write_all(from, new_save, new_n);

    // nothing to copy
    write_all(slot, old_save, old_n);
    rv = file_replace(path("no.such.file"), slot, NULL);
    bad += expect(rv != 0 && !differs(slot, old_save, old_n) && !leftovers(slot),
                  "a missing source: the slot changed, a name was left, or no error");

    // the card full in the middle of the copy
    {
        struct rlimit was, small;
        const char *big = path("big.dat");

        put_junk(big, 200000);
        write_all(slot, old_save, old_n);
        signal(SIGXFSZ, SIG_IGN);
        getrlimit(RLIMIT_FSIZE, &was);
        small = was;
        small.rlim_cur = 100000;
        setrlimit(RLIMIT_FSIZE, &small);
        rv = file_replace(big, slot, NULL);
        setrlimit(RLIMIT_FSIZE, &was);
        bad += expect(rv != 0 && !differs(slot, old_save, old_n) && !leftovers(slot),
                      "the card full: the slot changed, a name was left, or no error");
        remove(big);
    }

    // `.tmp` can't be made (a folder with something in it, which remove leaves)
    write_all(slot, old_save, old_n);
    mkdir(tmp, 0700);
    snprintf(inside, sizeof(inside), "%s/x", tmp);
    write_all(inside, old_save, old_n);
    rv = file_replace(from, slot, NULL);
    bad += expect(rv != 0 && !differs(slot, old_save, old_n) && access(fresh, F_OK) != 0,
                  "no `.tmp`: the slot changed, a `.new` was made, or no error");
    remove(inside);
    rmdir(tmp);

    // `.tmp` can't be renamed `.new`
    write_all(slot, old_save, old_n);
    fail_rename = 1;
    rv = file_replace(from, slot, NULL);
    fail_rename = 0;
    bad += expect(rv != 0 && !differs(slot, old_save, old_n) && !leftovers(slot),
                  "the first rename failing: the slot changed, a name was left, or no error");

    // `.new` can't be renamed to the slot, which the rename has removed:
    // `.new` must have it all, and a start-up put it in place
    write_all(slot, old_save, old_n);
    fail_rename = 2;
    rv = file_replace(from, slot, NULL);
    fail_rename = 0;
    bad += expect(rv != 0 && !differs(fresh, new_save, new_n), "the last rename failing: no error, or `.new` not whole");
    file_replace_finish(slot);
    bad += expect(!differs(slot, new_save, new_n) && !leftovers(slot),
                  "after the last rename failed: the slot isn't the new save, or a name was left");

    // stopped in the middle of the copy, over a slot and into an empty one
    write_all(slot, old_save, old_n);
    put_junk(tmp, 7);
    file_replace_finish(slot);
    bad += expect(!differs(slot, old_save, old_n) && !leftovers(slot),
                  "a part `.tmp` over a slot: the slot changed, or `.tmp` left");
    remove(slot);
    put_junk(tmp, 7);
    file_replace_finish(slot);
    bad += expect(differs(slot, NULL, -1) == 0 && !leftovers(slot),
                  "a part `.tmp` into an empty slot: a slot was made, or `.tmp` left");

    // stopped before the last rename, and in it (the slot removed, `.new` not
    // yet renamed)
    write_all(slot, old_save, old_n);
    write_all(fresh, new_save, new_n);
    file_replace_finish(slot);
    bad += expect(!differs(slot, new_save, new_n) && !leftovers(slot),
                  "a `.new` beside a slot: the slot isn't the new save, or a name was left");
    remove(slot);
    write_all(fresh, new_save, new_n);
    file_replace_finish(slot);
    bad += expect(!differs(slot, new_save, new_n) && !leftovers(slot),
                  "a `.new` without a slot: the slot isn't the new save, or a name was left");

    // nothing beside the slot: it stays (the rename would remove it)
    write_all(slot, old_save, old_n);
    file_replace_finish(slot);
    bad += expect(!differs(slot, old_save, old_n), "nothing beside a slot: the slot changed");

    remove(slot);
    remove(from);
    *cases = 11;
    return bad;
}

int main(int argc, char **argv) {
    static const long sizes[] = {0, 1, 2, 1023, 1024, 1025, 4096, 65535, 65536, 65537, 1000000, 3000001};
    int bad = 0, copies = 0, replaces = 0, failures, i;
    unsigned seed = 12345;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <a folder to work in> [a real file]\n", argv[0]);
        return 2;
    }
    snprintf(dir, sizeof(dir), "%s", argv[1]);

    for (i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); i++) {
        unsigned char *bytes = malloc(sizes[i] + 1);
        char from[600];
        long k;

        // (its own copy: path's few buffers are reused by the checks)
        snprintf(from, sizeof(from), "%s", path("copy.in"));
        for (k = 0; k < sizes[i]; k++) {
            seed = seed * 1103515245u + 12345u;
            bytes[k] = (unsigned char)(seed >> 16);
        }
        write_all(from, bytes, sizes[i]);
        free(bytes);
        // nothing there before, something shorter, something longer
        bad += check(from, -1) + check(from, sizes[i] / 2) + check(from, sizes[i] * 2 + 100);
        copies += 3;
        bad += check_replace(from, -1) + check_replace(from, sizes[i] / 2) + check_replace(from, sizes[i] * 2 + 100);
        replaces += 3;
    }
    if (argc > 2) {
        bad += check(argv[2], -1) + check(argv[2], 5000000);
        copies += 2;
        bad += check_replace(argv[2], -1) + check_replace(argv[2], 5000000);
        replaces += 2;
    }

    // a source that isn't there: an error, and the destination as it was
    {
        const char *to = path("copy.out");
        unsigned char kept[] = "what was there";
        long size;
        unsigned char *after;
        int rv;

        write_all(to, kept, sizeof(kept));
        rv = file_copy(path("no.such.file"), to, NULL);
        after = read_all(to, &size);
        if (rv == 0 || after == NULL || size != sizeof(kept) || memcmp(after, kept, sizeof(kept)) != 0) {
            printf("  a missing source: returned %d, destination now %ld bytes\n", rv, size);
            bad++;
        }
        free(after);
    }
    remove(path("copy.in"));
    remove(path("copy.out"));
    remove(path("slot.dat"));

    bad += replace_failures(&failures);

    printf("%d copies, a missing source, %d replacements and %d failures: %d wrong\n", copies, replaces, failures, bad);
    return bad != 0;
}
