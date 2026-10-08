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
 * $Source: r:/prj/cit/src/RCS/hflip.c $
 * $Revision: 1.6 $
 * $Author: mahk $
 * $Date: 1994/10/31 22:08:14 $
 */

#include "gr2ss.h"
#include "cybmem.h"

// MLA #define REAL_HFLIP

void shock_hflip_in_place(grs_bitmap *bm) {
    grs_canvas big_canvas;
    grs_canvas bm_canvas;
    gr_init_canvas(&big_canvas, big_buffer, BMT_FLAT8, bm->w, bm->h);
    gr_init_canvas(&bm_canvas, bm->bits, BMT_FLAT8, bm->w, bm->h);
    gr_push_canvas(&big_canvas);
    gr_hflip_bitmap(bm, 0, 0);
    gr_pop_canvas();
    gr_push_canvas(&bm_canvas);
    ss_bitmap(&big_canvas.bm, 0, 0);
    gr_pop_canvas();
}

