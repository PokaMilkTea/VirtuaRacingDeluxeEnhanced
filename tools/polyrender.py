"""polyrender: redraws one tick's 3D polygons from a vrdetrace access log and compares
the result with the frame buffer the SH2s drew.

The polygon records are rebuilt from the writes of the geometry stage (code overlay A),
and drawn in the order the master SH2 reads them back (code overlay B), which is the
game's painter's-algorithm order. At scale 1 the SH2's own rasterization rules are used,
so the result can be compared pixel by pixel.

usage: python3 polyrender.py access_log frame framebuffer.bin palette.bin output.ppm [scale]
  access_log      output of: vrdetrace -c <frame-1> -a 06010000:06018000 -a 04000000:04040000
                  -d <frame+2> ..., running to the end of the tick (frame + 2)
  frame           the frame in which the geometry stage ran (the tick's second frame);
                  this frame and the next two (the rest of the tick) are replayed
  framebuffer.bin raw 32X frame buffer from vrdetrace -d at the end of the tick
                  (frame + 2), the one drawn that tick (it is on screen after that frame)
  palette.bin     32X palette from vrdetrace -d
  scale           1 (default): draws with the SH2's own rasterization rules and prints
                  the match against the frame buffer; 2, 4, ...: draws at a higher
                  resolution (pixel centres inside the polygon)

Coordinates in the records are relative to the screen centre (see SCREEN_CENTRE),
with y pointing up."""
import sys

# Code addresses (in the SH2 cache, where the overlays run) that this tool relies on.
# Code that writes finished polygon records (all in the same format):
RECORD_WRITERS = (
    (0xC00002B0, 0xC000030A),       # overlay A, the 3D geometry stage (both SH2s)
    (0x060031E6, 0x06003214),       # master, SDRAM code: other polygons, e.g. HUD parts
)
DRAW_ORDER_READ = 0xC0000292        # overlay B: first read of each record, in drawing order

POLYGON_RECORDS_START = 0x06010000  # master from 06010000, slave from 06013000,
POLYGON_RECORDS_END = 0x06018000    # the other polygons from 06016000
CACHE_THROUGH_BIT = 0x20000000

SCREEN_WIDTH, SCREEN_HEIGHT = 320, 224
SCREEN_CENTRE = (160, 112)


def signed16(value):
    return value - 0x10000 if value & 0x8000 else value


FRAMEBUFFER_START = 0x04000000
FRAMEBUFFER_END = 0x04040000         # including the overwrite area (+20000)
FRAMEBUFFER_PIXELS = 0x4000          # first pixel's offset; each line is 512 bytes


class AccessLog:
    """The polygons of one tick in drawing order, replayed from the log in time order:
    writes of the geometry stage update the record memory, and each time overlay B reads
    a record (DRAW_ORDER_READ) the record is taken as it is at that moment. A tick can
    take longer than its three frames to draw, so the frames after it are replayed too,
    and records may be read after their addresses were reused."""

    def __init__(self, path, frame):
        self.memory = {}
        self.polygons = []            # (colour word, [(x, y), ...]) in drawing order
        self.master_pixels = set()    # pixels the master wrote this tick: the HUD
        for line in open(path):
            if not line.startswith('A '):
                continue
            fields = line.split()
            log_frame, cpu = int(fields[1]), fields[3]
            pc, kind = int(fields[4], 16), fields[5]
            address = int(fields[6], 16) & ~CACHE_THROUGH_BIT
            value = int(fields[7], 16)
            if not frame <= log_frame <= frame + 2:
                continue
            if FRAMEBUFFER_START <= address < FRAMEBUFFER_END:
                if kind[0] == 'W' and cpu == 'msh2':
                    self.note_master_pixels(address, int(kind[1]))
                continue
            if not POLYGON_RECORDS_START <= address < POLYGON_RECORDS_END:
                continue
            is_record_writer = any(first <= pc <= last for first, last in RECORD_WRITERS)
            if kind[0] == 'W' and is_record_writer:
                self.store(address, value, int(kind[1]))
            if kind[0] == 'R' and cpu == 'msh2' and pc == DRAW_ORDER_READ:
                record_start = address - 2    # the reader starts at the colour word, +2
                self.polygons.append(self.read_record(record_start))

    def note_master_pixels(self, address, size):
        """The master draws the HUD straight into the frame buffer; those pixels are
        left out of the comparison."""
        offset = (address & 0x1FFFF) - FRAMEBUFFER_PIXELS
        for i in range(size):
            y, x = divmod(offset + i, 512)
            if 0 <= y < SCREEN_HEIGHT and x < SCREEN_WIDTH:
                self.master_pixels.add(y * SCREEN_WIDTH + x)

    def store(self, address, value, size):
        for i in range(size):
            shift = 8 * (size - 1 - i)
            self.memory[address + i] = (value >> shift) & 0xFF

    def word(self, address):
        return (self.memory.get(address, 0) << 8) | self.memory.get(address + 1, 0)

    def read_record(self, record):
        """Record: +0 word (unused here), +2 colour word (high byte = palette index,
        bit 0 = triangle, bit 1 = dithered), then one (x, y) word pair per vertex,
        relative to the screen centre with y pointing up."""
        colour_word = self.word(record + 2)
        vertex_count = 3 if colour_word & 1 else 4
        vertices = []
        for i in range(vertex_count):
            x = signed16(self.word(record + 4 + 4 * i))
            y = signed16(self.word(record + 6 + 4 * i))
            # y points up in the records, down on screen.
            vertices.append((SCREEN_CENTRE[0] + x, SCREEN_CENTRE[1] - y))
        return colour_word, vertices


def fill_polygon(image, width, height, vertices, scale, value):
    """Fills a convex polygon: every pixel whose centre lies inside."""
    points = [(x * scale, y * scale) for x, y in vertices]
    top = max(0, int(min(y for _, y in points)))
    bottom = min(height - 1, int(max(y for _, y in points)))
    for row in range(top, bottom + 1):
        centre_y = row + 0.5
        crossings = []
        for (x0, y0), (x1, y1) in zip(points, points[1:] + points[:1]):
            if (y0 <= centre_y < y1) or (y1 <= centre_y < y0):
                crossings.append(x0 + (centre_y - y0) * (x1 - x0) / (y1 - y0))
        if len(crossings) < 2:
            continue
        left = max(0, int(min(crossings) + 0.5))
        right = min(width, int(max(crossings) + 0.5))
        for column in range(left, right):
            image[row * width + column] = value


# ---- The SH2's own rasterization (native resolution only) ----
#
# Overlay B (master) turns each polygon record into two vertex chains, left and right,
# from the top vertex to the bottom one, clipped to the 3D view; overlay C (slave) walks
# both chains with a fixed-point DDA and fills one span per line. The integer arithmetic
# below follows the SH2 code so that the same pixels come out.

VIEW_LEFT, VIEW_RIGHT = 0, 319          # overlay B's clipping window (context +1C, +20)
VIEW_TOP, VIEW_BOTTOM = 16, 207         # (context +14, +18)


def to_signed16(value):
    value &= 0xFFFF
    return value - 0x10000 if value & 0x8000 else value


def to_signed32(value):
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value & 0x80000000 else value


def reciprocal(d):
    """The game's 1/d table (SDRAM 060048D0, indexed by d from -255 to 255):
    0x4000 / d rounded toward zero, and 32767 for d = 0"""
    if d == 0:
        return 32767
    quotient = abs(0x4000) // abs(d)
    return quotient if d > 0 else -quotient


def edge_step(x, next_x, y, next_y):
    """Per-line change of x along an edge, 16.16 fixed point: MULS.W of the 16-bit
    x difference and 1/(y difference), shifted left by 2"""
    product = to_signed16(x - next_x) * to_signed16(reciprocal(y - next_y))
    return to_signed32(product << 2)


def interpolate_at_y(vertex1, vertex2, target_y):
    """Overlay B C00003F6: the vertex where the edge vertex1-vertex2 crosses target_y"""
    (x1, y1), (x2, y2) = vertex1, vertex2
    if not y2 >= y1:
        (x1, y1), (x2, y2) = (x2, y2), (x1, y1)
    dx = x1 - x2
    dy = y1 - y2                        # <= 0
    if dy < -255:
        # Too tall for the table: the SH2's hardware divider, rounding toward zero.
        numerator = to_signed16(target_y - y1) * to_signed16(dx)
        quotient = abs(numerator) // abs(dy)
        if (numerator < 0) != (dy < 0):
            quotient = -quotient
        new_x = x1 + quotient
    else:
        slope = to_signed32((to_signed16(dx) * to_signed16(reciprocal(dy))) << 2)
        offset = to_signed32((target_y - y1) << 16)
        new_x = x1 + ((slope * offset) >> 32)
    return to_signed16(new_x), target_y


def chain_unclipped(vertices, start, end, step):
    """Overlay B C0000372: the vertices from start to end, walking by step"""
    chain = []
    index = start
    while True:
        chain.append(vertices[index])
        if index == end:
            return chain
        index = (index + step) % 4


def chain_clipped_vertically(vertices, start, end, step):
    """Overlay B C0000392: like chain_unclipped, but edges crossing the top or bottom
    of the view end at a new vertex there."""
    chain = []
    index = start
    current = vertices[index]
    if current[1] >= VIEW_TOP:
        chain.append(current)
        index = (index + step) % 4
    else:
        while True:
            previous = current
            index = (index + step) % 4
            current = vertices[index]
            if current[1] >= VIEW_TOP:
                break
        chain.append(interpolate_at_y(current, previous, VIEW_TOP))
        # The interpolation (C00003F6) leaves the upper vertex in the register that the
        # loop below takes as the previous one, so an edge that also crosses the bottom
        # is interpolated along the same edge.
        current = previous
    while True:
        previous = current
        current = vertices[index]
        if current[1] > VIEW_BOTTOM:
            chain.append(interpolate_at_y(current, previous, VIEW_BOTTOM))
            return chain
        chain.append(current)
        if index == end:
            return chain
        index = (index + step) % 4


def top_and_bottom(vertices):
    """Overlay B C0000320: indices of the first highest and first lowest vertex"""
    top = bottom = 0
    for index in range(1, 4):
        y = vertices[index][1]
        if y < vertices[top][1]:
            top = index
        elif y > vertices[bottom][1]:
            bottom = index
    return top, bottom


LEFT_OF_VIEW, RIGHT_OF_VIEW = 4, 2       # overlay B's outcodes for x


def horizontal_outcode(x):
    """Overlay B C00004B6"""
    code = 0
    if x < VIEW_LEFT:
        code |= LEFT_OF_VIEW
    if x > VIEW_RIGHT:
        code |= RIGHT_OF_VIEW
    return code


def cross_vertical_border(previous, current, code):
    """Overlay B C00004CC: where the edge crosses x = 0 or x = 319. It is the same
    interpolation as for y, with x and y swapped."""
    border_x = VIEW_RIGHT if code == RIGHT_OF_VIEW else VIEW_LEFT
    y, x = interpolate_at_y((previous[1], previous[0]), (current[1], current[0]), border_x)
    return x, y


def clip_chain_horizontally(chain):
    """Overlay B C0000470: (clipped chain, outcode of its first vertex, outcode of its last)"""
    clipped = []
    first_code = horizontal_outcode(chain[0][0])
    if first_code == 0:
        clipped.append(chain[0])
    previous_code = first_code
    for previous, current in zip(chain, chain[1:]):
        current_code = horizontal_outcode(current[0])
        both_outside_same_side = previous_code & current_code
        if not both_outside_same_side:
            if previous_code:
                clipped.append(cross_vertical_border(previous, current, previous_code))
            if current_code:
                clipped.append(cross_vertical_border(previous, current, current_code))
            if current_code == 0:
                clipped.append(current)
        previous_code = current_code
    return clipped, first_code, previous_code


def corners_at(y):
    return (VIEW_LEFT, y), (VIEW_RIGHT, y)


def join_clipped_chains(left, right, left_code, right_code, y, at_top):
    """Overlay B C00004F6: after horizontal clipping the two chains can start (or end)
    at different points on the view's border. A corner of the view, or the other
    chain's end point, is added so that both chains start (end) at the same line.
    Returns (vertices to add to the left chain, vertices to add to the right chain)."""
    left_corner, right_corner = corners_at(y)
    corner_for = {LEFT_OF_VIEW: left_corner, RIGHT_OF_VIEW: right_corner}
    if left_code == right_code:
        if left_code == 0:
            return [], []
        # Both outside on the same side: copy the end point of the chain that reaches
        # further (up at the top, down at the bottom) to the other chain.
        left_end = (left[0] if at_top else left[-1]) if left else None
        right_end = (right[0] if at_top else right[-1]) if right else None
        if at_top:
            if left_end is None:
                return [right_end] if right_end else [], []
            if right_end is None or right_end[1] > left_end[1]:
                return [], [left_end]
            return [right_end], []
        if right_end is not None and left_end is not None and right_end[1] > left_end[1]:
            return [right_end], []
        return [], [left_end] if left_end else []
    left_add = [corner_for[left_code]] if left_code else []
    right_add = [corner_for[right_code]] if right_code else []
    return left_add, right_add


def vertex_chains(vertices):
    """Overlay B's per-polygon decision: (left chain, right chain), or None if nothing
    is drawn. "Left" and "right" are the walking directions; overlay C decides which
    one is really on the left."""
    xs = [x for x, _ in vertices]
    ys = [y for _, y in vertices]
    needs_horizontal_clip = min(xs) < VIEW_LEFT or max(xs) > VIEW_RIGHT
    needs_vertical_clip = min(ys) < VIEW_TOP or max(ys) > VIEW_BOTTOM
    if needs_vertical_clip or needs_horizontal_clip:
        if not max(ys) > VIEW_TOP or min(ys) > VIEW_BOTTOM:
            return None
    top, bottom = top_and_bottom(vertices)
    if vertices[top][1] == vertices[bottom][1]:
        return None
    if not (needs_vertical_clip or needs_horizontal_clip):
        return chain_unclipped(vertices, top, bottom, 1), chain_unclipped(vertices, top, bottom, -1)

    first = chain_clipped_vertically(vertices, top, bottom, 1)
    second = chain_clipped_vertically(vertices, top, bottom, -1)
    if not needs_horizontal_clip:
        return first, second

    first, first_top_code, first_bottom_code = clip_chain_horizontally(first)
    second, second_top_code, second_bottom_code = clip_chain_horizontally(second)
    add_first, add_second = join_clipped_chains(first, second, first_top_code, second_top_code, VIEW_TOP, True)
    first = add_first + first
    second = add_second + second
    add_first, add_second = join_clipped_chains(first, second, first_bottom_code, second_bottom_code, VIEW_BOTTOM, False)
    return first + add_first, second + add_second


def span_pixels(left, right):
    """Overlay C C0000188: the pixels of one span, both ends included. Odd left and
    even right ends are single byte writes, the pairs between come from the 32X VDP's
    fill. A span with right < left still writes those single bytes."""
    if left == right:
        return [left]
    pixels = []
    if left & 1:
        pixels.append(left)
        left += 1
    if not right & 1:
        pixels.append(right)
    count = right - left + 1
    if count >= 2:
        pixels += range(left, left + 2 * (count >> 1))
    return pixels


def dithered_span_pixels(left, right):
    """Overlay C C00001F4: a dithered span, written from right to left: the right end
    alone if even, then pairs, then the left end alone if odd. The same pixels as
    span_pixels unless right < left, where the pixels written lie further left."""
    if left == right:
        return [left]
    pixels = []
    end, last = right + 1, right
    if not right & 1:
        pixels.append(right)
        end, last = right, right - 1
    count = last - left + 1
    if count >= 2:
        for _ in range(count >> 1):
            end -= 2
            pixels += [end, end + 1]
    if left & 1:
        pixels.append(end - 1)
    return pixels


class EdgeWalker:
    """One vertex chain, walked down one line at a time (overlay C)"""

    def __init__(self, chain):
        self.chain = chain + [(-1, -1)]          # the packet's end marker
        self.position = 0

    def next_edge(self, check_end_x):
        """Moves to the next edge that goes down. Returns False at the end of the chain.
        Sets x (16.16), step and lines. The left chain also stops at a negative x."""
        while True:
            x, y = self.chain[self.position]
            self.position += 1
            if check_end_x and x < 0:
                return False
            next_x, next_y = self.chain[self.position]
            if next_y == y:
                continue                          # horizontal edge: skip
            if y - next_y > 0:
                return False                      # going up: the chain is over
            self.x = x << 16
            self.step = edge_step(x, next_x, y, next_y)
            self.lines = next_y - y
            self.start_y = y
            return True

    def next_left_edge(self):
        """Overlay C C00000C4-C0000124, the left chain's reload. Unlike next_edge, every
        step first checks whether the following vertex is the end marker; if so the
        chain is finished and one more span is drawn. Returns 'edge', 'finish' or 'stop'."""
        while True:
            x, y = self.chain[self.position]
            self.position += 1
            next_x, next_y = self.chain[self.position]
            if next_y < 0:
                return 'finish'
            if next_y == y:
                continue                          # horizontal edge: skip
            if y - next_y > 0:
                return 'stop'
            self.x = x << 16                      # no +0.5 here
            self.step = edge_step(x, next_x, y, next_y)
            self.lines = next_y - y
            return 'edge'


def rasterize(chains, colour_word):
    """Overlay C: [(y, [x, ...]), ...] for one polygon, plus whether it is dithered"""
    first, second = (EdgeWalker(chain) for chain in chains)
    if not first.next_edge(check_end_x=True) or not second.next_edge(check_end_x=True):
        return [], False
    line = first.start_y
    first.x |= 0x8000                             # +0.5, only for the two first edges
    second.x |= 0x8000
    left, right = first, second
    keep_order = second.x > first.x or (second.x == first.x and second.step > first.step)
    if not keep_order:
        left, right = second, first

    dithered = bool(colour_word & 2)
    span_of = dithered_span_pixels if dithered else span_pixels
    spans = []
    while True:
        spans.append((line, span_of(left.x >> 16, right.x >> 16)))
        line += 1
        left.lines -= 1
        left.x = to_signed32(left.x + left.step)
        if left.lines == 0:
            result = left.next_left_edge()
            if result == 'finish':
                # End of the left chain: one more span on the next line, then stop.
                right.x = to_signed32(right.x + right.step)
                spans.append((line, span_of(left.x >> 16, right.x >> 16)))
                return spans, dithered
            if result == 'stop':
                return spans, dithered
        right.lines -= 1
        right.x = to_signed32(right.x + right.step)
        if right.lines == 0:
            if not right.next_edge(check_end_x=False):
                return spans, dithered


def draw_polygon_sh2(image, vertices, colour_word):
    """Draws one polygon the way the SH2s do, into a native-resolution index image"""
    if len(vertices) == 3:
        vertices = vertices + [vertices[2]]       # triangles repeat their last vertex
    vertices = [(to_signed16(x), to_signed16(y)) for x, y in vertices]
    chains = vertex_chains(vertices)
    if chains is None:
        return
    spans, dithered = rasterize(chains, colour_word)
    palette_index = colour_word >> 8
    for y, pixels in spans:
        if not VIEW_TOP <= y <= VIEW_BOTTOM:
            continue
        # Dithered polygons write only every other pixel, alternating per line
        # (to the frame buffer's overwrite area, so the rest stays as it was).
        for x in pixels:
            if not VIEW_LEFT <= x <= VIEW_RIGHT:
                continue
            if dithered and (x + y) % 2 == 1:
                continue
            image[y * SCREEN_WIDTH + x] = palette_index


def read_framebuffer_indices(path):
    """Palette index of every visible pixel of a raw 32X frame buffer (packed pixel mode)."""
    data = open(path, 'rb').read()
    indices = bytearray(SCREEN_WIDTH * SCREEN_HEIGHT)
    for row in range(SCREEN_HEIGHT):
        line_start_word = (data[row * 2] << 8) | data[row * 2 + 1]
        start = line_start_word * 2
        indices[row * SCREEN_WIDTH:(row + 1) * SCREEN_WIDTH] = data[start:start + SCREEN_WIDTH]
    return indices


def read_palette(path):
    """256 BGR555 colours as (r, g, b) bytes."""
    data = open(path, 'rb').read()
    colours = []
    for i in range(256):
        colour = (data[2 * i] << 8) | data[2 * i + 1]
        colours.append(((colour & 0x1F) << 3, ((colour >> 5) & 0x1F) << 3, ((colour >> 10) & 0x1F) << 3))
    return colours


def save_ppm(path, indices, width, height, palette):
    with open(path, 'wb') as file:
        file.write(b'P6\n%d %d\n255\n' % (width, height))
        file.write(bytes(channel for index in indices for channel in palette[index]))


# In the frame buffer, index 1 (black) fills the rows above and below the 3D view, which
# do not come from the polygon list.
NOT_FROM_POLYGONS = 1


def compare(rendered, framebuffer, master_pixels):
    """Share of pixels with the same palette index in the 3D area: pixels that either
    image drew (index 0 is the empty background), minus the frame buffer's index-1
    pixels (borders) and the pixels the master wrote (HUD)."""
    drawn = same = 0
    for pixel, (ours, theirs) in enumerate(zip(rendered, framebuffer)):
        if ours == 0 and theirs == 0:
            continue
        if theirs == NOT_FROM_POLYGONS or pixel in master_pixels:
            continue
        drawn += 1
        if ours == theirs:
            same += 1
    return same, drawn


def main():
    if len(sys.argv) not in (6, 7):
        print(__doc__)
        sys.exit(1)
    log_path, frame, framebuffer_path, palette_path, output_path = sys.argv[1:6]
    scale = int(sys.argv[6]) if len(sys.argv) == 7 else 1

    log = AccessLog(log_path, int(frame))
    palette = read_palette(palette_path)
    width, height = SCREEN_WIDTH * scale, SCREEN_HEIGHT * scale
    image = bytearray(width * height)
    for colour_word, vertices in log.polygons:
        if scale == 1:
            draw_polygon_sh2(image, vertices, colour_word)
        else:
            fill_polygon(image, width, height, vertices, scale, colour_word >> 8)
    save_ppm(output_path, image, width, height, palette)
    print(f'{len(log.polygons)} polygons drawn at {scale}x into {output_path}')

    if scale == 1:
        same, drawn = compare(image, read_framebuffer_indices(framebuffer_path), log.master_pixels)
        print(f'3D area match: {same}/{drawn} pixels ({100 * same / drawn:.1f}%)')


if __name__ == '__main__':
    main()
