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
 * $Source: r:/prj/cit/src/RCS/mlimbs.c $
 * $Revision: 1.32 $
 * $Author: dc $
 * $Date: 1994/11/23 00:13:15 $
 */

#include <string.h>

#include "mlimbs.h"

#define CHANNEL_MAP

#define LOCK_ALL_CHANNELS

//uchar mlimbs_on = FALSE;
//char mlimbs_status = 0; // could make this one bitfield of status, on/off, enable/not, so on
int mlimbs_timer_id;    // what our timer handle is

static uchar *mlimbs_theme = NULL; // data about the current theme
volatile int mlimbs_master_slot = -1;
static int mlimbs_cur_theme_id = -1;

volatile struct mlimbs_piece_info xseq_info[MAX_SEQUENCES];                    // Sequence specific information
volatile struct mlimbs_request_info current_request[MLIMBS_MAX_SEQUENCES - 1]; // Request information
volatile struct mlimbs_channel_info channel_info[MLIMBS_MAX_CHANNELS];         // MIDI channel information
volatile struct mlimbs_playing_info userID[MLIMBS_MAX_SEQUENCES - 1];          // Sequence instance specific information
volatile uchar mlimbs_update_requests = FALSE;

volatile uchar num_XMIDI_sequences = 0;
volatile uchar num_master_measures;
volatile uchar voices_used = 0;
volatile uchar max_voices = 0;

volatile void (*mlimbs_AI)(void) = NULL;
volatile ulong mlimbs_counter = 0;
volatile long mlimbs_error;
volatile uchar mlimbs_semaphore = FALSE;

int master_volume = 100;

char curr_play_list[10];
char loop_list[10];
char cpl_num;

// gruesome hacks to try and get this working for now
int mlimbs_priority[MLIMBS_MAX_SEQUENCES];

// convienience psuedo-function defines
#define _uiD_seq(i) ((SEQUENCE *)snd_get_sequence(userID[i].seq_id))
#define _mlimbs_top         \
    if (mlimbs_status == 0) \
    return -1

// LONG mlimbs_timbre_callback(MDI_DRIVER *mdi, LONG bank, LONG patch);

/////////////////////////////////////////////////////////////////
//	mlimbs_init (void)
//
//	purpose:
//		This routine initializes the MLIMBS system.  This also
//		must be used if you want to switch the SndMIDIDevice that
//		MLIMBS uses.  This must be called before using mlimbs.
//
//	inputs:
//    now uses the default global midi device always
int mlimbs_init(void) {
    int i;

    //   if (!music_card) return -1;

    if (mlimbs_status != 0)
        return 1;
    for (i = 0; i < 10; i++) {
        curr_play_list[i] = -1;
        loop_list[i] = -1;
    }
    cpl_num = 0;

    /* What is max_voices???
            // Determine maximum number of voices
    #ifdef AIL_2
                            case SOUNDBLASTER:
                            case SOUNDBLASTERPRO:
                            case ADLIB:
                                    max_voices = 9;
                                    break;
                            case SOUNDBLASTERPRO2:
                                    max_voices = 18;
                            case MT32:
                                    // Switch max_voices to 32 when Roland specific dat files are
                                    // available, since voices usage should be tracked using partials on a Roland.
    //				max_voices = 32;	// Note that this here is actually the # of partials available.
                                    max_voices = 9;	// For now, since we're using only SB/ADLIB dat files, set voice
    limit to 9. master_volume = 90;	// Reduces distortion in Roland MT-32's break; default: max_voices = 9; break;
                    }
    #else
       max_voices=18;
    #endif

       snd_set_midi_sequences(MLIMBS_MAX_SEQUENCES);

    #ifdef CALLBACK_ON
    // Install mlimbs_callback, which will be called by the master sequence twice per loop.
    // each one has a different value, one means half done, get ready, the other means do the switch
       seq_miditrig=mlimbs_callback;
       AIL_register_timbre_callback((MDI_DRIVER *)snd_midi,mlimbs_timbre_callback);

    //   seq_finish=mlimbs_seq_done_call;

    //	Since we cannot stop and start XMIDI sequences from within a MIDI callback, use
    // a timer, running at 100 hz to start and stop XMIDI sequences. The mlimbs_callback
    // simply sets a flag, letting the timer know when to update sequence status.
            if ((mlimbs_timer_id = tm_add_process(mlimbs_timer_callback, 0, (TMD_FREQ/MLIMBS_TIMER_FREQUENCY))) == -1)
                    goto die;
    #endif

    #ifdef LOCK_ALL_CHANNELS
    //	Lock all MIDI channels on the current mlimbs_device, and
    //	initialize the channel_info[] array.
            for (i = 0; i < MLIMBS_MAX_CHANNELS; i++)
            {
                    channel_info[i].mchannel = AIL_lock_channel((MDI_DRIVER *)snd_midi);
                    channel_info[i].usernum = -1;	// This indicates what sequence is using this channel. -1 is stale
    handle. channel_info[i].status = MLIMBS_STOPPED;	// Current status of the channel.
            }
    #endif
    */
    mlimbs_status = 1;
    return 1;

die:
    mlimbs_shutdown();
    return -1;
}

/////////////////////////////////////////////////////////////////
//	mlimbs_shutdown
//
//	purpose:
//		This shuts down the mlimbs system.  After calling this,
//		call mlimbs_init again to restart it.
//
/////////////////////////////////////////////////////////////////
void mlimbs_shutdown(void) {
    if (mlimbs_status == 0)
        return;

    /* later, man
            mlimbs_purge_theme();

    #ifdef CALLBACK_ON
       seq_miditrig=NULL;
    //   seq_finish=NULL;
       AIL_register_timbre_callback((MDI_DRIVER *)snd_midi,NULL);
       tm_remove_process(mlimbs_timer_id);
    #endif

    #ifdef LOCK_ALL_CHANNELS
       {	// Release all the locked MIDI channels, and clear the channel_info[] array
          int i;
                    for (i = 0; i < MLIMBS_MAX_CHANNELS; i++)
                    {
                            if (channel_info[i].mchannel >= 0) AIL_release_channel((MDI_DRIVER
    *)snd_midi,channel_info[i].mchannel); channel_info[i].mchannel = -1; channel_info[i].usernum = -1;
                            channel_info[i].status = MLIMBS_STOPPED;
               }
       }
    #endif
    */
    mlimbs_status = 0;
}

