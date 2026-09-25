"""Render one sub-image of a Bina/*.BWH file: bwh_render.py FILE INDEX WIDTH OUT.png
BWH = big-endian u32 offset table followed by raw 8bpp pixels. The game UI palette is that of Bmp/Ball/Ball0.bmp (Bdec.bmp gives wrong colours)."""
import struct, sys
from PIL import Image
ROOT = __file__.rsplit('work', 1)[0] + 'source_exe_01/'
bmp = open(ROOT + 'Bmp/Ball/Ball0.bmp', 'rb').read()
hdr = struct.unpack_from('<I', bmp, 14)[0]
pal = [(bmp[14 + hdr + i*4 + 2], bmp[14 + hdr + i*4 + 1], bmp[14 + hdr + i*4]) for i in range(256)]
def render(path, idx, width):
    d = open(path, 'rb').read()
    first = struct.unpack_from('>I', d, 0)[0]
    offs = [struct.unpack_from('>I', d, i)[0] for i in range(0, first, 4)] + [len(d)]
    blk = d[offs[idx]:offs[idx+1]]
    h = len(blk) // width
    im = Image.new('RGB', (width, h))
    im.putdata([pal[b] for b in blk[:width*h]])
    return im
if __name__ == '__main__':
    f, i, w, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    im = render(f, i, w); im.resize((im.width*2, im.height*2), Image.NEAREST).save(out)
