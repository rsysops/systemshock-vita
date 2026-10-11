// A resource file read from its copy in memory (ResUseImage) must give what
// it gives read from the disk: the same resources, each the same bytes, and
// the same tables of the compound ones. Both when the copy is there before
// the file is opened and when it arrives while the file is open.
// Usage: resimage_test <res file>...

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "res.h"
#include "res_.h"

typedef struct {
    Id id;
    size_t size;
    uint8_t *bytes;
    size_t table_size;
    RefTable *table;
} kept;

// Every resource of the file as it reads now: kept if `to` is NULL-filled
// (count 0), else held against what was kept. Returns how many differ.
static int read_all(int32_t fn, kept **list, int *count, int compare, int want_image) {
    int n = 0, bad = 0;
    Id id;

    for (id = ID_MIN; id <= resDescMax; id++) {
        size_t size, table_size = 0;
        uint8_t *bytes;
        RefTable *table = NULL;

        if (!ResInUse(id) || ResFilenum(id) != (uint32_t)fn)
            continue;
        if (ResOnImage(id) != (want_image != 0)) {
            bad++;
            n++;
            continue;
        }
        size = RESDESC(id)->fsize;
        bytes = malloc(size + 1);
        if (!ResRetrieve(id, bytes)) {
            free(bytes);
            bad++;
            n++;
            continue;
        }
        if (ResIsCompound(id)) {
            table = ResReadRefTable(id);
            table_size = table != NULL ? REFTABLESIZE(table->numRefs) : 0;
        }
        if (!compare) {
            *list = realloc(*list, sizeof(kept) * (size_t)(n + 1));
            (*list)[n] = (kept){id, size, bytes, table_size, table};
        } else {
            const kept *k = n < *count ? &(*list)[n] : NULL;
            if (k == NULL || k->id != id || k->size != size || memcmp(k->bytes, bytes, size) != 0 ||
                k->table_size != table_size || (table_size != 0 && memcmp(k->table, table, table_size) != 0))
                bad++;
            free(bytes);
            if (table != NULL)
                ResFreeRefTable(table);
        }
        n++;
    }
    if (!compare)
        *count = n;
    else if (n != *count)
        bad++;
    return bad;
}

static int check_file(const char *path) {
    kept *list = NULL;
    int count = 0, bad_disk, bad_switched, bad_opened, i;
    FILE *f;
    long size;
    uint8_t *image;
    int32_t fn;

    fn = ResOpenFile(path);
    if (fn < 0) {
        printf("%s: cannot open\n", path);
        return 1;
    }
    read_all(fn, &list, &count, 0, 0);
    // twice from the disk gives the same, or nothing below means anything
    bad_disk = read_all(fn, &list, &count, 1, 0);

    f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    image = malloc((size_t)size);
    if (fread(image, 1, (size_t)size, f) != (size_t)size || !ResUseImage(path, image, (size_t)size)) {
        printf("%s: no copy made\n", path);
        return 1;
    }
    fclose(f);

    // the copy came while the file was open
    bad_switched = read_all(fn, &list, &count, 1, 1);
    ResCloseFile(fn);
    // and now it is there before
    fn = ResOpenFile(path);
    bad_opened = fn < 0 ? count : read_all(fn, &list, &count, 1, 1);
    if (fn >= 0)
        ResCloseFile(fn);

    printf("%s: %d resources, %.1f MB; differ: %d read again from the disk, %d after the copy came, %d opened on it\n",
           path, count, size / (1024.0 * 1024.0), bad_disk, bad_switched, bad_opened);
    for (i = 0; i < count; i++) {
        free(list[i].bytes);
        if (list[i].table != NULL)
            ResFreeRefTable(list[i].table);
    }
    free(list);
    return count == 0 || bad_disk != 0 || bad_switched != 0 || bad_opened != 0;
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
