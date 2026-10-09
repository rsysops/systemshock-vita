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

volatile struct mlimbs_request_info current_request[MLIMBS_MAX_SEQUENCES - 1]; // Request information
volatile struct mlimbs_playing_info userID[MLIMBS_MAX_SEQUENCES - 1];          // Sequence instance specific information
volatile uchar mlimbs_update_requests = FALSE;

volatile uchar max_voices = 0;

volatile ulong mlimbs_counter = 0;

// convienience psuedo-function defines
#define _uiD_seq(i) ((SEQUENCE *)snd_get_sequence(userID[i].seq_id))
#define _mlimbs_top         \
    if (mlimbs_status == 0) \
    return -1

// LONG mlimbs_timbre_callback(MDI_DRIVER *mdi, LONG bank, LONG patch);

