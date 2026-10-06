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
//		Prefs.h	-	Handles saving and loading preferences.
//
//====================================================================================

//--------------------
//  Types
//--------------------
typedef struct {
    short prefVer;       // Version - set to 0 for now.
    short prefPlayIntro; // Play intro at startup if non-zero.

    // Game Options
    short goMsgLength; // 0 - normal, 1 - brief
    bool goPopupLabels;
    bool goOnScreenHelp;
    short goLanguage; // 0 - English, 1 - French, 2 - German
    bool goCaptureMouse;
    bool goInvertMouseY;

    // Sound Options
    bool soBackMusic;
    bool soSoundFX;
    short soMusicVolume;
    short soSfxVolume;
    short soAudioLogVolume;
    short soMidiBackend; // 0 => adlmidi, 1 => native, 2 => fluidsynth
    short soMidiOutput;  // which of the MIDI backend's outputs to use

    // Display Options
    short doVideoMode;
    short doResolution; // 0 - High, 1 - Low
    short doDetail;     // 0 - Min, 1-Low, 2-High, 3-Max
    short doGamma;
    bool doUseQD;
    bool doUseOpenGL;
    // 0 => unfiltered
    // 1 => bilinear
    // TODO: add trilinear, anisotropic?
    short doTextureFilter;

#ifdef VITA
    bool gyroAiming;
    short gyroAimingSpeed;
    short controllerAimingSpeed;
    bool showCursor; // draw the cursor in menus and let the right stick move it there
    unsigned char renderer; // what fills the 3D view's pixels: VITA_RENDERER_*
#endif
} ShockPrefs;

//--------------------
//  Globals
//--------------------
extern ShockPrefs gShockPrefs;

//--------------------
//  Prototypes
//--------------------
void SetDefaultPrefs(void);
int16_t LoadPrefs(void);
int16_t SavePrefs(void);
#ifdef VITA
// The original drawing, call by call on one core; the passes recorded and
// their pixels filled on three cores (docs/PERFORMANCE-CPU.md); or recorded
// and drawn by the GPU (docs/PERFORMANCE-GPU.md), which is three cores again
// when the GPU can't be used.
enum { VITA_RENDERER_1_CORE, VITA_RENDERER_3_CORES, VITA_RENDERER_GPU, VITA_RENDERERS };
// Makes the renderer follow gShockPrefs.renderer.
void VitaApplyRenderer(void);
#endif

//-------------------
//  Enums
//-------------------
enum OPT_SEQ_ { // Must be in the same order as in wraper.h
    OPT_SEQ_ADLMIDI = 0,
    OPT_SEQ_NativeMI,
#ifdef USE_FLUIDSYNTH
    OPT_SEQ_FluidSyn,
#endif // USE_FLUIDSYNTH
    OPT_SEQ_Max
};

#ifdef VITA
// The Vita has no native MIDI, so its slot picks ADLMIDI's other OPL3 emulator.
#define OPT_SEQ_DOSBox OPT_SEQ_ADLMIDI
#define OPT_SEQ_Nuked  OPT_SEQ_NativeMI
#endif
