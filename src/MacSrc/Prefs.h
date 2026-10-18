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
    bool goAutoplayLogs; // a new e-mail or log plays on arrival (see select_email)
    bool goSaveTemplate; // a save is offered the name "Save N (floor)" (see save_name_template)
    bool goCaptureMouse;
    bool goInvertMouseY;

    // Sound Options
    bool soBackMusic;
    bool soSoundFX;
    short soMusicVolume;
    short soSfxVolume;
    short soAudioLogVolume;
    short soMidiBackend; // 0 => ADLMIDI with DOSBox's OPL3, 1 => with Nuked's (see OPT_SEQ_DOSBox)
    short soMidiOutput;  // which of the MIDI backend's outputs to use

    // Display Options
    short doVideoMode;
    short doDetail;     // 0 - Min, 1-Low, 2-High, 3-Max
    short doGamma;

    bool gyroAiming;
    short gyroAimingSpeed;
    short controllerAimingSpeed;
    bool showCursor; // draw the cursor in menus and let the right stick move it there

    // The Input page's double-click bar, 0 to USHRT_MAX (see dclick_dealfunc)
    ushort goDoubleClick;
} ShockPrefs;

// The double-click bar's default: a third along, where its marker is
#define DCLICK_DEFAULT (65536 / 3)

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
// A save asked for from the middle of the game, where writing to the card
// is a long frame: done at the next SavePrefs() or SavePrefsIfWanted(),
// which is called where the game waits anyway (begin_wait).
void SavePrefsLater(void);
void SavePrefsIfWanted(void);

//-------------------
//  Enums
//-------------------
enum OPT_SEQ_ { // the values of soMidiBackend, midi-backend in the prefs file
    OPT_SEQ_ADLMIDI = 0,
    OPT_SEQ_NativeMI,
    OPT_SEQ_Max
};

// The Vita has no native MIDI, so its slot picks ADLMIDI's other OPL3 emulator.
#define OPT_SEQ_DOSBox OPT_SEQ_ADLMIDI
#define OPT_SEQ_Nuked  OPT_SEQ_NativeMI
