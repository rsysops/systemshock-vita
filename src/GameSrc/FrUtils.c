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
 * FrUtils.c
 *
 *  MLA - 4/14/95
 *
 *  Contiains Mac specific utils for the renderer (fast draw slot view, full screen, etc.)
 *
 */

#include "FrUtils.h"
#include "gr2ss.h"
#include "Shock.h"
#include "2d.h"

// hard coded to copy from 56,57 to 56+536,57+259 to the screen
#define kFastSlotWide 536
#define kFastSlotHigh 259
#define kFastSlotLeft 28
#define kFastSlotTop  24

// copy the slot view from offscreen to on
void Fast_Slot_Copy(grs_bitmap *bm) {
    gr_bitmap(bm, SCONV_X(kFastSlotLeft), SCONV_Y(kFastSlotTop));
}

// where on the screen Fast_Slot_Copy puts the slot view
void Fast_Slot_Place(int *x, int *y) {
    *x = SCONV_X(kFastSlotLeft);
    *y = SCONV_Y(kFastSlotTop);
}

// copy the full screen view from offscreen to on
// hard coded to copy from 0,0 to 640,480 to the screen
void Fast_FullScreen_Copy(grs_bitmap *bm) { gr_bitmap(bm, 0, 0); }
