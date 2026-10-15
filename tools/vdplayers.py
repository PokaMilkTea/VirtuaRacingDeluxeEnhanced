#!/usr/bin/env python3
"""Split a 32X frame into its layers: the Mega Drive VDP's plane B, plane A (with the
window) and sprites, and the 32X frame buffer, then composite them the way PicoDrive does.
Adapted from the MD version's tool of the same name.

usage: vdplayers.py VDP.bin DRAM.bin PAL.bin [SHOT.ppm] [OUTPREFIX]
  VDP.bin   vrdetrace -v of the frame
  DRAM.bin  the 32X frame buffer on screen during that frame (vrdetrace -d, dram_*_N.bin;
            packed pixel or run length mode;
            vrdetrace -c prints which one, "S <frame> shown N")
  PAL.bin   the 32X palette (vrdetrace -d, pal32x_*.bin)
  SHOT.ppm  vrdetrace -w of the same frame: the composite is compared line by line;
            mismatching lines point at mid-frame (raster) changes the end-of-frame dump misses
  OUTPREFIX writes PREFIX_{b,a,s,32x,composite,source}.ppm, transparent pixels in magenta;
            the source map colours each pixel by the layer it comes from (see SOURCE_COLOURS)
Pick a frame in the middle of a tick, so the frame buffers do not swap during it.
Output is ROM-derived: keep it in out/.
"""
import struct
import sys

MAGENTA = (255, 0, 255)


def load(path):
    """(MD VDP registers, VRAM, CRAM, VSRAM, 32X VDP registers) from a vrdetrace -v dump"""
    d = open(path, 'rb').read()
    assert d[:4] == b'VDPX', 'not a vrdetrace -v dump'
    reg = d[4:0x24]
    vram = d[0x24:0x10024]
    cram = struct.unpack('>64H', d[0x10024:0x100A4])
    vsram = struct.unpack('>64H', d[0x100A4:0x10124])
    vdp32x = struct.unpack('>16H', d[0x10124:0x10144])
    return reg, vram, cram, vsram, vdp32x


def rgb565_to_rgb(colour):
    """RGB565 -> RGB the way vrdetrace's PPM writer does it"""
    return (colour >> 11) << 3, ((colour >> 5) & 0x3F) << 2, (colour & 0x1F) << 3


def md_rgb565(cram_word):
    """A CRAM word (0000BBB0GGG0RRR0) as PicoDrive draws it (pico/draw.c: PXCONV, then
    the two corrections in PicoDoHighPal555 for the MD's non-linear output)"""
    t = ((cram_word & 0x000E) << 12) | ((cram_word & 0x00E0) << 3) | ((cram_word & 0x0E00) >> 7)
    low_bits = 0x0841                     # the lowest bit of red, green and blue
    t |= (t >> 4) & low_bits
    t |= ((t ^ low_bits) & ((t >> 3) | (t >> 2)) & low_bits) << 1
    return t


def rgb(cram_word):
    return rgb565_to_rgb(md_rgb565(cram_word))


def x32_rgb565(colour, invert_priority):
    """A 32X palette colour (BGR555, bit 15 = priority) as PicoDrive draws it
    (pico/32x/draw.c: convert_pal555). The priority bit ends up as the lowest green bit,
    so it shows in the output colour too."""
    if invert_priority:
        colour ^= 0x8000
    red, green, blue = colour & 0x1F, (colour >> 5) & 0x1F, (colour >> 10) & 0x1F
    priority = colour >> 15
    return (red << 11) | (green << 6) | (priority << 5) | blue


X32_PRIORITY = 0x0020                 # where the priority bit lands in x32_rgb565's result


class VDP:
    """The VDP state of a dump and its mode 5 rendering, one pixel at a time"""

    def __init__(self, reg, vram, cram, vsram, vdp32x):
        self.reg, self.vram, self.cram, self.vsram = reg, vram, cram, vsram
        self.vdp32x = vdp32x
        self.h40 = bool(reg[12] & 0x81)
        self.width = 320 if self.h40 else 256
        self.height = 240 if reg[1] & 0x08 else 224
        sizes = (32, 64, 32, 128)                                 # plane size codes, in cells
        self.plane_width_cells = sizes[reg[16] & 3]
        self.plane_height_cells = sizes[(reg[16] >> 4) & 3]
        self.plane_a_table = (reg[2] & 0x38) << 10
        self.plane_b_table = (reg[4] & 0x07) << 13
        self.window_table = (reg[3] & (0x3C if self.h40 else 0x3E)) << 10
        self.sprite_table = (reg[5] & (0x7E if self.h40 else 0x7F)) << 9
        self.hscroll_table = (reg[13] & 0x3F) << 10

    def word(self, address):
        return (self.vram[address & 0xFFFF] << 8) | self.vram[(address + 1) & 0xFFFF]

    def tile_pixel(self, entry, x, y):
        """(palette index 0-63, or 0 if transparent; priority) for pixel x, y inside the cell
        that a name table entry names"""
        if entry & 0x0800:                                        # horizontal flip
            x = 7 - x
        if entry & 0x1000:                                        # vertical flip
            y = 7 - y
        tile_byte = self.vram[((entry & 0x7FF) * 32 + y * 4 + x // 2) & 0xFFFF]
        colour = (tile_byte >> 4) if x % 2 == 0 else (tile_byte & 15)
        palette = (entry >> 13) & 3
        index = (palette << 4 | colour) if colour else 0
        return index, bool(entry & 0x8000)

    def hscroll(self, y):
        """(plane A, plane B) horizontal scroll of line y, by the scroll mode in reg 11"""
        mode = self.reg[11] & 3
        if mode == 0:
            line = 0                    # whole screen
        elif mode == 2:
            line = y & ~7               # per cell row
        elif mode == 3:
            line = y                    # per line
        else:
            line = y & 7                # mode 1: the first 8 entries repeat
        address = self.hscroll_table + line * 4
        return self.word(address) & 0x3FF, self.word(address + 2) & 0x3FF

    def vscroll(self, x):
        """(plane A, plane B) vertical scroll at screen x: whole plane, or per 16-pixel column"""
        column = (x // 16) * 2 if self.reg[11] & 4 else 0
        return self.vsram[column] & 0x7FF, self.vsram[column + 1] & 0x7FF

    def in_window(self, x, y):
        """Whether screen x, y is in the window: whole lines above or below a row (reg 18),
        otherwise the columns left or right of a position (reg 17)"""
        horizontal, vertical = self.reg[17], self.reg[18]
        row_line = (vertical & 0x1F) * 8
        below = vertical & 0x80
        if (below and y >= row_line) or (not below and y < row_line):
            return True
        column_x = (horizontal & 0x1F) * 16
        right = horizontal & 0x80
        return x >= column_x if right else x < column_x

    def plane(self, which, x, y):
        """(index, priority, source) of plane 'A' (or the window, source 'W') or 'B' at x, y"""
        if which == 'A' and self.in_window(x, y):
            window_columns = 64 if self.h40 else 32
            entry = self.word(self.window_table + ((y // 8) * window_columns + x // 8) * 2)
            return self.tile_pixel(entry, x % 8, y % 8) + ('W',)
        plane_index = 0 if which == 'A' else 1
        hscroll = self.hscroll(y)[plane_index]
        vscroll = self.vscroll(x)[plane_index]
        plane_x = (x - hscroll) % (self.plane_width_cells * 8)
        plane_y = (y + vscroll) % (self.plane_height_cells * 8)
        table = self.plane_a_table if which == 'A' else self.plane_b_table
        entry = self.word(table + ((plane_y // 8) * self.plane_width_cells + plane_x // 8) * 2)
        return self.tile_pixel(entry, plane_x % 8, plane_y % 8) + (which,)

    def sprite_order(self):
        """Sprite numbers in drawing priority, following the link field from sprite 0"""
        limit = 80 if self.h40 else 64
        order, number = [], 0
        while len(order) < limit:
            order.append(number)
            number = self.vram[self.sprite_table + number * 8 + 3] & 0x7F
            if number == 0 or number >= limit:
                break
        return order

    def sprites(self):
        """Per pixel (index, priority, sprite number) of the topmost opaque sprite pixel, or None"""
        out = [[None] * self.width for _ in range(self.height)]
        sprites_on_line = [0] * self.height
        max_per_line = 20 if self.h40 else 16
        for number in self.sprite_order():
            entry = self.sprite_table + number * 8
            top = (self.word(entry) & 0x3FF) - 128
            cells_across = ((self.vram[entry + 2] >> 2) & 3) + 1
            cells_down = (self.vram[entry + 2] & 3) + 1
            attributes = self.word(entry + 4)
            left = (self.word(entry + 6) & 0x1FF) - 128
            for sprite_y in range(cells_down * 8):
                y = top + sprite_y
                if not 0 <= y < self.height:
                    continue
                sprites_on_line[y] += 1
                if sprites_on_line[y] > max_per_line:
                    continue
                for sprite_x in range(cells_across * 8):
                    x = left + sprite_x
                    if not 0 <= x < self.width or out[y][x] is not None:
                        continue
                    # the cell's tile: tiles run down each column of cells; flips mirror the cells
                    cell_x, cell_y = sprite_x // 8, sprite_y // 8
                    if attributes & 0x0800:
                        cell_x = cells_across - 1 - cell_x
                    if attributes & 0x1000:
                        cell_y = cells_down - 1 - cell_y
                    tile = ((attributes & 0x7FF) + cell_x * cells_down + cell_y) & 0x7FF
                    index, priority = self.tile_pixel((attributes & 0xF800) | tile, sprite_x % 8, sprite_y % 8)
                    if index:
                        out[y][x] = (index, priority, number)
        return out


def write_ppm(path, w, h, pixels):
    with open(path, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (w, h))
        f.write(bytes(c for p in pixels for c in p))


def read_ppm(path):
    data = open(path, 'rb').read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, [tuple(parts[3][i:i + 3]) for i in range(0, w * h * 3, 3)]


def composite_index(sprite, plane_a, plane_b, backdrop):
    """The palette index shown, by priority: sprites high > A high > B high > sprites low >
    A low > B low > backdrop. sprite is None or (index, priority, number); planes (index, priority, ...)."""
    sprite_index = sprite[0] if sprite else 0
    sprite_high = bool(sprite and sprite[1])
    candidates = (
        (sprite_index, sprite_high),
        (plane_a[0], plane_a[1]),
        (plane_b[0], plane_b[1]),
        (sprite_index, bool(sprite) and not sprite_high),
        (plane_a[0], not plane_a[1]),
        (plane_b[0], not plane_b[1]),
    )
    for index, applies in candidates:
        if index and applies:
            return index
    return backdrop


PACKED_PIXEL, RUN_LENGTH = 1, 3      # 32X display modes (VDP register 0, bits 0-1)


class Framebuffer32X:
    """The 32X frame buffer on screen, as a palette index per pixel"""

    def __init__(self, dram_path, palette_path, vdp):
        self.dram = open(dram_path, 'rb').read()
        palette_data = open(palette_path, 'rb').read()
        self.palette = struct.unpack('>256H', palette_data[:512])
        self.mode = vdp.vdp32x[0] & 3
        self.invert_priority = bool(vdp.vdp32x[0] & 0x80)
        assert self.mode in (PACKED_PIXEL, RUN_LENGTH), 'unsupported 32X mode %d' % self.mode
        self.lines = {}

    def line_start(self, y):
        """The buffer starts with one word per line: where the line starts, in words"""
        return ((self.dram[y * 2] << 8) | self.dram[y * 2 + 1]) * 2

    def line(self, y):
        """The 320 palette indices of line y"""
        if y not in self.lines:
            start = self.line_start(y)
            if self.mode == PACKED_PIXEL:
                indices = [self.dram[(start + x) & 0x1FFFF] for x in range(320)]   # a byte per pixel
            else:
                indices = []                                   # words: run length - 1, index
                position = start
                while len(indices) < 320:
                    run = self.dram[position] + 1
                    indices += [self.dram[position + 1]] * run
                    position = (position + 2) & 0x1FFFF       # word addresses wrap at 128 KB
                indices = indices[:320]
            self.lines[y] = indices
        return self.lines[y]

    def index(self, x, y):
        return self.line(y)[x]

    def rgb565(self, x, y):
        return x32_rgb565(self.palette[self.index(x, y)], self.invert_priority)


# Colours for the source map
SOURCE_COLOURS = {
    '32X': (255, 255, 255),
    'A': (255, 0, 0),
    'B': (0, 0, 255),
    'S': (0, 255, 0),
    'W': (255, 0, 255),
    'backdrop': (0, 0, 0),
}


def md_pixel(vdp, sprites, x, y):
    """(CRAM index shown by the MD VDP, which layer it comes from)"""
    sprite = sprites[y][x]
    plane_a = vdp.plane('A', x, y)
    plane_b = vdp.plane('B', x, y)
    index = composite_index(sprite, plane_a, plane_b, 0)
    if index == 0:
        return vdp.reg[7] & 0x3F, 'backdrop'
    if sprite and sprite[0] == index and (sprite[1] or not (plane_a[0] and plane_a[1]) and not (plane_b[0] and plane_b[1])):
        return index, 'S'
    if plane_a[0] == index:
        return index, plane_a[2]
    return index, 'B'


def composite_pixel(vdp, framebuffer, sprites, x, y):
    """(RGB565 shown, layer) by PicoDrive's rule (pico/32x/draw.c, do_line_pp): the 32X
    pixel shows where the MD pixel is the background colour, or where its palette colour
    has the priority bit; otherwise the MD pixel shows."""
    md_index, md_layer = md_pixel(vdp, sprites, x, y)
    x32_colour = framebuffer.rgb565(x, y)
    md_is_background = md_index == (vdp.reg[7] & 0x3F)
    if md_is_background or x32_colour & X32_PRIORITY:
        return x32_colour, '32X'
    return md_rgb565(vdp.cram[md_index]), md_layer


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)
    vdp = VDP(*load(sys.argv[1]))
    framebuffer = Framebuffer32X(sys.argv[2], sys.argv[3], vdp)
    shot = read_ppm(sys.argv[4]) if len(sys.argv) > 4 else None
    prefix = sys.argv[5] if len(sys.argv) > 5 else None
    reg = vdp.reg
    print('MD: mode %s %dx%d  display %s  shadow/highlight %s  bg colour %d' % (
        'H40' if vdp.h40 else 'H32', vdp.width, vdp.height, 'on' if reg[1] & 0x40 else 'off',
        bool(reg[12] & 8), reg[7] & 0x3F))
    print('plane %dx%d cells  A @%04x  B @%04x  window @%04x (H %02x V %02x)  sprites @%04x  hscroll @%04x' % (
        vdp.plane_width_cells, vdp.plane_height_cells, vdp.plane_a_table, vdp.plane_b_table,
        vdp.window_table, reg[17], reg[18], vdp.sprite_table, vdp.hscroll_table))
    print('hscroll mode %d  vscroll %s  vsram A %d B %d' % (
        reg[11] & 3, '2-cell' if reg[11] & 4 else 'full', vdp.vsram[0], vdp.vsram[1]))
    print('32X: mode %d  priority %s' % (framebuffer.mode, 'inverted' if framebuffer.invert_priority else 'normal'))

    sprites = vdp.sprites()
    composite, sources = [], []
    for y in range(vdp.height):
        for x in range(vdp.width):
            colour, source = composite_pixel(vdp, framebuffer, sprites, x, y)
            composite.append(rgb565_to_rgb(colour))
            sources.append(source)

    if shot:
        shot_w, shot_h, shot_pixels = shot
        differing = [y for y in range(min(vdp.height, shot_h))
                     if composite[y * vdp.width:(y + 1) * vdp.width] != shot_pixels[y * shot_w:(y + 1) * shot_w]]
        detail = ''
        if differing:
            detail = '; differing lines %d-%d (%d lines)' % (differing[0], differing[-1], len(differing))
        print('composite vs screenshot: %d / %d lines identical%s' % (vdp.height - len(differing), vdp.height, detail))

    for layer in ('32X', 'A', 'W', 'B', 'S', 'backdrop'):
        count = sources.count(layer)
        if count:
            print('shown from %-8s %6d pixels' % (layer + ':', count))

    if prefix:
        def plane_image(which):
            pixels = []
            for y in range(vdp.height):
                for x in range(vdp.width):
                    index = vdp.plane(which, x, y)[0]
                    pixels.append(rgb(vdp.cram[index]) if index else MAGENTA)
            return pixels

        sprite_pixels = [rgb(vdp.cram[p[0]]) if p else MAGENTA for row in sprites for p in row]
        x32_pixels = []
        for y in range(vdp.height):
            for x in range(vdp.width):
                index = framebuffer.index(x, y)
                x32_pixels.append(rgb565_to_rgb(framebuffer.rgb565(x, y)) if index else MAGENTA)

        write_ppm(prefix + '_b.ppm', vdp.width, vdp.height, plane_image('B'))
        write_ppm(prefix + '_a.ppm', vdp.width, vdp.height, plane_image('A'))
        write_ppm(prefix + '_s.ppm', vdp.width, vdp.height, sprite_pixels)
        write_ppm(prefix + '_32x.ppm', vdp.width, vdp.height, x32_pixels)
        write_ppm(prefix + '_composite.ppm', vdp.width, vdp.height, composite)
        write_ppm(prefix + '_source.ppm', vdp.width, vdp.height, [SOURCE_COLOURS[s] for s in sources])


if __name__ == '__main__':
    main()
