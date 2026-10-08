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
 * $Source: r:/prj/lib/src/2d/RCS/fl8bldbl.c $
 * $Revision: 1.3 $
 * $Author: kevin $
 * $Date: 1994/12/01 14:59:38 $
 *
 * $Log: fl8bldbl.c $
 * Revision 1.3  1994/12/01  14:59:38  kevin
 * Added sub/bitmap blending routines.
 *
 * Revision 1.2  1994/09/08  00:01:07  kevin
 * removed smooth_hv_doubler (replaced by asm version).
 *
 * Revision 1.1  1994/03/14  17:51:09  kevin
 * Initial revision
 *
 */

#include "blncon.h"
#include "blndat.h"
#include "blnfcn.h"
#include "cnvdat.h"
#include "grs.h"
#include "lg.h"
#include <string.h>

void flat8_flat8_v_double_ubitmap(grs_bitmap *bm) {
    int i, j, bpv, row, b_row; /* loop controls, bottom pixel value */
    uchar *src = bm->bits, *dst = grd_bm.bits, *src_nxt, *dst_nxt;
    int dst_skip = grd_bm.row - bm->w, src_skip = bm->row - bm->w;
    uchar *local_grd_half_blend;

    local_grd_half_blend = grd_half_blend;
    row = grd_bm.row;
    b_row = bm->row;

    LG_memcpy(dst, src, bm->w);     /* first copy the top line */
    for (i = 0; i < bm->h - 1; i++) /* for each row source, 2 destination */
    {
        src_nxt = src + b_row;      /* next line of source */
        dst += row;                 /* interpolated row */
        dst_nxt = dst + row;        /* next clone row */
        for (j = 0; j < bm->w; j++) /* all pixels in vertical clone */
        {
            *dst++ = local_grd_half_blend[((bpv = *src_nxt++) << 8) | (*src++)];
            *dst_nxt++ = bpv; /* is this faster than another memcpy? in asm probably? */
        }
        dst += dst_skip;
        src += src_skip; /* get to the next line */
    }
}

#define QSB_SIZE 4
#define LOG_QSB_SIZE 2
#define D_ROW 8 * QSB_SIZE

uchar *grd_last_sub_bm;
uchar grd_sub_bm_buffer[D_ROW * D_ROW];

#define DSB_SIZE 8
#define LOG_DSB_SIZE 3
