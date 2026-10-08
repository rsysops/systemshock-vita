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
 * $Source: r:/prj/cit/src/RCS/olh.c $
 * $Revision: 1.27 $
 * $Author: dc $
 * $Date: 1994/11/21 09:02:38 $
 *
 */

#include <stdlib.h>
#include <string.h>

#include "Prefs.h"
#include "VitaGpu.h"
#include "Shock.h"

#include "player.h"
#include "gamestrn.h"
#include "objapp.h"
#include "objects.h"
#include "objprop.h"
#include "hudobj.h"
#include "hud.h"
#include "mainloop.h"
#include "gamescr.h"
#include "olhint.h"
#include "faketime.h"
#include "objbit.h"
#include "objuse.h"
#include "olhscan.h"
#include "doorparm.h"
#include "render.h"
#include "sdl_events.h"
#include "strwrap.h"
#include "tools.h"
#include "trigger.h"
#include "game_screen.h"
#include "fullscrn.h"
#include "input.h"
#include "grenades.h"
#include "mfdext.h"
#include "olhext.h"
#include "vprof.h"
#include "citres.h"
#include "cit2d.h"
#include "gr2ss.h"
#include "hkeyfunc.h"
#include "status.h"

#include "otrip.h"
#include "cybstrng.h"

// -------------------------------------------------
//          ON-LINE HELP FOR SYSTEM SHOCK
// -------------------------------------------------

uchar olh_active = TRUE;
uchar olh_overlay_on = FALSE;
olh_data olh_object = {OBJ_NULL, {0, 0}};

// ---------
// INTERNALS
// ---------

char *get_olh_string(ObjID obj, char *buf);
LGPoint draw_olh_string(char *s, short xl, short yl);
void olh_do_panel_ref(short xl, short yl);
void olh_do_callout(short xl, short yl);
uchar is_compound_use_obj(ObjID obj);
void olh_do_cursor(short xl, short yl);

#define IS_SCREEN(obj)                                                                                       \
    (objs[obj].obclass == CLASS_BIGSTUFF && objs[obj].subclass == BIGSTUFF_SUBCLASS_ONTHEWALL &&             \
     (objs[obj].info.type == TRIP2TY(SCREEN_TRIPLE) || objs[obj].info.type == TRIP2TY(SUPERSCREEN_TRIPLE) || \
      objs[obj].info.type == TRIP2TY(BIGSCREEN_TRIPLE)))

uchar olh_candidate(ObjID obj) {
    uchar check_dist = FALSE;
    uchar retval = FALSE;

    if (objs[obj].info.inst_flags & OLH_INST_FLAG)
        return FALSE;
    switch (ID2TRIP(obj)) {
    case CAMERA_TRIPLE:
    case LARGCPU_TRIPLE:
        check_dist = FALSE;
        retval = TRUE;
        break;
    default:
        if (USE_MODE(obj) == NULL_USE_MODE)
            return FALSE;
        break;
    }
    switch (objs[obj].obclass) {
    case CLASS_DOOR:
        if ((ID2TRIP(obj) == LABFORCE_TRIPLE) || (ID2TRIP(obj) == RESFORCE_TRIPLE))
            return FALSE;
        check_dist = !door_locked(obj) && !door_moving(obj, FALSE) && DOOR_REALLY_CLOSED(obj);
        break;
    case CLASS_BIGSTUFF:
        if (IS_SCREEN(obj)) {
            extern char camera_map[NUM_HACK_CAMERAS];
            extern ObjID hack_cam_objs[NUM_HACK_CAMERAS];
            ObjSpecID sid = objs[obj].specID;
            short v = objBigstuffs[sid].data2 & 0x7F;
            if ((v >= FIRST_CAMERA_TMAP) && (v < FIRST_CAMERA_TMAP + NUM_HACK_CAMERAS)) {
                if (camera_map[v - FIRST_CAMERA_TMAP] && hack_cam_objs[v - FIRST_CAMERA_TMAP])
                    check_dist = TRUE;
            }
            break;
        }
        if (ID2TRIP(obj) == SURG_MACH_TRIPLE) {
            check_dist = TRUE;
            break;
        }
        if (((ObjProps[OPNUM(obj)].flags & CLASS_FLAGS) >> CLASS_FLAGS_SHF) == STUFF_OBJUSE_FLAG) {
            if (objBigstuffs[objs[obj].specID].data1 != 0)
                check_dist = TRUE;
            break;
        }
    case CLASS_SMALLSTUFF:
        if (USE_MODE(obj) == USE_USE_MODE) {
            check_dist = TRUE;
            break;
        }
        if (USE_MODE(obj) == PICKUP_USE_MODE) {
            if (ObjProps[OPNUM(obj)].flags & INVENTORY_GENERAL)
                check_dist = TRUE;
            break;
        }
        // smallstuff falls through to default.
    default:
        if (USE_MODE(obj) == PICKUP_USE_MODE || USE_MODE(obj) == USE_USE_MODE)
            check_dist = TRUE;
        break;
    }

    if (check_dist) {
        int mode = USE_MODE(obj);
        fix crit = (mode == PICKUP_USE_MODE) ? MAX_PICKUP_DIST : MAX_USE_DIST;

        if (check_object_dist(obj, PLAYER_OBJ, crit))
            retval = TRUE;
    }
    return retval;
}

short use_mode_idx[] = {
    REFINDEX(REF_STR_helpTake),
    REFINDEX(REF_STR_helpUse),
    -1,
    -1,
};

short weap_subclass_idx[] = {
    REFINDEX(REF_STR_helpAttackGun), REFINDEX(REF_STR_helpAttackAuto), REFINDEX(REF_STR_helpAttackGun),
    REFINDEX(REF_STR_helpAttackHTH), REFINDEX(REF_STR_helpAttackGun),  REFINDEX(REF_STR_helpAttackGun),
};

// basically we chose a string id for a string
// that has  %s, and lg_strintf the name into the
// the string.
char *get_olh_string(ObjID obj, char *buf) {

    int *d1, *d2;
    int obclass = objs[obj].obclass;
    Ref r = 0;
    short mode;

    switch (obclass) {
    case CLASS_CRITTER: {
        int w = player_struct.actives[ACTIVE_WEAPON];
        int type = player_struct.weapons[w].type;
        if (type == EMPTY_WEAPON_SLOT)
            return strcpy(buf, get_object_long_name(ID2TRIP(obj), NULL, 0));
        r = MKREF(RES_olh_strings, weap_subclass_idx[type]);
        goto got_id;
    }
    case CLASS_DOOR:
        r = REF_STR_helpDoor;
        goto got_id;
    default:
        break;
    }
    switch (ID2TRIP(obj)) {
    case CAMERA_TRIPLE:
    case LARGCPU_TRIPLE:
        r = REF_STR_helpSecurity;
        goto got_id;
    }
    mode = USE_MODE(obj);
    if (is_container(obj, &d1, &d2) && mode == USE_USE_MODE) {
        r = REF_STR_helpSearch;
        goto got_id;
    }
    if (use_mode_idx[mode] != -1) {
        r = MKREF(RES_olh_strings, use_mode_idx[mode]);
        goto got_id;
    }
    // more cases go here...

got_id:
    if (r != 0) {
        char *s = (char *)RefLock(r);
        sprintf(buf, s, get_object_long_name(ID2TRIP(obj), NULL, 0));
        RefUnlock(r);
    }
    return buf;
}

// ---------
// EXTERNALS
// ---------
extern bool DoubleSize;

//-----------------------------
// olh_scan_objects()
//
// This gets called to detect objects in front of the player that have
// help strings.  It sets up for olh_do_hudobjs.

#define SCAN_FREQ_SHF (APPROX_CIT_CYCLE_SHFT - 2)

void olh_scan_objects(void) {
    static uint last_scan = 0;

    if (*tmd_ticks >> SCAN_FREQ_SHF <= last_scan)
        return;
    if (input_cursor_mode == INPUT_OBJECT_CURSOR)
        return;
    if (player_struct.panel_ref != OBJ_NULL)
        return;
    olh_scan_objs();
    if (olh_object.obj == OBJ_NULL)
        return;
}

static uchar scan_render; // the render under way is the game loop's
static uchar scan_ran;    // and the scan ran in it

void olh_scan_render_begin(void) {
    scan_render = TRUE;
    scan_ran = FALSE;
}

void olh_scan_render_end(void) { scan_render = FALSE; }

uchar olh_scan_in_render(void) {
    // the game loop's own conditions (see game_loop)
    if (!scan_render || scan_ran || olh_overlay_on || !olh_active || global_fullmap->cyber)
        return FALSE;
    scan_ran = TRUE;
    VPROF_RUN(VPROF_HELPSCAN, olh_scan_objects());
    return TRUE;
}

uchar olh_scan_ran_in_render(void) {
    uchar ran = scan_ran;
    scan_ran = FALSE;
    return ran;
}

LGPoint draw_olh_string(char *s, short xl, short yl) {
    short w, h;
    short x, y;

    string_replace_char(s, '\n', CHAR_SOFTSP);
    gr_set_font(ResGet(RES_tinyTechFont));
    gr_set_fcolor(hud_colors[hud_color_bank][2]);
    gr_string_wrap(s, OLH_WRAP_WID);
    gr_string_size(s, &w, &h);
    ss_point_convert(&xl, &yl, TRUE);
    if (DoubleSize) {
        xl *= 2;
        yl = yl * 2 + 1;
    }
    x = SCREEN_VIEW_X - xl + SCREEN_VIEW_WIDTH - w - 1;
    y = SCREEN_VIEW_Y - yl + SCREEN_VIEW_HEIGHT - h - 1;
    draw_shadowed_string(s, x, y, TRUE);
    return MakePoint(x - 1, y + (h / 2));
}

ushort fixture_panel_stringrefs[] = {
    REFINDEX(REF_STR_helpPanel),    REFINDEX(REF_STR_helpPanel),    REFINDEX(REF_STR_helpPanel),
    REFINDEX(REF_STR_helpPanel),    REFINDEX(REF_STR_helpElevator), REFINDEX(REF_STR_helpElevator),
    REFINDEX(REF_STR_helpElevator), REFINDEX(REF_STR_helpKeypad),   REFINDEX(REF_STR_helpKeypad),
    REFINDEX(REF_STR_helpPanel),    REFINDEX(REF_STR_helpPanel),
};

void olh_do_panel_ref(short xl, short yl) {
    int *d1, *d2;
    ObjID obj = player_struct.panel_ref;
    char buf[80];

    buf[0] = '\0';
    if (is_container(obj, &d1, &d2) && ((d1 != NULL && *d1 != 0) || (d2 != NULL && *d2 != 0))) {
        char namebuf[80];

        get_object_lookname(obj, namebuf, sizeof(namebuf));
        sprintf(buf, get_temp_string(REF_STR_helpGump), namebuf);
    } else if (objs[obj].obclass == CLASS_FIXTURE) {
        Ref ref = 0;
        if (objs[obj].subclass == FIXTURE_SUBCLASS_CONTROL)
            ref = REF_STR_helpSwitch;
        else if (objs[obj].subclass == FIXTURE_SUBCLASS_PANEL) {
            uchar special;
            ObjFixture *pfixt = &objFixtures[objs[obj].specID];
            uchar rv = comparator_check(pfixt->comparator, obj, &special);
            if (special == 0)
                ref = MKREF(RES_olh_strings, fixture_panel_stringrefs[objs[obj].info.type]);
        }
        if (ref != 0)
            get_string(ref, buf, sizeof(buf));
    }
    if (buf[0] != '\0')
        draw_olh_string(buf, xl, yl);
}

void olh_do_callout(short xl, short yl) {
    int best_rect = -1;
    ObjID obj = olh_object.obj;

    if (obj == OBJ_NULL)
        return;
       //   if (obj != OBJ_NULL)
    {
        char buf[80];
        char *s = get_olh_string(obj, buf);
        LGPoint spos = draw_olh_string(s, xl, yl);
        LGPoint pos = olh_object.loc;
        pos.x = (int)(pos.x + 1) * SCAN_RATIO;
        pos.y = (int)(pos.y + 1) * SCAN_RATIO;
        if (DoubleSize) {
            pos.x *= 2;
            pos.y *= 2;
        }
        ss_int_line(spos.x - 1, spos.y - 1, pos.x, pos.y);
    }
    if (best_rect != -1) {
        struct _hudobj_data *dat = &hudobj_vec[best_rect];
        gr_set_fcolor(hud_colors[hud_color_bank][0]);
        ss_box(dat->xl - 1, dat->yl - 1, dat->xh + 1, dat->yh + 1);
    }
}

// A "compound use" object is one of those nasty objects
// that used on another object by double clicking on that
// object.

uchar is_compound_use_obj(ObjID obj) {
    if (objs[obj].obclass != CLASS_SMALLSTUFF)
        return FALSE;
    if (objs[obj].subclass == SMALLSTUFF_SUBCLASS_PLOT)
        return TRUE;
    switch (ID2TRIP(obj)) {
    case EPICK_TRIPLE:
    case HEAD_TRIPLE:
    case HEAD2_TRIPLE:
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

void olh_do_cursor(short xl, short yl) {
    ObjID obj = object_on_cursor;
    // this should be a different string if the cursor is
    // a live grenade.
    char buf[80];
    if ((objs[obj].obclass == CLASS_GRENADE &&
         objGrenades[objs[obj].specID].flags & GREN_ACTIVE_FLAG) ||
        ObjProps[OPNUM(obj)].flags & USELESS_FLAG)
        get_string(REF_STR_helpGrenade, buf, sizeof(buf));
    else {
        char stringbuf[80];
        char namebuf[80];
        Ref id = is_compound_use_obj(obj) ? REF_STR_helpCompound : REF_STR_helpCursor;

        get_string(id, stringbuf, sizeof(stringbuf));
        get_object_lookname(obj, namebuf, sizeof(namebuf));
        sprintf(buf, stringbuf, namebuf);
    }
    draw_olh_string(buf, xl, yl);
}

// -------------------------------------------------
// olh_do_hudobjs is called by the hud system to
// draw olh hud.

void olh_do_hudobjs(short xl, short yl) {
    extern uchar saveload_static;
    if (global_fullmap->cyber || saveload_static)
        return;
    if (input_cursor_mode == INPUT_OBJECT_CURSOR)
        olh_do_cursor(xl, yl);
    else if (player_struct.panel_ref != OBJ_NULL) {
        int i;
        for (i = 0; i < NUM_MFDS; i++)
            if (player_struct.mfd_current_slots[i] == MFD_INFO_SLOT) {
                olh_do_panel_ref(xl, yl);
                return;
            }
    } else
        olh_do_callout(xl, yl);
}

/*KLC - no longer used
void olh_init(void)
{
   extern  void olh_init_scan(void);
   olh_init_scan();
}
*/

void olh_closedown(void) { olh_object.obj = OBJ_NULL; }

void olh_shutdown(void) {
    olh_free_scan();
}

short _olh_overlay_keys[] = {
    ' ' | KB_FLAG_DOWN,
    KEY_ENTER | KB_FLAG_DOWN, // the pad's cross, circle and triangle (see sdl_events.c)
    '?' | KB_FLAG_DOWN,
};

#define NUM_OVERLAY_KEYS (sizeof(_olh_overlay_keys) / sizeof(_olh_overlay_keys[0]))

// The help screen (English, French or German image), reworded for the Vita pad and relabelled
// with the game font. The image is copied and its lettering erased with each box's own fill; the
// labels are then drawn with ss_string(), which picks the font matching the screen resolution
// (see ss_scale_string()), so they're as sharp as the rest of the interface's text. The font is
// CP437: \x8E, \x99 and \x9A are the umlauted A, O and U. Like the original French image, the
// French labels go without accents, which the font mostly lacks in capitals.

#define OLH_BOX_TEXT(c) ((c) == 0x31 || (c) == 0x35) // text colours inside the boxes
#define OLH_CENTRED -1                                 // x of a box whose lines are centred

// pad buttons inside a line, drawn with draw_pad_glyph()
#define OLH_CROSS "\001"
#define OLH_CIRCLE "\002"
#define OLH_TRIANGLE "\003"
#define OLH_GLYPH_SIZE 5 // font row 0 (blank) down to the baseline

typedef struct {
    short x0, y0, x1, y1; // interior, inclusive: all its lettering is erased
    uchar color;          // text colour
    short x, right;       // left edge of the lines (or OLH_CENTRED), right edge of the right-aligned column
    short top[4];         // first row of each line's capitals
    const char *left[4], *right_text[4];
} olh_box;

// every box of each image, at the positions of its original lettering
static const olh_box olh_boxes_en[] = {
    {190, 2, 230, 9, 0x35, OLH_CENTRED, 0, {4}, {"HEALTH"}, {NULL}},
    {190, 13, 230, 20, 0x35, OLH_CENTRED, 0, {15}, {"ENERGY"}, {NULL}},
    {35, 24, 75, 31, 0x35, OLH_CENTRED, 0, {26}, {"BIOMETER"}, {NULL}},
    // posture panel, look half
    {112, 29, 157, 50, 0x35, 116, 0, {32, 38, 44}, {"TAP HERE", "TO LOOK UP", "AND DOWN"}, {NULL}},
    // posture panel, stance half
    {166, 29, 211, 50, 0x35, 170, 0, {32, 38, 44}, {"TAP HERE", "TO CROUCH", "AND LEAN"}, {NULL}},
    // was "click here for fullscreen mode", out of the pad's reach
    {23, 48, 91, 61, 0x35, 28, 0, {50, 56}, {"SWIPE REAR PAD", "FOR MFD PAGES"}, {NULL}},
    {134, 66, 174, 73, 0x35, OLH_CENTRED, 0, {68}, {"3D VIEW"}, {NULL}},
    {232, 66, 279, 87, 0x35, 234, 0, {69, 75, 81}, {"SOCKETS FOR", "NEUROGRAFT", "HARDWARE"}, {NULL}},
    // keys
    {85, 78, 222, 99, 0x31, 87, 220, {80, 86, 93},
     {OLH_CROSS " " OLH_CIRCLE " " OLH_TRIANGLE, "START, " OLH_TRIANGLE, "L / R"},
     {"CONTINUE GAME", "BRINGS THIS SCREEN BACK", "USE / ATTACK"}},
    // inventory
    {90, 118, 228, 143, 0x35, 93, 0, {122, 128, 135},
     {"TAP OBJECTS IN INVENTORY", "TO SELECT, DOUBLE TAP TO USE,", "POINT AND PRESS R TO REMOVE"},
     {NULL}},
    {283, 140, 310, 145, 0x35, OLH_CENTRED, 0, {141}, {"WEAPON"}, {NULL}},
    {16, 148, 72, 161, 0x35, 17, 0, {150, 156}, {"MULTI-FUNCTION", "DISPLAY (MFD)"}, {NULL}},
    {283, 150, 310, 155, 0x35, OLH_CENTRED, 0, {151}, {"ITEM"}, {NULL}},
    {234, 155, 271, 176, 0x35, 237, 0, {158, 164, 170}, {"MFD", "SELECTION", "BUTTONS"}, {NULL}},
    {95, 158, 208, 165, 0x35, OLH_CENTRED, 0, {160}, {"INVENTORY SELECTION BUTTONS"}, {NULL}},
    {283, 161, 310, 166, 0x35, OLH_CENTRED, 0, {162}, {"MAP"}, {NULL}},
    {283, 172, 310, 177, 0x35, OLH_CENTRED, 0, {173}, {"TARGET"}, {NULL}},
    {106, 179, 140, 184, 0x35, OLH_CENTRED, 0, {180}, {"HARDWARE"}, {NULL}},
    {283, 183, 310, 188, 0x35, OLH_CENTRED, 0, {184}, {"DATA"}, {NULL}},
    {194, 185, 229, 190, 0x35, OLH_CENTRED, 0, {186}, {"SOFTWARE"}, {NULL}},
    {89, 188, 106, 193, 0x35, OLH_CENTRED, 0, {189}, {"MAIN"}, {NULL}},
    {132, 188, 161, 193, 0x35, OLH_CENTRED, 0, {189}, {"GENERAL"}, {NULL}},
};

static const olh_box olh_boxes_fr[] = {
    {190, 2, 230, 9, 0x35, OLH_CENTRED, 0, {4}, {"SANTE"}, {NULL}},
    {190, 13, 230, 20, 0x35, OLH_CENTRED, 0, {15}, {"ENERGIE"}, {NULL}},
    {35, 24, 75, 31, 0x35, OLH_CENTRED, 0, {26}, {"BIOMETRE"}, {NULL}},
    // posture panel, look half
    {112, 29, 157, 50, 0x35, 116, 0, {32, 38, 44}, {"TOUCHER ICI", "POUR VUE", "HAUT ET BAS"}, {NULL}},
    // posture panel, stance half
    {166, 29, 211, 50, 0x35, 170, 0, {32, 38, 44}, {"TOUCHER ICI", "-SE TAPIR", "-PENCHER"}, {NULL}},
    // was fullscreen mode
    {23, 48, 84, 61, 0x35, 28, 0, {50, 56}, {"PAVE ARRIERE :", "PAGES DES VMF"}, {NULL}},
    {134, 66, 174, 73, 0x35, OLH_CENTRED, 0, {68}, {"VUE 3D"}, {NULL}},
    {232, 66, 279, 87, 0x35, 234, 0, {69, 75, 81}, {"PRISES POUR", "IMPLANTS", "NEUROL."}, {NULL}},
    // keys
    {90, 80, 227, 101, 0x31, 92, 225, {82, 88, 94},
     {OLH_CROSS " " OLH_CIRCLE " " OLH_TRIANGLE, "START, " OLH_TRIANGLE, "L / R"},
     {"CONTINUER LE JEU", "REVOIR CET ECRAN", "UTILISER / ATTAQUER"}},
    // inventory
    {90, 118, 228, 143, 0x35, 93, 0, {122, 128, 135},
     {"TOUCHER UN OBJET POUR LE CHOISIR,", "TOUCHER DEUX FOIS POUR L'UTILISER,", "VISER ET PRESSER R POUR L'ENLEVER"},
     {NULL}},
    {283, 140, 310, 145, 0x35, OLH_CENTRED, 0, {141}, {"ARME"}, {NULL}},
    {16, 148, 72, 161, 0x35, 17, 0, {150, 156}, {"(VMF) VISUEL", "MULTI-FONCTION"}, {NULL}},
    {283, 150, 310, 155, 0x35, OLH_CENTRED, 0, {151}, {"OBJET"}, {NULL}},
    {234, 155, 271, 176, 0x35, 237, 0, {158, 164, 170}, {"TOUCHES", "SELECTION", "VMF"}, {NULL}},
    {95, 158, 208, 165, 0x35, OLH_CENTRED, 0, {160}, {"TOUCHES SELECTION STOCK"}, {NULL}},
    {283, 161, 310, 166, 0x35, OLH_CENTRED, 0, {162}, {"CARTE"}, {NULL}},
    {283, 172, 310, 177, 0x35, OLH_CENTRED, 0, {173}, {"CIBLE"}, {NULL}},
    {106, 179, 140, 184, 0x35, OLH_CENTRED, 0, {180}, {"MATERIEL"}, {NULL}},
    {283, 183, 310, 188, 0x35, OLH_CENTRED, 0, {184}, {"DATA"}, {NULL}},
    {194, 185, 229, 190, 0x35, OLH_CENTRED, 0, {186}, {"LOGICIEL"}, {NULL}},
    {83, 188, 114, 193, 0x35, OLH_CENTRED, 0, {189}, {"ARMES"}, {NULL}},
    {132, 188, 161, 193, 0x35, OLH_CENTRED, 0, {189}, {"GENERAL"}, {NULL}},
};

static const olh_box olh_boxes_de[] = {
    {190, 2, 230, 9, 0x35, OLH_CENTRED, 0, {4}, {"GESUNDHEIT"}, {NULL}},
    {190, 13, 230, 20, 0x35, OLH_CENTRED, 0, {15}, {"ENERGIE"}, {NULL}},
    {35, 24, 75, 31, 0x35, OLH_CENTRED, 0, {26}, {"BIOMETER"}, {NULL}},
    // posture panel, look half
    {112, 29, 157, 50, 0x35, 116, 0, {32, 38, 44}, {"HIER TIPPEN:", "AUF- UND", "ABBLICKEN"}, {NULL}},
    // posture panel, stance half
    {166, 29, 211, 50, 0x35, 170, 0, {32, 38, 44}, {"HIER TIPPEN:", "KAUERN UND", "LEHNEN"}, {NULL}},
    // was fullscreen mode
    {23, 48, 91, 61, 0x35, 28, 0, {50, 56}, {"HINTEN WISCHEN:", "MFD-SEITEN"}, {NULL}},
    {134, 66, 174, 73, 0x35, OLH_CENTRED, 0, {68}, {"3D BILD"}, {NULL}},
    {232, 66, 279, 87, 0x35, 236, 0, {69, 75, 81}, {"STECKER F\x9AR", "NEURALE", "HARDWARE"}, {NULL}},
    // keys
    {89, 86, 226, 107, 0x31, 91, 224, {89, 95, 101},
     {OLH_CROSS " " OLH_CIRCLE " " OLH_TRIANGLE, "START, " OLH_TRIANGLE, "L / R"},
     {"SPIEL FORTSETZEN", "DIESEN BILDSCHIRM ZEIGEN", "BENUTZEN / ANGREIFEN"}},
    // inventory
    {90, 118, 228, 143, 0x35, 93, 0, {119, 126, 132, 138},
     {"OBJEKTE IM INVENTAR ANTIPPEN,", "UM SIE ZU W\x8EHLEN, DOPPELT TIPPEN,", "UM SIE ZU BENUTZEN, ZIELEN UND R", "DR\x9A" "CKEN, UM SIE ZU ENTFERNEN"},
     {NULL}},
    {283, 140, 310, 145, 0x35, OLH_CENTRED, 0, {141}, {"WAFFEN"}, {NULL}},
    {16, 148, 72, 161, 0x35, OLH_CENTRED, 0, {150, 156}, {"MULTIFUNKTIONS", "ANZEIGE (MFD)"}, {NULL}},
    {283, 150, 311, 155, 0x35, OLH_CENTRED, 0, {151}, {"GEGENST"}, {NULL}},
    {234, 155, 271, 176, 0x35, 239, 0, {161, 167}, {"MFD", "AUSWAHL"}, {NULL}},
    {95, 158, 208, 165, 0x35, OLH_CENTRED, 0, {161}, {"INVENTARAUSWAHLKN\x99PFE"}, {NULL}},
    {283, 161, 310, 166, 0x35, OLH_CENTRED, 0, {162}, {"KARTE"}, {NULL}},
    {283, 172, 310, 177, 0x35, OLH_CENTRED, 0, {173}, {"ZIEL"}, {NULL}},
    {106, 179, 140, 184, 0x35, OLH_CENTRED, 0, {180}, {"HARDWARE"}, {NULL}},
    {283, 183, 310, 188, 0x35, OLH_CENTRED, 0, {184}, {"DATEN"}, {NULL}},
    {194, 185, 229, 190, 0x35, OLH_CENTRED, 0, {186}, {"SOFTWARE"}, {NULL}},
    {83, 188, 114, 193, 0x35, OLH_CENTRED, 0, {189}, {"WAFFEN"}, {NULL}},
    {132, 188, 167, 193, 0x35, OLH_CENTRED, 0, {189}, {"ALLGEMEIN"}, {NULL}},
};

// indexed by which_lang
static const struct {
    const olh_box *boxes;
    int count;
} olh_langs[] = {
    {olh_boxes_en, sizeof(olh_boxes_en) / sizeof(olh_boxes_en[0])},
    {olh_boxes_fr, sizeof(olh_boxes_fr) / sizeof(olh_boxes_fr[0])},
    {olh_boxes_de, sizeof(olh_boxes_de) / sizeof(olh_boxes_de[0])},
};

#define OLH_NUM_LANGS (sizeof(olh_langs) / sizeof(olh_langs[0]))

// draws (or with draw FALSE, only measures) a line: text runs with the current font, pad
// buttons as glyphs; returns its width
static short olh_line(const char *s, short x, short y, uchar draw) {
    char run[64];
    short start = x;

    while (*s) {
        if (*s >= '\001' && *s <= '\003') {
            if (draw)
                draw_pad_glyph(*s == '\001' ? 'x' : *s == '\002' ? 'o' : 't', x, y, OLH_GLYPH_SIZE);
            x += OLH_GLYPH_SIZE + 2;
            s++;
        } else {
            short w, h;
            int n = 0;

            while (s[n] && (s[n] < '\001' || s[n] > '\003') && n < sizeof(run) - 1) {
                run[n] = s[n];
                n++;
            }
            run[n] = '\0';
            gr_string_size(run, &w, &h);
            if (draw)
                ss_string(run, x, y);
            x += w;
            s += n;
        }
    }
    return x - start;
}

// The old text is filled in from the box's fill, which is a smooth texture: ring by ring, each
// text pixel takes the commonest colour among its already filled neighbours, which leaves no
// letter shapes behind. Colour 0 (transparent) never occurs inside a box, so it marks the holes.
static void olh_erase_text(grs_bitmap *bm, const olh_box *b) {
    int w = b->x1 - b->x0 + 1, h = b->y1 - b->y0 + 1;
    uchar *next = malloc(w * h); // this ring's colours, 0 = pixel not filled yet
    uchar filled;
    int x, y, dx, dy, i, j;

    if (next == NULL)
        return;
    for (y = b->y0; y <= b->y1; y++)
        for (x = b->x0; x <= b->x1; x++)
            if (OLH_BOX_TEXT(bm->bits[y * bm->row + x]))
                bm->bits[y * bm->row + x] = 0;

    do {
        filled = FALSE;
        memset(next, 0, w * h);
        for (y = b->y0; y <= b->y1; y++)
            for (x = b->x0; x <= b->x1; x++) {
                uchar near[8], best = 0;
                int n = 0, best_count = 0;

                if (bm->bits[y * bm->row + x])
                    continue;
                for (dy = -1; dy <= 1; dy++)
                    for (dx = -1; dx <= 1; dx++) {
                        uchar c;
                        if ((!dx && !dy) || x + dx < b->x0 || x + dx > b->x1 || y + dy < b->y0 || y + dy > b->y1)
                            continue;
                        c = bm->bits[(y + dy) * bm->row + x + dx];
                        if (c)
                            near[n++] = c;
                    }
                for (i = 0; i < n; i++) {
                    int count = 0;
                    for (j = 0; j < n; j++)
                        count += near[j] == near[i];
                    if (count > best_count || (count == best_count && near[i] < best)) {
                        best = near[i];
                        best_count = count;
                    }
                }
                next[(y - b->y0) * w + x - b->x0] = best;
            }
        for (y = b->y0; y <= b->y1; y++)
            for (x = b->x0; x <= b->x1; x++)
                if (next[(y - b->y0) * w + x - b->x0]) {
                    bm->bits[y * bm->row + x] = next[(y - b->y0) * w + x - b->x0];
                    filled = TRUE;
                }
    } while (filled);
    free(next);
}

// draws the help screen of language lang with the Vita wording; FALSE if it couldn't be loaded
static uchar olh_draw_vita_overlay(int lang) {
    const olh_box *boxes = olh_langs[lang].boxes;
    grs_bitmap bm;
    uchar old_over = gr2ss_override;
    int i, l;

    if (simple_load_res_bitmap(&bm, REF_IMG_bmHelpOverlayEnglish + MKREF(lang, 0)) != OK)
        return FALSE;
    if (bm.type != BMT_FLAT8 || bm.w < 320 || bm.h < 200) {
        free(bm.bits);
        return FALSE;
    }
    for (i = 0; i < olh_langs[lang].count; i++)
        olh_erase_text(&bm, &boxes[i]);
    ss_bitmap(&bm, 0, 0);
    free(bm.bits);

    gr2ss_override = OVERRIDE_ALL;
    gr_set_font((grs_font *)ResLock(RES_tinyTechFont));
    for (i = 0; i < olh_langs[lang].count; i++) {
        const olh_box *b = &boxes[i];

        gr_set_fcolor(b->color);
        for (l = 0; l < 4; l++) {
            // the font's row 0 is blank: its capitals start one row down
            short y = b->top[l] - 1;

            if (b->left[l]) {
                short x = b->x;
                // widths end with the last letter's blank column, hence the + 2
                if (x == OLH_CENTRED)
                    x = (b->x0 + b->x1 + 2 - olh_line(b->left[l], 0, 0, FALSE)) / 2;
                olh_line(b->left[l], x, y, TRUE);
            }
            if (b->right_text[l])
                olh_line(b->right_text[l], b->right + 2 - olh_line(b->right_text[l], 0, 0, FALSE), y, TRUE);
        }
    }
    ResUnlock(RES_tinyTechFont);
    gr2ss_override = old_over;
    return TRUE;
}

void olh_overlay(void) {
    extern LGCursor globcursor;
    extern char which_lang;
    uchar done = FALSE;

    status_bio_end();
    VitaSyncView(); // the overlay is drawn on the screen buffer, over the view
    uiPushGlobalCursor(&globcursor);
    gr_push_canvas(grd_screen_canvas);
    uiHideMouse(NULL);
    if (which_lang < 0 || which_lang >= OLH_NUM_LANGS || !olh_draw_vita_overlay(which_lang))
        draw_res_bm(REF_IMG_bmHelpOverlayEnglish + MKREF(which_lang, 0), 0, 0);
    uiShowMouse(NULL);
    gr_pop_canvas();
    uiFlush();

    while (!done) {
        ushort key;
        ss_mouse_event me;
        pump_events(); // DG: apparently this can loop for a long time waiting for input w/o game_loop() being able to
                       // update events

        tight_loop(FALSE);
        // FIXME It crash on Linux
        /*if (mouse_next(&me) == OK)
        {
          if (me.type == MOUSE_LDOWN)
                  done = TRUE;
        }*/
        if (kb_get_cooked(&key)) {
            int i;
            for (i = 0; i < NUM_OVERLAY_KEYS; i++)
                if (_olh_overlay_keys[i] == key) {
                    done = TRUE;
                    if (key == ('?' | KB_FLAG_DOWN))
                        hotkey_dispatch(key);
                }
        }

        SDLDraw();
    }

    uiPopGlobalCursor();
    uiFlush();
    olh_overlay_on = FALSE;
    // in fullscreen, the caller's render_run() repaints it all; screen_draw() is the normal HUD
    if (!full_game_3d) {
        gr_clear(0); //makes red pixels go away, but real problem is probably in REF_IMG_bmBlankMFD
        screen_draw();
    }
    status_bio_start();
}

uchar toggle_olh_func(ushort keycode, uint32_t context, intptr_t data) {
    if (!olh_active) {
        string_message_info(REF_STR_helpOn);
        olh_active = TRUE;
    } else {
        string_message_info(REF_STR_helpOff);
        olh_active = FALSE;
        ResUnlock(RES_olh_strings); // KLC - added to free strings.
    }
    gShockPrefs.goOnScreenHelp = olh_active; // KLC - Yeah, got to update this one too and
    SavePrefs();                             // KLC - save the prefs out to disk.
    return TRUE;
}

uchar olh_overlay_func(ushort keycode, uint32_t context, intptr_t data) {
    if (global_fullmap->cyber) {
        string_message_info(REF_STR_NotAvailCspace);
        return TRUE;
    }
    if (full_game_3d) {
        change_mode_func(keycode, context, GAME_LOOP);
    }
    olh_overlay_on = TRUE;
    return TRUE;
}
