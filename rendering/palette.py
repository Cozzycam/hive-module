"""Deep forest nest palette. Mossy greens on dark loam — same 16-ish
colour budget as the warm variant, same structural roles per slot so
the renderer doesn't need to know which theme is loaded.

All values are (R, G, B) tuples in the 0..255 range.
"""

# Transparent sentinel (never drawn). Sprite decoder treats this as a
# see-through pixel.
TRANSPARENT = (255, 0, 255)

# Background / chamber — dark loam and moss
SOIL_DARK    = (10, 20, 12)     # deepest nest interior
SOIL_MID     = (22, 42, 26)     # chamber floor
SOIL_LIGHT   = (38, 66, 42)     # near-entry soil, brood-warm
WALL         = (6, 12, 8)       # chamber border

# Cool highlights (moss / lichen glow)
GLOW_WARM    = (60, 120, 70)
GLOW_AMBER   = (110, 180, 90)

# Queen — deep emerald
QUEEN_BODY   = (28, 80, 44)
QUEEN_HEAD   = (44, 112, 60)
QUEEN_EYE    = (8, 14, 10)

# Workers — olive
ANT_BODY     = (32, 52, 28)
ANT_LIMB     = (20, 34, 18)

# Major workers (Pheidole/Messor soldiers) — lighter olive head capsule
# to read clearly against minor workers.
MAJOR_HEAD   = (66, 104, 48)
MAJOR_BODY   = (36, 60, 30)

# Brood — pale mint cream
EGG_COLOUR   = (220, 238, 210)
LARVA_COLOUR = (214, 232, 184)
PUPA_COLOUR  = (176, 200, 130)

# UI
UI_TEXT      = (200, 230, 200)
UI_DIM       = (108, 132, 108)
UI_ALERT     = (220, 96, 60)    # warning colour kept warm for contrast

# Entry indicators (edge markers)
ENTRY_IDLE   = (42, 70, 44)
ENTRY_ACTIVE = (100, 200, 80)

# Food piles (seeds / husks)
FOOD_LIGHT   = (200, 218, 100)
FOOD_DARK    = (110, 150, 50)

# Pheromone overlay — semi-transparent tints painted over cells with
# scent above a threshold. Home trail is a cool cyan-teal (ants
# remembering the way back); food trail is a warm yellow-gold
# (ants recruiting foragers).
HOME_SCENT   = (100, 200, 220)
FOOD_SCENT   = (240, 220, 100)
