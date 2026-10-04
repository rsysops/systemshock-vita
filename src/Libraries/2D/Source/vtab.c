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
//                                                     
// $Source: r:/prj/lib/src/2d/RCS/vtab.c $
// $Revision: 1.1 $
// $Author: kevin $
// $Date: 1994/07/28 01:23:36 $
//
// Procedure to create temporary vtab.
//
// This file is part of the 2d library.
//

#include "grs.h"
#include "buffer.h"


// Build a table of line starts for the bitmap parameter.
//
// Rounding at the ends of a span can make the mappers ask for the line just
// before the first or just after the last. Those two entries used to be
// whatever memory surrounded the table, so the pixel drawn depended on what
// had used temporary memory before, and would differ between the threads of
// a split replay (3D/Source/rastq.c). They now repeat the first and the last
// line.
int32_t *gr_make_vtab (grs_bitmap *bm)
 {
 	int32_t *tab;
 	int32_t i,add,row;
 	int32_t maxh;
 	
 	tab = (int32_t *) gr_alloc_temp((bm->h + 2) * sizeof(int32_t));
 	row = bm->row;
	add = 0L;
	maxh = bm->h;
	
	tab[0] = 0;
	for (i=0; i<maxh; i++)
	 {
	 	tab[i+1] = add;
	 	add += row;
	 }
	tab[maxh+1] = add - row;

 	return(tab + 1);
 }

// Free a table made by gr_make_vtab.
void gr_free_vtab (int32_t *vtab)
 {
 	gr_free_temp(vtab - 1);
 }
