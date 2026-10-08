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
 * $Source: r:/prj/lib/src/lg/rcs/tmpalloc.h $
 * $Revision: 1.4 $
 * $Author: kaboom $
 * $Date: 1994/08/03 23:09:27 $
 *
 * Header for routines for controlling temporary stacks of big_buffer
 *
 * This file is part of the 2d library.
 */

extern MemStack *temp_mem_get_stack(void);
extern int temp_mem_init(MemStack *ms);
extern int temp_mem_uninit(void);
extern void *temp_malloc(long n);
extern void *temp_realloc(void *p,long n);
extern int temp_free(void *p);

#define TempMemInit temp_mem_init
#define TempMemUninit temp_mem_uninit
#define TempMalloc temp_malloc
#define TempRealloc temp_realloc
#define TempFree temp_free
