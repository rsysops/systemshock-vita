// The art files held whole in memory: see resimage.h.

#include <stdio.h>
#include <stdlib.h>

#include <SDL.h>
#include <psp2/kernel/threadmgr.h>

#include "res.h"
#include "resimage.h"

// By the names the game opens them with, most read in the middle of the
// game first. The films, the audio logs and the saved game are left on the
// card; the sound effects have a thread of their own (snd_preload).
static const char *const files[] = {
    "res/data/objart3.res", "res/data/objart2.res", "res/data/citmat.res",  "res/data/obj3d.res",
    "res/data/mfdart.res",  "res/data/mfdfrn.res",  "res/data/mfdger.res",  "res/data/gamescr.res",
    "res/data/handart.res", "res/data/objart.res",  "res/data/sideart.res", "res/data/cybstrng.res",
    "res/data/frnstrng.res", "res/data/gerstrng.res", "res/data/vidmail.res", "res/data/texture.res",
};
#define FILES (int)(sizeof(files) / sizeof(files[0]))

// Read this much at a time, for the game's own reads to get through.
#define SLICE (128 * 1024)

enum { IMAGE_NOT_YET, IMAGE_READ, IMAGE_GIVEN, IMAGE_FAILED };

static struct {
    uint8_t *bytes;
    size_t size;
    SDL_atomic_t state;
} images[FILES];

static SDL_Thread *thread;
static int priority;
static int all_given;
static SDL_atomic_t read_ms, read_bytes;

static int read_file(int i) {
    FILE *f = fopen_caseless(files[i], "rb");
    long size;
    size_t at = 0;
    uint8_t *bytes = NULL;

    if (f == NULL)
        return 0;
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (bytes = malloc((size_t)size)) != NULL) {
        while (at < (size_t)size) {
            size_t want = (size_t)size - at < SLICE ? (size_t)size - at : SLICE;
            if (fread(bytes + at, 1, want, f) != want)
                break;
            at += want;
        }
        if (at != (size_t)size) {
            free(bytes);
            bytes = NULL;
        }
    }
    fclose(f);
    if (bytes == NULL)
        return 0;
    images[i].bytes = bytes;
    images[i].size = at;
    return 1;
}

static int reader(void *unused) {
    Uint32 began = SDL_GetTicks();
    int i;

    (void)unused;
    // On any core and below the game's threads, as the thread that decodes
    // the sound effects.
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_ALL);
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), priority);

    for (i = 0; i < FILES; i++) {
        int ok = read_file(i);
        if (ok)
            SDL_AtomicAdd(&read_bytes, (int)images[i].size);
        SDL_AtomicSet(&read_ms, (int)(SDL_GetTicks() - began));
        // last: the main thread takes bytes and size once it sees this
        SDL_AtomicSet(&images[i].state, ok ? IMAGE_READ : IMAGE_FAILED);
    }
    return 0;
}

void resimage_start(void) {
    SceKernelThreadInfo info;

    if (thread != NULL)
        return;
    info.size = sizeof(info);
    priority = (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0 ? 160 : info.currentPriority) + 2;
    thread = SDL_CreateThread(reader, "res_images", NULL);
    if (thread != NULL)
        SDL_DetachThread(thread);
}

void resimage_poll(void) {
    int i, left = 0;

    if (all_given || thread == NULL)
        return;
    for (i = 0; i < FILES; i++) {
        int state = SDL_AtomicGet(&images[i].state);

        if (state == IMAGE_READ) {
            // a copy the resource system has no room for is of no use
            if (!ResUseImage(files[i], images[i].bytes, images[i].size)) {
                SDL_AtomicAdd(&read_bytes, -(int)images[i].size);
                free(images[i].bytes);
                images[i].bytes = NULL;
                SDL_AtomicSet(&images[i].state, IMAGE_FAILED);
            } else
                SDL_AtomicSet(&images[i].state, IMAGE_GIVEN);
        } else if (state == IMAGE_NOT_YET)
            left++;
    }
    all_given = left == 0;
}

void resimage_hold_all(int filenum) {
    Id id;

    for (id = ID_MIN; id <= resDescMax; id++)
        if (ResInUse(id) && ResFilenum(id) == (uint32_t)filenum && ResIsCompound(id))
            ResLock(id);
}

void resimage_release_all(int filenum) {
    Id id;

    for (id = ID_MIN; id <= resDescMax; id++)
        if (ResInUse(id) && ResFilenum(id) == (uint32_t)filenum && ResIsCompound(id) && ResPtr(id) != NULL &&
            ResLocked(id))
            ResUnlock(id);
}

void resimage_stats(int *ready, int *wanted, int *bytes, int *ms) {
    int i;

    *ready = 0;
    for (i = 0; i < FILES; i++) {
        int state = SDL_AtomicGet(&images[i].state);
        *ready += state == IMAGE_READ || state == IMAGE_GIVEN;
    }
    *wanted = FILES;
    *bytes = SDL_AtomicGet(&read_bytes);
    *ms = SDL_AtomicGet(&read_ms);
}
