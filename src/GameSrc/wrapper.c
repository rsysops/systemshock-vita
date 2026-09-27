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
 * $Source: r:/prj/cit/src/RCS/wrapper.c $
 * $Revision: 1.146 $
 * $Author: dc $
 * $Date: 1994/11/28 06:40:50 $
 */

#include <limits.h>

#include "newmfd.h"
#include "wrapper.h"
#include "tools.h"
#include "invent.h"
#include "invpages.h"
#include "gamescr.h"
#include "mainloop.h"
#include "hkeyfunc.h"
#include "gamewrap.h"
#include "colors.h"
#include "cybstrng.h"
#include "fullscrn.h"
#include "game_screen.h"
#include "render.h"
#include "gametime.h"
#include "musicai.h"
#include "input.h"
#include "gamestrn.h"
#include "mfdext.h"
#include "miscqvar.h"
#include "cit2d.h"
#include "rendtool.h"
#include "sideicon.h"
#include "sndcall.h"
#include "sfxlist.h"
#include "criterr.h"
#include "gr2ss.h"
#include "player.h"
#include "popups.h"
#include "olhext.h"
#include "Xmi.h"
#include "Prefs.h"

#include "OpenGL.h"


#ifdef AUDIOLOGS
#include "audiolog.h"
#endif

#include "mfdart.h" // for the slider bar

#include "MacTune.h"

#define LOAD_BUTTON          0
#define SAVE_BUTTON          1
#define AUDIO_BUTTON         2
#define INPUT_BUTTON         3
#define OPTIONS_BUTTON       4
#define VIDEO_BUTTON         5
#define RETURN_BUTTON        6
#define QUIT_BUTTON          7
#define AUDIO_OPT_BUTTON     8
#define SCREENMODE_BUTTON    9
#define HEAD_RECENTER_BUTTON 10
#define HEADSET_BUTTON       11

#define MOUSE_DOWN (MOUSE_LDOWN | MOUSE_RDOWN | UI_MOUSE_LDOUBLE)
#define MOUSE_UP   (MOUSE_LUP | MOUSE_RUP)
#define MOUSE_LEFT (MOUSE_LDOWN | UI_MOUSE_LDOUBLE)
#define MOUSE_WHEEL (MOUSE_WHEELUP | MOUSE_WHEELDN)

#define STATUS_X      4
#define STATUS_Y      1
#define STATUS_HEIGHT 20
#define STATUS_WIDTH  312

#ifdef VITA
#include "sdl_events.h"
void vita_input_init(uchar butid);
#endif

LGCursor option_cursor;
grs_bitmap option_cursor_bmap;

extern LGRegion *inventory_region;
int wrap_id = -1, wrapper_wid, wrap_key_id;
uchar clear_panel = TRUE, wrapper_panel_on = FALSE;
grs_font *opt_font;
uchar olh_temp;
static bool digi_gain = true; // enable sfx volume slider
errtype (*wrapper_cb)(int num_clicked);
errtype (*slot_callback)(int num_clicked);
static uchar cursor_loaded = FALSE;
#if defined(VFX1_SUPPORT) || defined(CTM_SUPPORT)
uchar headset_track = TRUE;
#define HEADSET_FOV_MIN 30
#define HEADSET_FOV_MAX 180
// these 3 should all be initialized for real elsewhere...
int inp6d_real_fov = 60;
int hack_headset_fov = 30;
#endif
int inp6d_curr_fov = 60;

errtype music_slots();
errtype wrapper_do_save();
errtype wrapper_panel_close(uchar clear_message);
errtype do_savegame_guts(uchar slot);
void quit_verify_pushbutton_handler(uchar butid);
uchar quit_verify_slorker(uchar butid);
void save_verify_pushbutton_handler(uchar butid);
uchar save_verify_slorker(uchar butid);
void free_options_cursor(void);

void input_screen_init(void);
void joystick_screen_init(void);
void sound_screen_init(void);
void soundopt_screen_init(void);
void video_screen_init(void);

uint multi_get_curval(uchar type, void *p);
void multi_set_curval(uchar type, void *p, uint val, void *deal);

extern LGCursor slider_cursor;
extern grs_bitmap slider_cursor_bmap;
extern char which_lang;

void options_screen_init(void);
void wrapper_init(void);
void load_screen_init(void);
void save_screen_init(void);

// screen the Return button goes back to; NULL on the top-level screen, where it closes the panel
static void (*wrapper_return_screen)(void) = NULL;

// Keyboard / D-pad navigation: one focused widget per screen, moved with the arrow keys or by
// hovering it with the cursor; Enter activates it, Home goes back, PgUp/PgDn switch page.
enum { PAGE_LOAD, PAGE_SAVE, PAGE_AUDIO, PAGE_INPUT, PAGE_OPTIONS, PAGE_VIDEO,
#ifdef VITA
       PAGE_VITA,
#endif
       NUM_PAGES };

#define PAGE_NONE -1           // main list: PgUp/PgDn do nothing there
#define VITA_INPUT_BUTTON 8    // main-list button opening the Vita input screen
#define JOYSTICK_BUTTON 3      // input-screen button opening the joystick screen
#define OPANEL_SLIDER_STEP 5   // pixels a grabbed bar moves per Left/Right press (same as the mouse wheel)
#define OPANEL_ALIGN_SLACK 8   // alignment tolerance, in doubled-centre units (4 px): bars sit lower in their slot than buttons
#define FOCUS_BUTTON_COLOR (BUTTON_COLOR - 2) // focused widget: lighter green
// current choice (e.g. the resolution in use): gold matching the button green's brightness,
// text/border (171,151,0) vs green (95,167,43), fill (59,39,0) vs green's (23,59,27)
#define CURRENT_CHOICE_COLOR (GREEN_YELLOW_BASE + 4)
#define CURRENT_CHOICE_SHADOW 6
#define GRABBED_BAR_COLOR GREEN_YELLOW_BASE   // bar grabbed with Enter: Left/Right move it

static void (*opanel_screen)(void) = NULL; // init function of the current screen, NULL for the verify screen
static int opanel_page = PAGE_NONE;        // top-level page the current screen belongs to
static uchar opanel_entry_button = 0;      // parent's button that opened this screen, focused again on Back
static int opanel_focus = -1;              // focused button, -1 = not chosen yet
static int opanel_prev_focus = -1;         // focus before the last clear_obuttons(), kept on a same-screen re-init
static int opanel_queued_focus = -1;       // focus the next screen should start on
static uchar opanel_slider_editing = FALSE; // focused bar grabbed with Enter: Left/Right move it
static int opanel_save_armed_line = -1;    // save slot whose name keyboard was opened by Enter; next Enter saves

static void opanel_screen_begin(void (*self)(void), void (*parent)(void), int page, uchar entry_button);
static void opanel_go_back(void);
void opanel_redraw(uchar back);
uchar textlist_handler(uiEvent *ev, uchar butid);

void draw_button(uchar butid);

#define SLOTNAME_HEIGHT 6
#define PANEL_MARGIN_Y 3
#define WRAPPER_PANEL_HEIGHT (INVENTORY_PANEL_HEIGHT - 2 * PANEL_MARGIN_Y)

#define OPTIONS_FONT RES_tinyTechFont

errtype (*verify_callback)(int num_clicked) = NULL;
char savegame_verify;
char comments[NUM_SAVE_SLOTS + 1][SAVE_COMMENT_LEN];
uchar pause_game_func(ushort keycode, uint32_t context, intptr_t data);
uchar really_quit_key_func(ushort keycode, uint32_t context, intptr_t data);

// separate mouse region for regular-screen and fullscreen.
#define NUM_MOUSEREGION_SCREENS 2
LGRegion options_mouseregion[NUM_MOUSEREGION_SCREENS];
uchar free_mouseregion = 0;

char save_game_name[] = "savgam00.dat";

extern grs_canvas *pinv_canvas;
extern grs_canvas inv_norm_canvas;
extern grs_canvas inv_fullscrn_canvas;
extern grs_canvas inv_view360_canvas;

#define FULL_BACK_X (GAME_MESSAGE_X - INVENTORY_PANEL_X)
#define FULL_BACK_Y (GAME_MESSAGE_Y - INVENTORY_PANEL_Y)

#define BUTTON_COLOR GREEN_BASE + 2
#define BUTTON_SHADOW 7

// SLIDER WIDGETS:
// structure for slider widget.  The slider has a pointer to a uchar,
// ushort, or uint, which it sets to a value in the range [0,maxval].
// It recalculates this value based on interpolation from the actual
// size of the slider.  The function dealfunc (if not NULL) is called
// when the value changes, and is passed the new value.  The value is
// updated continuously if smooth==TRUE; otherwise it is updated upon
// mouse-up.
//
typedef struct {
    uchar color;
    uchar bvalcol;
    uchar sliderpos;
    uchar active;
    uchar dragging; // press started on this slider; only then does it follow the pointer
    Ref descrip;
    uint maxval;
    uchar baseval;
    uchar type;
    uchar smooth;
    void *curval;
    void *dealfunc;
} opt_slider_state;

// PUSHBUTTON WIDGETS:
// the simplest widgets.  Calls pushfunc, passing in its own button ID,
// upon mouse left-click upon it, or on a keyboard event corresponding
// to keyeq.
//
typedef struct {
    uchar keyeq;
    Ref descrip;
    uchar fcolor;
    uchar shadow;
    void (*pushfunc)(uchar butid);
} opt_pushbutton_state;

// MULTI_STATE WIDGETS:
// these are much like pushbuttons, but also have a pointer to a uchar,
// ushort, or uint, which takes on a value in the range [0,num_opts-1].
// The button is labelled both with its description string (descrip) and
// with a string offset from optbase by an amount equal to the current
// value of its associated variable.  Whenever its value changes, it calls
// dealfunc, and message-lines a string offset from feedbackbase by an
// amount equal to its current value.
//
typedef struct {
    uchar keyeq;
    uchar type;
    uchar num_opts;
    Ref optbase;
    Ref descrip;
    Ref feedbackbase;
    void *curval;
    void *dealfunc;
} opt_multi_state;

// TEXT WIDGET
// nothing but a piece of text, folks.  No handler, simple draw func.
//
typedef struct {
    Ref descrip;
    uchar color;
} opt_text_state;

// TEXTLIST WIDGET
// used for editing and selecting (and responding to the editing and
// selecting of) a list of text strings.  You provide a block of text,
// which is assumed to be a 2-D array of chars (you inform the widget
// of the dimension of the subarrays).  The widget may either be edit-
// allowing or not.  If not, then it calls its dealfunc whenever a
// text string is selected (by mouse-clicking on it or by using the
// keyboard to move the highlight to it and hitting ENTER).  If the
// strings are editable, it calls its dealfunc only when you are done
// selecting and editing one.  A mask may be provided of what entries
// on the list are valid candidates for selection, and a string resource
// is given to display in the place of uninitialized selections.  Different
// colors are provided for selectable text, currently selected text,
// and non-selectable text.  Note that the user is responsible for
// providing space for one more line of text than the widget uses, for
// the purposes of saving string information.
//
typedef struct {
    char *text;
    uchar numblocks;
    uchar blocksiz;

    char currstring;
    char index;
    uchar modified;

    uchar editable;
    ushort editmask;
    ushort selectmask;
    ushort initmask;
    Ref invalidstr;

    Ref selectprompt;

    uchar validcol;
    uchar selectcol;
    uchar invalidcol;

    void (*dealfunc)(uchar butid, uchar index);
} opt_textlist_state;

// SLORKER WIDGET
// used to implement default actions in the keyboard interface to options
// screens, slorker widgets respond to no mouse events, but will respond
// to any keyboard events which actually reach them by calling their function
// with their button id as an argument.  Thus, any keypress which is not
// handled by another gadget is taken by the slorker.
//
typedef uchar (*slorker)(uchar butid);

typedef struct {
    LGRect rect;
    union {
        opt_slider_state     slider_st;
        opt_pushbutton_state pushbutton_st;
        opt_text_state       text_st;
        opt_multi_state      multi_st;
        opt_textlist_state   textlist_st;
        slorker              sl;
    } user;
    ulong evmask;
    void (*drawfunc)(uchar butid);
    uchar (*handler)(uiEvent *ev, uchar butid);
} opt_button;

void verify_screen_init(void (*verify)(uchar butid), slorker slork);
// void verify_screen_init(void (*verify)(uchar butid), void (*slork)(uchar butid));

#define OPT_SLIDER_BAR REF_IMG_BeamSetting

#define MAX_OPTION_BUTTONS 12
#define BR(i) (OButtons[i].rect)

#ifdef STATIC_BUTTON_STORE
opt_button OButtons[MAX_OPTION_BUTTONS];
#else
extern grs_canvas _offscreen_mfd;
opt_button *OButtons;
uchar fv;
#endif

#define OPTIONS_COLOR RED_BROWN_BASE + 4

// Every options screen places its widgets in one 3x3 grid of slots snapped to the panel
// background (REF_IMG_bmBlankInventoryPanel), whose 1-px grid lines are every 8 px at
// x = 8k, y = 1 + 8k (18 x 6 cells). A slot is 6 x 2 cells; widgets sit 1 px inside its grid
// lines, so the line shows between neighbours. Slot n is column n % 3, row n / 3; screens
// with two rows of widgets use the top two rows.
#define OPANEL_GRID_CELL 8
#define OPANEL_GRID_Y0 1
#define OPANEL_SLOT_W (6 * OPANEL_GRID_CELL)
#define OPANEL_SLOT_H (2 * OPANEL_GRID_CELL)

// override get_temp_string() to support hard-coded custom strings without
// providing an actual resource file

#define MIDI_OUT_STR_SIZE 1024
static char MIDI_STR_BUFFER[MIDI_OUT_STR_SIZE];

static char *_get_temp_string(int num) {
    switch (num) {
        case REF_STR_Renderer: return "Renderer";
        case REF_STR_Software: return "Software";
        case REF_STR_OpenGL:   return "OpenGL";

        case REF_STR_TextFilt: return "Tex Filter";
        case REF_STR_TFUnfil:  return "Unfiltered";
        case REF_STR_TFBilin:  return "Bilinear";

        case REF_STR_MousLook: return "Mouselook";
        case REF_STR_MousNorm: return "Normal";
        case REF_STR_MousInv:  return "Inverted";

#ifdef VITA
        case REF_STR_VitaOptions: return "Vita Input";
        case REF_STR_GyroAiming: return "Gyro Aiming";
        case REF_STR_GyroOn: return "Enabled";
        case REF_STR_GyroOff: return "Disabled";
        case REF_STR_ControllerLookSpeed: return "Look Speed";
        case REF_STR_GyroLookSpeed: return "Gyro Speed";
        case REF_STR_VitaRes1: return "320 x 200";
        case REF_STR_VitaRes2: return "480 x 272";
        case REF_STR_VitaRes3: return "640 x 400";
        case REF_STR_VitaRes4: return "640 x 480";
        case REF_STR_VitaRes5: return "960 x 544";
#endif

        case REF_STR_Seqer:    return "Midi Player";
        case REF_STR_ADLMIDI:  return "ADLMIDI";
        case REF_STR_NativeMI: return "Native MIDI";
#ifdef USE_FLUIDSYNTH
        case REF_STR_FluidSyn: return "FluidSynth";
#endif

        case REF_STR_MidiOut:  return "Midi Output";
    }

    if (num >= REF_STR_MidiOutX && num <= (REF_STR_MidiOutX | 0x0fffffff))
    {
        const unsigned int midiOutputIndex = (unsigned int)num - REF_STR_MidiOutX;
        MIDI_STR_BUFFER[0] = '\0';
        GetOutputNameXMI(midiOutputIndex, &MIDI_STR_BUFFER[0], MIDI_OUT_STR_SIZE);
        return &MIDI_STR_BUFFER[0];
    }

    return get_temp_string(num);
}

#define get_temp_string _get_temp_string

//#ifdef NOT_YET //

// colour a widget is drawn in: lighter when it has the keyboard / D-pad focus
static uchar opanel_widget_color(uchar butid, uchar normal) {
    return (butid == opanel_focus) ? FOCUS_BUTTON_COLOR : normal;
}

void draw_button(uchar butid) {
    if (OButtons[butid].drawfunc) {
#ifdef SVGA_SUPPORT
        uchar old_over;
        old_over = gr2ss_override;
        gr2ss_override = OVERRIDE_ALL;
#endif
        uiHideMouse(NULL);
        gr_push_canvas(&inv_norm_canvas);
        gr_set_font(opt_font);
        OButtons[butid].drawfunc(butid);
        gr_pop_canvas();
        uiShowMouse(NULL);
#ifdef GR2SS_OVERRIDE
        gr2ss_override = old_over;
#endif
    }
}

void wrapper_draw_background(short ulx, short uly, short lrx, short lry) {
    short cx1, cx2, cy1, cy2;
    extern grs_bitmap inv_backgnd;
    short a1, a2, a3, a4;

#ifdef SVGA_SUPPORT
    uchar old_over;
    old_over = gr2ss_override;
    gr2ss_override = OVERRIDE_ALL;
#endif
    // draw background behind the slider.
    STORE_CLIP(cx1, cy1, cx2, cy2);
    ss_safe_set_cliprect(ulx, uly, lrx, lry);
    if (full_game_3d) {
        //      gr_bitmap(&inv_view360_canvas.bm,FULL_BACK_X,FULL_BACK_Y);
        gr_get_cliprect(&a1, &a2, &a3, &a4);
        ss_noscale_bitmap(&inv_view360_canvas.bm, FULL_BACK_X, FULL_BACK_Y);
    } else
        ss_bitmap(&inv_backgnd, 0, 0);
    RESTORE_CLIP(cx1, cy1, cx2, cy2);
#ifdef SVGA_SUPPORT
    gr2ss_override = old_over;
#endif
}

void slider_draw_func(uchar butid) {
    opt_slider_state *st = &(OButtons[butid].user.slider_st);
    short w, h, sw;
    char *title;
    uchar col;

#ifdef SVGA_SUPPORT
    uchar old_over;
    old_over = gr2ss_override;
    gr2ss_override = OVERRIDE_ALL;
#endif

    if (butid == opanel_focus && opanel_slider_editing)
        col = GRABBED_BAR_COLOR;
    else
        col = opanel_widget_color(butid, st->color);

    sw = res_bm_width(OPT_SLIDER_BAR);
    gr_set_fcolor(col);
    title = get_temp_string(st->descrip);
    gr_string_size(title, &w, &h);

    // draw background behind the slider
    wrapper_draw_background(BR(butid).ul.x - sw / 2, BR(butid).ul.y - h, BR(butid).lr.x + sw / 2, BR(butid).lr.y);
    draw_shadowed_string(title, BR(butid).ul.x, BR(butid).ul.y - h, full_game_3d);

    gr_set_fcolor(st->bvalcol);
    ss_vline(BR(butid).ul.x + st->baseval, BR(butid).ul.y, BR(butid).lr.y - 1);

    gr_set_fcolor(col);
    ss_box(BR(butid).ul.x, BR(butid).ul.y, BR(butid).lr.x, BR(butid).lr.y);

    if (!(st->active))
        draw_raw_resource_bm(OPT_SLIDER_BAR, BR(butid).ul.x + st->sliderpos + 1 - sw / 2, BR(butid).ul.y);

#ifdef SVGA_SUPPORT
    gr2ss_override = old_over;
#endif
}

void slider_deal(uchar butid, uchar deal) {
    opt_slider_state *st = &(OButtons[butid].user.slider_st);
    uint val;

    deal = deal || st->smooth;

    val = (st->sliderpos * (st->maxval + 1)) / (BR(butid).lr.x - BR(butid).ul.x - 3);
    if (val > st->maxval) val = st->maxval;

    multi_set_curval(st->type, st->curval, val, deal ? st->dealfunc : NULL);
}

//
// every time you find yourself,
// you lose a little bit of me, from within
//

uchar slider_handler(uiEvent *ev, uchar butid) {
    opt_slider_state *st = &(OButtons[butid].user.slider_st);

    switch (ev->type) {
    case UI_EVENT_MOUSE_MOVE:
        // ignore drags that began elsewhere, e.g. the rest of a tap on the menu button
        // that was just replaced by this slider
        if (st->dragging && ev->mouse_data.buttons) {
            st->sliderpos = ev->pos.x - BR(butid).ul.x;
            slider_deal(butid, TRUE);
            draw_button(butid);
        }
        break;
    case UI_EVENT_MOUSE:
        if (ev->mouse_data.action & MOUSE_WHEELUP) {
            st->sliderpos = st->sliderpos <= 5 ? 0 : st->sliderpos - 5;
        } else if (ev->mouse_data.action & MOUSE_WHEELDN) {
            uchar max = BR(butid).lr.x - BR(butid).ul.x - 3;
            st->sliderpos = lg_min(st->sliderpos + 5, max);
        } else if (ev->subtype & MOUSE_DOWN) {
            st->dragging = TRUE;
            st->sliderpos = ev->pos.x - BR(butid).ul.x;
        } else if ((ev->subtype & MOUSE_UP) && st->dragging) {
            st->dragging = FALSE;
            st->sliderpos = ev->pos.x - BR(butid).ul.x;
        } else {
            return FALSE;
        }
        slider_deal(butid, TRUE);
        draw_button(butid);
        return TRUE;
    default:
        break;
    }
    return FALSE;
}

void slider_init(uchar butid, Ref descrip, uchar type, uchar smooth, void *var, uint maxval, uchar baseval,
                 void *dealfunc, LGRect *r) {
    opt_slider_state *st = &OButtons[butid].user.slider_st;
    uint val;

    if (maxval)
    {
        val = ((r->lr.x - r->ul.x - 3) * multi_get_curval(type, var)) / maxval;
    }
    else
    {
        // just put it in the middle
        val = (r->lr.x - r->ul.x - 3) / 2;
    }

    st->color = BUTTON_COLOR;
    st->bvalcol = GREEN_YELLOW_BASE + 1;
    st->sliderpos = val;
    st->baseval = baseval;
    st->maxval = maxval;
    st->active = FALSE;
    st->dragging = FALSE;
    st->descrip = descrip;
    st->type = type;
    // note that in these settings, we don't care what size of
    // variable we're dealing with, 'cause we secretly know that
    // all pointers are represented the same and we don't
    // have to actually dereference these.
    st->dealfunc = dealfunc;
    st->curval = var;
    st->smooth = smooth;

    OButtons[butid].evmask = UI_EVENT_MOUSE | UI_EVENT_MOUSE_MOVE;
    OButtons[butid].drawfunc = slider_draw_func;
    OButtons[butid].handler = slider_handler;
    OButtons[butid].rect = *r;
}

void pushbutton_draw_func(uchar butid) {
    char *btext;
    short w, h;
    opt_pushbutton_state *st = &OButtons[butid].user.pushbutton_st;

    w = BR(butid).lr.x - BR(butid).ul.x;
    h = BR(butid).lr.y - BR(butid).ul.y;

    btext = get_temp_string(st->descrip);
    gr_string_wrap(btext, BR(butid).lr.x - BR(butid).ul.x - 3);
    text_button(btext, BR(butid).ul.x, BR(butid).ul.y, opanel_widget_color(butid, st->fcolor), st->shadow, -w, -h);
    gr_font_string_unwrap(btext);
}

uchar pushbutton_handler(uiEvent *ev, uchar butid) {
    if (((ev->type == UI_EVENT_MOUSE) && (ev->subtype & MOUSE_DOWN)) ||
        ((ev->type == UI_EVENT_KBD_COOKED) &&
         ((ev->cooked_key_data.code & 0xFF) == OButtons[butid].user.pushbutton_st.keyeq))) {
        OButtons[butid].user.pushbutton_st.pushfunc(butid);
        return TRUE;
    }
    return FALSE;
}

void pushbutton_init(uchar butid, uchar keyeq, Ref descrip, void (*pushfunc)(uchar butid), LGRect *r) {
    opt_pushbutton_state *st = &OButtons[butid].user.pushbutton_st;

    OButtons[butid].rect = *r;
    OButtons[butid].evmask = UI_EVENT_MOUSE | UI_EVENT_KBD_COOKED;
    OButtons[butid].drawfunc = pushbutton_draw_func;
    OButtons[butid].handler = pushbutton_handler;
    st->fcolor = BUTTON_COLOR;
    st->shadow = BUTTON_SHADOW;
    st->keyeq = keyeq;
    st->descrip = descrip;
    st->pushfunc = pushfunc;
}

void dim_pushbutton(uchar butid) {
    opt_pushbutton_state *st = &OButtons[butid].user.pushbutton_st;
    OButtons[butid].evmask = 0;
    st->fcolor += 4;
    st->shadow -= 3;
}

void bright_pushbutton(uchar butid) {
    opt_pushbutton_state *st = &OButtons[butid].user.pushbutton_st;
    OButtons[butid].evmask = 0;
    st->fcolor = CURRENT_CHOICE_COLOR;
    st->shadow = CURRENT_CHOICE_SHADOW;
}

// text widget
void text_draw_func(uchar butid) {
    opt_text_state *st = &OButtons[butid].user.text_st;
    char *s = get_temp_string(st->descrip);

    gr_string_wrap(s, BR(butid).lr.x - BR(butid).ul.x);
    gr_set_fcolor(st->color);
    draw_shadowed_string(s, BR(butid).ul.x, BR(butid).ul.y, full_game_3d);
    gr_font_string_unwrap(s);
}

void textwidget_init(uchar butid, uchar color, Ref descrip, LGRect *r) {
    opt_text_state *st = &OButtons[butid].user.text_st;

    OButtons[butid].rect = *r;
    st->descrip = descrip;
    st->color = color;
    OButtons[butid].drawfunc = text_draw_func;
    OButtons[butid].handler = NULL;
    OButtons[butid].evmask = 0;
}

// a keywidget is just like a pushbutton, but invisible.
//
void keywidget_init(uchar butid, uchar keyeq, void (*pushfunc)(uchar butid)) {
    opt_pushbutton_state *st = &OButtons[butid].user.pushbutton_st;

    OButtons[butid].evmask = UI_EVENT_KBD_COOKED;
    OButtons[butid].drawfunc = NULL;
    OButtons[butid].handler = pushbutton_handler;
    st->keyeq = keyeq;
    st->pushfunc = pushfunc;
}

// gets the current "value" of a multi-option widget, whatever size
// thing that may be.
uint multi_get_curval(uchar type, void *p) {
    uint val = 0;

    switch (type) {
    case sizeof(uchar):
        val = *((uchar *)p);
        break;
    case sizeof(ushort):
        val = *((ushort *)p);
        break;
    case sizeof(uint):
        val = *((uint *)p);
        break;
    }
    return val;
}

// sets the current value pointed to by a multi-option widget.
void multi_set_curval(uchar type, void *p, uint val, void *deal) {
    switch (type) {
    case sizeof(uchar):
        *((uchar *)p) = (uchar)val;
        if (deal)
            ((void (*)(uchar))deal)((uchar)val);
        break;
    case sizeof(ushort):
        *((ushort *)p) = (ushort)val;
        if (deal)
            ((void (*)(ushort))deal)((ushort)val);
        break;
    case sizeof(uint):
        *((uint *)p) = (uint)val;
        if (deal)
            ((void (*)(uint))deal)((uint)val);
        break;
    }
}

void multi_draw_func(uchar butid) {
    char *btext;
    short w, h, x, y;
    uint val = 0;
    opt_multi_state *st = &OButtons[butid].user.multi_st;
    uchar col = opanel_widget_color(butid, BUTTON_COLOR);

    gr_set_fcolor(col);
    ss_rect(BR(butid).ul.x, BR(butid).ul.y, BR(butid).lr.x, BR(butid).lr.y);
    gr_set_fcolor(col + BUTTON_SHADOW);
    ss_rect(BR(butid).ul.x + 1, BR(butid).ul.y + 1, BR(butid).lr.x - 1, BR(butid).lr.y - 1);
    gr_set_fcolor(col);
    x = (BR(butid).lr.x + BR(butid).ul.x) / 2;
    y = (BR(butid).lr.y + BR(butid).ul.y) / 2;
    btext = get_temp_string(st->descrip);
    gr_string_size(btext, &w, &h);
    ss_string(btext, x - w / 2, y - h);
    val = multi_get_curval(st->type, st->curval);
    btext = get_temp_string(st->optbase + val);
    gr_string_size(btext, &w, &h);
    ss_string(btext, x - w / 2, y);
}

// advance a multi-choice option by delta (num_opts - 1 goes back one)
static void multi_cycle(uchar butid, uint delta) {
    opt_multi_state *st = &OButtons[butid].user.multi_st;
    uint val;

    val = multi_get_curval(st->type, st->curval);
    val = (val + delta) % (st->num_opts);
    multi_set_curval(st->type, st->curval, val, st->dealfunc);
    draw_button(butid);
    if (st->feedbackbase) {
        string_message_info(st->feedbackbase + val);
    }
}

uchar multi_handler(uiEvent *ev, uchar butid) {
    uint delta = 0;
    opt_multi_state *st = &OButtons[butid].user.multi_st;

    if (ev->type == UI_EVENT_MOUSE) {
        if (ev->subtype & MOUSE_LEFT)
            delta = 1;
        else if (ev->subtype & MOUSE_RDOWN)
            delta = st->num_opts - 1;
    } else if (ev->type == UI_EVENT_KBD_COOKED) {
	short code = ev->cooked_key_data.code;
        if (tolower(code & 0xFF) == st->keyeq) {
            if (isupper(code & 0xFF))
                delta = st->num_opts - 1;
            else
                delta = 1;
        }
    }

    if (delta) {
        multi_cycle(butid, delta);
        return TRUE;
    }
    return FALSE;
}

void multi_init(uchar butid, uchar key, Ref descrip, Ref optbase, Ref feedbase, uchar type, void *var, uchar num_opts,
                void *dealfunc, LGRect *r) {
    opt_multi_state *st = &OButtons[butid].user.multi_st;

    OButtons[butid].rect = *r;
    OButtons[butid].drawfunc = multi_draw_func;
    OButtons[butid].handler = multi_handler;
    OButtons[butid].evmask = UI_EVENT_MOUSE | UI_EVENT_KBD_COOKED;
    st->descrip = descrip;
    st->optbase = optbase;
    st->feedbackbase = feedbase;
    st->type = type;
    st->keyeq = key;
    st->num_opts = num_opts;
    // note that in these settings, we don't care what size of
    // variable we're dealing with, 'cause we secretly know that
    // all pointers are represented the same and we don't
    // have to actually dereference these.
    st->dealfunc = dealfunc;
    st->curval = var;
}

#pragma disable_message(202)
uchar keyslork_handler(uiEvent *ev, uchar butid) {
    slorker *slork = &OButtons[butid].user.sl;

    return ((*slork)(butid));
}
#pragma enable_message(202)

void slork_init(uchar butid, slorker slork) {
    LG_memset(&OButtons[butid].rect, 0, sizeof(LGRect));
    OButtons[butid].user.sl = slork;
    OButtons[butid].evmask = UI_EVENT_KBD_COOKED;
    OButtons[butid].drawfunc = NULL;
    OButtons[butid].handler = keyslork_handler;
}

char *textlist_string(opt_textlist_state *st, int ind) { return (st->text + ind * (st->blocksiz)); }

void textlist_draw_line(opt_textlist_state *st, int line, uchar butid) {
    short w, h;
    LGRect scrrect;
    LGRect r;
    char *s;
    uchar col;
#ifdef SVGA_SUPPORT
    uchar old_over;
#endif

    scrrect = BR(butid);
    scrrect.ul.x += INVENTORY_PANEL_X;
    scrrect.ul.y += INVENTORY_PANEL_Y;
    scrrect.lr.x += INVENTORY_PANEL_X;
    scrrect.lr.y += INVENTORY_PANEL_Y;

    if (((1 << line) & (st->initmask)) || (line == st->currstring && st->index >= 0))
        s = textlist_string(st, line);
    else
        s = get_temp_string(st->invalidstr);

    if (line == st->currstring)
        col = st->selectcol;
    else if (st->selectmask & (1 << line))
        col = st->validcol;
    else
        col = st->invalidcol;
    gr_push_canvas(&inv_norm_canvas);
    gr_set_fcolor(col);

    gr_set_font(opt_font);
    gr_string_size(s, &w, &h);
    r.ul.x = BR(butid).ul.x;
    r.ul.y = BR(butid).ul.y + h * line;
    r.lr.x = BR(butid).lr.x;
    r.lr.y = r.ul.y + h;

    uiHideMouse(&scrrect);
#ifdef SVGA_SUPPORT
    old_over = gr2ss_override;
    gr2ss_override = OVERRIDE_ALL;
#endif
    wrapper_draw_background(r.ul.x, r.ul.y, r.lr.x, r.lr.y);
    draw_shadowed_string(s, r.ul.x, r.ul.y, full_game_3d);
#ifdef SVGA_SUPPORT
    gr2ss_override = old_over;
#endif
    uiShowMouse(&scrrect);
    gr_pop_canvas();
}

void textlist_draw_func(uchar butid) {
    int i;
    opt_textlist_state *st = &OButtons[butid].user.textlist_st;

    for (i = 0; i < st->numblocks; i++) {
        textlist_draw_line(st, i, butid);
    }
}

void textlist_cleanup(opt_textlist_state *st) {
    if (st->editable && st->currstring >= 0 && st->index >= 0) {
        strcpy(textlist_string(st, st->currstring), textlist_string(st, st->numblocks));
        st->index = -1;
    }
}

#ifdef WE_USED_THIS
void textlist_edit_line(opt_textlist_state *st, uchar butid, uchar line, uchar end) {
    char *s, *bak;
    char tmp;

    gr_push_canvas(&inv_norm_canvas);
    s = textlist_string(st, line);
    bak = textlist_string(st, st->numblocks);
    tmp = st->currstring;
    st->currstring = line;
    if (tmp >= 0) {
        strcpy(textlist_string(st, tmp), bak);
        textlist_draw_line(st, tmp, butid);
    }
    strcpy(bak, s);
    st->index = end ? strlen(s) : 0;
    s[0] = '\0';
    textlist_draw_line(st, line, butid);
    gr_pop_canvas();
}
#endif

void textlist_select_line(opt_textlist_state *st, uchar butid, uchar line, uchar deal) {
    char tmp;

    gr_push_canvas(&inv_norm_canvas);
    tmp = st->currstring;
    st->currstring = line;
    st->index = -1;
    if (tmp >= 0)
        textlist_draw_line(st, tmp, butid);
    textlist_draw_line(st, line, butid);
    gr_pop_canvas();
    if (deal)
        st->dealfunc(butid, line);
}

uchar textlist_handler(uiEvent *ev, uchar butid) {
    uchar line;
    opt_textlist_state *st = &OButtons[butid].user.textlist_st;

    if ((ev->type == UI_EVENT_MOUSE) && (ev->subtype & MOUSE_DOWN)) {
        short w, h;

        gr_set_font(opt_font);
        gr_char_size('X', &w, &h);

        line = (ev->pos.y - BR(butid).ul.y) / h;

        if (st->editable && (st->editmask & (1 << line))) {
            // this is how you would do this if you wanted right-click to select
            // a line w/ confirm, which would be the right thing to do, instead
            // of confirm without selection, which is what Harvey at Origin wants.
            //
            //          if(st->selectprompt)
            //             textlist_select_line(st,butid,line,FALSE);
            //          textlist_select_line(st,butid,line,(ev->subtype&MOUSE_RDOWN)!=0);
            //
            if (ev->subtype & MOUSE_RDOWN) {
                if (st->currstring >= 0)
                    st->dealfunc(butid, st->currstring);
            } else if (!st->modified) {
#ifdef VITA
                // savegame text input
                VitaStartTextInput(0);
#endif
                string_message_info(st->selectprompt);
                if (st->selectprompt)
                    textlist_select_line(st, butid, line, FALSE);
            }
        } else if (st->selectmask & (1 << line)) {
            textlist_select_line(st, butid, line, TRUE);
        }
        return TRUE;
    } else if (ev->type == UI_EVENT_KBD_COOKED) {
	short code = ev->cooked_key_data.code;
        char k = code & 0xFF;
        uint keycode = code & ~KB_FLAG_DOWN;
        uchar special = ((code & KB_FLAG_SPECIAL) != 0);
        char *s;
        char upness = 0;
        char cur = st->currstring;

        // explicitly do not deal with alt-x, but leave
        // it to more capable hands.
        if (keycode == (KB_FLAG_ALT | 'x'))
            return FALSE;
        if (cur >= 0)
            s = textlist_string(st, cur);
        if (st->editable && cur >= 0 && !special && kb_isprint(keycode)) {
            if (st->index < 0) {
                strcpy(textlist_string(st, st->numblocks), textlist_string(st, st->currstring));
                st->index = 0;
            }
            if (st->index + 1 < st->blocksiz) {
                s[st->index] = k;
                st->index++;
                s[st->index] = '\0';
                textlist_draw_line(st, cur, butid);
            }
            st->modified = TRUE;
            return TRUE;
        }
        switch (keycode) {
        case KEY_BS:
            if (st->editable && cur >= 0) {
                if (st->index < 0) {
                    strcpy(textlist_string(st, st->numblocks), textlist_string(st, st->currstring));
                    st->index = strlen(s);
                }
                if (st->index > 0)
                    st->index--;
                s[st->index] = '\0';
                textlist_draw_line(st, cur, butid);
            }
            break;
        case KEY_UP:
            upness = st->numblocks - 1;
            break;
        case KEY_DOWN:
            upness = 1;
            break;
        case KEY_ENTER:
            if (st->currstring >= 0) {
                st->dealfunc(butid, cur);
                return TRUE;
            }
            break;
        case KEY_ESC:
            // on ESC, clean up but pass the event through.
            textlist_cleanup(st);
            wrapper_panel_close(TRUE);
            return FALSE;
        }
        if (upness != 0) {
            char newstring;
            uchar safety = 0;

            newstring = cur;
            if (newstring < 0)
                newstring = (upness == 1) ? st->numblocks - 1 : 0;
            do {
                newstring = (newstring + upness) % st->numblocks;
                safety++;
            } while (safety < st->numblocks && !((1 << newstring) & st->selectmask));
            if (safety >= st->numblocks)
                newstring = cur;
            if (newstring != cur) {
                textlist_cleanup(st);
                st->currstring = newstring;
                if (cur >= 0 && cur < st->numblocks)
                    textlist_draw_line(st, cur, butid);
                textlist_draw_line(st, newstring, butid);
            }
        }
        return TRUE;
    }
    return TRUE;
}

void textlist_init(uchar butid, char *text, uchar numblocks, uchar blocksiz, uchar editable, ushort editmask,
                   ushort selectmask, ushort initmask, Ref invalidstr, uchar validcol, uchar selectcol,
                   uchar invalidcol, Ref selectprompt, void (*dealfunc)(uchar butid, uchar index), LGRect *r) {
    opt_textlist_state *st = &OButtons[butid].user.textlist_st;

    if (r == NULL) {
        BR(butid).ul.x = 2;
        BR(butid).ul.y = 2;
        BR(butid).lr.x = INVENTORY_PANEL_WIDTH;
        BR(butid).lr.y = INVENTORY_PANEL_HEIGHT;
    } else
        OButtons[butid].rect = *r;
    OButtons[butid].drawfunc = textlist_draw_func;
    OButtons[butid].handler = textlist_handler;
    OButtons[butid].evmask = UI_EVENT_MOUSE | UI_EVENT_KBD_COOKED;
    st->text = text;
    st->numblocks = numblocks;
    st->blocksiz = blocksiz;
    st->editable = editable;
    st->editmask = editmask;
    st->selectmask = selectmask;
    st->initmask = initmask;
    st->invalidstr = invalidstr;
    st->validcol = validcol;
    st->selectcol = selectcol;
    st->invalidcol = invalidcol;
    st->dealfunc = dealfunc;
    st->selectprompt = selectprompt;

    st->currstring = -1;
    st->index = -1;
    st->modified = FALSE;
}

//
// KEYBOARD / D-PAD NAVIGATION
//

void wrapper_pushbutton_func(uchar butid);
uchar can_save();

#ifdef VITA
static void vita_input_screen(void) { vita_input_init(VITA_INPUT_BUTTON); }
#endif

// Every screen init calls this right after clear_obuttons(): it records where Back goes
// (parent, focusing entry_button there) and which page L/R switch from.
static void opanel_screen_begin(void (*self)(void), void (*parent)(void), int page, uchar entry_button) {
    // the same screen rebuilding itself (e.g. after a setting change) keeps its focus
    if (self != NULL && self == opanel_screen && opanel_queued_focus < 0)
        opanel_queued_focus = opanel_prev_focus;

    opanel_screen = self;
    wrapper_return_screen = parent;
    opanel_page = page;
    opanel_entry_button = entry_button;
}

static uchar opanel_focusable(int b) {
    opt_button *ob = &OButtons[b];

    if (ob->drawfunc == NULL || !(ob->evmask & UI_EVENT_MOUSE)) // hidden, or dimmed
        return FALSE;
    return ob->handler == pushbutton_handler || ob->handler == multi_handler || ob->handler == slider_handler ||
           ob->handler == textlist_handler;
}

// a slot list needs a current line for Enter to act on
static void opanel_textlist_preselect(int b) {
    opt_textlist_state *st = &OButtons[b].user.textlist_st;
    int line;

    if (st->currstring >= 0)
        return;
    for (line = 0; line < st->numblocks; line++) {
        if (st->selectmask & (1 << line)) {
            st->currstring = line;
            st->index = -1;
            return;
        }
    }
}

// pick the focus of a freshly built screen: the queued one if usable, else the first focusable button
static void opanel_focus_initial(void) {
    int b = opanel_queued_focus;

    opanel_queued_focus = -1;
    if (b < 0 || b >= MAX_OPTION_BUTTONS || !opanel_focusable(b)) {
        // top-left control on screen: button ids don't follow the layout (e.g. Video mode is
        // SCREENMODE_BUTTON). Rows are compared by bottom edge, which bars share with buttons.
        int i;

        b = -1;
        for (i = 0; i < MAX_OPTION_BUTTONS; i++) {
            if (!opanel_focusable(i))
                continue;
            if (b < 0 || BR(i).lr.y < BR(b).lr.y - OPANEL_ALIGN_SLACK / 2 ||
                (abs(BR(i).lr.y - BR(b).lr.y) <= OPANEL_ALIGN_SLACK / 2 && BR(i).ul.x < BR(b).ul.x))
                b = i;
        }
    }
    if (b < 0)
        return;

    opanel_focus = b;
    if (OButtons[b].handler == textlist_handler)
        opanel_textlist_preselect(b);
}

static void opanel_set_focus(int b) {
    int old = opanel_focus;

    if (b == old)
        return;
    opanel_focus = b;
    opanel_slider_editing = FALSE;
    if (old >= 0) {
        draw_button(old);
    }
    if (b >= 0) {
        if (OButtons[b].handler == textlist_handler)
            opanel_textlist_preselect(b);
        draw_button(b);
    }
}

// Move to the nearest focusable button in the arrow's direction, by rectangle centres.
// Buttons more in that direction than off to the side (a 45 degree cone) are preferred; the
// rest of the half-plane is only a fallback, so sparse layouts stay fully reachable.
// Offsets up to OPANEL_ALIGN_SLACK are ignored, so a bar and a button in the same row count as level.
static void opanel_move_focus(int key) {
    int b, best = -1, best_cone = -1;
    long best_score = 0, best_cone_score = 0;
    int cx0, cy0;

    if (opanel_focus < 0)
        return;
    cx0 = BR(opanel_focus).ul.x + BR(opanel_focus).lr.x; // doubled centres, no rounding
    cy0 = BR(opanel_focus).ul.y + BR(opanel_focus).lr.y;

    for (b = 0; b < MAX_OPTION_BUTTONS; b++) {
        int dx, dy, primary, secondary;
        long score;

        if (b == opanel_focus || !opanel_focusable(b))
            continue;
        dx = BR(b).ul.x + BR(b).lr.x - cx0;
        dy = BR(b).ul.y + BR(b).lr.y - cy0;
        switch (key) {
        case KEY_LEFT:
            primary = -dx;
            secondary = abs(dy);
            break;
        case KEY_RIGHT:
            primary = dx;
            secondary = abs(dy);
            break;
        case KEY_UP:
            primary = -dy;
            secondary = abs(dx);
            break;
        default: // KEY_DOWN
            primary = dy;
            secondary = abs(dx);
            break;
        }
        if (primary <= OPANEL_ALIGN_SLACK)
            continue;
        score = primary + 2L * secondary;
        if (primary > secondary) {
            if (best_cone < 0 || score < best_cone_score) {
                best_cone = b;
                best_cone_score = score;
            }
        } else if (best < 0 || score < best_score) {
            best = b;
            best_score = score;
        }
    }
    if (best_cone >= 0)
        opanel_set_focus(best_cone);
    else if (best >= 0)
        opanel_set_focus(best);
}

static void opanel_slider_step(int b, int delta) {
    opt_slider_state *st = &OButtons[b].user.slider_st;
    int max = BR(b).lr.x - BR(b).ul.x - 3;
    int pos = st->sliderpos + delta;

    if (pos < 0)
        pos = 0;
    else if (pos > max)
        pos = max;
    st->sliderpos = pos;
    slider_deal(b, TRUE);
    draw_button(b);
}

static void opanel_textlist_enter(int b) {
    opt_textlist_state *st = &OButtons[b].user.textlist_st;
    char cur = st->currstring;

    if (cur < 0)
        return;
    if (st->editable && (st->editmask & (1 << cur)) && !st->modified && opanel_save_armed_line != cur) {
        // first press on a save slot: open the name keyboard, like tapping the slot does;
        // the next press saves, with the new name or the old one if nothing was typed
        opanel_save_armed_line = cur;
#ifdef VITA
        VitaStartTextInput(0);
#endif
        string_message_info(st->selectprompt);
        textlist_select_line(st, b, cur, FALSE);
        return;
    }
    opanel_save_armed_line = -1;
    st->dealfunc(b, cur);
}

static void opanel_activate(int b) {
    opt_button *ob = &OButtons[b];

    if (ob->handler == pushbutton_handler) {
        ob->user.pushbutton_st.pushfunc(b);
    } else if (ob->handler == multi_handler) {
        multi_cycle(b, 1);
    } else if (ob->handler == slider_handler) {
        opanel_slider_editing = !opanel_slider_editing;
        draw_button(b); // its colour shows whether the bar is grabbed
    } else if (ob->handler == textlist_handler) {
        opanel_textlist_enter(b);
    }
}

// throw away a half-typed save name before leaving the slot list
static void opanel_leave_textlist(void) {
    if (opanel_focus >= 0 && OButtons[opanel_focus].handler == textlist_handler)
        textlist_cleanup(&OButtons[opanel_focus].user.textlist_st);
}

// back one screen, like the Return button; closes the panel from the main list
static void opanel_go_back(void) {
    opanel_leave_textlist();
    if (wrapper_return_screen) {
        opanel_queued_focus = opanel_entry_button;
        wrapper_return_screen();
    } else {
        wrapper_panel_close(TRUE);
    }
}

static void opanel_open_page(int page) {
    switch (page) {
    case PAGE_LOAD:
        wrapper_pushbutton_func(LOAD_BUTTON);
        break;
    case PAGE_SAVE:
        wrapper_pushbutton_func(SAVE_BUTTON);
        break;
    case PAGE_AUDIO:
        wrapper_pushbutton_func(AUDIO_BUTTON);
        break;
    case PAGE_INPUT:
        wrapper_pushbutton_func(INPUT_BUTTON);
        break;
    case PAGE_OPTIONS:
        wrapper_pushbutton_func(OPTIONS_BUTTON);
        break;
    case PAGE_VIDEO:
        wrapper_pushbutton_func(VIDEO_BUTTON);
        break;
#ifdef VITA
    case PAGE_VITA:
        vita_input_screen();
        break;
#endif
    }
}

// L/R: previous/next top-level page, wrapping; Save is skipped where saving isn't allowed
static void opanel_switch_page(int dir) {
    int page = opanel_page;

    if (page == PAGE_NONE)
        return;
    do {
        page = (page + dir + NUM_PAGES) % NUM_PAGES;
    } while (page == PAGE_SAVE && !can_save());

    opanel_leave_textlist();
    opanel_open_page(page);
}

// the cursor moves the focus too, so Enter (cross) acts on whatever is under it
static void opanel_hover(LGPoint pos) {
    int b;

    if (opanel_slider_editing)
        return;
    for (b = 0; b < MAX_OPTION_BUTTONS; b++) {
        if (opanel_focusable(b) && RECT_TEST_PT(&BR(b), pos))
            break;
    }
    if (b >= MAX_OPTION_BUTTONS)
        return;

    opanel_set_focus(b);

    if (OButtons[b].handler == textlist_handler) {
        opt_textlist_state *st = &OButtons[b].user.textlist_st;
        short w, h;
        int line;
        char old = st->currstring;

        if (st->index >= 0) // a name is being typed into the current slot
            return;
        gr_set_font(opt_font);
        gr_char_size('X', &w, &h);
        line = (pos.y - BR(b).ul.y) / h;
        if (line >= 0 && line < st->numblocks && line != old && (st->selectmask & (1 << line))) {
            st->currstring = line;
            if (old >= 0)
                textlist_draw_line(st, old, b);
            textlist_draw_line(st, line, b);
        }
    }
}

// top left of the paused 3D view: the options panel's 3x3 grid has no room for them
#define OPANEL_HINT_X (SCREEN_VIEW_X + 2)
#define OPANEL_HINT_Y (SCREEN_VIEW_Y + 2)
#define OPANEL_HINT_PAD 2 // between the box border, glyphs and labels

static void opanel_hint_glyph_triangle(short x, short y, short g) {
    ss_thick_int_line(x + g / 2, y, x, y + g - 1);
    ss_thick_int_line(x + g / 2, y, x + g - 1, y + g - 1);
    ss_thick_int_line(x, y + g - 1, x + g - 1, y + g - 1);
}

static void opanel_hint_glyph_square(short x, short y, short g) {
    ss_rect(x, y, x + g, y + g);
    gr_set_fcolor(BUTTON_COLOR + BUTTON_SHADOW);
    ss_rect(x + 1, y + 1, x + g - 1, y + g - 1);
    gr_set_fcolor(BUTTON_COLOR);
}

// "(triangle) Help (square) Music": the pad's F1 / F2 in opanel_nav_key(), drawn in the
// button style. The 3D view doesn't redraw while the panel keeps the game paused, so this
// only needs drawing again after an explicit render_run() (see opanel_render_view()).
static void opanel_draw_hints(void) {
    char *help = "Help", *music = "Music";
    short hw, mw, th, g, w, h, x, y;
    LGRect r;
#ifdef SVGA_SUPPORT
    uchar old_over = gr2ss_override;
    gr2ss_override = OVERRIDE_ALL;
#endif
    gr_push_canvas(grd_screen_canvas);
    gr_set_font(opt_font);
    gr_string_size(help, &hw, &th);
    gr_string_size(music, &mw, &th);
    g = th | 1; // odd, so the triangle's apex is centred
    w = OPANEL_HINT_PAD + g + OPANEL_HINT_PAD + hw + 2 * OPANEL_HINT_PAD + g + OPANEL_HINT_PAD + mw + OPANEL_HINT_PAD;
    h = g + 2 * OPANEL_HINT_PAD;
    x = OPANEL_HINT_X;
    y = OPANEL_HINT_Y;
    RECT_FILL(&r, x, y, x + w, y + h);
    uiHideMouse(&r);

    gr_set_fcolor(BUTTON_COLOR);
    ss_rect(x, y, x + w, y + h);
    gr_set_fcolor(BUTTON_COLOR + BUTTON_SHADOW);
    ss_rect(x + 1, y + 1, x + w - 1, y + h - 1);
    gr_set_fcolor(BUTTON_COLOR);

    x += OPANEL_HINT_PAD;
    y += OPANEL_HINT_PAD;
    opanel_hint_glyph_triangle(x, y, g);
    x += g + OPANEL_HINT_PAD;
    ss_string(help, x, y);
    x += hw + 2 * OPANEL_HINT_PAD;
    opanel_hint_glyph_square(x, y, g);
    x += g + OPANEL_HINT_PAD;
    ss_string(music, x, y);

    uiShowMouse(&r);
    gr_pop_canvas();
#ifdef SVGA_SUPPORT
    gr2ss_override = old_over;
#endif
}

// render_run() repaints the 3D view, over the hints
static void opanel_render_view(void) {
    render_run();
    opanel_draw_hints();
}

// the help / controls screen, over the whole screen; returns once it's closed
static void opanel_show_help(void) {
    extern uchar redraw_paused;
    short page = inv_last_page; // page shown before the panel opened

    if (global_fullmap->cyber) {
        string_message_info(REF_STR_NotAvailCspace);
        return;
    }
    if (full_game_3d)
        return; // it labels the normal HUD

    // it labels the HUD underneath: show the inventory instead of the panel, and the 3D view
    // without the hints
    render_run();
    if (page >= 0) {
        inventory_page = page;
        inv_last_page = -1; // draw it whole
        inventory_draw();
        inventory_page = -1;
        inv_last_page = page;
    } else {
        inventory_clear();
    }

    olh_overlay_on = TRUE;
    olh_overlay();
    // it redraws the HUD, but not the 3D view, the panel or "Pause"
    render_run();
    opanel_redraw(TRUE);
    redraw_paused = TRUE;
}

// Arrows, Enter, Home (back), PgUp/PgDn (page), F1 (help) and F2 (music) are handled here
// before the widgets see them: the slot list would swallow them and the verify screen's
// slorker closes the panel.
static uchar opanel_nav_key(uiEvent *ev, int key) {
    int b = opanel_focus;

    switch (key) {
    case KEY_UP:
    case KEY_DOWN:
    case KEY_LEFT:
    case KEY_RIGHT:
        if (b >= 0 && OButtons[b].handler == textlist_handler) {
            if (key == KEY_UP || key == KEY_DOWN)
                textlist_handler(ev, b); // moves between slots
        } else if (b >= 0 && opanel_slider_editing) {
            if (key == KEY_LEFT || key == KEY_RIGHT)
                opanel_slider_step(b, key == KEY_LEFT ? -OPANEL_SLIDER_STEP : OPANEL_SLIDER_STEP);
        } else {
            opanel_move_focus(key);
        }
        return TRUE;

    case KEY_ENTER:
        if (b >= 0)
            opanel_activate(b);
        return TRUE;

    case KEY_HOME:
        if (b >= 0 && opanel_slider_editing) {
            opanel_slider_editing = FALSE;
            draw_button(b);
        } else {
            opanel_go_back();
        }
        return TRUE;

    case KEY_PGUP:
    case KEY_PGDN:
        opanel_switch_page(key == KEY_PGUP ? -1 : 1);
        return TRUE;

    case KEY_F1:
        opanel_show_help();
        return TRUE;

    case KEY_F2:
        toggle_music_func(0, 0, 0);
        return TRUE;
    }
    return FALSE;
}

// One, true mouse handler for all options panel mouse events.
// checks all options panel widgets which enclose point of mouse
// event to see if they want to deal with it.
//
#pragma disable_message(202)
uchar opanel_mouse_handler(uiEvent *ev, LGRegion *r, intptr_t user_data) {
    int b;
    uiEvent mev = *ev;

    if (!(ev->type & (UI_EVENT_MOUSE | UI_EVENT_MOUSE_MOVE)))
        return FALSE;
    if (ev->type == UI_EVENT_MOUSE && !(ev->subtype & (MOUSE_DOWN | MOUSE_UP | MOUSE_WHEEL)))
        return FALSE;

    mev.pos.x -= inventory_region->r->ul.x;
    mev.pos.y -= inventory_region->r->ul.y;

    if (ev->type == UI_EVENT_MOUSE_MOVE)
        opanel_hover(mev.pos);

    for (b = 0; b < MAX_OPTION_BUTTONS; b++) {
        if (RECT_TEST_PT(&BR(b), mev.pos) && (ev->type & OButtons[b].evmask)) {
            if (OButtons[b].handler && OButtons[b].handler((uiEvent *)(&mev), b))
                break;
        }
    }

    // a release outside a slider never reaches its handler, so end any drag here
    if (ev->type == UI_EVENT_MOUSE && (ev->subtype & MOUSE_UP)) {
        for (b = 0; b < MAX_OPTION_BUTTONS; b++) {
            if (OButtons[b].handler == slider_handler)
                OButtons[b].user.slider_st.dragging = FALSE;
        }
    }
    return TRUE;
}

// One, true keyboard handler for all options mode events.
// checks all options panel widgets to see if they want to deal.
//
uchar opanel_kb_handler(uiEvent *ev, LGRegion *r, intptr_t user_data) {
    int b;
    short code = ev->cooked_key_data.code;

    if (!(code & KB_FLAG_DOWN))
        return TRUE;

    if (opanel_nav_key(ev, code & ~KB_FLAG_DOWN))
        return TRUE;

    for (b = 0; b < MAX_OPTION_BUTTONS; b++) {
        if ((ev->type & OButtons[b].evmask) && OButtons[b].handler && OButtons[b].handler(ev, b))
            return TRUE;
    }
    // if no-one else has hooked KEY_ESC, it defaults to closing
    // the wrapper panel.
    //
    if ((code & 0xFF) == KEY_ESC)
        wrapper_panel_close(TRUE);
    return TRUE;
}
#pragma enable_message(202)

void clear_obuttons() {
    uiCursorStack *cs;
    extern uiSlab *uiCurrentSlab;

    uiGetSlabCursorStack(uiCurrentSlab, &cs);
    uiPopCursorEvery(cs, &slider_cursor);
    mouse_unconstrain();
    LG_memset(OButtons, 0, MAX_OPTION_BUTTONS * sizeof(opt_button));

    // new screen: focus is picked again by opanel_redraw()
    opanel_prev_focus = opanel_focus;
    opanel_focus = -1;
    opanel_slider_editing = FALSE;
    opanel_save_armed_line = -1;
}

void opanel_redraw(uchar back) {
    extern grs_bitmap inv_backgnd;
    int but;
    LGRect r = {{INVENTORY_PANEL_X, INVENTORY_PANEL_Y},
                {INVENTORY_PANEL_X + INVENTORY_PANEL_WIDTH, INVENTORY_PANEL_Y + INVENTORY_PANEL_HEIGHT}};
#ifdef SVGA_SUPPORT
    uchar old_over = gr2ss_override;
    gr2ss_override = OVERRIDE_ALL; // Since we are really going straight to screen in our heart of hearts
#endif
    if (!full_game_3d)
        inventory_clear();
    gr_push_canvas(&inv_norm_canvas);
    uiHideMouse(NULL);
    gr_set_font(opt_font);
    if (back) {
        if (full_game_3d)
            ss_noscale_bitmap(&inv_view360_canvas.bm, FULL_BACK_X, FULL_BACK_Y);
        else
            ss_bitmap(&inv_backgnd, 0, 0);
    }

    if (opanel_focus < 0)
        opanel_focus_initial();

    for (but = 0; but < MAX_OPTION_BUTTONS; but++) {
        if (OButtons[but].drawfunc) {
            OButtons[but].drawfunc(but);
        }
    }
    uiShowMouse(&r);
    gr_pop_canvas();
#ifdef SVGA_SUPPORT
    gr2ss_override = old_over;
#endif
    opanel_draw_hints();
}

// fills in the Rect r with one of the "standard" button rects,
// assuming buttons in three columns, ro rows, high enough for
// a specified number of lines of text.
//
// rect of grid slot "slot" (see OPANEL_GRID_CELL); lr is exclusive, so the slot's grid lines stay visible
void standard_button_rect(LGRect *r, uchar slot) {
    int col = slot % 3, row = slot / 3;

    r->ul.x = col * OPANEL_SLOT_W + 1;
    r->lr.x = (col + 1) * OPANEL_SLOT_W;
    r->ul.y = OPANEL_GRID_Y0 + row * OPANEL_SLOT_H + 1;
    r->lr.y = OPANEL_GRID_Y0 + (row + 1) * OPANEL_SLOT_H;
}

// a bar sits at the bottom of its slot, its title drawn above it
void standard_slider_rect(LGRect *r, uchar slot) {
    short sh, sw;

    standard_button_rect(r, slot);

    sh = res_bm_height(OPT_SLIDER_BAR);
    sw = res_bm_height(OPT_SLIDER_BAR);
    r->ul.x += sw / 2;
    r->lr.x -= sw / 2;
    r->ul.y = r->lr.y - sh;
}

errtype wrapper_panel_close(uchar clear_message) {
    uiCursorStack *cs;
    extern uiSlab *uiCurrentSlab;
    int i;

    if (!wrapper_panel_on)
        return ERR_NOEFFECT;
    mouse_unconstrain();
    if (clear_message)
        message_info("");
    wrapper_panel_on = FALSE;
    // the next opening starts fresh on the main list
    opanel_screen = NULL;
    opanel_focus = -1;
    opanel_queued_focus = -1;
    opanel_slider_editing = FALSE;
    SavePrefs();
    inventory_page = inv_last_page;
    if (inventory_page < 0 && inventory_page != INV_3DVIEW_PAGE)
        inventory_page = 0;
    pause_game_func(0, 0, 0);
    uiGetSlabCursorStack(uiCurrentSlab, &cs);
    uiPopCursorEvery(cs, &slider_cursor);
    uiReleaseFocus(inventory_region, UI_EVENT_KBD_COOKED | UI_EVENT_MOUSE);
    uiRemoveRegionHandler(inventory_region, wrap_id);
    uiRemoveRegionHandler(inventory_region, wrap_key_id);
#ifndef STATIC_BUTTON_STORE
    full_visible = fv;
#endif
    inventory_clear();
    inventory_draw();
#ifdef SVGA_SUPPORT
    mfd_clear_all();
#endif
    for (i = 0; i < NUM_MFDS; i++)
        mfd_force_update_single(i);
    ResUnlock(OPTIONS_FONT);
    resume_game_time();
    return (OK);
}

extern uchar game_paused;

uchar can_save() {
    uchar gp = game_paused;
    if (global_fullmap->cyber) {
        // spoof the game as not being paused so that the message won't go to the
        // phantom message line in full screen mode, where it will stay only for a frame.
        game_paused = FALSE;
        string_message_info(REF_STR_NoCyberSave);
        game_paused = gp;
        return (FALSE);
    }
    if (input_cursor_mode == INPUT_OBJECT_CURSOR) {
        string_message_info(REF_STR_CursorObjSave);
        return (FALSE);
    }
    return (TRUE);
}

//
// THE TOP LEVEL OPTIONS: Initialization, handler
//

void wrapper_pushbutton_func(uchar butid) {
    switch (butid) {
    case LOAD_BUTTON: // Load Game
#ifdef DEMO
        wrapper_panel_close(FALSE);
#else
        load_screen_init();
        string_message_info(REF_STR_LoadSlot);
#endif
        break;
    case SAVE_BUTTON: // Save Game
#ifdef DEMO
        wrapper_panel_close(FALSE);
#else
        if (can_save()) {
            save_screen_init();
            string_message_info(REF_STR_SaveSlot);
        } else
            wrapper_panel_close(FALSE);
#endif
        break;
    case AUDIO_BUTTON: // Audio
        sound_screen_init();
        break;
    case INPUT_BUTTON: // Input
        input_screen_init();
        break;
    case VIDEO_BUTTON: // Input
        video_screen_init();
        break;
#ifdef SVGA_SUPPORT
    case SCREENMODE_BUTTON: // Input
        screenmode_screen_init();
        break;
    case HEAD_RECENTER_BUTTON: // Input
    {
        // extern uchar recenter_headset(ushort keycode, uint32_t context, intptr_t data);
        // recenter_headset(0,0,0);
    } break;
    case HEADSET_BUTTON:
        // headset_screen_init();
        break;
#endif
    case AUDIO_OPT_BUTTON:
        soundopt_screen_init();
        break;
    case OPTIONS_BUTTON: // Options
        options_screen_init();
        break;
    case RETURN_BUTTON: // Return
        opanel_go_back(); // back to the parent screen, or close from the main list
        break;
    case QUIT_BUTTON: // Quit
        verify_screen_init(quit_verify_pushbutton_handler, quit_verify_slorker);
        string_message_info(REF_STR_QuitConfirm);
        break;
    }
    return;
}

void wrapper_init(void) {
    LGRect r;
    int i;
    char *keyequivs;

    keyequivs = get_temp_string(REF_STR_KeyEquivs0);

    clear_obuttons();
    opanel_screen_begin(wrapper_init, NULL, PAGE_NONE, 0); // top level: Return closes the panel
    for (i = 0; i < 8; i++) {
        // Return sits centre right, like on every submenu: it swaps slots with Video
        int slot = (i == VIDEO_BUTTON) ? RETURN_BUTTON : (i == RETURN_BUTTON) ? VIDEO_BUTTON : i;

        standard_button_rect(&r, slot);
        pushbutton_init(i, keyequivs[i], REF_STR_WrapperText + i, wrapper_pushbutton_func, &r);
    }

#ifdef VITA
    standard_button_rect(&r, 8);
    pushbutton_init(8, 'v', REF_STR_VitaOptions, vita_input_init, &r);
#endif

#ifdef DEMO
    dim_pushbutton(LOAD_BUTTON);
    dim_pushbutton(SAVE_BUTTON);
#endif
    opanel_redraw(TRUE);
}

    //
    // THE VERIFY SCREEN: Initialization, handlers
    //

#pragma disable_message(202)
void quit_verify_pushbutton_handler(uchar butid) { really_quit_key_func(0, 0, 0); }

uchar quit_verify_slorker(uchar butid) {
    wrapper_panel_close(TRUE);
    return TRUE;
}

void save_verify_pushbutton_handler(uchar butid) { do_savegame_guts(savegame_verify); }

uchar save_verify_slorker(uchar butid) {
    strcpy(comments[savegame_verify], comments[NUM_SAVE_SLOTS]);
    wrapper_panel_close(TRUE);
    return TRUE;
}
#pragma enable_message(202)

void verify_screen_init(void (*verify)(uchar butid), slorker slork) {
    LGRect r;
    // Back ("no") returns to the screen that asked, e.g. the main list for Quit or the save slots
    void (*parent)(void) = opanel_screen;
    int page = opanel_page;

    clear_obuttons();
    opanel_screen_begin(NULL, parent, page, opanel_prev_focus < 0 ? 0 : opanel_prev_focus);

    standard_button_rect(&r, 1);
    pushbutton_init(0, tolower(get_temp_string(REF_STR_VerifyText)[0]), REF_STR_VerifyText, verify, &r);

    standard_button_rect(&r, 4);
    pushbutton_init(1, tolower(get_temp_string(REF_STR_VerifyText + 1)[0]), (REF_STR_VerifyText + 1), (void (*)(uchar))slork, &r);

    slork_init(2, slork);

    opanel_redraw(TRUE);
}

void quit_verify_init(void) { verify_screen_init(quit_verify_pushbutton_handler, quit_verify_slorker); }

//
// THE SOUND OPTIONS SCREEN: Initialization, update funcs

uchar curr_vol_lev = 100;
uchar curr_sfx_vol = 100;
uchar curr_alog_vol = 100;

void recompute_music_level(ushort vol) {
    //   curr_vol_lev=long_sqrt(100*vol);
    curr_vol_lev = QVAR_TO_VOLUME(vol);
    if (vol == 0) {
        music_on = FALSE;
        // stop_music_func(0,0,0);
    } else {
        if (!music_on) {
            music_on = TRUE;
            // start_music_func(0,0,0);
        }
        // mlimbs_change_master_volume(curr_vol_lev);
    }
    MacTuneUpdateVolume();
}

void recompute_digifx_level(ushort vol) {
    sfx_on = (vol != 0);
    curr_sfx_vol = QVAR_TO_VOLUME(vol);
    if (sfx_on) {
#ifdef DEMO
        play_digi_fx(73, 1);
#else
        // play a sample (if not alreay playing)
        if (!digi_fx_playing(SFX_NEAR_1, NULL))
            play_digi_fx(SFX_NEAR_1, 1);
        // update volume (main loop is not running at this point)
        sound_frame_update();
#endif
    } else {
#ifdef AUDIOLOGS
        audiolog_stop();
#endif
        stop_digi_fx();
    }
}

#ifdef AUDIOLOGS
void recompute_audiolog_level(ushort vol) {
    curr_alog_vol = QVAR_TO_VOLUME(vol);
    sound_frame_update();
}
#endif

#pragma disable_message(202)
void digi_toggle_deal(uchar offon) {
    int vol;
    vol = (sfx_on) ? 100 : 0;
    recompute_digifx_level(vol);
    QUESTVAR_SET(SFX_VOLUME_QVAR, vol);
}

#ifdef AUDIOLOGS
void audiolog_dealfunc(short val) {
    if (!val)
        audiolog_stop();
    QUESTVAR_SET(ALOG_OPT_QVAR, audiolog_setting);
}
#endif

char hack_digi_channels = 1;

void digichan_dealfunc(short val) {
    hack_digi_channels = val;
    switch (hack_digi_channels) {
    case 0:
        cur_digi_channels = 2;
        break;
    case 1:
        cur_digi_channels = 4;
        break;
    case 2:
        cur_digi_channels = 8;
        break;
    }
    QUESTVAR_SET(DIGI_CHANNELS_QVAR, hack_digi_channels);
    // snd_set_digital_channels(cur_digi_channels);
}

static void seqer_dealfunc(short val) {
//    INFO("Selected MIDI device %d", val);
    gShockPrefs.soMidiOutput = 0;
    ReloadDecXMI(); // Reload Midi decoder
    soundopt_screen_init();
    (void)val;
}

static void midi_output_dealfunc(short val) {
//    INFO("Selected MIDI output %d", val);
    ReloadDecXMI(); // Reload Midi decoder
    soundopt_screen_init();
    (void)val;
}

#pragma enable_message(202)

#define SLIDER_OFFSET_3 0
void soundopt_screen_init() {
    LGRect r;
    char retkey;
    int i = 0;

    clear_obuttons();
    opanel_screen_begin(soundopt_screen_init, sound_screen_init, PAGE_AUDIO, AUDIO_OPT_BUTTON);

    standard_button_rect(&r, i);
    retkey = tolower(get_temp_string(REF_STR_AilThreeText)[0]);
    multi_init(i, retkey, REF_STR_AilThreeText, REF_STR_DigiChannelState, ID_NULL, sizeof(hack_digi_channels),
               &hack_digi_channels, 3, digichan_dealfunc, &r);
    i++;

    standard_button_rect(&r, i);
    retkey = tolower(get_temp_string(REF_STR_AilThreeText + 1)[0]);
    // multi_init(i, retkey, REF_STR_AilThreeText+1, REF_STR_StereoReverseState, NULL,
    //   sizeof(snd_stereo_reverse), &snd_stereo_reverse, 2, NULL, &r);
    // i++;

#ifdef AUDIOLOGS
    standard_button_rect(&r, i);
    retkey = tolower(get_temp_string(REF_STR_MusicText + 3)[0]);
    multi_init(i, retkey, REF_STR_MusicText + 3, REF_STR_AudiologState, ID_NULL, sizeof(audiolog_setting),
               &audiolog_setting, 3, audiolog_dealfunc, &r);
    i++;
#endif

    standard_button_rect(&r, i);
    multi_init(i, 'p', REF_STR_Seqer, REF_STR_ADLMIDI, ID_NULL,
               sizeof(gShockPrefs.soMidiBackend), &gShockPrefs.soMidiBackend, OPT_SEQ_Max, seqer_dealfunc, &r);
    i++;
/* standard button is too narrow, so use a slider instead
    const unsigned int numMidiOutputs = GetOutputCountXMI();
    INFO("numMidiOutputs=%d", numMidiOutputs);
    standard_button_rect(&r, i);
    multi_init(i, 'o', REF_STR_MidiOut, REF_STR_MidiOutX, ID_NULL,
               sizeof(gShockPrefs.soMidiOutput), &gShockPrefs.soMidiOutput, numMidiOutputs, midi_output_dealfunc, &r);
    i++;
*/
    unsigned int midiOutputCount = GetOutputCountXMI();
    if (midiOutputCount > 1)
    {
        standard_slider_rect(&r, i);
        r.lr.x += OPANEL_SLOT_W; // spans two slots
        slider_init(i, REF_STR_MidiOutX + gShockPrefs.soMidiOutput, sizeof(gShockPrefs.soMidiOutput), FALSE, &gShockPrefs.soMidiOutput, midiOutputCount - 1,
                    0, midi_output_dealfunc, &r);
        i++;
    }
    else if (midiOutputCount == 1)
    {
        // just show a text label
        standard_button_rect(&r, i);
        textwidget_init(i, BUTTON_COLOR, REF_STR_MidiOutX, &r);
        i++;
    }

    standard_button_rect(&r, 5);
    retkey = tolower(get_temp_string(REF_STR_MusicText + 2)[0]);
    pushbutton_init(RETURN_BUTTON, retkey, REF_STR_MusicText + 2, wrapper_pushbutton_func, &r);

    // FIXME: Cannot pass a keycode with modifier flags as uchar
    keywidget_init(QUIT_BUTTON, /*KB_FLAG_ALT |*/ 'x', wrapper_pushbutton_func);
    opanel_redraw(TRUE);
}

void sound_screen_init(void) {
    LGRect r;
    uchar sliderbase;
    char retkey;

    clear_obuttons();
    opanel_screen_begin(sound_screen_init, wrapper_init, PAGE_AUDIO, AUDIO_BUTTON);

    if (music_card) {
        standard_slider_rect(&r, 0);
        r.lr.x += OPANEL_SLOT_W; // spans two slots
        sliderbase = r.lr.x - r.ul.x - 2;
        slider_init(0, REF_STR_MusicText, sizeof(ushort), TRUE, &player_struct.questvars[MUSIC_VOLUME_QVAR], 100,
                    sliderbase, recompute_music_level, &r);
    } else {
        standard_button_rect(&r, 0);
        r.lr.x += OPANEL_SLOT_W; // spans two slots
        textwidget_init(0, BUTTON_COLOR, REF_STR_MusicFeedbackText + 2, &r);
    }

    if (digi_gain) {
        standard_slider_rect(&r, 3);
        r.lr.x += OPANEL_SLOT_W; // spans two slots
        slider_init(1, REF_STR_MusicText + 1, sizeof(ushort), FALSE, &player_struct.questvars[SFX_VOLUME_QVAR], 100,
                    sliderbase, recompute_digifx_level, &r);
    } else {
        standard_button_rect(&r, 3);
        multi_init(1, get_temp_string(REF_STR_MusicText + 1)[0], REF_STR_MusicText + 1, REF_STR_OffonText,
                   REF_STR_MusicFeedbackText + 5, sizeof(sfx_on), &sfx_on, 2, digi_toggle_deal, &r);
    }

#ifdef AUDIOLOGS
    standard_slider_rect(&r, 6);
    r.lr.x += OPANEL_SLOT_W; // spans two slots
    slider_init(2, REF_STR_MusicText + 4, sizeof(ushort), FALSE, &player_struct.questvars[ALOG_VOLUME_QVAR], 100,
                sliderbase, recompute_audiolog_level, &r);
#endif

    standard_button_rect(&r, 2);
    retkey = tolower(get_temp_string(REF_STR_AilThreeText + 2)[0]);
    pushbutton_init(AUDIO_OPT_BUTTON, retkey, REF_STR_AilThreeText + 2, wrapper_pushbutton_func, &r);

    standard_button_rect(&r, 5);
    retkey = tolower(get_temp_string(REF_STR_MusicText + 2)[0]);
    pushbutton_init(RETURN_BUTTON, retkey, REF_STR_MusicText + 2, wrapper_pushbutton_func, &r);

    // FIXME: Cannot pass a keycode with modifier flags as uchar
    keywidget_init(QUIT_BUTTON, /*KB_FLAG_ALT |*/ 'x', wrapper_pushbutton_func);

    opanel_redraw(TRUE);
}

    //
    // THE OPTIONS SCREEN: Initialization, update funcs
    //

    /*void gamma_dealfunc(ushort gamma_qvar)
    {
       fix gamma;

    //   gamma=FIX_UNIT-fix_make(0,gamma_qvar);
    //   gamma=fix_mul(gamma,gamma)+(FIX_UNIT/2);
       gamma=QVAR_TO_GAMMA(gamma_qvar);
       gr_set_gamma_pal(0,256,gamma);
    }*/

#ifdef SVGA_SUPPORT
uchar wrapper_screenmode_hack = FALSE;
void screenmode_change(uchar new_mode) {
    extern short mode_id;
    mode_id = new_mode;
    QUESTVAR_SET(SCREENMODE_QVAR, new_mode);
    change_mode_func(0, 0, _current_loop);
    wrapper_screenmode_hack = TRUE;

    INFO("Changed screen mode to %i\n", mode_id);
    wrapper_panel_close(TRUE);
}
#endif

void language_change(uchar lang) {
    extern int string_res_file, mfdart_res_file;
    extern char *mfdart_files[];
    extern char *language_files[];

    ResCloseFile(string_res_file);
    ResCloseFile(mfdart_res_file);

    mfdart_res_file = ResOpenFile(mfdart_files[lang]);
    if (mfdart_res_file < 0)
        critical_error(CRITERR_RES | 2);

    string_res_file = ResOpenFile(language_files[lang]);
    if (string_res_file < 0)
        critical_error(CRITERR_RES | 0);

    QUESTVAR_SET(LANGUAGE_QVAR, lang);

    // in case we got here from interpret_qvars, and thus
    // haven't set this yet
    which_lang = lang;

    invent_language_change();
    mfd_language_change();
    side_icon_language_change();
    // free_options_cursor();
    make_options_cursor();
}

void language_dealfunc(uchar lang) {
    language_change(lang);

    render_run();
    opanel_redraw(FALSE);
}

void dclick_dealfunc(ushort dclick_qvar) {
    uiDoubleClickDelay = QVAR_TO_DCLICK(dclick_qvar, 0);
    uiDoubleClickTime = QVAR_TO_DCLICK(dclick_qvar, 1);
}

void joysens_dealfunc(ushort joysens_qvar) {
    extern fix inpJoystickSens;

    inpJoystickSens = QVAR_TO_JOYSENS(joysens_qvar);
}

#pragma disable_message(202)
void center_joy_go(uchar butid) {

    // recenter_joystick(0,0,0);
    joystick_screen_init();
}
#pragma enable_message(202)

void center_joy_pushbutton_func(uchar butid) {
    int i;
    string_message_info(REF_STR_CenterJoyPrompt);

    // take over this button, null the other buttons
    // except for RETURN and QUIT;

    for (i = 0; i < MAX_OPTION_BUTTONS; i++) {
        if (i == butid)
            keywidget_init(i, KEY_ENTER, center_joy_go);
        else if (i != RETURN_BUTTON && i != QUIT_BUTTON)
            OButtons[i].evmask = 0;
    }
}

static void renderer_dealfunc(bool unused) {
    uiHideMouse(NULL);
    opanel_render_view();
    if (full_game_3d) {
        // update stored background bitmap and redraw menu
        ss_get_bitmap(&inv_view360_canvas.bm, GAME_MESSAGE_X, GAME_MESSAGE_Y);
        opanel_redraw(FALSE);
    }
    uiShowMouse(NULL);
    // recalculate menu in case a button needs to be added or removed
    video_screen_init();
    // suppress compiler warning
    (void)unused;
}

void detail_dealfunc(uchar det) {

    change_detail_level(det);
    uiHideMouse(NULL);
    opanel_render_view();
    if (full_game_3d)
        opanel_redraw(FALSE);
    uiShowMouse(NULL);
}

void mousehand_dealfunc(ushort lefty) {
    // mouse_set_lefty(lefty);
}

#if defined(VFX1_SUPPORT) || defined(CTM_SUPPORT)
#pragma disable_message(202)
void headset_stereo_dealfunc(uchar st_on) {
    extern uchar inp6d_headset;
    extern uchar inp6d_stereo;
    //   extern uchar ui_stereo_on;
    if ((inp6d_headset) && (i6d_device != I6D_ALLPRO)) {
        //      ui_stereo_on = inp6d_stereo;
        if (!inp6d_stereo)
            i6_video(I6VID_CLOSEDOWN, NULL); // this will want to be I6VID_STR_CLOSE at some point
        else {
            if (i6_video(I6VID_STR_START, NULL)) {
                Warning(("Headset stereo startup failed!\n"));
                return;
            }
        }
    }
}

void headset_tracking_dealfunc(uchar tr_on) {
    Warning(("tracking now %d!\n", tr_on));
    return;
}

void headset_fov_dealfunc(int hackval) {
    inp6d_curr_fov = hack_headset_fov + HEADSET_FOV_MIN;
    Warning(("FOV now %d!\n", inp6d_curr_fov));
    return;
}
#pragma enable_message(202)
#endif

#pragma disable_message(202)
void olh_dealfunc(uchar olh) {
    toggle_olh_func(0, 0, 0);
}
#pragma enable_message(202)

#ifdef STEREO_SUPPORT
#define INITIAL_OCULAR_DIST fix_make(3, 0x4000)
#endif

ushort wrap_joy_type = 0;
ushort high_joy_flags;
void joystick_type_func(ushort new_joy_type) {
    extern uchar joystick_count;
    // joystick_count = joy_init(high_joy_flags | new_joy_type);
    // config_set_single_value("joystick",CONFIG_INT_TYPE,(config_valtype)(high_joy_flags|new_joy_type));
    joystick_screen_init();
}

void joystick_screen_init(void) {
    LGRect r;
    int i = 0;
    char *keys;
    extern uchar inp6d_headset;
    uchar sliderbase;

    extern uchar joystick_count;
    keys = get_temp_string(REF_STR_KeyEquivs6);
    clear_obuttons();
    opanel_screen_begin(joystick_screen_init, input_screen_init, PAGE_INPUT, JOYSTICK_BUTTON);

    standard_button_rect(&r, i);
    multi_init(i, keys[i], REF_STR_JoystickType, REF_STR_JoystickTypes, ID_NULL, sizeof(wrap_joy_type),
               &wrap_joy_type, 4, joystick_type_func, &r);
    i++;

    standard_button_rect(&r, i);
    pushbutton_init(i, keys[i], REF_STR_CenterJoy, center_joy_pushbutton_func, &r);
    if (!joystick_count && !inp6d_headset) {
        dim_pushbutton(i);
    }
    i++;

    if (joystick_count) {
        standard_slider_rect(&r, i);
        sliderbase = (r.lr.x - r.ul.x - 2) >> 1;
        slider_init(i, REF_STR_JoystickSens, sizeof(ushort), FALSE, &player_struct.questvars[JOYSENS_QVAR], 256,
                    sliderbase, joysens_dealfunc, &r);
    }
    i++;

    standard_button_rect(&r, 5);
    pushbutton_init(RETURN_BUTTON, keys[i], REF_STR_OptionsText + 5, wrapper_pushbutton_func, &r);

    // FIXME: Cannot pass a keycode with modifier flags as uchar
    keywidget_init(QUIT_BUTTON, /*KB_FLAG_ALT |*/ 'x', wrapper_pushbutton_func);

    opanel_redraw(TRUE);
}

#pragma disable_message(202)
void joystick_button_func(uchar butid) { joystick_screen_init(); }
#pragma enable_message(202)

void input_screen_init(void) {
    LGRect r;
    char *keys;
    int i = 0;
    uchar sliderbase;
    extern uchar inp6d_headset;

    keys = get_temp_string(REF_STR_KeyEquivs1);
    clear_obuttons();
    opanel_screen_begin(input_screen_init, wrapper_init, PAGE_INPUT, INPUT_BUTTON);

    standard_button_rect(&r, i);
    multi_init(i, keys[0], REF_STR_OptionsText + 0, REF_STR_OffonText, REF_STR_PopupCursFeedback, sizeof(popup_cursors),
               &popup_cursors, 2, NULL, &r);
    i++;

    standard_button_rect(&r, i);
    multi_init(i, keys[1], REF_STR_OptionsText + 1, REF_STR_MouseHand, REF_STR_HandFeedback,
               sizeof(player_struct.questvars[MOUSEHAND_QVAR]), &player_struct.questvars[MOUSEHAND_QVAR], 2,
               mousehand_dealfunc, &r);
    i++;

    standard_slider_rect(&r, i);
    sliderbase = ((r.lr.x - r.ul.x - 3) * (FIX_UNIT / 3)) / USHRT_MAX;
    slider_init(i, REF_STR_DoubleClick, sizeof(ushort), FALSE, &player_struct.questvars[DCLICK_QVAR], USHRT_MAX,
                sliderbase, dclick_dealfunc, &r);
    i++;

    standard_button_rect(&r, i);
    pushbutton_init(i, keys[2], REF_STR_Joystick, joystick_button_func, &r);
    i++;

    standard_button_rect(&r, i);
    multi_init(i, keys[3], REF_STR_MousLook, REF_STR_MousNorm, ID_NULL,
               sizeof(gShockPrefs.goInvertMouseY), &gShockPrefs.goInvertMouseY, 2, NULL, &r);
    i++;

    standard_button_rect(&r, 5);
    pushbutton_init(RETURN_BUTTON, keys[3], REF_STR_OptionsText + 5, wrapper_pushbutton_func, &r);

    // FIXME: Cannot pass a keycode with modifier flags as uchar
    keywidget_init(QUIT_BUTTON, /*KB_FLAG_ALT |*/ 'x', wrapper_pushbutton_func);

    opanel_redraw(TRUE);
}

#ifdef VITA
void vita_input_init(uchar butid) {
    LGRect r;
    char *keys;
    int i = 0;
    uchar sliderbase;
    extern uchar inp6d_headset;

    keys = get_temp_string(REF_STR_KeyEquivs1);
    clear_obuttons();
    opanel_screen_begin(vita_input_screen, wrapper_init, PAGE_VITA, VITA_INPUT_BUTTON);

    // gyro aiming
    standard_button_rect(&r, i);
    multi_init(i, keys[0], REF_STR_GyroAiming, REF_STR_GyroOff, ID_NULL,
                sizeof(gShockPrefs.gyroAiming), &gShockPrefs.gyroAiming, 2, NULL, &r);
    i++;

    sliderbase = ((r.lr.x - r.ul.x - 1) * 5 / 15);

    standard_slider_rect(&r, i);
    slider_init(i, REF_STR_GyroLookSpeed, sizeof(ushort), FALSE, &gShockPrefs.gyroAimingSpeed, 15,
                sliderbase, NULL, &r);
    i++;

    sliderbase = ((r.lr.x - r.ul.x - 1) * 10 / 25);

    standard_slider_rect(&r, i);
    slider_init(i, REF_STR_ControllerLookSpeed, sizeof(ushort), FALSE, &gShockPrefs.controllerAimingSpeed, 25,
                sliderbase, NULL, &r);
    i++;

    standard_button_rect(&r, 5);
    pushbutton_init(RETURN_BUTTON, keys[3], REF_STR_OptionsText + 5, wrapper_pushbutton_func, &r);

    // FIXME: Cannot pass a keycode with modifier flags as uchar
    keywidget_init(QUIT_BUTTON, /*KB_FLAG_ALT |*/ 'x', wrapper_pushbutton_func);

    opanel_redraw(TRUE);
}
#endif

//gamma param not used here; see SetSDLPalette() in Shock.c
void gamma_slider_dealfunc(ushort gamma_qvar) {
    gr_set_gamma_pal(0, 256, 0);

    uiHideMouse(NULL);
    opanel_render_view();
    if (full_game_3d)
        opanel_redraw(FALSE);
    uiShowMouse(NULL);
}

void video_screen_init(void) {
    LGRect r;
    int i;
    char *keys;
#ifdef SVGA_SUPPORT
    extern short mode_id;
#endif
    uchar sliderbase;
#ifdef STEREO_SUPPORT
    extern uchar inp6d_headset;
#endif

    keys = get_temp_string(REF_STR_KeyEquivs3);
    clear_obuttons();
    opanel_screen_begin(video_screen_init, wrapper_init, PAGE_VIDEO, VIDEO_BUTTON);
    i = 0;

#ifdef USE_OPENGL
    // renderer
    if(can_use_opengl()) {
        standard_button_rect(&r, i);
        multi_init(i, 'g', REF_STR_Renderer, REF_STR_Software, ID_NULL,
                   sizeof(gShockPrefs.doUseOpenGL), &gShockPrefs.doUseOpenGL, 2, renderer_dealfunc, &r);
        i++;
    }
#endif

#ifdef SVGA_SUPPORT
    // video mode
    standard_button_rect(&r, i);
    pushbutton_init(SCREENMODE_BUTTON, keys[0], REF_STR_VideoText, wrapper_pushbutton_func, &r);
    i++;
#endif

    // detail level
    standard_button_rect(&r, i);
    multi_init(i, keys[1], REF_STR_OptionsText + 4, REF_STR_DetailLvl, REF_STR_DetailLvlFeedback,
               sizeof(_fr_global_detail), &_fr_global_detail, 4, detail_dealfunc, &r);
    i++;

    // gamma
    standard_slider_rect(&r, i);
    sliderbase = ((r.lr.x - r.ul.x - 1) * 29 / 100);
    slider_init(i, REF_STR_OptionsText + 3, sizeof(ushort), TRUE, &(gShockPrefs.doGamma), 100,
                sliderbase, gamma_slider_dealfunc, &r);
    i++;

#if defined(VFX1_SUPPORT) || defined(CTM_SUPPORT)
    standard_button_rect(&r, i);
    pushbutton_init(HEADSET_BUTTON, keys[2], REF_STR_HeadsetText, wrapper_pushbutton_func, &r);
    if (!inp6d_headset)
        dim_pushbutton(HEADSET_BUTTON);
    i++;
#endif

#ifdef USE_OPENGL
    // textre filter
    if(can_use_opengl() && gShockPrefs.doUseOpenGL) {
        standard_button_rect(&r, i);
        multi_init(i, 't', REF_STR_TextFilt, REF_STR_TFUnfil, ID_NULL,
                   sizeof(gShockPrefs.doTextureFilter), &gShockPrefs.doTextureFilter, 2, renderer_dealfunc, &r);
        i++;
    }
#endif

    // return (fixed at position 5)
    standard_button_rect(&r, 5);
    pushbutton_init(RETURN_BUTTON, keys[3], REF_STR_OptionsText + 5, wrapper_pushbutton_func, &r);

    // FIXME: Cannot pass a keycode with modifier flags as uchar
    keywidget_init(QUIT_BUTTON, /*KB_FLAG_ALT |*/ 'x', wrapper_pushbutton_func);

    opanel_redraw(TRUE);
}

#if defined(VFX1_SUPPORT) || defined(CTM_SUPPORT)
void headset_screen_init(void) {
    LGRect r;
    int i;
    char *keys;
#ifdef STEREO_SUPPORT
    extern uchar inp6d_stereo;
    extern int inp6d_stereo_div;
#endif

    keys = get_temp_string(REF_STR_KeyEquivs5);

    clear_obuttons();
    opanel_screen_begin(headset_screen_init, video_screen_init, PAGE_VIDEO, HEADSET_BUTTON);

    i = 0;

    standard_button_rect(&r, i);
    pushbutton_init(HEAD_RECENTER_BUTTON, keys[0], REF_STR_HeadsetText + 1, wrapper_pushbutton_func, &r);

#ifdef STEREO_SUPPORT
    i++;
    standard_slider_rect(&r, i);
    slider_init(i, REF_STR_HeadsetText + 2, sizeof(inp6d_stereo_div), FALSE, &inp6d_stereo_div, fix_make(10, 0),
                INITIAL_OCULAR_DIST, NULL, &r);

    i++;
    standard_button_rect(&r, i);
    multi_init(i, keys[1], REF_STR_HeadsetText + 3, REF_STR_OffonText, ID_NULL, sizeof(inp6d_stereo),
               &inp6d_stereo, 2, headset_stereo_dealfunc, &r);

    if (i6d_device == I6D_ALLPRO)
        dim_pushbutton(i);

    i++;
    standard_button_rect(&r, i);
    multi_init(i, keys[3], REF_STR_MoreHeadset + 1, REF_STR_OffonText, ID_NULL, sizeof(headset_track),
               &headset_track, 2, headset_tracking_dealfunc, &r);

    i++;
    standard_slider_rect(&r, i);
    slider_init(i, REF_STR_MoreHeadset, sizeof(hack_headset_fov), FALSE, &hack_headset_fov,
                HEADSET_FOV_MAX - HEADSET_FOV_MIN, inp6d_real_fov - HEADSET_FOV_MIN, headset_fov_dealfunc, &r);
#endif

    // Standard return button and other bureaucracy
    standard_button_rect(&r, 5);
    pushbutton_init(RETURN_BUTTON, keys[2], REF_STR_OptionsText + 5, wrapper_pushbutton_func, &r);
    keywidget_init(QUIT_BUTTON, KB_FLAG_ALT | 'x', wrapper_pushbutton_func);
    opanel_redraw(TRUE);
}
#endif

#ifdef SVGA_SUPPORT
void screenmode_screen_init(void) {
    LGRect r;
    int i;
    char *keys;

    if (wrapper_screenmode_hack && !(can_use_opengl() && gShockPrefs.doUseOpenGL)) {
        uiHideMouse(NULL);
        render_run();
        uiShowMouse(NULL);
        wrapper_screenmode_hack = FALSE;
    }

    keys = get_temp_string(REF_STR_KeyEquivs4);

    clear_obuttons();
    opanel_screen_begin(screenmode_screen_init, video_screen_init, PAGE_VIDEO, SCREENMODE_BUTTON);

    for (i = 0; i < 5; i++) {
        extern short svga_mode_data[];
        uchar mode_ok = FALSE;
        char j = 0;
        standard_button_rect(&r, i);
#ifdef VITA
        pushbutton_init(i, keys[i], REF_STR_VitaRes1 + i, screenmode_change, &r);
#else
        pushbutton_init(i, keys[i], REF_STR_ScreenModeText + i, screenmode_change, &r);
#endif
        while ((grd_info.modes[j] != -1) && !mode_ok) {
            if (grd_info.modes[j] == svga_mode_data[i])
                mode_ok = TRUE;
            j++;
        }
        if (!mode_ok)
            dim_pushbutton(i);
        else if (i == convert_use_mode)
            bright_pushbutton(i);
    }

    standard_button_rect(&r, 5);
    pushbutton_init(RETURN_BUTTON, keys[2], REF_STR_OptionsText + 5, wrapper_pushbutton_func, &r);

    // FIXME: Cannot pass a keycode with modifier flags as uchar
    keywidget_init(QUIT_BUTTON, /*KB_FLAG_ALT |*/ 'x', wrapper_pushbutton_func);

    opanel_redraw(TRUE);
}
#endif

void options_screen_init(void) {
    LGRect r;
    char *keys;
    int i = 0;

    keys = get_temp_string(REF_STR_KeyEquivs2);
    clear_obuttons();
    opanel_screen_begin(options_screen_init, wrapper_init, PAGE_OPTIONS, OPTIONS_BUTTON);

    // olh_temp=(QUESTBIT_GET(OLH_QBIT)==0);

    olh_temp = olh_active;

    // okay, I admit it, we're going to tweak these "standard"
    // button rects a little bit.

    standard_button_rect(&r, 0);
    multi_init(i, keys[i], REF_STR_OptionsText + 2, REF_STR_TerseText, REF_STR_TerseFeedback,
               sizeof(gShockPrefs.goMsgLength), &(gShockPrefs.goMsgLength), 2, NULL, &r);
    i++;

    i++;

    standard_button_rect(&r, 1);
    multi_init(i, keys[i], REF_STR_OnlineHelp, REF_STR_OffonText, ID_NULL, sizeof(olh_temp), &olh_temp, 2,
               olh_dealfunc, &r);
    i++;

    i++;

    standard_button_rect(&r, 2);
    multi_init(i, keys[i], REF_STR_Language, REF_STR_Languages, ID_NULL, sizeof(which_lang), &which_lang, 3,
               language_dealfunc, &r);
    i++;

    standard_button_rect(&r, 5);
    pushbutton_init(RETURN_BUTTON, keys[i], REF_STR_OptionsText + 5, wrapper_pushbutton_func, &r);

    // FIXME: Cannot pass a keycode with modifier flags as uchar
    keywidget_init(QUIT_BUTTON, /*KB_FLAG_ALT |*/ 'x', wrapper_pushbutton_func);

    opanel_redraw(TRUE);
}

#pragma disable_message(202)
uchar wrapper_options_func(ushort keycode, uint32_t context, intptr_t data) {
    wrapper_start(wrapper_init);
    return (OK);
}
#pragma enable_message(202);

//
// THE LOAD GAME SCREEN: Initialization, update funcs
//

#pragma disable_message(202)
void load_dealfunc(uchar butid, uchar index) {
    begin_wait();
    Poke_SaveName(index);
    // Spew(DSRC_EDITOR_Save,("attempting to load from %s\n",save_game_name));

    if (load_game(save_game_name) != OK) {
        WARN("%s: Load game failed!", __FUNCTION__);
    } else {
        INFO("Game %d loaded!", index);
        // Spew(DSRC_EDITOR_Restore,("Game %d loaded!\n",index));
    }
    end_wait();
    // spoof_mouse_event();
    wrapper_panel_close(TRUE);
}
#pragma enable_message(202)

void load_screen_init(void) {
    extern uchar valid_save;

    clear_obuttons();
    opanel_screen_begin(load_screen_init, wrapper_init, PAGE_LOAD, LOAD_BUTTON);

    textlist_init(0, *comments, NUM_SAVE_SLOTS, SAVE_COMMENT_LEN, FALSE, 0, valid_save, valid_save, REF_STR_UnusedSave,
                  BUTTON_COLOR, WHITE, BUTTON_COLOR + 2, 0, load_dealfunc, NULL);

    // FIXME: Cannot pass a keycode with modifier flags as uchar
    keywidget_init(QUIT_BUTTON, /*KB_FLAG_ALT |*/ 'x', wrapper_pushbutton_func);

    opanel_redraw(TRUE);
}

//
// THE SAVE GAME SCREEN: Initialization, update funcs
//

void save_dealfunc(uchar butid, uchar index) {
    if (!ObjSysOkay()) {
        string_message_info(REF_STR_ObjSysBad);
        savegame_verify = index;
        verify_screen_init(save_verify_pushbutton_handler, save_verify_slorker);
    } else {
        message_info("");
        do_savegame_guts(index);
    }
}

void save_screen_init(void) {
    extern uchar valid_save;

    clear_obuttons();
    opanel_screen_begin(save_screen_init, wrapper_init, PAGE_SAVE, SAVE_BUTTON);

    textlist_init(0, *comments, NUM_SAVE_SLOTS, SAVE_COMMENT_LEN, TRUE, 0xFFFF, 0xFFFF, valid_save, REF_STR_UnusedSave,
                  BUTTON_COLOR, WHITE, BUTTON_COLOR + 2, REF_STR_EnterSaveString, save_dealfunc, NULL);

    // FIXME: Cannot pass a keycode with modifier flags as uchar
    keywidget_init(QUIT_BUTTON, /*KB_FLAG_ALT |*/ 'x', wrapper_pushbutton_func);

    opanel_redraw(TRUE);
}

void wrapper_start(void (*init)(void)) {
    if (wrapper_panel_on)
        return;
    inv_last_page = inventory_page;
    if (!game_paused)
        pause_game_func(0, 0, 0);
    if (!full_game_3d)
        message_info("");
    inventory_page = -1;
    wrapper_panel_on = TRUE;
    suspend_game_time();
    opt_font = ResLock(OPTIONS_FONT);
#ifndef STATIC_BUTTON_STORE
    OButtons = (opt_button *)(_offscreen_mfd.bm.bits);
    fv = full_visible;
    full_visible = 0;
#endif
    render_run(); //move here to fix ghost mouse cursor
    uiHideMouse(NULL);
    if (full_game_3d) {
#ifdef SVGA_SUPPORT
        uchar old_over = gr2ss_override;
#endif
        gr_push_canvas(grd_screen_canvas);
#ifdef SVGA_SUPPORT
        gr2ss_override = OVERRIDE_ALL;
#endif
        ss_get_bitmap(&inv_view360_canvas.bm, GAME_MESSAGE_X, GAME_MESSAGE_Y);
#ifdef SVGA_SUPPORT
        gr2ss_override = old_over;
#endif
        gr_pop_canvas();
    } else
        inventory_clear();
    uiShowMouse(NULL);
    uiInstallRegionHandler(inventory_region, UI_EVENT_MOUSE | UI_EVENT_MOUSE_MOVE, opanel_mouse_handler, 0,
                           &wrap_id);
    uiInstallRegionHandler(inventory_region, UI_EVENT_KBD_COOKED, opanel_kb_handler, 0, &wrap_key_id);
    uiGrabFocus(inventory_region, UI_EVENT_KBD_COOKED | UI_EVENT_MOUSE);
    region_set_invisible(inventory_region, FALSE);
    reset_input_system();
    init();
}

#define NEEDED_DISKSPACE 630000
errtype check_free_diskspace(int *needed) {
    /*struct diskfree_t freespace;
    _dos_getdiskfree(0, &freespace);
    if (freespace.avail_clusters * freespace.sectors_per_cluster * freespace.bytes_per_sector < NEEDED_DISKSPACE)
    {
       *needed = NEEDED_DISKSPACE - (freespace.avail_clusters * freespace.sectors_per_cluster *
    freespace.bytes_per_sector); return(ERR_NOMEM);
    }
    *needed = 0;*/
    return (OK);
}

errtype do_savegame_guts(uchar slot) {
    extern uchar valid_save;
    errtype retval = OK;

    begin_wait();
    if (!(valid_save & (1 << slot))) {
        int needed;
        //      char buf1[128],buf2[128];
        if (check_free_diskspace(&needed) == ERR_NOMEM) {
            //         lg_sprintf(buf2, get_string(REF_STR_InsufficientDisk, buf1, 128), needed);
            string_message_info(REF_STR_InsufficientDisk);
            retval = ERR_NOMEM;
        }
    }
    if (retval == OK) {
        Poke_SaveName(slot);
        if (save_game(save_game_name, comments[slot]) != OK) {
            ERROR("Save game failed!");
            message_info("Game save failed!");
            //      strcpy(comments[comment_mode], original_comment);
            retval = ERR_NOEFFECT;
            valid_save &= ~(1 << slot);
        } else
            // Spew(DSRC_EDITOR_Save, ("Game %d saved!\n", slot));
            if (retval == OK)
            valid_save |= 1 << slot;
    }
    end_wait();
    // spoof_mouse_event();
    if (retval == OK)
        wrapper_panel_close(TRUE);
    return (retval);
}

    //#endif // NOT_YET

#pragma disable_message(202)
uchar wrapper_region_mouse_handler(uiEvent *ev, LGRegion *r, intptr_t data) {
    /*if (global_fullmap->cyber)
    {
       uiSetRegionDefaultCursor(r,NULL);
       return FALSE;
    }
    else*/

    uiSetRegionDefaultCursor(r, &option_cursor);

    if (ev->mouse_data.action & MOUSE_DOWN) {
        wrapper_options_func(0, 0, TRUE);
        return TRUE;
    }
    return FALSE;
}
#pragma enable_message(202)

errtype make_options_cursor(void) {
    char *s;
    short w, h;
    LGPoint hot = {0, 0};
    grs_canvas cursor_canv;
    short orig_w;
    extern uchar svga_options_cursor_bits[];
    uchar old_over = gr2ss_override;
    gr2ss_override = OVERRIDE_ALL;

    orig_w = w = res_bm_width(REF_IMG_bmOptionCursor);
    h = res_bm_height(REF_IMG_bmOptionCursor);
    ss_point_convert(&w, &h, FALSE);
    gr_init_bm(&option_cursor_bmap, svga_options_cursor_bits, BMT_FLAT8, BMF_TRANS, w, h);
    gr_make_canvas(&option_cursor_bmap, &cursor_canv);
    gr_push_canvas(&cursor_canv);
    gr_clear(0);
    s = get_temp_string(REF_STR_ClickForOptions);
    gr_set_font(ResLock(OPTIONS_FONT));
    gr_string_wrap(s, orig_w - 3);
    gr_string_size(s, &w, &h);
    gr_set_fcolor(0xB8);
    ss_rect(1, 1, w + 2, h + 2);
    gr_set_fcolor(0xD3);
    ss_string(s, 2, 1);
    gr_font_string_unwrap(s);
    uiMakeBitmapCursor(&option_cursor, &option_cursor_bmap, hot);
    gr_pop_canvas();
    ResUnlock(OPTIONS_FONT);
    cursor_loaded = TRUE;
    gr2ss_override = old_over;

    return OK;
}

/*void free_options_cursor(void)
{
#ifndef SVGA_SUPPORT
   if(cursor_loaded)
      Free(option_cursor_bmap.bits);
#endif
}*/

errtype wrapper_create_mouse_region(LGRegion *root) {
    errtype err;
    int id;
    LGRect r = {{0, 0}, {STATUS_X, STATUS_HEIGHT}};
    LGRegion *reg;

#ifdef VITA
    // no "Click for Options" corner: START opens the options, and the cursor stuck
    // against the top-left screen edge would keep showing it
    return OK;
#endif
    reg = &(options_mouseregion[free_mouseregion++]);
    err = region_create(root, reg, &r, 2, 0, REG_USER_CONTROLLED | AUTODESTROY_FLAG, NULL, NULL, NULL, NULL);
    if (err != OK)
        return err;
    err = uiInstallRegionHandler(reg, UI_EVENT_MOUSE | UI_EVENT_MOUSE_MOVE, wrapper_region_mouse_handler,
                                 0, &id);
    if (err != OK)
        return err;
    if (!cursor_loaded) {
        err = make_options_cursor();
        if (err != OK)
            return err;
    }
    uiSetRegionDefaultCursor(reg, &option_cursor);
    return OK;
}

//#ifdef NOT_YET //
#pragma disable_message(202)
uchar saveload_hotkey_func(ushort keycode, uint32_t context, intptr_t data) {
#ifdef DEMO
    return (TRUE);
#else
    if ((!data) && (!can_save()))
        return (TRUE);
    wrapper_start(data ? load_screen_init : save_screen_init);
    string_message_info(data ? REF_STR_LoadSlot : REF_STR_SaveSlot);
    return (TRUE);
#endif
}

uchar demo_quit_func(ushort keycode, uint32_t context, intptr_t data) {
    wrapper_start(quit_verify_init);
    string_message_info(REF_STR_QuitConfirm);
    return (TRUE);
}
#pragma enable_message(202)

//#endif // NOT_YET
