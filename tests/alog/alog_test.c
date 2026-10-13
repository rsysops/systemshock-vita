// An audio log's sound read two ways must be the same bytes: whole, from the
// resource (AfilePrepareRes + AfileGetAudio, how a log was played until it
// was converted as it plays), and as it is played now: its bytes read from
// the file at the place ResFilePlace gives, then a block at a time from
// memory (AfilePrepareMem + AmovReadNextAudioChunk).
// Usage: alog_test <res file>...   (the logs' and the barks' files)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "afile.h"
#include "draw4x4.h"
#include "huff.h"
#include "log.h"
#include "movie.h"
#include "res.h"

// What the libraries want from the game and from the movie library's picture
// decoders, which are 32-bit code and of no use to a log.
int32_t gScreenRowbytes;
void Draw4x4(uchar *p, int width, int height) {}
void Draw4x4Reset(uchar *colorset, uchar *hufftab) {}
void HuffExpandFlashTables(uchar *pFlashTab, uint lenTab, uint *pc, int tokSize) {}

static int check_file(const char *path) {
    int32_t fn = ResOpenFile(path);
    int logs = 0, bad = 0, unplaced = 0;
    long bytes = 0;
    Id id;

    if (fn < 0) {
        printf("%s: cannot open\n", path);
        return 1;
    }
    for (id = ID_MIN; id <= resDescMax; id++) {
        Afile whole, blocks;
        uint8_t *a, *b, *raw;
        FILE *fp;
        int32_t filenum, got;
        uint32_t offset, size;
        long len, at = 0;

        if (!ResInUse(id) || ResFilenum(id) != (uint32_t)fn)
            continue;
        logs++;

        if (AfilePrepareRes(id, &whole) < 0) {
            printf("%s: $%x doesn't open from memory\n", path, id);
            bad++;
            continue;
        }
        len = (long)AfileAudioLength(&whole) * MOVIE_DEFAULT_BLOCKLEN;
        a = malloc(len + 1);
        AfileGetAudio(&whole, a);
        AfileFree(&whole);

        if (!ResFilePlace(id, &filenum, &offset, &size) || filenum != fn) {
            // such a log is played from memory
            unplaced++;
            free(a);
            continue;
        }
        raw = malloc(size);
        fp = fopen(path, "rb");
        if (fp == NULL || fseek(fp, offset, SEEK_SET) != 0 || fread(raw, 1, size, fp) != size ||
            AfilePrepareMem(raw, size, &blocks) < 0) {
            printf("%s: $%x doesn't open from the file\n", path, id);
            bad++;
            if (fp != NULL)
                fclose(fp);
            free(raw);
            free(a);
            continue;
        }
        fclose(fp);
        b = malloc(len + MOVIE_DEFAULT_BLOCKLEN);
        while (at <= len - MOVIE_DEFAULT_BLOCKLEN && (got = AmovReadNextAudioChunk(&blocks, b + at)) > 0)
            at += got;
        // nothing may be left
        got = AmovReadNextAudioChunk(&blocks, b + at);
        AfileFree(&blocks);

        if (at != len || got > 0) {
            printf("%s: $%x is %ld bytes whole, %ld%s in blocks\n", path, id, len, at, got > 0 ? " and more" : "");
            bad++;
        } else if (memcmp(a, b, len) != 0) {
            long first = 0, differing = 0, i;

            for (i = 0; i < len; i++) {
                if (a[i] != b[i]) {
                    if (differing++ == 0)
                        first = i;
                }
            }
            printf("%s: $%x differs in %ld of %ld bytes, the first at %ld\n", path, id, differing, len, first);
            bad++;
        }
        bytes += len;
        free(a);
        free(b);
        free(raw);
    }
    ResCloseFile(fn);
    printf("%s: %d logs, %.1f MB of sound, %d differ, %d not readable from the file\n", path, logs,
           bytes / (1024.0 * 1024.0), bad, unplaced);
    return bad != 0;
}

int main(int argc, char **argv) {
    int i, status = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <res file>...\n", argv[0]);
        return 2;
    }
    log_set_quiet(1);
    ResInit();
    for (i = 1; i < argc; i++)
        status |= check_file(argv[i]);
    return status;
}
