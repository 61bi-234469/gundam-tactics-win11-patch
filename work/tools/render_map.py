"""Render a battle map of gundam.exe.

Bg/BGnn.bmp is a 512x512 8bpp tileset of 32x32 tiles (16 per row). Bg/BGnn.MAP holds
60 columns x 45 rows of tile numbers stored column by column (index = row + col * 45;
high nibble = tile row, low nibble = tile column), as drawn by FUN_0040cbf0.
One tile covers 2x2 cells, so the battlefield is 120 x 90 cells = 1920 x 1440 px.
Units are drawn at screen x = (+0x02 second coord) * 16, screen y = (+0x00 first coord) * 16
(FUN_00402xxx drawing code), i.e. the first stored coordinate (0..89) is vertical.

usage: render_map.py BMP_NO MAP_NO OUT [SCALE]"""
import os, sys
from PIL import Image

GAME = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'source_exe_01')


def render(bmp_no, map_no):
    tiles = Image.open(os.path.join(GAME, 'Bg', f'Bg{bmp_no:02d}.bmp')).convert('RGB')
    cells = open(os.path.join(GAME, 'Bg', f'Bg{map_no:02d}.map'), 'rb').read()
    out = Image.new('RGB', (60 * 32, 45 * 32))
    for col in range(60):
        for row in range(45):
            b = cells[row + col * 45]
            tx, ty = (b & 0xF) * 32, (b >> 4) * 32
            out.paste(tiles.crop((tx, ty, tx + 32, ty + 32)), (col * 32, row * 32))
    return out


if __name__ == '__main__':
    bmp_no, map_no, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    scale = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
    im = render(bmp_no, map_no)
    if scale != 1.0:
        im = im.resize((round(im.width * scale), round(im.height * scale)), Image.LANCZOS)
    im.save(out)
