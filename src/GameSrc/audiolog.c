/*

Copyright (C) 2015-2018 Night Dive Studios, LLC.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/
/*
 * $Source: r:/prj/cit/src/RCS/audiolog.c $
 * $Revision: 1.17 $
 * $Author: dc $
 * $Date: 1994/11/19 20:35:27 $
 */
//	Mac version by Ken Cobb,  2/9/95

#include <stdio.h>
#include <SDL.h>

#include "MacTune.h"
#include "afile.h"
#include "movie.h"
#include "audiolog.h"
#include "map.h"
#include "tools.h"
#include "musicai.h"
#include "mainloop.h"
#include "bark.h"
#include "miscqvar.h"
#include "vprof.h"

#define AUDIOLOG_BASE_ID 2741
#define AUDIOLOG_BARK_BASE_ID 3100

#define ALOG_MUSIC_DUCK 0.7

// A log's sound is converted as it plays, as the cutscenes' is (cutsloop.c):
// handed to the stream in slices of a block, so that the audio thread
// converts a little in each of its calls and never a block's worth at once.
#define ALOG_FEED_BYTES 1024

// The card is another thread's business. A log is read whole into memory by
// a reader of its own (alog_reader); the audio thread gives silence until it
// is there and never waits for the card, nor does the main thread.
#define ALOG_READ_BYTES 65536

typedef struct alog_job {
    char path[64];
    long offset, size; // where the log is in its file
    uint8_t *bytes;
    SDL_atomic_t loaded;   // 0 while it is being read, 1 all there, -1 the read failed
    SDL_atomic_t cancel;   // the game doesn't want it any more
    SDL_atomic_t released; // its reader won't touch it again
    struct alog_job *next; // among those set aside
} alog_job;

// The log under way's, set before the callback is hooked and left alone while
// it is. NULL for a log read through the resource system (see audiolog_play()).
static alog_job *alog_job_now = NULL;
// Those the game is through with, to be freed once their readers are too
static alog_job *alog_jobs_aside = NULL;
static SDL_SpinLock alog_aside_lock;

static SDL_AudioStream *alog_stream = NULL;
static Afile *alog_file = NULL;
static uint8_t alog_block[MOVIE_DEFAULT_BLOCKLEN];
static int alog_block_len, alog_block_pos;
static bool alog_drained; // no block is left, and the stream has been flushed
// A log is under way, from audiolog_play() to the end of audiolog_finish()
static SDL_atomic_t alog_hooked;

int curr_alog = -1;
uchar audiolog_setting = 1;
char secret_pending_hack;

char *bark_files[] = {"res/data/citbark.res", "res/data/frnbark.res", "res/data/gerbark.res"};
char *alog_files[] = {"res/data/citalog.res", "res/data/frnalog.res", "res/data/geralog.res"};

// The logs' file and the barks' are opened once and stay open: finding a log
// is then a look in the resource table. (Their ids are in no other file of
// the game.)
static int alog_res_fn[2] = {-1, -1};
static char alog_res_lang[2];

extern uchar curr_vol_lev;
extern uchar curr_alog_vol;
extern char which_lang;

static int alog_res_file(int barks) {
    if (alog_res_fn[barks] >= 0 && alog_res_lang[barks] != which_lang) {
        ResCloseFile(alog_res_fn[barks]);
        alog_res_fn[barks] = -1;
    }
    if (alog_res_fn[barks] < 0) {
        alog_res_fn[barks] = ResOpenFile(barks ? bark_files[which_lang] : alog_files[which_lang]);
        alog_res_lang[barks] = which_lang;
    }
    return alog_res_fn[barks];
}

static int alog_reader(void *data) {
    alog_job *job = data;
    FILE *f = fopen_caseless(job->path, "rb");
    bool ok = f != NULL && fseek(f, job->offset, SEEK_SET) == 0;
    long at = 0;

    // in pieces, so as to stop soon when the log is cancelled
    while (ok && at < job->size && !SDL_AtomicGet(&job->cancel)) {
        long piece = job->size - at;

        if (piece > ALOG_READ_BYTES)
            piece = ALOG_READ_BYTES;
        ok = fread(job->bytes + at, 1, piece, f) == (size_t)piece;
        at += piece;
    }
    if (f != NULL)
        fclose(f);
    SDL_AtomicSet(&job->loaded, ok && at == job->size ? 1 : -1);
    SDL_AtomicSet(&job->released, 1);
    return 0;
}

// Frees the logs set aside whose readers are through with them.
static void alog_jobs_collect(void) {
    alog_job **link, *job, *done = NULL;

    SDL_AtomicLock(&alog_aside_lock);
    for (link = &alog_jobs_aside; (job = *link) != NULL;) {
        if (SDL_AtomicGet(&job->released)) {
            *link = job->next;
            job->next = done;
            done = job;
        } else
            link = &job->next;
    }
    SDL_AtomicUnlock(&alog_aside_lock);

    while ((job = done) != NULL) {
        done = job->next;
        free(job->bytes);
        free(job);
    }
}

// The game is through with the log: its reader may not be.
static void alog_job_set_aside(alog_job *job) {
    SDL_AtomicSet(&job->cancel, 1);
    SDL_AtomicLock(&alog_aside_lock);
    job->next = alog_jobs_aside;
    alog_jobs_aside = job;
    SDL_AtomicUnlock(&alog_aside_lock);
    alog_jobs_collect();
}

// Takes the log's movie, if it opened, and makes the stream that converts
// its sound.
static bool alog_take(Afile *palog, bool opened) {
    SDL_AudioStream *stream;

    if (!opened) {
        free(palog);
        return false;
    }
    stream = SDL_NewAudioStream(AUDIO_U8, 1, fix_int(palog->a.sampleRate), AUDIO_S16SYS, 2, 48000);
    if (stream == NULL) {
        AfileFree(palog);
        free(palog);
        return false;
    }
    alog_file = palog;
    alog_stream = stream;
    return true;
}

static void audiolog_feed(Uint8 *stream, int len) {
    int gotten;

    while (!alog_drained && SDL_AudioStreamAvailable(alog_stream) < len) {
        int slice;

        if (alog_block_pos == alog_block_len) {
            int32_t got = AmovReadNextAudioChunk(alog_file, alog_block);
            if (got <= 0) {
                SDL_AudioStreamFlush(alog_stream);
                alog_drained = true;
                break;
            }
            alog_block_len = got;
            alog_block_pos = 0;
        }
        slice = alog_block_len - alog_block_pos;
        if (slice > ALOG_FEED_BYTES)
            slice = ALOG_FEED_BYTES;
        if (SDL_AudioStreamPut(alog_stream, alog_block + alog_block_pos, slice) < 0) {
            alog_drained = true;
            break;
        }
        alog_block_pos += slice;
    }

    gotten = SDL_AudioStreamGet(alog_stream, stream, len);
    if (gotten < 0)
        gotten = 0;
    if (gotten < len)
        SDL_memset(stream + gotten, 0, len - gotten);
}

void audiolog_callback(void *userdata, Uint8 *stream, int len) {
    if (!SDL_AtomicGet(&alog_hooked))
        return;

    if (alog_file == NULL) {
        // the log's movie isn't open yet: is the log there?
        int loaded = SDL_AtomicGet(&alog_job_now->loaded);

        if (loaded == 0) {
            SDL_memset(stream, 0, len);
#ifdef VITA_PROFILE
            vprof_alog_dry();
#endif
            return;
        }
        if (loaded > 0) {
            // its movie, from memory
            Afile *palog = malloc(sizeof(Afile));

            loaded = alog_take(palog, AfilePrepareMem(alog_job_now->bytes, alog_job_now->size, palog) >= 0);
        }
        if (loaded <= 0) {
            WARN("%s: Cannot read or open the audiolog", __FUNCTION__);
            audiolog_stop();
            return;
        }
    }

#ifdef VITA_PROFILE
    SceInt64 t0 = sceKernelGetProcessTimeWide();
#endif
    audiolog_feed(stream, len);
#ifdef VITA_PROFILE
    vprof_alog_add((unsigned)(sceKernelGetProcessTimeWide() - t0));
#endif

    // all of it played
    if (alog_drained && SDL_AudioStreamAvailable(alog_stream) == 0)
        audiolog_stop();
}

errtype audiolog_play(int email_id) {
    int barks = email_id > (AUDIOLOG_BARK_BASE_ID - AUDIOLOG_BASE_ID);
    Id id = AUDIOLOG_BASE_ID + email_id;
    alog_job *job = NULL;
    int32_t filenum, fn;
    uint32_t offset, size;
    bool ready;

    if (!sfx_on || !audiolog_setting)
        return ERR_NOEFFECT;

    // KLC - Big-time hack to prevent bark #389 from trying to play twice (and thus skipping).
    if (email_id == 389 && curr_alog == email_id)
        return ERR_NOEFFECT;

    // Stop any currently playing alogs.
    audiolog_stop();

    // woo hoo, what a hack!
    // this is for the player's log-to-self which has no audiolog
    if (email_id == 0x44)
        return ERR_NOEFFECT;

    begin_wait();
    // from here to the log on its way: all of it before the first sound
    VPROF_MARK_BEGIN(VPROF_ALOGLOAD);

    // The appropriate sound-only movie file.
    fn = alog_res_file(barks);

    // Make sure this is a thing we have an audiolog for...
    if (fn < 0 || !ResInUse(id)) {
        VPROF_MARK_END(VPROF_ALOGLOAD);
        end_wait();
        return ERR_FREAD;
    }

    if (ResFilePlace(id, &filenum, &offset, &size) && filenum == fn) {
        // read from the file by a thread of its own; the callback takes it
        // from there
        SDL_Thread *reader = NULL;

        job = calloc(1, sizeof(*job));
        job->bytes = malloc(size);
        snprintf(job->path, sizeof(job->path), "%s", barks ? bark_files[which_lang] : alog_files[which_lang]);
        job->offset = offset;
        job->size = size;
        if (job->bytes != NULL)
            reader = SDL_CreateThread(alog_reader, "alog_reader", job);
        ready = reader != NULL;
        if (ready)
            SDL_DetachThread(reader);
        else {
            free(job->bytes);
            free(job);
            job = NULL;
        }
    } else {
        // compressed in its file: read whole here, through the resource system
        Afile *palog = malloc(sizeof(Afile));

        ready = alog_take(palog, AfilePrepareRes(id, palog) >= 0);
    }
    if (!ready) {
        WARN("%s: Cannot open Afile by id $%x", __FUNCTION__, id);
        VPROF_MARK_END(VPROF_ALOGLOAD);
        end_wait();
        return ERR_FREAD;
    }

    DEBUG("%s: Playing email", __FUNCTION__);

    alog_job_now = job;
    alog_block_len = alog_block_pos = 0;
    alog_drained = false;

    VPROF_MARK_END(VPROF_ALOGLOAD);
    end_wait();

    // bureaucracy
    curr_alog = email_id;

    // Duck the music
    if (music_on) {
        curr_vol_lev = QVAR_TO_VOLUME(QUESTVAR_GET(MUSIC_VOLUME_QVAR));
        curr_vol_lev = curr_vol_lev * ALOG_MUSIC_DUCK;
        MacTuneUpdateVolume();
    }

    snd_stop_music();
    SDL_AtomicSet(&alog_hooked, 1);
    Mix_HookMusic((void (*)(void*, Uint8*, int))audiolog_callback, NULL);

    return OK;
}

// Lets go of the log under way. Only where audiolog_callback() can't be
// running or run again: from the callback itself, or once the music's
// callback is back in its place.
static void audiolog_finish(void) {
    if (!SDL_AtomicGet(&alog_hooked))
        return;
    SDL_AtomicSet(&alog_hooked, 0);

    if (alog_stream != NULL) {
        SDL_FreeAudioStream(alog_stream);
        alog_stream = NULL;
    }
    if (alog_file != NULL) {
        AfileFree(alog_file);
        free(alog_file);
        alog_file = NULL;
    }
    // after the movie, which reads from its memory
    if (alog_job_now != NULL) {
        alog_job_set_aside(alog_job_now);
        alog_job_now = NULL;
    }

    // Restore music volume
    if (music_on) {
        curr_vol_lev = QVAR_TO_VOLUME(QUESTVAR_GET(MUSIC_VOLUME_QVAR));
        MacTuneUpdateVolume();
    }

    curr_alog = -1;

    if (secret_pending_hack) {
        INFO("Game over.");

        secret_pending_hack = 0;

        // Back to the main menu
        _new_mode = SETUP_LOOP;
        chg_set_flg(GL_CHG_LOOP);
    }
}

// Called by the game, and by the callback when the log has played to its end.
void audiolog_stop(void) {
    if (!SDL_AtomicGet(&alog_hooked))
        return;

    // The music's callback back first: that waits for a call of ours that is
    // under way, and none comes after. Only then is what it uses let go of.
    snd_resume_music();
    audiolog_finish();
}

errtype audiolog_loop_callback(void) {
    // a cancelled log whose reader has finished since
    if (alog_jobs_aside != NULL)
        alog_jobs_collect();
    return OK;
}

//-------------------------------------------------------------
// if email_id is -1, returns whether or not anything is playing
// if email_id != -1, matches whether or not that specific email_id is playing
//-------------------------------------------------------------
bool audiolog_playing(int email_id) {
    if (email_id == -1)
        return (curr_alog != -1);
    else
        return (curr_alog == email_id);
}

//-------------------------------------------------------------
//  Start playing a bark file.
//-------------------------------------------------------------
errtype audiolog_bark_play(int bark_id) {
    if (global_fullmap->cyber)
        return ERR_NOEFFECT;
    else
        return (audiolog_play(bark_id + (AUDIOLOG_BARK_BASE_ID - AUDIOLOG_BASE_ID)));
}

//-------------------------------------------------------------
//  Stop playing audiolog (in response to a hotkey).
//-------------------------------------------------------------
uchar audiolog_cancel_func(ushort s, uint32_t l, intptr_t v) {
    audiolog_stop();
    return TRUE;
}
