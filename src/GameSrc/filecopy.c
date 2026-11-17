// A whole file copied in one read and one write, and a save put in its slot
// through two names beside it: see filecopy.h.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "filecopy.h"

extern FILE *fopen_caseless(const char *path, const char *mode); // see caseless.c

// Should there be no memory for the whole file: this much at a time.
#define PIECE (64 * 1024)

// The longest name beside a slot, its end included.
#define NAME_LEN 1024

// Closes what was written, first waiting for it to be on the card if asked.
// (The sync's own answer isn't counted: one that fails leaves the bytes as
// they were written, and how the emulator answers it isn't known.)
static int close_out(FILE *out, int sync) {
    int ok = 1;

    if (sync) {
        ok = fflush(out) == 0;
        if (ok)
            (void)fsync(fileno(out));
    }
    if (fclose(out) != 0)
        ok = 0;
    return ok;
}

static int copy(const char *from, const char *to, long *size, int sync) {
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
            if (!close_out(out, sync))
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
        if (out != NULL && !close_out(out, sync))
            ok = 0;
    }
    if (ok && size != NULL)
        *size = length;
    return ok ? 0 : -1;
}

int file_copy(const char *from, const char *to, long *size) { return copy(from, to, size, 0); }

// The two names beside `to`; 0 if they don't fit.
static int beside(const char *to, char *tmp, char *fresh) {
    return snprintf(tmp, NAME_LEN, "%s.tmp", to) < NAME_LEN && snprintf(fresh, NAME_LEN, "%s.new", to) < NAME_LEN;
}

void file_replace_finish(const char *to) {
    char tmp[NAME_LEN], fresh[NAME_LEN];

    if (!beside(to, tmp, fresh))
        return;
    remove(tmp);
    // (only if it's there: the rename removes `to` before looking)
    if (access(fresh, F_OK) == 0)
        rename(fresh, to);
}

int file_replace(const char *from, const char *to, long *size) {
    char tmp[NAME_LEN], fresh[NAME_LEN];

    if (!beside(to, tmp, fresh))
        return -1;
    file_replace_finish(to);
    if (copy(from, tmp, size, 1) != 0 || rename(tmp, fresh) != 0) {
        remove(tmp);
        return -1;
    }
    // should this fail, `.new` stays for the next file_replace_finish
    return rename(fresh, to) == 0 ? 0 : -1;
}
