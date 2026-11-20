#ifndef __SHADELUT_H
#define __SHADELUT_H

// The smooth shadows' table of colours (docs/SHADOWS.md): for each of the
// light table's 16 rows and each of the 256 texels, the colour the screen
// shows a lit pixel with, red, green, blue and alpha. Row r is the colour at
// light level r + 0.5, the middle of the light table's row r; the screen's
// shader reads it filtered between two rows, so that the light between two
// rows' middles is a blend of the two. A texel's colour at a row is its
// colour at row 0 times the light table's brightness at that row. Plain C,
// for the PC tests too.

#define SHADE_ROWS 16
// The darkest a lit texel goes, as a share of its colour at row 0: dim rather
// than black, as the Enhanced Edition has it. The light table's last rows go
// to black.
#define SHADE_DARK_FLOOR 0.07f

// From the light table (16 rows of 256 palette indices) and the palette
// before its gamma (256 times red, green, blue, 0 to 255): the brightness of
// each row against row 0's, the median over the texels, never rising from a
// row to the next and never under SHADE_DARK_FLOOR; and which texels are the
// same index in every row (fullbright colours, and every texel of the
// infrared table), which keep their colour. Once per light table.
void shade_lut_ratios(float k[SHADE_ROWS], unsigned char constant[256], const unsigned char raw[768],
                      const unsigned char ltab[SHADE_ROWS * 256]);

// The table, 16 rows of 256 colours of 4 bytes, in `out`: every texel, or
// with `only` those it marks. `gamma` maps a channel to the screen's
// (Shock.c's gamma row); with `black255` index 255 is black, as the game
// shows it.
void shade_lut_fill(unsigned char out[SHADE_ROWS * 256 * 4], const float k[SHADE_ROWS],
                    const unsigned char constant[256], const unsigned char raw[768],
                    const unsigned char ltab[SHADE_ROWS * 256], const unsigned char gamma[256], int black255,
                    const unsigned char *only);

// Marks in `only` the texels whose colours come from a palette index marked
// in `changed` (their index at row 0), and returns how many: what
// shade_lut_fill has to make again after those palette entries changed.
int shade_lut_affected(unsigned char only[256], const unsigned char changed[256],
                       const unsigned char ltab[SHADE_ROWS * 256]);

#endif // __SHADELUT_H
