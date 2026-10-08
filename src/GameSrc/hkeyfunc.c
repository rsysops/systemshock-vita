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
 * $Source: r:/prj/cit/src/RCS/hkeyfunc.c $
 * $Revision: 1.173 $
 * $Author: dc $
 * $Date: 1994/11/18 00:24:50 $
 */

#include <string.h>
#include <hkeyfunc.h>

#include "Shock.h"
#include "Prefs.h"

#include "fullscrn.h"
#include "grenades.h"
#include "invent.h"
#include "loops.h"
#include "mfdint.h"
#include "mfdext.h"
#include "MacTune.h"
#include "musicai.h"
#include "objwpn.h"
#include "saveload.h"
#include "softdef.h"
#include "tools.h"
#include "wares.h"
#include "mouselook.h"
#include "audiolog.h"
#include "Xmi.h"

//--------------
//  PROTOTYPES
//--------------
int select_object_by_class(int obclass, int num, ubyte *quantlist);

int current_palette_mode = TERRAIN_MODE;

uchar really_quit_key_func(ushort keycode, uint32_t context, intptr_t data) {
    // Avoid ever hitting process exit (atexit/SDL_Quit/GXM teardown) on Vita, where
    // it's a reproducible Vita3K crash real hardware doesn't have - go back to the
    // main menu instead, the same way the game already does after cutscenes/death.
    // Close the pause/options panel first (same as the "No" path already does) so
    // game_paused, the pushed cursor, and wrapper_panel_on don't leak into the next
    // game session and break its pause screen.
    extern errtype wrapper_panel_close(uchar clear_message);
    wrapper_panel_close(TRUE);
    return change_mode_func(keycode, context, SETUP_LOOP);
}

uchar toggle_bool_func(ushort keycode, uint32_t context, intptr_t data) {
    bool *tgl = (bool *)data;
    *tgl = !*tgl;
    return TRUE;
}

extern bool DoubleSize;

uchar change_mode_func(ushort keycode, uint32_t context, intptr_t data) {
    int newm = (int)data;

    if ((newm == AUTOMAP_LOOP) && ((!player_struct.hardwarez[HARDWARE_AUTOMAP]) || (global_fullmap->cyber)))
        return TRUE;
    _new_mode = newm;
    chg_set_flg(GL_CHG_LOOP);
    return TRUE;
}

void start_music(void) {
    //   if (music_card)
    //   {
    if (MacTuneInit() == 0) {
        music_on = TRUE;
        mlimbs_on = TRUE;
        mlimbs_AI_init();
        load_score_for_location(PLAYER_BIN_X, PLAYER_BIN_Y);
        MacTuneStartCurrentTheme();
    } else {
        gShockPrefs.soBackMusic = FALSE;
        SavePrefs();
    }
    //   }
}

void stop_music(void) {
    extern uchar mlimbs_on;

    MacTuneShutdown();
    music_on = FALSE;
    mlimbs_on = FALSE;
    mlimbs_peril = DEFAULT_PERIL_MIN;
    mlimbs_monster = NO_MONSTER;
}

uchar toggle_music_func(ushort keycode, uint32_t context, intptr_t data) {
    if (music_on) {
        message_info("Music off.");
        StopTheMusic(); //do this here, not in stop_music(), to prevent silence when changing levels
        stop_music();
    } else {
        start_music();
        message_info("Music on.");
    }

    gShockPrefs.soBackMusic = music_on;
    SavePrefs();

    return (FALSE);
}

uchar arm_grenade_hotkey(ushort keycode, uint32_t context, intptr_t data) {
    extern uchar show_all_actives;
    extern short inv_last_page;
    int i, row, act;

    if (!show_all_actives) {
        show_all_actives = TRUE;
        inv_last_page = -1;
        chg_set_flg(INVENTORY_UPDATE);
        mfd_force_update();
        return TRUE;
    }
    if (activate_grenade_on_cursor())
        return TRUE;
    act = player_struct.actives[ACTIVE_GRENADE];
    for (i = row = 0; i < act; i++)
        if (player_struct.grenades[i])
            row++;
    super_drop_func(ACTIVE_GRENADE, row);
    return TRUE;
}

int select_object_by_class(int obclass, int num, ubyte *quantlist) {
    extern uchar show_all_actives;
    extern short inv_last_page;
    int act = player_struct.actives[obclass];
    int newobj = act;

    inv_last_page = -1;
    chg_set_flg(INVENTORY_UPDATE);
    if (!show_all_actives) {
        show_all_actives = TRUE;
        return -1;
    }
    do {
        newobj = (newobj + 1) % num;
    } while (quantlist[newobj] == 0 && newobj != act);

    player_struct.actives[obclass] = newobj;
    return newobj;
}

uchar select_grenade_hotkey(ushort keycode, uint32_t context, intptr_t data) {
    int newobj;

    newobj = select_object_by_class(ACTIVE_GRENADE, NUM_GRENADES, player_struct.grenades);
    set_inventory_mfd(MFD_INV_GRENADE, newobj, TRUE);
    return TRUE;
}

uchar select_drug_hotkey(ushort keycode, uint32_t context, intptr_t data) {
    int newobj;

    newobj = select_object_by_class(ACTIVE_DRUG, NUM_DRUGS, player_struct.drugs);
    set_inventory_mfd(MFD_INV_DRUG, newobj, TRUE);
    return TRUE;
}

uchar use_drug_hotkey(ushort keycode, uint32_t context, intptr_t data) {
    extern uchar show_all_actives;
    extern short inv_last_page;
    int i, row, act;

    if (!show_all_actives) {
        show_all_actives = TRUE;
        inv_last_page = -1; // to force redraw
        chg_set_flg(INVENTORY_UPDATE);
        return TRUE;
    }
    act = player_struct.actives[ACTIVE_DRUG];
    for (i = row = 0; i < act; i++)
        if (player_struct.drugs[i])
            row++;
    super_use_func(ACTIVE_DRUG, row);
    return TRUE;
}

uchar clear_fullscreen_func(ushort keycode, uint32_t context, intptr_t data) {
    extern char last_message[128];
    extern MFD mfd[2];

    full_lower_region(&mfd[MFD_RIGHT].reg2);
    full_lower_region(&mfd[MFD_LEFT].reg2);
    full_lower_region(inventory_region_full);
    full_visible = 0;
    strcpy(last_message, "");
    chg_unset_sta(FULLSCREEN_UPDATE);
    return (FALSE);
}

char conv_hex(char val);
uchar location_spew_func(ushort, uint32_t, intptr_t);

char conv_hex(char val) {
    char retval = '?';
    if ((val >= 0) && (val <= 9))
        retval = '0' + val;
    else if ((val >= 10) && (val <= 15))
        retval = 'a' + (val - 10);
    return (retval);
}
/*KLC   moved to TOOLS.C
int str_to_hex(char val)
{
   int retval = 0;
   if ((val >= '0') && (val <= '9'))
      retval = val - '0';
   else if ((val >= 'A') && (val <= 'F'))
      retval = 10 + val - 'A';
   else if ((val >= 'a') && (val <= 'f'))
      retval = 10 + val - 'a';
   return(retval);
}

uchar location_spew_func(ushort , uint32_t , intptr_t )
{
   char goofy_string[32];

//#ifdef SVGA_SUPPORT
//   sprintf(goofy_string,"00:00.00:%s",get_temp_string(REF_STR_ScreenModeText + convert_use_mode));
//#else
   strcpy(goofy_string,"00:00.00 ");
//#endif
   goofy_string[0] = conv_hex( player_struct.level / 16 );
   goofy_string[1] = conv_hex( player_struct.level % 16 );
   if (!time_passes)
      goofy_string[2] = '!';
   goofy_string[3] = conv_hex( PLAYER_BIN_X / 16 );
   goofy_string[4] = conv_hex( PLAYER_BIN_X % 16 );
   if (!physics_running)
      goofy_string[5] = '*';
   goofy_string[6] = conv_hex( PLAYER_BIN_Y / 16 );
   goofy_string[7] = conv_hex( PLAYER_BIN_Y % 16 );

   message_info(goofy_string);
   return(FALSE);
}
*/

uchar toggle_physics_func(ushort keycode, uint32_t context, intptr_t data) {
    physics_running = !physics_running;

    extern uchar pacifism_on;
    pacifism_on = !physics_running;

    if (physics_running)
        message_info("Physics turned on");
    else
        message_info("Physics turned off");

    return (FALSE);
}

uchar toggle_giveall_func(ushort keycode, uint32_t context, intptr_t data) {
    message_info("Kick some ass!");

    for (int i = 0; i < NUM_HARDWAREZ; i++)
        player_struct.hardwarez[i] = 1;
    player_struct.hardwarez[HARDWARE_360] = 3;

    //rail gun
    player_struct.weapons[0].type = GUN_SUBCLASS_SPECIAL;
    player_struct.weapons[0].subtype = 1;
    player_struct.weapons[0].ammo = 50;
    player_struct.weapons[0].ammo_type = 0;
    player_struct.weapons[0].make_info = 0;

    //ion beam
    player_struct.weapons[1].type = GUN_SUBCLASS_BEAM;
    player_struct.weapons[1].subtype = 2;
    player_struct.weapons[1].heat = 0;
    player_struct.weapons[1].setting = 40;
    player_struct.weapons[1].make_info = 0;

    //riot gun, hollow
    player_struct.weapons[2].type = GUN_SUBCLASS_PISTOL;
    player_struct.weapons[2].subtype = 4;
    player_struct.weapons[2].ammo = 100;
    player_struct.weapons[2].ammo_type = 0;
    player_struct.weapons[2].make_info = 0;

    //skorpion, slag
    player_struct.weapons[3].type = GUN_SUBCLASS_AUTO;
    player_struct.weapons[3].subtype = 1;
    player_struct.weapons[3].ammo = 150;
    player_struct.weapons[3].ammo_type = 0;
    player_struct.weapons[3].make_info = 0;

    //magpulse
    player_struct.weapons[4].type = GUN_SUBCLASS_SPECIAL;
    player_struct.weapons[4].subtype = 0;
    player_struct.weapons[4].ammo = 50;
    player_struct.weapons[4].ammo_type = 0;
    player_struct.weapons[4].make_info = 0;

    //sparq
    player_struct.weapons[5].type = GUN_SUBCLASS_BEAM;
    player_struct.weapons[5].subtype = 0;
    player_struct.weapons[5].heat = 0;
    player_struct.weapons[5].setting = 40;
    player_struct.weapons[5].make_info = 0;

    //laser rapier
    player_struct.weapons[6].type = GUN_SUBCLASS_HANDTOHAND;
    player_struct.weapons[6].subtype = 1;
    player_struct.weapons[6].heat = 0;
    player_struct.weapons[6].setting = 0;
    player_struct.weapons[6].make_info = 0;

    player_struct.hit_points = 255;
    player_struct.energy = 255;

    // Software stuff
    player_struct.softs.misc[SOFTWARE_TURBO] = 5;
    player_struct.softs.misc[SOFTWARE_FAKEID] = 5;
    player_struct.softs.misc[SOFTWARE_DECOY] = 5;
    player_struct.softs.misc[SOFTWARE_RECALL] = 5;

    // So we put games in your game so you can play game while you playing game!
    player_struct.softs.misc[SOFTWARE_GAMES] = 255;

    chg_set_flg(INVENTORY_UPDATE);
    chg_set_flg(VITALS_UPDATE);
    mfd_force_update();

    return (FALSE);
}

uchar toggle_up_level_func(ushort keycode, uint32_t context, intptr_t data) {
    message_info("Changing level!");
    go_to_different_level((player_struct.level + 1 + 15) % 15);

    return (TRUE);
}

uchar toggle_down_level_func(ushort keycode, uint32_t context, intptr_t data) {
    message_info("Changing level!");
    go_to_different_level((player_struct.level - 1 + 15) % 15);

    return (TRUE);
}

uchar pause_game_func(ushort keycode, uint32_t context, intptr_t data) {
    extern uchar game_paused, redraw_paused;

    game_paused = !game_paused;
    CaptureMouse(!game_paused);

    extern LGCursor globcursor;
    if (game_paused) uiPushGlobalCursor(&globcursor);
    else uiPopGlobalCursor();

    if (game_paused) {
        redraw_paused = TRUE;
		snd_kill_all_samples();
        audiolog_stop();
        return FALSE;
    }

    mouse_look_unpause();

    return TRUE;
    /* KLC - not needed for Mac version
            game_paused = !game_paused;
            if (game_paused)
            {
                    uiPushGlobalCursor(&globcursor);
                    uiInstallRegionHandler(inventory_region, UI_EVENT_MOUSE_MOVE, pause_callback, NULL, &pause_id);
                    uiGrabFocus(inventory_region, UI_EVENT_MOUSE_MOVE);
                    stop_digi_fx();
                    redraw_paused=TRUE;
            }
            else
            {
                    uiRemoveRegionHandler(inventory_region, pause_id);
                    uiReleaseFocus(inventory_region, UI_EVENT_MOUSE_MOVE);
                    uiPopGlobalCursor();
            }
    */
}

/*KLC - not needed for Mac version
uchar unpause_game_func(ushort, uint32_t, intptr_t)
{
        extern uchar game_paused;
        extern LGRegion *inventory_region;

        if (game_paused)
        {
                game_paused = !game_paused;
                uiRemoveRegionHandler(inventory_region, pause_id);
                uiReleaseFocus(inventory_region, UI_EVENT_MOUSE_MOVE|UI_EVENT_JOY);
                uiPopGlobalCursor();
        }
        return(FALSE);
}
*/

uchar toggle_mouse_look(ushort keycode, uint32_t context, intptr_t data) {
    mouse_look_toggle();
    return (TRUE);
}

//--------------------------------------------------------------------
//  For Mac version.  Save the current game.
//--------------------------------------------------------------------
/*
uchar save_hotkey_func(ushort keycode, uint32_t context, intptr_t data) {
    if (global_fullmap->cyber) // Can't save in cyberspace.
    {
        message_info("Can't save game in cyberspace.");
        return TRUE;
    }

    if (music_on) // Setup the environment for doing Mac stuff.
        MacTuneKillCurrentTheme();
    uiHideMouse(NULL);
    SS_ShowCursor();

    // CopyBits(&gMainWindow->portBits, &gMainOffScreen.bits->portBits, &gActiveArea, &gOffActiveArea, srcCopy, 0L);

    if (gIsNewGame) // Do the save thang.
    {
        status_bio_end();

        // Fixme: Save game here!

        status_bio_start();
    }

    uiShowMouse(NULL);
    if (music_on)
        MacTuneStartCurrentTheme();

    return TRUE;
}
*/

