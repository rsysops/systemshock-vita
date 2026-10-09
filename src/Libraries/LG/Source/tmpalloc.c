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
 * $Source: r:/prj/lib/src/lg/rcs/tmpalloc.c $
 * $Revision: 1.4 $
 * $Author: kaboom $
 * $Date: 1994/08/03 23:09:14 $
 *
 * Routines for controlling temporary memory buffer.
 *
 * This file is part of the 2d library.
 */

#include "lg.h"
#include "memall.h"
#include "tmpalloc.h"
#include "lgslot.h"
//#include <_lg.h>

/* arbitrary size for buffer.  used if a buffer isn't explicitly set. */
#define TEMP_BUF_SIZE 16384

/* memstack to use for temporary memory requests: one per thread slot, so
   the rasterizer's worker threads don't share one (see lgslot.h). */
static MemStack *temp_mem_stacks[LG_MAX_SLOTS];

/* TRUE if buffer is allocated by temp_mem_init. */
static uchar stack_dynamics[LG_MAX_SLOTS];

MemStack *temp_mem_get_stack(void)
{
   int slot=lg_slot();
   return temp_mem_stacks[slot];
}

/* sets the memstack to be used by the temporary memory routines to ms.
   if ms is NULL, it attempts to allocate a dynamic buffer of size given
   by TEMP_BUF_SIZE.  returns 0 if all is well, nonzero if there is an
   error. */
int temp_mem_init(MemStack *ms)
{
   int slot=lg_slot();
   if (ms==NULL) {
      /* allocate memstack struct and buffer dynamically. */
//      Spew(DSRC_LG_Tempmem,
//           ("TempMemInit: dynamically allocating stack of %d bytes\n",
//            TEMP_BUF_SIZE));
//      if ((ms=(MemStack *)Malloc(sizeof(MemStack)+TEMP_BUF_SIZE))==NULL) {
      if ((ms=(MemStack *)malloc(sizeof(MemStack)+TEMP_BUF_SIZE))==NULL) {
         WARN("%s: can't allocate dynamic buffer.", __FUNCTION__);
         return -1;
      }
      stack_dynamics[slot]=TRUE;
      ms->baseptr=(void *)(ms+1);
      ms->sz=TEMP_BUF_SIZE;
      MemStackInit(ms);
      temp_mem_stacks[slot]=ms;      /* save pointer to temp memstack */
      return 0;
   } else {
      /* use passed in memstack. */
      temp_mem_stacks[slot]=ms;
      return 0;
   }
}

/* sets the memstack used by the temporary memory routines to NULL.
   if the buffer was allocated dynamically, it's freed. */
int temp_mem_uninit(void)
{
   int slot=lg_slot();
   if (stack_dynamics[slot]==TRUE) {
//      Spew(DSRC_LG_Tempmem,
//           ("TempMemUninit: freeing dynamically allocated stack\n"));
      free(temp_mem_stacks[slot]);
//      free((Ptr)temp_mem_stacks[slot]);
      stack_dynamics[slot]=FALSE;
   }
   temp_mem_stacks[slot]=NULL;
   return 0;
}

/* allocate a temporary buffer of size n from the temporary memstack. */
void *temp_malloc(long n)
{
   int slot=lg_slot();
   if (temp_mem_stacks[slot]==NULL)
      if (temp_mem_init(NULL)!=0)
         return NULL;
   return MemStackAlloc(temp_mem_stacks[slot],n);
}

/* resize temporary buffer pointed to by p to be new size n. */
void *temp_realloc(void *p,long n)
{
   int slot=lg_slot();
   return MemStackRealloc(temp_mem_stacks[slot],p,n);
}

/* free temporary buffer pointed to by p. */
int temp_free(void *p)
{
   int slot=lg_slot();
   return MemStackFree(temp_mem_stacks[slot],p)==FALSE;
}

