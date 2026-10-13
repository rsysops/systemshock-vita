// A whole file copied in one read and one write: see filecopy.h.

#include <stdio.h>
#include <stdlib.h>

#include "filecopy.h"

extern FILE *fopen_caseless(const char *path, const char *mode); // see caseless.c

// Should there be no memory for the whole file: this much at a time.
#define PIECE (64 * 1024)

int file_copy(const char *from, const char *to, long *size) {
    static unsigned char piece[PIECE];
    FILE *in = fopen_caseless(from, "rb"), *out;
    unsigned char *bytes;
    long length;
    int ok;

    if (in == NULL)
        return -1;
    if (fseek(in, 0, SEEK_END) != 0 || (length = ftell(in)) < 0 || fseek(in, 0, SEEK_SET) != 0) {
        fclose(in);
        return -1;
    }

    bytes = malloc(length > 0 ? (size_t)length : 1);
    if (bytes != NULL) {
        ok = fread(bytes, 1, (size_t)length, in) == (size_t)length;
        fclose(in);
        out = ok ? fopen_caseless(to, "wb") : NULL;
        if (out != NULL) {
            ok = fwrite(bytes, 1, (size_t)length, out) == (size_t)length;
            if (fclose(out) != 0)
                ok = 0;
        } else
            ok = 0;
        free(bytes);
    } else {
        long left = length;

        out = fopen_caseless(to, "wb");
        ok = out != NULL;
        while (ok && left > 0) {
            size_t n = left < PIECE ? (size_t)left : PIECE;
            ok = fread(piece, 1, n, in) == n && fwrite(piece, 1, n, out) == n;
            left -= (long)n;
        }
        fclose(in);
        if (out != NULL && fclose(out) != 0)
            ok = 0;
    }
    if (ok && size != NULL)
        *size = length;
    return ok ? 0 : -1;
}
