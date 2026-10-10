#include "Xmi.h"
#include "MusicDevice.h"
#include "vprof.h"

static snd_digi_parms digi_parms_by_channel[SND_MAX_SAMPLES];

#include <SDL.h>
#include <SDL_mixer.h>
#include <psp2/kernel/threadmgr.h>

extern char curr_alog_vol;

// The decoded sound effects, by snd_ref. An effect is decoded once, by
// whoever claims it first: the preload thread on its way through all of
// them (snd_preload), or the main thread for one that is played before the
// thread has got to it.
#define SND_REFS 512
enum { EFFECT_UNTOUCHED, EFFECT_CLAIMED, EFFECT_READY };

typedef struct {
    SDL_atomic_t state;
    Mix_Chunk *chunk; // set by who claimed it, before it says ready
} snd_effect;

static snd_effect effects[SND_REFS];

static snd_effect *effect_of(int snd_ref) {
    return snd_ref >= 0 && snd_ref < SND_REFS ? &effects[snd_ref] : NULL;
}

// An effect's bytes (a VOC file) decoded and converted to the mixer's format
static Mix_Chunk *decode_effect(const uchar *smp, int len) {
    return Mix_LoadWAV_RW(SDL_RWFromConstMem(smp, len), 1);
}

// For who claimed the effect: ready if it has a chunk, untouched again if
// not, to be tried at its next play.
static void publish_effect(snd_effect *e, Mix_Chunk *chunk) {
    e->chunk = chunk;
    SDL_AtomicSet(&e->state, chunk != NULL ? EFFECT_READY : EFFECT_UNTOUCHED);
}

#define SND_PRELOAD_MAX 256

static struct {
    char path[64];
    snd_preload_item items[SND_PRELOAD_MAX];
    int count;
    int priority;
    SDL_Thread *thread;
    SDL_atomic_t stop;
    SDL_atomic_t decode_ms, bytes; // of the thread's own decoding
} preload;

// Reads each effect of the list from the file, through a handle of its own
// (the resource system isn't made for two threads), and decodes it.
static int preload_thread(void *unused) {
    FILE *f;
    int i;

    (void)unused;
    // On any core, and below the game's threads: it works when one of them
    // has nothing better to do, which the menus and the loading leave plenty
    // of, and never holds up a worker the main thread waits for.
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_ALL);
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), preload.priority);

    f = fopen_caseless(preload.path, "rb");
    if (f == NULL)
        return 0;
    for (i = 0; i < preload.count && !SDL_AtomicGet(&preload.stop); i++) {
        const snd_preload_item *item = &preload.items[i];
        snd_effect *e = effect_of(item->snd_ref);
        Mix_Chunk *chunk = NULL;
        uchar *bytes;
        Uint32 began;

        if (e == NULL || !SDL_AtomicCAS(&e->state, EFFECT_UNTOUCHED, EFFECT_CLAIMED))
            continue;
        began = SDL_GetTicks();
        bytes = malloc(item->size);
        if (bytes != NULL && fseek(f, item->offset, SEEK_SET) == 0 && fread(bytes, item->size, 1, f) == 1)
            chunk = decode_effect(bytes, item->size);
        free(bytes);
        if (chunk != NULL)
            SDL_AtomicAdd(&preload.bytes, chunk->alen);
        SDL_AtomicAdd(&preload.decode_ms, SDL_GetTicks() - began);
        publish_effect(e, chunk);
    }
    fclose(f);
    return 0;
}

// The thread is through, or gives up after the effect it is on, before the
// mixer closes.
static void preload_stop(void) {
    SDL_AtomicSet(&preload.stop, 1);
    SDL_WaitThread(preload.thread, NULL);
    preload.thread = NULL;
}

void snd_preload(const char *path, const snd_preload_item *items, int count) {
    SceKernelThreadInfo info;

    if (preload.thread != NULL || count <= 0)
        return;
    if (count > SND_PRELOAD_MAX)
        count = SND_PRELOAD_MAX;
    snprintf(preload.path, sizeof(preload.path), "%s", path);
    memcpy(preload.items, items, count * sizeof(items[0]));
    preload.count = count;
    // two steps below the caller's: the rasterizer's workers are one below
    info.size = sizeof(info);
    preload.priority =
        (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0 ? 160 : info.currentPriority) + 2;

    preload.thread = SDL_CreateThread(preload_thread, "sfx_preload", NULL);
    if (preload.thread != NULL)
        atexit(preload_stop);
}

void snd_preload_stats(int *ready, int *total, int *decode_ms, int *bytes) {
    int i;

    *ready = 0;
    for (i = 0; i < preload.count; i++) {
        snd_effect *e = effect_of(preload.items[i].snd_ref);

        if (e != NULL && SDL_AtomicGet(&e->state) == EFFECT_READY)
            (*ready)++;
    }
    *total = preload.count;
    *decode_ms = SDL_AtomicGet(&preload.decode_ms);
    *bytes = SDL_AtomicGet(&preload.bytes);
}

static void wait_for_effect(snd_effect *e) {
    while (SDL_AtomicGet(&e->state) == EFFECT_CLAIMED)
        SDL_Delay(1);
}

int snd_sample_decoded(int snd_ref) {
    snd_effect *e = effect_of(snd_ref);

    if (e == NULL)
        return 0;
    // The thread is on this very one: it is ready sooner by waiting for it
    // than by starting over.
    if (SDL_AtomicGet(&e->state) == EFFECT_CLAIMED)
        VPROF_RUN(VPROF_SNDLOAD, wait_for_effect(e));
    return SDL_AtomicGet(&e->state) == EFFECT_READY;
}

extern struct MusicDevice *MusicDev;

extern void MusicCallback(void *userdata, Uint8 *stream, int len);

int snd_start_digital(void) {

    // Startup the sound system

    if (Mix_Init(MIX_INIT_MP3) < 0) {
        ERROR("%s: Init failed", __FUNCTION__);
    }

    if (Mix_OpenAudio(48000, AUDIO_S16SYS, 2, 2048) < 0) {
        ERROR("%s: Couldn't open audio device", __FUNCTION__);
    }

    Mix_AllocateChannels(SND_MAX_SAMPLES);

    Mix_HookMusic(MusicCallback, (void *)&MusicDev);
    Mix_VolumeMusic(MIX_MAX_VOLUME); // use max volume for music stream

    InitReadXMI();

    atexit(Mix_CloseAudio);
    atexit(SDL_CloseAudio);

    return OK;
}

void snd_stop_music() {
    Mix_HookMusic(NULL, NULL);
    Mix_VolumeMusic(curr_alog_vol * ((float)MIX_MAX_VOLUME / 100.f));
}

void snd_resume_music() {
    Mix_HookMusic(NULL, NULL);
    Mix_HookMusic(MusicCallback, (void *)&MusicDev);
    Mix_VolumeMusic(MIX_MAX_VOLUME);
}

int snd_sample_play(int snd_ref, int len, uchar *smp, struct snd_digi_parms *dprm) {

    // Play one of the VOC format sounds

    snd_effect *e = effect_of(snd_ref);
    Mix_Chunk *sample;

    if (e == NULL) {
        DEBUG("%s: No such sample", __FUNCTION__);
        return ERR_NOEFFECT;
    }

    if (!snd_sample_decoded(snd_ref) && smp != NULL &&
        SDL_AtomicCAS(&e->state, EFFECT_UNTOUCHED, EFFECT_CLAIMED)) {
        // not decoded ahead: decoded and converted here, the first time the
        // sound is played
        VPROF_RUN(VPROF_SNDLOAD, sample = decode_effect(smp, len));
        publish_effect(e, sample);
    }

    // (waits for the thread if it claimed the effect in between)
    if (!snd_sample_decoded(snd_ref)) {
        DEBUG("%s: Failed to load sample", __FUNCTION__);
        return ERR_NOEFFECT;
    }
    sample = e->chunk;

    int loops = dprm->loops > 0 ? dprm->loops - 1 : -1;
    int channel = Mix_PlayChannel(-1, sample, loops);
    if (channel < 0) {
        DEBUG("%s: Failed to play sample", __FUNCTION__);
        return ERR_NOEFFECT;
    }
    digi_parms_by_channel[channel] = *dprm;
    snd_sample_reload_parms(&digi_parms_by_channel[channel]);

    return channel;
}

void snd_end_sample(int hnd_id) {
    Mix_HaltChannel(hnd_id);
}

bool snd_sample_playing(int hnd_id) { return Mix_Playing(hnd_id); }

snd_digi_parms *snd_sample_parms(int hnd_id) { return &digi_parms_by_channel[hnd_id]; }

void snd_kill_all_samples(void) {
    for (int channel = 0; channel < SND_MAX_SAMPLES; channel++) {
        snd_end_sample(channel);
    }

    // assume we want these too
    //    StopTheMusic(); // no, don't stop the music
}

void snd_sample_reload_parms(snd_digi_parms *sdp) {
    // ignore if *sdp is not one of the items in digi_parms_by_channel[]
    if (sdp < digi_parms_by_channel || sdp > digi_parms_by_channel + SND_MAX_SAMPLES)
        return;
    int channel = sdp - digi_parms_by_channel;

    if (!Mix_Playing(channel))
        return;

    // sdp->vol ranges from 0..255
    Mix_Volume(channel, (sdp->vol * 128) / 100);

    // sdp->pan ranges from 1 (left) to 127 (right)
    uint8_t right = 2 * sdp->pan;
    Mix_SetPanning(channel, 254 - right, right);
}

int MacTuneLoadTheme(char *theme_base, int themeID) {
    char filename[40];
    FILE *f;
    int i;

#define NUM_SCORES 8
#define SUPERCHUNKS_PER_SCORE 4
#define NUM_TRANSITIONS 9
#define NUM_LAYERS 32
#define MAX_KEYS 10
#define NUM_LAYERABLE_SUPERCHUNKS 22
#define KEY_BAR_RESOLUTION 2

    extern uchar track_table[NUM_SCORES][SUPERCHUNKS_PER_SCORE];
    extern uchar transition_table[NUM_TRANSITIONS];
    extern uchar layering_table[NUM_LAYERS][MAX_KEYS];
    extern uchar key_table[NUM_LAYERABLE_SUPERCHUNKS][KEY_BAR_RESOLUTION];

    StopTheMusic();

    FreeXMI();

    if (strncmp(theme_base, "thm", 3)) {
        sprintf(filename, "res/sound/%s/%s.xmi", MusicDev->musicType, theme_base);
        ReadXMI(filename);
    } else {
        sprintf(filename, "res/sound/%s/thm%i.xmi", MusicDev->musicType, themeID);
        ReadXMI(filename);

        sprintf(filename, "res/sound/thm%i.bin", themeID);
        extern FILE *fopen_caseless(const char *path, const char *mode); // see caseless.c
        f = fopen_caseless(filename, "rb");
        if (f != 0) {
            fread(track_table, NUM_SCORES * SUPERCHUNKS_PER_SCORE, 1, f);
            fread(transition_table, NUM_TRANSITIONS, 1, f);
            fread(layering_table, NUM_LAYERS * MAX_KEYS, 1, f);
            fread(key_table, NUM_LAYERABLE_SUPERCHUNKS * KEY_BAR_RESOLUTION, 1, f);

            fclose(f);
        }
    }

    return OK;
}

void MacTuneKillCurrentTheme(void) { StopTheMusic(); }

// Unimplemented sound stubs

void snd_startup(void) {}
