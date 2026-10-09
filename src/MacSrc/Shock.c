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
//====================================================================================
//
//		System Shock - ©1994-1995 Looking Glass Technologies, Inc.
//
//		Shock.c	-	Mac-specific initialization and main event loop.
//
//====================================================================================

//--------------------
//  Includes
//--------------------
#include <math.h>
#include <SDL.h>

#include "InitMac.h"
#include "Modding.h"
#include "Prefs.h"
#include "Shock.h"
#include "ShockBitmap.h"

#include "amaploop.h"
#include "gr2ss.h"
#include "hkeyfunc.h"
#include "mainloop.h"
#include "rastq.h"
#include "setup.h"
#include "shockolate_version.h"
#include "status.h"
#include "version.h"
#include "vprof.h"

#include <psp2/kernel/clib.h>
#include <psp2/message_dialog.h>
#include <psp2/power.h>
#include <vita2d.h>
#include "VitaGpu.h"
#ifdef VITA_PROFILE
#include <psp2/kernel/processmgr.h>
#endif
#include <unistd.h>

int _newlib_heap_size_user = 256 * 1024 * 1024;

enum
{
    VITA_FULLSCREEN_WIDTH = 960,
    VITA_FULLSCREEN_HEIGHT = 544,
};

SDL_Surface *surface = NULL;
vita2d_texture *texBuffer;
uint8_t *palettedTexturePointer;
SDL_Rect destRect;
SDL_GameController *gameController;
SDL_Sensor *vitaGyro;

void *memcpy(void *destination, const void *source, size_t n)
{
	return sceClibMemcpy(destination, source, n);
}

void *memset(void *destination, int c, size_t n)
{
	return sceClibMemset(destination, c, n);
}

void *memmove(void *destination, const void *source, size_t n)
{
	return sceClibMemmove(destination, source, n);
}

int memcmp(const void *arr1, const void *arr2, size_t n)
{
	return sceClibMemcmp(arr1, arr2, n);
}

//--------------------
//  Globals
//--------------------
bool gPlayingGame;

grs_screen *cit_screen;
SDL_Window *window;
SDL_Palette *sdlPalette;
SDL_Renderer *renderer;

SDL_AudioDeviceID device;

int num_args;
char **arg_values;

extern grs_screen *svga_screen;
extern frc *svga_render_context;

//--------------------
//  Prototypes
//--------------------
extern void init_all(void);
extern void inv_change_fullscreen(uchar on);
extern errtype load_da_palette(void);

// see Prefs.c
extern void CreateDefaultKeybindsFile(void);
extern void LoadHotkeyKeybinds(void);
extern void LoadMoveKeybinds(void);

void OpenController()
{
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            gameController = SDL_GameControllerOpen(i);
        }
    }
}

void OpenGyro()
{
    for (int i = 0; i < SDL_NumSensors(); ++i) {
        if (SDL_SensorGetDeviceType(i) == SDL_SENSOR_GYRO) {
            vitaGyro = SDL_SensorOpen(i);
        }
    }
}

void SetRenderRect(int width, int height)
{
    // screen scaling calculation
    destRect.x = 0;
    destRect.y = 0;
    destRect.w = width;
    destRect.h = height;

    int isFullScreen = 1;

    if ( width != VITA_FULLSCREEN_WIDTH || height != VITA_FULLSCREEN_HEIGHT ) {
        if ( isFullScreen ) {
            //vita2d_texture_set_filters(texBuffer, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
            if (((float)( VITA_FULLSCREEN_WIDTH ) / VITA_FULLSCREEN_HEIGHT ) >= ((float)( width ) / height ) ) {
                const float scale = (float)( VITA_FULLSCREEN_HEIGHT ) / height;
                destRect.w = (int32_t)((float)( width ) * scale );
                destRect.h = VITA_FULLSCREEN_HEIGHT;
                destRect.x = ( VITA_FULLSCREEN_WIDTH - destRect.w ) / 2;
            }
            else {
                const float scale = (float)( VITA_FULLSCREEN_WIDTH ) / width;
                destRect.w = VITA_FULLSCREEN_WIDTH;
                destRect.h = (int32_t)( (float)( height ) * scale );
                destRect.y = ( VITA_FULLSCREEN_HEIGHT - destRect.h ) / 2;
            }
        }
        else {
            // center game area
            destRect.x = ( VITA_FULLSCREEN_WIDTH - width ) / 2;
            destRect.y = ( VITA_FULLSCREEN_HEIGHT - height ) / 2;
        }
    }
}

// Shows the system's message box over the screen as it is, and waits for the
// player to close it.
static void VitaAlert(const char *text)
{
    SceMsgDialogUserMessageParam message;
    SceMsgDialogParam param;

    memset(&message, 0, sizeof(message));
    message.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    message.msg = (const SceChar8 *)text;

    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &message;

    if (sceMsgDialogInit(&param) < 0)
        return;
    while (sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING)
        SDLDraw();
    sceMsgDialogTerm();

    // the press or the tap that closed the box isn't for the game
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
}

//------------------------------------------------------------------------------------
//		Main function.
//------------------------------------------------------------------------------------
int main(int argc, char **argv) {
    if (chdir(VITA_PATH) != 0)
    {
        sceClibPrintf("Unable to chdir!\n");
        return 1;
    }

	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);
    // Save the arguments for later

    num_args = argc;
    arg_values = argv;

    // FIXME externalize this
    log_set_quiet(0);
    log_set_level(LOG_INFO);

    INFO("Logger initialized");

    // init mac managers

    InitMac();

    // Initialize the preferences file.

    SetDefaultPrefs();
    LoadPrefs();

    // The 3D view is recorded and drawn by the GPU or, where it can't be used,
    // on three cores (see docs/ARCHITECTURE.md, "Rendering paths")

    rastq_set_mode(RASTQ_TRUST_STABLE);
    rastq_set_threads(RASTQ_THREADS);
    rastq_set_min_rows(RASTQ_SMALL_VIEW_ROWS);
    rastq_use_gpu(1);

    // see Prefs.c
    CreateDefaultKeybindsFile(); // only if it doesn't already exist
    // even if keybinds file still doesn't exist, defaults will be set here
    LoadHotkeyKeybinds();
    LoadMoveKeybinds();

    // Process some startup arguments

    bool show_splash = !CheckArgument("-nosplash");

    // CC: Modding support! This is so exciting.

    ProcessModArgs(argc, argv);

    // Initialize

    init_all();
    setup_init();

    gPlayingGame = true;

    load_da_palette();
    gr_clear(0xFF);

    // Without the shader compiler there is no GPU renderer: say so

    if (vgpu_compiler_missing())
        VitaAlert("libshacccg.suprx is not installed in ur0:data/.\n\n"
                  "Without it the game cannot use the GPU and will run poorly.");

    // Draw the splash screen

    INFO("Showing splash screen");
    splash_draw(show_splash);

    // Start in the Main Menu loop

    _new_mode = _current_loop = SETUP_LOOP;
    loopmode_enter(SETUP_LOOP);

    // Start the main loop

    INFO("Showing main menu, starting game loop");
    mainloop(argc, argv);

    status_bio_end();
    stop_music();

    return 0;
}

bool CheckArgument(char *arg) {
    if (arg == NULL)
        return false;

    for (int i = 1; i < num_args; i++) {
        if (strcmp(arg_values[i], arg) == 0) {
            return true;
        }
    }

    return false;
}

// The canvases the GPU draws views into (see docs/PERFORMANCE-GPU.md):
// paletted textures like the screen's, used in turn (see vgpu_canvas). A view
// drawn there is shown from there, without copying it to the screen buffer:
// alone if it fills the screen, over the screen buffer's picture if it is
// the paneled view's window.
static vita2d_texture *viewTextures[VGPU_CANVASES];
// The view the next SDLDraw shows from its canvas, and the one the last
// SDLDraw showed, for as long as the screen buffer doesn't hold it; and the
// part of the screen each covers.
typedef struct {
    int x, y, w, h;
} ViewPlace;
static vita2d_texture *shownView, *lastView;
static ViewPlace shownPlace, lastPlace;

static int FillsScreen(const ViewPlace *place)
{
    return place->x == 0 && place->y == 0 && place->w == gScreenWide && place->h == gScreenHigh;
}

static void MakeViewTextures(int width, int height)
{
    void *pixels[VGPU_CANVASES];
    int i, made = 0;

    vgpu_set_canvases(NULL, 0, 0, 0);
    vita2d_wait_rendering_done();
    shownView = lastView = NULL;
    for (i = 0; i < VGPU_CANVASES; i++) {
        if (viewTextures[i] != NULL)
            vita2d_free_texture(viewTextures[i]);
        viewTextures[i] = NULL;
    }
    vita2d_texture_set_alloc_memblock_type( SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW );
    for (i = 0; i < VGPU_CANVASES; i++) {
        viewTextures[i] = vita2d_create_empty_texture_format(width, height, SCE_GXM_TEXTURE_FORMAT_P8_ABGR);
        if (viewTextures[i] == NULL)
            break;
        pixels[i] = vita2d_texture_get_datap(viewTextures[i]);
        memset(pixels[i], 0, vita2d_texture_get_stride(viewTextures[i]) * height);
        memcpy(vita2d_texture_get_palette(viewTextures[i]), vita2d_texture_get_palette(texBuffer), sizeof(uint32_t) * 256);
        made++;
    }
    if (made == VGPU_CANVASES)
        vgpu_set_canvases(pixels, width, height, vita2d_texture_get_stride(viewTextures[0]));
}

// A view that the GPU drew into one of its canvases, to go at (x, y) on the
// screen, can be shown from there. Returns whether the next SDLDraw will do
// that, in which case the view needn't be copied to the screen buffer.
//
// Not when something the game has on the screen buffer goes over the view,
// which the canvas would hide: the caller then copies the view there, as it
// always did. That is while the game is paused (the options panel, a video
// mail), and for the paneled view while a zoom rectangle is on its way from
// an object to a panel (tools.c draws it on the screen buffer around every
// SDLDraw).
int VitaShowView(const unsigned char *bits, int x, int y, int width, int height)
{
    extern unsigned char game_paused;
    extern bool ZoomEnable;
    ViewPlace place = {x, y, width, height};
    int i;

    shownView = lastView = NULL;
    if (game_paused)
        return 0;
    if (!FillsScreen(&place) && ZoomEnable)
        return 0;
    if (x < 0 || y < 0 || x + width > gScreenWide || y + height > gScreenHigh)
        return 0;
    for (i = 0; i < VGPU_CANVASES; i++) {
        vita2d_texture *t = viewTextures[i];
        // a view is drawn into the top left of its canvas
        if (t != NULL && bits == vita2d_texture_get_datap(t) && width <= (int)vita2d_texture_get_width(t) &&
            height <= (int)vita2d_texture_get_height(t)) {
            shownView = t;
            shownPlace = place;
            return 1;
        }
    }
    return 0;
}

// The game is about to draw on the screen without drawing the view first
// (pause, a panel, a video mail, another screen): if the view is shown from a
// GPU canvas, the screen buffer doesn't hold it. This copies it there, once.
void VitaSyncView(void)
{
    vita2d_texture *view = shownView != NULL ? shownView : lastView;
    ViewPlace place = shownView != NULL ? shownPlace : lastPlace;
    const uint8_t *from;
    uint8_t *to;
    int y, stride;

    shownView = lastView = NULL;
    if (view == NULL || drawSurface == NULL)
        return;
    from = vita2d_texture_get_datap(view);
    stride = vita2d_texture_get_stride(view);
    to = (uint8_t *)drawSurface->pixels + place.y * drawSurface->pitch + place.x;
    for (y = 0; y < place.h; y++)
        memcpy(to + y * drawSurface->pitch, from + y * stride, place.w);
}

void InitVita2D(int width, int height)
{
    vita2d_init();

    window = SDL_CreateWindow("", 0, 0, width, height, 0);

    vita2d_texture_set_alloc_memblock_type( SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW );
    texBuffer = vita2d_create_empty_texture_format(width, height, SCE_GXM_TEXTURE_FORMAT_P8_ABGR);
    palettedTexturePointer = (uint8_t*)(vita2d_texture_get_datap(texBuffer));
    memset(palettedTexturePointer, 0, width * height * sizeof(uint8_t));

    vgpu_init();
    MakeViewTextures(width, height);

    SetRenderRect(width, height);
}

void ResizeVita2D(int width, int height)
{
    if (texBuffer != NULL) {
        vita2d_free_texture(texBuffer);
        texBuffer = NULL;
    }

    vita2d_texture_set_alloc_memblock_type( SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW );
    texBuffer = vita2d_create_empty_texture_format(width, height, SCE_GXM_TEXTURE_FORMAT_P8_ABGR);
    palettedTexturePointer = (uint8_t*)(vita2d_texture_get_datap(texBuffer));
    memset(palettedTexturePointer, 0, width * height * sizeof(uint8_t));
    MakeViewTextures(width, height);

    if (window != NULL) {
        SDL_SetWindowSize(window, width, height);
    }

    SetRenderRect(width, height);
}

void InitSDL() {
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_SENSOR) < 0) {
        DEBUG("%s: Init failed", __FUNCTION__);
    }

    gr_init();

    extern short svga_mode_data[];
    gr_set_mode(svga_mode_data[gShockPrefs.doVideoMode], TRUE);

    INFO("Setting up screen and render contexts");

    // Create a canvas to draw to
    SetupOffscreenBitmaps(grd_cap->w, grd_cap->h);

    InitVita2D(grd_cap->w, grd_cap->h);

    OpenController();
    OpenGyro();

    // Create the palette
    sdlPalette = SDL_AllocPalette(256);

    // Setup the screen
    svga_screen = cit_screen = gr_alloc_screen(grd_cap->w, grd_cap->h);
    gr_set_screen(svga_screen);

    gr_alloc_ipal();

    atexit(SDL_Quit);

    SDLDraw();
}

SDL_Color gamePalette[256];
bool UseCutscenePalette = FALSE; // see cutsloop.c
void SetSDLPalette(int index, int count, uchar *pal) {
    static bool gammalut_init = 0;
    static uchar gammalut[100 - 10 + 1][256];
    if (!gammalut_init) {
        double factor = 2.2;
        int i, j;
        for (i = 10; i <= 100; i++) {
            double gamma = (double)i * 1.0 / 100;
            gamma = 1 - gamma;
            gamma *= gamma;
            gamma = 1 - gamma;
            gamma = 1 / (gamma * factor);
            for (j = 0; j < 256; j++)
                gammalut[i - 10][j] = (uchar)(pow((double)j / 255, gamma) * 255);
        }
        gammalut_init = 1;
        INFO("Gamma LUT init\'ed");
    }

    int gam = gShockPrefs.doGamma;
    if (gam < 10)
        gam = 10;
    if (gam > 100)
        gam = 100;
    gam -= 10;

    for (int i = index; i < index + count; i++) {
        gamePalette[i].r = gammalut[gam][*pal++];
        gamePalette[i].g = gammalut[gam][*pal++];
        gamePalette[i].b = gammalut[gam][*pal++];
        gamePalette[i].a = 0xff;
    }

    if (!UseCutscenePalette) {
        // Hack black!
        gamePalette[255].r = 0x0;
        gamePalette[255].g = 0x0;
        gamePalette[255].b = 0x0;
        gamePalette[255].a = 0xff;
    }

    SDL_SetPaletteColors(sdlPalette, gamePalette, 0, 256);
    SDL_SetSurfacePalette(drawSurface, sdlPalette);
    SDL_SetSurfacePalette(offscreenDrawSurface, sdlPalette);

    uint32_t palette32Bit[256u];

    if (!surface) {
        surface = SDL_CreateRGBSurface(0, 1, 1, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
    }

    for ( size_t i = 0; i < 256u; ++i ) {
        palette32Bit[i] = SDL_MapRGBA(surface->format, gamePalette[i].r, gamePalette[i].g, gamePalette[i].b, gamePalette[i].a);
    }

    memcpy(vita2d_texture_get_palette(texBuffer), palette32Bit, sizeof(uint32_t) * 256);
    for (int i = 0; i < VGPU_CANVASES; i++)
        if (viewTextures[i] != NULL)
            memcpy(vita2d_texture_get_palette(viewTextures[i]), palette32Bit, sizeof(uint32_t) * 256);
}

void SDLDraw() {
    vita2d_texture *view = NULL;
    float scaleX = (float)(destRect.w) / gScreenWide, scaleY = (float)(destRect.h) / gScreenHigh;

    if (shownView != NULL) {
        // this frame has the 3D view as the GPU left it in one of its canvases
        view = lastView = shownView;
        lastPlace = shownPlace;
        shownView = NULL;
    } else if (lastView != NULL) {
        // No view drawn since, and nothing has said that the screen buffer
        // holds it (VitaSyncView): the last view stays on.
        view = lastView;
    }
    // The screen buffer is what is shown, unless a view covers all of it.
    if (view == NULL || !FillsScreen(&lastPlace))
        SDL_memcpy(palettedTexturePointer, drawSurface->pixels, gScreenWide * gScreenHigh * sizeof(uint8_t));

    vita2d_start_drawing();

    vita2d_draw_rectangle(0, 0, VITA_FULLSCREEN_WIDTH, VITA_FULLSCREEN_HEIGHT, 0xff000000);
    if (view == NULL || !FillsScreen(&lastPlace))
        vita2d_draw_texture_scale(texBuffer, destRect.x, destRect.y, scaleX, scaleY);
    if (view != NULL && FillsScreen(&lastPlace))
        vita2d_draw_texture_scale(view, destRect.x, destRect.y, scaleX, scaleY);
    else if (view != NULL) // the paneled view's window, over the panels
        vita2d_draw_texture_part_scale(view, destRect.x + lastPlace.x * scaleX, destRect.y + lastPlace.y * scaleY, 0, 0,
                                       lastPlace.w, lastPlace.h, scaleX, scaleY);
#ifdef VITA_PROFILE
    vprof_overlay_draw();
#endif
    vita2d_end_drawing();
    vita2d_common_dialog_update();
#ifdef VITA_PROFILE
    {
        // handing a frame over waits when two are already waiting for the
        // screen: time that isn't work
        long long before = sceKernelGetProcessTimeWide();
        vita2d_swap_buffers();
        vgpu_counters.swap_wait_us += sceKernelGetProcessTimeWide() - before;
    }
#else
    vita2d_swap_buffers();
#endif
}

bool MouseCaptured = FALSE;

extern int mlook_enabled;

void CaptureMouse(bool capture) {
    MouseCaptured = (capture && gShockPrefs.goCaptureMouse);

    if (!MouseCaptured && mlook_enabled && SDL_GetRelativeMouseMode() == SDL_TRUE) {
        SDL_SetRelativeMouseMode(SDL_FALSE);

        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        SDL_WarpMouseInWindow(window, w / 2, h / 2);
    } else
        SDL_SetRelativeMouseMode(MouseCaptured ? SDL_TRUE : SDL_FALSE);
}
