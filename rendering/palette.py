"""Warm dark nest palette. 16-ish colours — comfortable on the ESP32
display and small enough to bake into firmware later as a lookup table.

All values are (R, G, B) tuples in the 0..255 range.
"""

# Transparent sentinel (never drawn). Sprite decoder treats this as a
# see-through pixel.
TRANSPARENT = (255, 0, 255)

# Background / chamber
SOIL_DARK    = (24, 18, 14)     # deepest nest interior
SOIL_MID     = (48, 34, 24)     # chamber floor
SOIL_LIGHT   = (74, 54, 38)     # near-entry soil, brood-warm
WALL         = (14, 10, 8)      # chamber border

# Warm highlights (candle / body-heat feel)
GLOW_WARM    = (120, 74, 40)
GLOW_AMBER   = (180, 110, 52)

# Queen
QUEEN_BODY   = (92, 40, 28)
QUEEN_HEAD   = (132, 60, 38)
QUEEN_EYE    = (12, 8, 6)

# Workers
ANT_BODY     = (48, 30, 22)
ANT_LIMB     = (30, 20, 14)

# Major workers (Pheidole/Messor soldiers) — distinct reddish-brown
# head capsule to read clearly against minor workers.
MAJOR_HEAD   = (104, 52, 30)
MAJOR_BODY   = (60, 36, 24)

# Brood
EGG_COLOUR   = (238, 228, 200)
LARVA_COLOUR = (232, 210, 168)
PUPA_COLOUR  = (200, 170, 120)

# UI
UI_TEXT      = (220, 210, 188)
UI_DIM       = (132, 118, 98)
UI_ALERT     = (220, 96, 60)

# Entry indicators (edge markers)
ENTRY_IDLE   = (70, 52, 38)
ENTRY_ACTIVE = (200, 140, 60)

# Food piles (seeds / husks)
FOOD_LIGHT   = (212, 168, 88)
FOOD_DARK    = (150, 100, 44)

# Pheromone overlay — semi-transparent tints painted over cells with
# scent above a threshold. Home trail is a cool blue (ants remembering
# the way back); food trail is a warm amber (ants recruiting foragers).
HOME_SCENT   = (80, 140, 200)
FOOD_SCENT   = (230, 170, 70)
