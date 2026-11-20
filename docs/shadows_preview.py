#!/usr/bin/env python3
"""The smooth shadows' look, from a frame dump of the profile build (Checks on).

    python3 -I docs/shadows_preview.py docs/profiles-gpu/gpudumps/02

reads that dump's NN-info.txt, NN-gpu.raw, NN-side.raw, NN-ltab.raw and
NN-palette.raw (docs/PROFILING.md) and writes, next to them, pictures any
image viewer opens:

    NN-original.ppm  the GPU's picture as the screen shows it now
    NN-fitted.ppm    smooth shadows, the default way: the texel's colour at
                     row 0 times the table's brightness at the pixel's light
    NN-table.ppm     smooth shadows, the other way: the colours of the two
                     rows the light is between, blended
    NN-mask.ppm      which pixels get them: green lit and smooth, grey the
                     palette's colour, red marked lit with an index that
                     isn't the picture's (there should be none)

See docs/SHADOWS.md. The palette in a dump has the gamma in it already, so
the ratios here are measured on it; the game will measure them on the
palette before the gamma. Python 3 only, nothing to install.
"""

import re
import sys

SIDE_LIT = 0x5A
ROWS = 16
# what the darkest row gives in place of black, as a share of row 0
DARK_FLOOR = 0.07


def brightness(c):
    return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]


def median(values):
    values = sorted(values)
    return values[len(values) // 2] if values else 1.0


def row_ratios(ltab, pal):
    """Each row's brightness against row 0's, the median over the texels."""
    ratios = []
    for r in range(ROWS):
        found = []
        for t in range(1, 255):
            base = brightness(pal[ltab[t]])
            if base >= 16:
                found.append(brightness(pal[ltab[r * 256 + t]]) / base)
        ratios.append(median(found))
    # row 15 is black for nearly everything: carry on from rows 13 and 14
    ratios[ROWS - 1] = max(2 * ratios[ROWS - 2] - ratios[ROWS - 3], DARK_FLOOR)
    return ratios


def between(level):
    """The rows a level is between, and how far: a row's colour is at its middle."""
    at = min(max(level - 0.5, 0.0), ROWS - 1.0)
    low = int(at)
    high = min(low + 1, ROWS - 1)
    return low, high, at - low


def main():
    stem = sys.argv[1]
    info = dict(re.findall(r'(\w+)=(\d+)', open(stem + '-info.txt').read()))
    width, height = int(info['width']), int(info['height'])
    if info.get('side') != '1':
        sys.exit('%s: no side buffer in this dump' % stem)
    picture = open(stem + '-gpu.raw', 'rb').read()
    side = open(stem + '-side.raw', 'rb').read()
    ltab = open(stem + '-ltab.raw', 'rb').read()
    raw = open(stem + '-palette.raw', 'rb').read()
    pal = [tuple(raw[4 * i:4 * i + 3]) for i in range(256)]

    ratios = row_ratios(ltab, pal)
    constant = [all(ltab[r * 256 + t] == ltab[t] for r in range(ROWS)) for t in range(256)]
    print('row ratios:', ' '.join('%.3f' % k for k in ratios))

    def fitted(texel, level):
        base = pal[ltab[texel]]
        if constant[texel]:
            return base
        low, high, part = between(level)
        k = ratios[low] + (ratios[high] - ratios[low]) * part
        return tuple(min(255, int(round(c * k))) for c in base)

    def table(texel, level):
        low, high, part = between(level)
        a, b = pal[ltab[low * 256 + texel]], pal[ltab[high * 256 + texel]]
        return tuple(int(round(x + (y - x) * part)) for x, y in zip(a, b))

    outputs = {name: bytearray() for name in ('original', 'fitted', 'table', 'mask')}
    lit = wrong = 0
    for i in range(width * height):
        index = picture[i]
        red, texel, level16, mark = side[4 * i:4 * i + 4]
        colour = pal[index]
        outputs['original'] += bytes(colour)
        if mark == SIDE_LIT and red == index:
            lit += 1
            # the level in sixteenths, rounded down or to the nearest (since
            # step 2c, as the GPU's output rounds): half a sixteenth either way
            level = (level16 + 0.5) / 16.0
            outputs['fitted'] += bytes(fitted(texel, level))
            outputs['table'] += bytes(table(texel, level))
            outputs['mask'] += b'\x30\xc0\x30'
        else:
            outputs['fitted'] += bytes(colour)
            outputs['table'] += bytes(colour)
            if mark == SIDE_LIT:
                wrong += 1
                outputs['mask'] += b'\xff\x00\x00'
            else:
                grey = int(brightness(colour) * 0.5)
                outputs['mask'] += bytes((grey, grey, grey))

    for name, pixels in outputs.items():
        with open('%s-%s.ppm' % (stem, name), 'wb') as out:
            out.write(b'P6 %d %d 255\n' % (width, height))
            out.write(pixels)
    print('%d of %d pixels smooth, %d marked lit with another index' % (lit, width * height, wrong))


if __name__ == '__main__':
    main()
