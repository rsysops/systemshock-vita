// file_copy must leave at the destination exactly the source's bytes, for
// files of any size, whatever was there before; and a source that can't be
// read must leave the destination alone.
// Usage: filecopy_test <a folder to work in> [a real file to copy too]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filecopy.h"

static char dir[512];

static const char *path(const char *name) {
    static char p[4][600];
    static int at;
    at = (at + 1) % 4;
    snprintf(p[at], sizeof(p[at]), "%s/%s", dir, name);
    return p[at];
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

// Copies `from` over a destination that already holds `before` bytes of
// something else. Returns 1 if the destination isn't the source afterwards.
static int check(const char *from, long before) {
    const char *to = path("copy.out");
    long from_size, to_size = -1, reported = -1;
    unsigned char *a = read_all(from, &from_size), *b, *junk;
    int rv, bad;

    if (before >= 0) {
        junk = malloc(before + 1);
        memset(junk, 0xa5, (size_t)before);
        write_all(to, junk, before);
        free(junk);
    } else
        remove(to);
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

int main(int argc, char **argv) {
    static const long sizes[] = {0, 1, 2, 1023, 1024, 1025, 4096, 65535, 65536, 65537, 1000000, 3000001};
    int bad = 0, copies = 0, i;
    unsigned seed = 12345;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <a folder to work in> [a real file]\n", argv[0]);
        return 2;
    }
    snprintf(dir, sizeof(dir), "%s", argv[1]);

    for (i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); i++) {
        const char *from = path("copy.in");
        unsigned char *bytes = malloc(sizes[i] + 1);
        long k;

        for (k = 0; k < sizes[i]; k++) {
            seed = seed * 1103515245u + 12345u;
            bytes[k] = (unsigned char)(seed >> 16);
        }
        write_all(from, bytes, sizes[i]);
        free(bytes);
        // nothing there before, something shorter, something longer
        bad += check(from, -1) + check(from, sizes[i] / 2) + check(from, sizes[i] * 2 + 100);
        copies += 3;
    }
    if (argc > 2) {
        bad += check(argv[2], -1) + check(argv[2], 5000000);
        copies += 2;
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

    printf("%d copies and a missing source: %d wrong\n", copies, bad);
    return bad != 0;
}
