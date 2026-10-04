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
 * $Source: r:/prj/lib/src/2d/RCS/fl8bl.c $
 * $Revision: 1.2 $
 * $Author: kevin $
 * $Date: 1994/09/08 01:12:51 $
 *
 * Generic routines for texture mapping rsd bitmaps.
 *
 * This file is part of the 2d library.
 *
 */

#include "bitmap.h"
#include "grs.h"
#include "ifcn.h"
#include "lg.h"
#include "rsdunpck.h"
#include "tmapint.h"
#include "tmapfcn.h"
#include "tmaptab.h"

extern void flat8_flat8_smooth_hv_double_ubitmap(grs_bitmap *src, grs_bitmap *dst);
void gri_trans_blend_clut_lin_umap_init(grs_tmap_loop_info *ti);

// Doubles bm into the unpack buffer and makes bm that doubled bitmap.
static void blend_double(grs_bitmap *bm) {
    grs_bitmap tbm;

    tbm.row = 2 * bm->w;
    if (bm->bits == grd_unpack_buf)
        tbm.bits = bm->bits + bm->row * bm->h;
    else
        tbm.bits = grd_unpack_buf;

    flat8_flat8_smooth_hv_double_ubitmap(bm, &tbm);
    bm->bits = tbm.bits;
    bm->row = tbm.row;
    bm->w *= 2;
    bm->h *= 2;
}

void gri_trans_blend_clut_lin_umap_init(grs_tmap_loop_info *ti) {
    if (grd_unpack_buf != NULL) {
        blend_double(&(ti->bm));
        ti->n = BMT_FLAT8 * GRD_FUNCS + GRC_TRANS_CLUT_BILIN;
        ((void (*)(grs_tmap_loop_info *))(grd_tmap_init_table[ti->n]))(ti);
    }
}

// If h_umap would draw bm through the blend mapper with the current fill
// type, does that mapper's doubling now and changes bm and ti to what it
// would go on to draw: the doubled bitmap, through the CLUT linear mapper.
// Returns whether it did. For the rasterizer queue (3D/Source/rastq.c), which
// draws later and on several threads, where the unpack buffer can't be used.
int gr_blend_prepare(grs_bitmap *bm, grs_tmap_info *ti) {
    int index = (bm->flags & BMF_TRANS) + ti->tmap_type + GRD_FUNCS * bm->type;

    if (grd_unpack_buf == NULL || grd_tmap_init_table[index] != (void (*)())gri_trans_blend_clut_lin_umap_init)
        return 0;
    blend_double(bm);
    ti->tmap_type = GRC_CLUT_BILIN;
    return 1;
}
