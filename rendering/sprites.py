"""Pixel art sprite data — blob-style for CELL_SIZE=16.

Each sprite is a list of equal-length strings. One character per pixel.
Map each character through SPRITE_KEY to a palette colour tuple.

Use '.' for transparent. Keeping sprites as plain data (not pygame
Surfaces) means this module can be imported on a microcontroller later
and the firmware can draw straight from the character grid into a
framebuffer.

Coordinates of drawn sprites are anchored on their *centre*, so even
dimensions still land cleanly on a grid cell.

Sprite canvases at CELL_SIZE=16 (30×20 grid on 480×320 display):
  Queen 44×44, Major 32×28, Minor 20×20, Nanitic 10×10,
  Egg 4×4, Larva 6×6, Pupa 8×8.
Pixel-doubled from hand-drawn 1× originals. Edit freely to add
detail — each sprite now has 2× the pixel budget per axis.
"""

from rendering import palette as P


SPRITE_KEY = {
    '.': P.TRANSPARENT,
    # queen
    'Q': P.QUEEN_BODY,
    'H': P.QUEEN_HEAD,
    'E': P.QUEEN_EYE,
    # minor workers
    'a': P.BODY,
    'l': P.LIMB,
    # pioneer workers (amber-brown founding brood)
    'n': P.PIONEER_BODY,
    'k': P.PIONEER_LIMB,
    # major workers (big-headed soldiers)
    'M': P.MAJOR_HEAD,
    'm': P.MAJOR_BODY,
    'e': P.QUEEN_EYE,     # reused — tiny dark eye
    # brood
    'o': P.EGG_COLOUR,
    'L': P.LARVA_COLOUR,
    'p': P.PUPA_COLOUR,
    # warm glow accents
    'g': P.GLOW_AMBER,
    'w': P.GLOW_WARM,
}


# ---- queen: 44×44, big mama blob with antennae and smile ----
QUEEN = [
    "..........HH....................HH..........",
    "..........HH....................HH..........",
    "............HH................HH............",
    "............HH................HH............",
    "................QQQQQQQQQQQQ................",
    "................QQQQQQQQQQQQ................",
    "............QQQQQQQQQQQQQQQQQQQQ............",
    "............QQQQQQQQQQQQQQQQQQQQ............",
    "..........QQQQQQQQQQQQQQQQQQQQQQQQ..........",
    "..........QQQQQQQQQQQQQQQQQQQQQQQQ..........",
    "........QQQQQQQQQQQQQQQQQQQQQQQQQQQQ........",
    "........QQQQQQQQQQQQQQQQQQQQQQQQQQQQ........",
    "......QQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQ......",
    "......QQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQ......",
    "....QQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQ....",
    "....QQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQ....",
    ".....QQQQQQQQooooQQQQQQQQQQooooQQQQQQQQ.....",
    ".....QQQQQQQQooooQQQQQQQQQQooooQQQQQQQQ.....",
    "....QQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQ....",
    "....QQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQ....",
    ".....QQQQQQQQQQQQQQQQwwwwQQQQQQQQQQQQQQ.....",
    ".....QQQQQQQQQQQQQQQQwwwwQQQQQQQQQQQQQQ.....",
    "..QQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQ..",
    "..QQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQ..",
    "..QQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQ..",
    "..QQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQ..",
    "...QQQQQQQQwwQQQQQQQQQQQQQQQQQQwwQQQQQQQQ...",
    "...QQQQQQQQwwQQQQQQQQQQQQQQQQQQwwQQQQQQQQ...",
    "....QQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQ....",
    "....QQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQQ....",
    ".......QQQQQQwwQQQQQQQQQQQQQQwwQQQQQQ.......",
    ".......QQQQQQwwQQQQQQQQQQQQQQwwQQQQQQ.......",
    "........QQQQQQQQQQQQQQQQQQQQQQQQQQQQ........",
    "........QQQQQQQQQQQQQQQQQQQQQQQQQQQQ........",
    "..........QQQQQQQQQQQQQQQQQQQQQQQQ..........",
    "..........QQQQQQQQQQQQQQQQQQQQQQQQ..........",
    "..............QQQQQQQQQQQQQQQQ..............",
    "..............QQQQQQQQQQQQQQQQ..............",
    "....................QQQQ....................",
    "....................QQQQ....................",
    "..................ll....ll..................",
    "..................ll....ll..................",
    "..................ll....ll..................",
    "..................ll....ll..................",
]

# ---- minor worker: 20×20, round blob with close-set dot eyes ----
WORKER_MINOR = [
    "......aaaaaaaa......",
    "......aaaaaaaa......",
    "....aaaaaaaaaaaa....",
    "....aaaaaaaaaaaa....",
    "..aaaaaaaaaaaaaaaa..",
    "..aaaaaaaaaaaaaaaa..",
    "..aaaaooaaaaoaaaaa..",
    "..aaaaooaaaaoaaaaa..",
    "..aaaaaaaaaaaaaaaa..",
    "..aaaaaaaaaaaaaaaa..",
    "....aaaaaaaaaaaa....",
    "....aaaaaaaaaaaa....",
    "....aaaaaaaaaaaa....",
    "....aaaaaaaaaaaa....",
    "......aaaaaaaa......",
    "......aaaaaaaa......",
    "......ll....ll......",
    "......ll....ll......",
    "......ll....ll......",
    "......ll....ll......",
]

# ---- pioneer worker: 10×10, tiny baby blob ----
WORKER_PIONEER = [
    "..nnnnnn..",
    "..nnnnnn..",
    "nnnnnnnnnn",
    "nnnnnnnnnn",
    "nnEEnnEEnn",
    "nnEEnnEEnn",
    "nnnnnnnnnn",
    "nnnnnnnnnn",
    "..kk..kk..",
    "..kk..kk..",
]

# Legacy alias
WORKER = WORKER_MINOR

# ---- major worker: 32×28, chunky blob with mandible pincers ----
MAJOR = [
    "..........MMMMMMMMMMMM..........",
    "..........MMMMMMMMMMMM..........",
    "......MMMMMMMMMMMMMMMMMMMM......",
    "......MMMMMMMMMMMMMMMMMMMM......",
    "MM..MMMMMMMMMMMMMMMMMMMMMMMM..MM",
    "MM..MMMMMMMMMMMMMMMMMMMMMMMM..MM",
    "MM..MMMMMMooooMMMMooooMMMMMM..MM",
    "MM..MMMMMMooooMMMMooooMMMMMM..MM",
    "....MMMMMMMMMMMMMMMMMMMMMMMM....",
    "....MMMMMMMMMMMMMMMMMMMMMMMM....",
    "......MMMMMMMMMMMMMMMMMMMM......",
    "......MMMMMMMMMMMMMMMMMMMM......",
    "..........mmmmmmmmmmmm..........",
    "..........mmmmmmmmmmmm..........",
    "........mmmmmmmmmmmmmmmm........",
    "........mmmmmmmmmmmmmmmm........",
    "........mmmmmmmmmmmmmmmm........",
    "........mmmmmmmmmmmmmmmm........",
    "..........mmmmmmmmmmmm..........",
    "..........mmmmmmmmmmmm..........",
    "............mmmmmmmm............",
    "............mmmmmmmm............",
    "............mmmmmmmm............",
    "............mmmmmmmm............",
    "..........mm........mm..........",
    "..........mm........mm..........",
    "..........mm........mm..........",
    "..........mm........mm..........",
]

# ---- egg: 4×4, tiny pale speck ----
EGG = [
    "oooo",
    "oooo",
    "oooo",
    "oooo",
]

# ---- larva: 6×6, small curled grub ----
LARVA = [
    "..LLLL",
    "..LLLL",
    "LLLLLL",
    "LLLLLL",
    "LLLL..",
    "LLLL..",
]

# ---- pupa: 8×8, smooth oval cocoon ----
PUPA = [
    "..pppp..",
    "..pppp..",
    "pppppppp",
    "pppppppp",
    "pppppppp",
    "pppppppp",
    "..pppp..",
    "..pppp..",
]


def decode(sprite):
    """Yield (dx, dy, rgb) for each non-transparent pixel of a sprite,
    anchored so (0,0) is the sprite's top-left."""
    for y, row in enumerate(sprite):
        for x, ch in enumerate(row):
            colour = SPRITE_KEY.get(ch, P.TRANSPARENT)
            if colour == P.TRANSPARENT:
                continue
            yield x, y, colour


def size(sprite):
    return len(sprite[0]), len(sprite)
