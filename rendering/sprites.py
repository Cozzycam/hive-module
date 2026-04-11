"""Pixel art sprite data.

Each sprite is a list of equal-length strings. One character per pixel.
Map each character through SPRITE_KEY to a palette colour tuple.

Use '.' for transparent. Keeping sprites as plain data (not pygame
Surfaces) means this module can be imported on a microcontroller later
and the firmware can draw straight from the character grid into a
framebuffer.

Coordinates of drawn sprites are anchored on their *centre*, so even
dimensions still land cleanly on a grid cell.
"""

from rendering import palette as P


SPRITE_KEY = {
    '.': P.TRANSPARENT,
    # queen
    'Q': P.QUEEN_BODY,
    'H': P.QUEEN_HEAD,
    'E': P.QUEEN_EYE,
    # minor workers
    'a': P.ANT_BODY,
    'l': P.ANT_LIMB,
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


# ---- queen: 12x12, large Messor-scale founding queen ----
QUEEN = [
    "....HHHH....",
    "...HEEEEH...",
    "...HHHHHH...",
    "....HHHH....",
    "...QQQQQQ...",
    "..QQwQQwQQ..",
    ".QQQQQQQQQQ.",
    "QQQQwQQwQQQQ",
    "QQQQQQQQQQQQ",
    ".QQwQQQQwQQ.",
    "..QQQQQQQQ..",
    "...QQQQQQ...",
]

# ---- minor worker (nanitic): 6x6, compact ----
WORKER_MINOR = [
    "..aa..",
    ".aaaa.",
    "laaaal",
    "laaaal",
    ".aaaa.",
    "..ll..",
]

# Legacy alias — old code called this WORKER.
WORKER = WORKER_MINOR

# ---- major worker: 8x7, oversized head capsule + mandibles ----
MAJOR = [
    "..MMMM..",
    ".MMMMMM.",
    "MMeMMeMM",
    ".MMMMMM.",
    "..mmmm..",
    ".mmmmmm.",
    "..l..l..",
]

# ---- egg: 3x3 ----
EGG = [
    ".o.",
    "ooo",
    ".o.",
]

# ---- larva: 4x3, curled grub ----
LARVA = [
    ".LL.",
    "LLLL",
    ".LL.",
]

# ---- pupa: 5x5, cocooned ----
PUPA = [
    ".ppp.",
    "ppppp",
    "ppppp",
    "ppppp",
    ".ppp.",
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
