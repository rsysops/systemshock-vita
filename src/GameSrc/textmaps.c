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
 * $Source: r:/prj/cit/src/RCS/textmaps.c $
 * $Revision: 1.77 $
 * $Author: xemu $
 * $Date: 1994/11/21 17:38:46 $
 */

#define __TEXTMAPS_SRC

#include <string.h>

#include "textmaps.h"
#include "gettmaps.h"
#include "tools.h"
#include "frprotox.h"
#include "cybmem.h"
#include "citres.h"
#include "rendtool.h"
#include "objects.h"
#include "objstuff.h"
#include "tpolys.h"
#include "statics.h"

#include "OpenGL.h"
#include "rastq.h"


uchar textures_loaded = FALSE;

#define READ(fd, x) read(fd, (char *)&(x), sizeof(x))

Id tmap_ids[NUM_TEXTURE_SIZES] = {TEXTURE_128_ID, TEXTURE_64_ID, TEXTURE_32_ID, TEXTURE_16_ID};
ushort tmap_sizes[NUM_TEXTURE_SIZES] = {128, 64, 32, 16};
uchar all_textures = TRUE;

extern uchar tmap_big_buffer[];

// prototypes
uchar set_animations(short start, short frames, uchar *anim_used);
void setup_tmap_bitmaps(void);
errtype texture_crunch_init(void);
errtype texture_crunch_go(void);

#define SET_ANIM_USED(x) anim_used[(x) >> 3] |= (1 << ((x)&0x7))
#define CHECK_ANIM_USED(x) anim_used[(x) >> 3] & (1 << ((x)&0x7))

#define ACTUAL_SMALL_ANIMS 101
uchar set_animations(short start, short frames, uchar *anim_used) {
    int loop;
    // Note that NUM_ACTUAL_SMALL_ANIMS contains the actual number of valid
    // animations, BUT anything in the gap between them will get caught by the
    // ResInUse call in load_small_texturemaps
    if ((start < 0) || (start + frames > MAX_SMALL_TMAPS)) {
        //      mprintf("PAIN SUFFERING SET ANIM DEATH %d %d\n",start,frames);
        return FALSE;
    }
    for (loop = start; loop < start + frames; loop++)
        SET_ANIM_USED(loop);
    return TRUE;
}

errtype load_small_texturemaps(void) {
    Id id = TEXTURE_SMALL_ID;
    char i = 0;
    extern uchar obj_is_display(int triple);
    int d;
    char rv = 0;
    ObjSpecID osid;
    uchar anim_used[MAX_SMALL_TMAPS / 8];

    LG_memset(anim_used, 0, MAX_SMALL_TMAPS / 8);

    // Figure out which animations are in use
    osid = objBigstuffs[0].id;
    while (osid != OBJ_SPEC_NULL) {
        if (obj_is_display(ID2TRIP(objBigstuffs[osid].id))) { //was objSmallstuffs
            d = objBigstuffs[osid].data2;
            if ((d & TPOLY_INDEX_MASK) && ((d & TPOLY_TYPE_MASK) == 0x100))
                rv = set_animations(d & TPOLY_INDEX_MASK, objBigstuffs[osid].cosmetic_value, anim_used);
            if (((d >> 16) & TPOLY_INDEX_MASK) && ((d & TPOLY_TYPE_MASK) == 0x100))
                rv |= set_animations((d >> 16) & TPOLY_INDEX_MASK, objBigstuffs[osid].cosmetic_value, anim_used);
            //         if (!rv) mprintf("Big badness for o %x, osid %x, data2 %d\n",objBigstuffs[osid].id,osid,d);
        }
        osid = objBigstuffs[osid].next;
    }

    osid = objSmallstuffs[0].id;
    while (osid != OBJ_SPEC_NULL) {
        if (obj_is_display(ID2TRIP(objSmallstuffs[osid].id))) {
            d = objSmallstuffs[osid].data2;
            if ((d & TPOLY_INDEX_MASK) && ((d & TPOLY_TYPE_MASK) == 0x100))
                rv = (char)set_animations(d & TPOLY_INDEX_MASK, objSmallstuffs[osid].cosmetic_value, anim_used);
            if (((d >> 16) & TPOLY_INDEX_MASK) && ((d & TPOLY_TYPE_MASK) == 0x100))
                rv |=
                    (char)set_animations((d >> 16) & TPOLY_INDEX_MASK, objSmallstuffs[osid].cosmetic_value, anim_used);
            //         if (!rv) mprintf("Small badness for o %x, osid %x, data2 %d\n",objSmallstuffs[osid].id,osid,d);
        }
        osid = objSmallstuffs[osid].next;
    }

    while (ResInUse(id + i)) {
        if (CHECK_ANIM_USED(i)) {
            ResLock(id + i);
            ResUnlock(id + i);
        }
        i++;
    }
    return (OK);
}

// should really do dynamic creation of grs_bitmap *'s for the textures
// but for now, we'll be lame

extern uchar tmap_static_mem[];
static uchar *tmap_dynamic_mem = NULL;

#define get_tmap_128x128(i) ((uchar *)&tmap_dynamic_mem[i * (128 * 128)])
#define get_tmap_64x64(i)   ((uchar *)&tmap_static_mem[i * SIZE_STATIC_TMAP])
#define get_tmap_32x32(i)   ((uchar *)&tmap_static_mem[(i * SIZE_STATIC_TMAP) + (64 * 64)])
#define get_tmap_16x16(i)   ((uchar *)&tmap_static_mem[(i * SIZE_STATIC_TMAP) + (64 * 64) + (32 * 32)])

// have we built the tables, do we have the extra memory, so on
static uchar tmaps_setup = FALSE;

static grs_bitmap tmap_bitmaps[NUM_TEXTURE_SIZES];
void setup_tmap_bitmaps(void) {
    int i;
    for (i = 0; i < 4; i++)
        gr_init_bm(&tmap_bitmaps[i], NULL, BMT_FLAT8, 0, tmap_sizes[i], tmap_sizes[i]);
}

grs_bitmap *get_texture_map(int idx, int sz) {
    ushort sz_add[NUM_TEXTURE_SIZES - 1] = {0, 64 * 64, (64 * 64) + (32 * 32)};
    uchar *bt;
//   mprintf("Getting tmap %d, sz %d\n",idx,sz);
    if (sz == 0) {
        if (all_textures)
            bt = get_tmap_128x128(idx);
        else
            sz = 1;
    }
    if (sz != 0) {
        bt = get_tmap_64x64(idx) + sz_add[sz - 1];
    }
    tmap_bitmaps[sz].bits = bt;
    return &tmap_bitmaps[sz];
}

void load_textures(void) {
    grs_bitmap *cur_bm;
    int i, n, c;
    errtype retval = OK;
    int atext_tmp = 1;

    {
        if (start_mem < EXTRA_TMAP_THRESHOLD)
            atext_tmp = 0;
        else {
            // Spew(DSRC_SYSTEM_Memory, ("Sufficient Memory detected for Loading 128s!\n"));
            atext_tmp = 1;
        }
    }

    // Spew(DSRC_GFX_Texturemaps, ("all_textures = %d\n",all_textures));
    all_textures = atext_tmp;
    // Spew(DSRC_GFX_Texturemaps, ("GAME_TEXTURES = %d\n",GAME_TEXTURES));

    if (!tmaps_setup) {
        if (all_textures) // get our butts some memory
            tmap_dynamic_mem = tmap_big_buffer;

        setup_tmap_bitmaps();
        tmaps_setup = TRUE;

        // Terrain texture pixels are only written when a level loads.
        rastq_stable_pixels(tmap_static_mem, sizeof(tmap_static_mem));
        if (all_textures)
            rastq_stable_pixels(tmap_big_buffer, sizeof(tmap_big_buffer));
    }

    for (c = 0; c < NUM_LOADED_TEXTURES; c++) {
        i = loved_textures[c];
        if (!ResInUse(TEXTURE_64_ID + i)) {
            // Warning(("Hey, invalid texture in palette! slot %d = %d\n",c,i));
            i = 0;
        } // Set local properties
        for (n = SMALLEST_SIZE_INDEX; n < NUM_TEXTURE_SIZES; n++) {
            if ((n != TEXTURE_128_INDEX) || all_textures) {
                cur_bm = get_texture_map(c, n);

                // This is a BLATANT hack to get around the 1 Meg limit in the resource system
                if ((n == TEXTURE_128_INDEX) || (n == TEXTURE_64_INDEX)) {
                    if (ResInUse(tmap_ids[n] + i)) {
                        retval = load_res_bitmap(cur_bm, MKREF(tmap_ids[n] + i, 0), FALSE);
                        cur_bm->flags = 0;
                    } else {
                        // Warning(("Hey, ResInUse failed in tmap_load and i'm so blue
                        // (%d,%d,%x)\n",n,i,tmap_ids[n]+i));
                        // should abort !!!
                    }
                } else {
                    retval = load_res_bitmap(cur_bm, MKREF(tmap_ids[n], i), FALSE);
                    cur_bm->flags = 0;
                }
                if ((cur_bm->w != tmap_sizes[n]) || (cur_bm->h != tmap_sizes[n])) {
                    // Warning(("Incorrect size in tmap %d! (%d)(%d x %d) vs (%d x %d)\n",i,c,cur_bm->w,
                    //   cur_bm->h, tmap_sizes[n], tmap_sizes[n]));
                    // should abort !!!
                    //               DBG(DSRC_GFX_Texturemaps, {
                    //                              gr_set_fcolor(0);
                    //                              gr_rect(0,0,320,200);
                    //                              gr_bitmap(cur_bm, 0, 0);
                    //               });
                }
                if(can_use_opengl())
                    opengl_cache_wall_texture(c, n, cur_bm);
            }
        }
    }
    // Load in texture properties for all textures
    load_master_texture_properties();

    // Copy the appropriate things into textprops
    for (i = 0; i < NUM_LOADED_TEXTURES; i++)
        textprops[i] = texture_properties[loved_textures[i]];

    // Get rid of the big set
    unload_master_texture_properties();
    textures_loaded = TRUE;
    game_fr_reparam(all_textures, -1, -1);
}

void free_textures(void) {
    tmaps_setup = FALSE;
}

errtype bitmap_array_unload(int *num_bitmaps, grs_bitmap *arr[]) {
    int i;

    if (*num_bitmaps == 0)
        return (ERR_NOEFFECT);

    // Spew(DSRC_SYSTEM_Memory, ("Freeing %d bitmaps...\n",*num_bitmaps));
    for (i = 0; i < *num_bitmaps; i++) {
        //      Spew(DSRC_SYSTEM_Memory, ("%d ",i));
        free(arr[i]->bits);
        free(arr[i]);
    }
    *num_bitmaps = 0;
    return (OK);
}

uchar empty_bitmap(grs_bitmap *bmp) {
    uchar *cur = &bmp->bits[0], *targ = cur + (bmp->w * bmp->h);
    while (cur < targ)
        if (*cur++ != 0)
            return FALSE;
    return TRUE;
}

errtype Init_Lighting(void) {
    int i;
    FILE *fp;

    fp = fopen_caseless("res/data/shadtabl.dat", "rb");
    if (fp == NULL)
        return (ERR_FOPEN);
    fread(shading_table, 1, 256 * 16, fp);
    fclose(fp);

    for (i = 0; i < 256 * 16; i += 256) {
        shading_table[i] = 0xFF; // i love our shading table
    }

    DEBUG("Set Light Table");
    gr_set_light_tab(shading_table);

    // now read bw shading table
    fp = fopen_caseless("res/data/bwtabl.dat", "rb");
    if (fp == NULL)
        return (ERR_FOPEN);
    fread(bw_shading_table, 1, 256 * 16, fp);
    fclose(fp);

    for (i = 0; i < 256 * 16; i += 256)
        bw_shading_table[i] = 0; // i love our shading table

    fr_set_cluts(shading_table, bw_shading_table, bw_shading_table, bw_shading_table);

    return (OK);
}

errtype load_master_texture_properties(void) {
    int version, i;
    char *cp;

    texture_properties = (TextureProp *)malloc(GAME_TEXTURES * sizeof(TextureProp));

    // Load Properties from disk
    clear_texture_properties();

    DEBUG("Loading texture properties");

    FILE *f = fopen_caseless("res/data/textprop.dat", "rb");

    if (f == NULL) {
        return (ERR_FOPEN);
    }

    fseek(f, 0, SEEK_END);
    int len = ftell(f);
    rewind(f);

    cp = (char *)malloc((len + 1) * sizeof(char));
    fread(cp, len, 1, f);
    fclose(f);

    {
        memmove(&version, cp, sizeof(version));
        cp += sizeof(version);

        if (version == TEXTPROP_VERSION_NUMBER) {
            // 363 seems magic. GAME_TEXTURES instead?
            for (i = 0; i < 363; i++) {
                memmove(&texture_properties[i], cp, 11);
                cp += 11;
            }
        } else {
            ERROR("Skipping loading textprops.dat, bad version!");
        }
    }

    /*res = GetResource('tprp',1000);
    if (res)
     {
      FlipLong((long *) (*res));
      version =  * (int *) (*res);
      if (version == TEXTPROP_VERSION_NUMBER)
       {
        // copy out the structs, fixing the 11->12 byte size difference
        for (i=0; i<363; i++)
          texture_properties[i] = * (TextureProp *) (*res+4+(i*11));

        // fix shorts in texture_properties
        for (i=0; i<GAME_TEXTURES; i++)
         {
          FlipShort(&texture_properties[i].resilience);
          FlipShort(&texture_properties[i].distance_mod);
         }
       }
          ReleaseResource(res);
     }*/
    // MLA- changed this to use resources
    // if (DatapathFind(&DataDirPath, levname, path))
    /*{
       FILE* f = fopen("res/data/textprop.dat", "rb");
       if (f != NULL)
       {
          fread(version, sizeof(short), 1, f);
          if (version == TEXTPROP_VERSION_NUMBER)
          {
             fread(&texture_properties, 1, GAME_TEXTURES * 10, f);
          }
          else {
             printf("Bad Texture Properties version number (%d).  Current = %d.\n",version,TEXTPROP_VERSION_NUMBER);
          }
 //         else
 //            Warning(("Bad Texture Properties version number (%d).  Current =
 %d.\n",version,TEXTPROP_VERSION_NUMBER)); fclose(f);
       }
       else {
          printf("Cannot load textprops!\n");
       }
     }*/

    return (OK);
}

errtype unload_master_texture_properties(void) {
    free(texture_properties);
    texture_properties = NULL;
    return (OK);
}

errtype clear_texture_properties(void) {
    int i;

    // clear it all
    LG_memset(texture_properties, 0, sizeof(TextureProp) * GAME_TEXTURES);

    // set the stuff that needs values besides zero
    for (i = 0; i < GAME_TEXTURES; i++) {
        texture_properties[i].family_texture = i;
        texture_properties[i].target_texture = i;
        texture_properties[i].resilience = 10;
    }
    return (OK);
}

    //#define TEXTURE_CRUNCH_HACK

    //#define TEXTURE_ANNIHILATION

