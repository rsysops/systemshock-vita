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
#ifndef __CIT2D_H
#define __CIT2D_H

/*
 * $Source: r:/prj/cit/src/inc/RCS/cit2d.h $
 * $Revision: 1.4 $
 * $Author: mahk $
 * $Date: 1994/08/18 17:20:17 $
 *
 */

// Stuff that we wish the 2d had, but is too cool.

#define FONT_IS_MONO(fontptr) ((fontptr)->id != 0xCCCC)

void draw_shadowed_string(char *s, short x, short y, uchar shadow);

#define safe_set_cliprect(a, b, c, d) gr_safe_set_cliprect(a, b, c, d)

#endif // __CIT2D_H
