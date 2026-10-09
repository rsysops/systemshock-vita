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
 * $Source: r:/prj/lib/src/2d/RCS/fl8lw.c $
 * $Revision: 1.4 $
 * $Author: kevin $
 * $Date: 1994/08/16 12:50:16 $
 *
 * Routines to wall floor map a flat8 bitmap to a generic canvas.
 *
 * This file is part of the 2d library.
 *
 */

#include "cnvdat.h"
#include "fl8tf.h"
#include "fl8tmapdv.h"
#include "gente.h"
#include "poly.h"
#include "scrmac.h"
#include "tmapint.h"
#include "band.h"
#include "vtab.h"

int gri_lit_wall_umap_loop(grs_tmap_loop_info *tli);
int gri_lit_wall_umap_loop_1D(grs_tmap_loop_info *tli);

int gri_lit_wall_umap_loop(grs_tmap_loop_info *tli) {
    fix u, v, i, du, dv, di, dy, d;

    // locals used to store copies of tli-> stuff, so its in registers on the PPC
    int k, y;
    uint32_t t_mask;
    uint32_t t_wlog;
    uchar *t_bits;
    uchar *p_dest;
    int32_t gr_row;
    uchar *g_ltab;
    fix inv_dy;
    int32_t *t_vtab;
    int b_top = tli->band_top, b_bot = tli->band_bot;
    int yt, yb;

    inv_dy = fix_div(fix_make(1, 0), tli->w);
    u = fix_mul_asm_safe(tli->left.u, inv_dy);
    du = fix_mul_asm_safe(tli->right.u, inv_dy) - u;
    v = fix_mul_asm_safe(tli->left.v, inv_dy);
    dv = fix_mul_asm_safe(tli->right.v, inv_dy) - v;
    i = fix_mul_asm_safe(tli->left.i, inv_dy);
    di = fix_mul_asm_safe(tli->right.i, inv_dy) - i;
    if (di >= -256 && di <= 256)
        i += 1024;

    dy = tli->right.y - tli->left.y;

    t_mask = tli->mask;
    t_wlog = tli->bm.wlog;
    g_ltab = grd_screen->ltab;
    t_vtab = tli->vtab;
    t_bits = tli->bm.bits;
    gr_row = grd_bm.row;

    do {
        if ((d = fix_ceil(tli->right.y) - fix_ceil(tli->left.y)) > 0) {
            d = fix_ceil(tli->left.y) - tli->left.y;

            inv_dy = fix_div(fix_make(1, 0) << 8, dy);
            di = fix_mul_asm_safe_light(di, inv_dy);
            inv_dy >>= 8;
            du = fix_mul_asm_safe(du, inv_dy);
            dv = fix_mul_asm_safe(dv, inv_dy);
            u += fix_mul(du, d);
            v += fix_mul(dv, d);
            i += fix_mul(di, d);

            // Only the rows in the band, with u, v and i stepped over the
            // rest. Unlike the other wall loops this one can't skip a whole
            // column: di, as scaled above, is carried into the next column's
            // lighting below.
            yt = fix_cint(tli->left.y);
            yb = fix_cint(tli->right.y);
            if (yt < b_top) {
                u = gr_band_skip(u, du, b_top - yt);
                v = gr_band_skip(v, dv, b_top - yt);
                i = gr_band_skip(i, di, b_top - yt);
                yt = b_top;
            }
            if (yb > b_bot)
                yb = b_bot;
            y = yb - yt;
            p_dest = grd_bm.bits + (gr_row * yt) + tli->x;

            switch (tli->bm.hlog) {
            case GRL_OPAQUE:
                for (; y > 0; y--) {
                    k = t_vtab[fix_fint(v)] + fix_fint(u);
                    *p_dest = g_ltab[t_bits[k] + fix_light(i)]; // gr_fill_upixel(g_ltab[t_bits[k]+fix_light(i)],t_x,y);
                    p_dest += gr_row;
                    u += du;
                    v += dv;
                    i += di;
                }
                break;
            case GRL_TRANS:
                for (; y > 0; y--) {
                    k = t_vtab[fix_fint(v)] + fix_fint(u);
                    k = t_bits[k];
                    if (k != 0)
                        *p_dest = g_ltab[k + fix_light(i)]; // gr_fill_upixel(g_ltab[k+fix_light(i)],t_x,y);
                    p_dest += gr_row;
                    u += du;
                    v += dv;
                    i += di;
                }
                break;
            case GRL_OPAQUE | GRL_LOG2:
                for (; y > 0; y--) {
                    k = ((fix_fint(v) << t_wlog) + fix_fint(u)) & t_mask;
                    *p_dest = g_ltab[t_bits[k] + fix_light(i)];
                    p_dest += gr_row;
                    u += du;
                    v += dv;
                    i += di;
                }
                break;
            case GRL_TRANS | GRL_LOG2:
                for (; y > 0; y--) {
                    k = ((fix_fint(v) << t_wlog) + fix_fint(u)) & t_mask;
                    k = t_bits[k];
                    if (k != 0)
                        *p_dest = g_ltab[k + fix_light(i)]; // gr_fill_upixel(g_ltab[k+fix_light(i)],t_x,y);
                    p_dest += gr_row;
                    u += du;
                    v += dv;
                    i += di;
                }
                break;
            }
        } else if (d < 0)
            return TRUE; /* punt this tmap */

        tli->w += tli->dw;

        // figure out new left u & v & i
        inv_dy = 0;
        k = tli->left.u + tli->left.du;
        y = tli->left.v + tli->left.dv;
        tli->left.i += tli->left.di;

        inv_dy = fix_div(fix_make(1, 0), tli->w);
        u = fix_mul_asm_safe(k, inv_dy);
        v = fix_mul_asm_safe(y, inv_dy);
        i = fix_mul_asm_safe(tli->left.i, inv_dy);
        if (di >= -256 && di <= 256)
            i += 1024;

        tli->left.u = k;
        tli->left.v = y;

        // figure out new right u & v & i
        k = tli->right.u + tli->right.du;
        y = tli->right.v + tli->right.dv;
        tli->right.i += tli->right.di;

        du = fix_mul_asm_safe(k, inv_dy) - u;
        dv = fix_mul_asm_safe(y, inv_dy) - v;
        di = fix_mul_asm_safe(tli->right.i, inv_dy) - i;
        tli->right.u = k;
        tli->right.v = y;

        tli->left.y += tli->left.dy;
        tli->right.y += tli->right.dy;
        dy = tli->right.y - tli->left.y;
        tli->x++;
    } while (--(tli->n) > 0);

    return FALSE; /* tmap OK */
}

void gri_trans_lit_wall_umap_init(grs_tmap_loop_info *tli) {
    if ((tli->bm.row == (1 << tli->bm.wlog)) && (tli->bm.h == (1 << tli->bm.hlog))) {
        tli->mask = (1 << (tli->bm.hlog + tli->bm.wlog)) - 1;
        tli->bm.hlog = GRL_TRANS | GRL_LOG2;
    } else {
        tli->vtab = gr_make_vtab(&(tli->bm));
        tli->bm.hlog = GRL_TRANS;
    }
    tli->loop_func = (void (*)())gri_lit_wall_umap_loop;
    tli->left_edge_func = (void (*)())gri_uviwy_edge;
    tli->right_edge_func = (void (*)())gri_uviwy_edge;
}

void gri_opaque_lit_wall_umap_init(grs_tmap_loop_info *tli) {
    if ((tli->bm.row == (1 << tli->bm.wlog)) && (tli->bm.h == (1 << tli->bm.hlog))) {
        tli->mask = (1 << (tli->bm.hlog + tli->bm.wlog)) - 1;
        tli->bm.hlog = GRL_OPAQUE | GRL_LOG2;
    } else {
        tli->vtab = gr_make_vtab(&(tli->bm));
        tli->bm.hlog = GRL_OPAQUE;
    }
    tli->loop_func = (void (*)())gri_lit_wall_umap_loop;
    tli->left_edge_func = (void (*)())gri_uviwy_edge;
    tli->right_edge_func = (void (*)())gri_uviwy_edge;
}

/*extern "C"
{
extern int HandleWallLitLoop1D_PPC(grs_tmap_loop_info *tli,
                                                                                                                                                fix u, fix v, fix i, fix dv, fix di, fix dy,
                                                                                                                                                uchar *g_ltab, int32_t *t_vtab, uchar *o_bits,
                                                                                                                                                int32_t gr_row, uint32_t t_mask, uint32_t t_wlog);
}*/

int HandleWallLitLoop1D_C(grs_tmap_loop_info *tli, fix u, fix v, fix i, fix dv, fix di, fix dy, uchar *g_ltab,
                          uchar *o_bits, int32_t gr_row, uint32_t t_mask, uint32_t t_wlog) {
    fix d, inv_dy;
    register fix lefty, righty;
    int32_t k, y;
    uchar *t_bits;
    uchar *p_dest;
    int b_top = tli->band_top, b_bot = tli->band_bot;
    int yt, yb;
    // does the column have rows in the band? If not, its u, v, i, dv and di
    // are never used and their divisions are skipped.
    int in_band;

    lefty = tli->left.y;
    righty = tli->right.y;
    in_band = (fix_cint(lefty) < b_bot && fix_cint(righty) > b_top);
    do {
        if ((d = fix_ceil(righty) - fix_ceil(lefty)) > 0 && in_band) {
            d = fix_ceil(lefty) - lefty;

            inv_dy = fix_div(fix_make(1, 0) << 8, dy);
            dv = fix_mul_asm_safe(dv, inv_dy >> 8);
            di = fix_mul_asm_safe_light(di, inv_dy);

            v += fix_mul(dv, d);
            i += fix_mul(di, d);

            if (di >= -256 && di <= 256)
                i += 256;

            // only the rows in the band, with v and i stepped over the rest
            yt = fix_cint(lefty);
            yb = fix_cint(righty);
            if (yt < b_top) {
                v = gr_band_skip(v, dv, b_top - yt);
                i = gr_band_skip(i, di, b_top - yt);
                yt = b_top;
            }
            if (yb > b_bot)
                yb = b_bot;
            y = yb - yt;
            p_dest = grd_bm.bits + (gr_row * yt) + tli->x;
            t_bits = o_bits + fix_fint(u);

            // inner loop
            for (; y > 0; y--) {
                k = (fix_fint(v) << t_wlog) & t_mask;
                *p_dest = g_ltab[t_bits[k] + fix_light(i)];
                p_dest += gr_row;
                v += dv;
                i += di;
            }

        } else if (d < 0)
            return TRUE; // punt this tmap

        tli->w += tli->dw;
        tli->left.u += tli->left.du;
        tli->left.v += tli->left.dv;
        tli->left.i += tli->left.di;
        tli->right.u += tli->right.du;
        tli->right.v += tli->right.dv;
        tli->right.i += tli->right.di;
        lefty += tli->left.dy;
        righty += tli->right.dy;
        dy = righty - lefty;
        tli->x++;

        // figure out new u, v, i, dv and di, if the next column needs them
        in_band = (fix_cint(lefty) < b_bot && fix_cint(righty) > b_top);
        if (in_band) {
            inv_dy = fix_div(fix_make(1, 0), tli->w);
            u = fix_mul_asm_safe(tli->left.u, inv_dy);
            v = fix_mul_asm_safe(tli->left.v, inv_dy);
            i = fix_mul_asm_safe(tli->left.i, inv_dy);
            dv = fix_mul_asm_safe(tli->right.v, inv_dy) - v;
            di = fix_mul_asm_safe(tli->right.i, inv_dy) - i;
            if (di >= -256 && di <= 256)
                i += 1024;
        }
    } while (--(tli->n) > 0);

    tli->left.y = lefty;
    tli->right.y = righty;

    return FALSE; // tmap OK
}

// ==================================================================================
// Wall_1D versions of routines
int gri_lit_wall_umap_loop_1D(grs_tmap_loop_info *tli) {
    fix u, v, i, dv, di, dy;

    // locals used to store copies of tli-> stuff, so its in registers on the PPC
    int k, y;
    uint32_t t_mask;
    uint32_t t_wlog;
    int32_t gr_row;
    uchar *g_ltab;
    uchar *o_bits;
    fix inv_dy;

    inv_dy = fix_div(fix_make(1, 0), tli->w);
    u = fix_mul_asm_safe(tli->left.u, inv_dy);
    v = fix_mul_asm_safe(tli->left.v, inv_dy);
    dv = fix_mul_asm_safe(tli->right.v, inv_dy) - v;
    i = fix_mul_asm_safe(tli->left.i, inv_dy);
    di = fix_mul_asm_safe(tli->right.i, inv_dy) - i;
    if (di >= -256 && di <= 256)
        i += 512;

    dy = tli->right.y - tli->left.y;

    t_mask = tli->mask;
    t_wlog = tli->bm.wlog;
    g_ltab = grd_screen->ltab;
    o_bits = tli->bm.bits;
    gr_row = grd_bm.row;

    return HandleWallLitLoop1D_C(tli, u, v, i, dv, di, dy, g_ltab, o_bits, gr_row, t_mask, t_wlog);
}

void gri_opaque_lit_wall1d_umap_init(grs_tmap_loop_info *tli) {
    // Wall1D is always log2
    /*   if ((tli->bm.row==(1<<tli->bm.wlog)) &&
                (tli->bm.h==(1<<tli->bm.hlog))) {*/
    tli->mask = (1 << (tli->bm.hlog + tli->bm.wlog)) - 1;
    tli->bm.hlog = GRL_OPAQUE | GRL_LOG2;
    /*   } else {
          tli->vtab=gr_make_vtab(&(tli->bm));
          tli->bm.hlog=GRL_OPAQUE;
       }*/
    tli->loop_func = (void (*)())gri_lit_wall_umap_loop_1D;
    tli->left_edge_func = (void (*)())gri_uviwy_edge;
    tli->right_edge_func = (void (*)())gri_uviwy_edge;
}
