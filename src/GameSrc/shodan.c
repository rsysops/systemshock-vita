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
#include "shodan.h"
#include "player.h"
#include "objclass.h"
#include "otrip.h"
#include "trigger.h"
#include "newmfd.h"
#include "hud.h"
#include "faketime.h"

// ---------------
// Internal Prototypes
// ---------------
errtype update_shodometer(short new_val, uchar game_stuff);

short compute_shodometer_value(uchar game_stuff) {
    if (player_struct.level <= MAX_SHODOMETER_LEVEL) {
        QUESTVAR_SET(SHODAN_QV, 0);
        for (ObjID oid = (objs[OBJ_NULL]).headused; oid != OBJ_NULL; oid = objs[oid].next) {
            increment_shodan_value(oid, game_stuff);
        }
        return (QUESTVAR_GET(SHODAN_QV));
    } else
        return (0);
}

#define HUD_SHODOMETER_TICKS (CIT_CYCLE << 1)

errtype update_shodometer(short new_val, uchar game_stuff) {
    QUESTVAR_SET(SHODAN_QV, new_val);
    if (game_stuff) {
        mfd_notify_func(MFD_PLOTWARE_FUNC, MFD_INFO_SLOT, FALSE, MFD_ACTIVE, FALSE);
        do_shodan_triggers();
        hud_set_time(HUD_SHODOMETER, HUD_SHODOMETER_TICKS);
    }
    return (OK);
}

short increment_shodan_value(ObjID oid, uchar game_stuff) {
    short curr_shodan_val = QUESTVAR_GET(SHODAN_QV);

    if (player_struct.level <= MAX_SHODOMETER_LEVEL) {
        switch (ID2TRIP(oid)) {
        case CAMERA_TRIPLE:
            curr_shodan_val += CAMERA_TRIPLE_VALUE;
            break;
        case SMALL_CPU_TRIPLE:
            curr_shodan_val += SMALL_CPU_TRIPLE_VALUE;
            break;
        case LARGCPU_TRIPLE:
            curr_shodan_val += LARGCPU_TRIPLE_VALUE;
            break;
        }
        if (curr_shodan_val != QUESTVAR_GET(SHODAN_QV))
            update_shodometer(curr_shodan_val, game_stuff);
        return (curr_shodan_val);
    } else
        return (0);
}

short decrement_shodan_value(ObjID oid, uchar game_stuff) {
    short curr_shodan_val = QUESTVAR_GET(SHODAN_QV);

    if (player_struct.level <= MAX_SHODOMETER_LEVEL) {
        switch (ID2TRIP(oid)) {
        case CAMERA_TRIPLE:
            curr_shodan_val -= CAMERA_TRIPLE_VALUE;
            break;
        case SMALL_CPU_TRIPLE:
            curr_shodan_val -= SMALL_CPU_TRIPLE_VALUE;
            break;
        case LARGCPU_TRIPLE:
            curr_shodan_val -= LARGCPU_TRIPLE_VALUE;
            break;
        }
        if (curr_shodan_val < 0) {
            //        Warning(("SHODOMETER negative after decrementing of id = 0x%x\n",oid));
            curr_shodan_val = 0;
        }
        if (curr_shodan_val != QUESTVAR_GET(SHODAN_QV))
            update_shodometer(curr_shodan_val, game_stuff);
        return (curr_shodan_val);
    } else
        return (0);
}
