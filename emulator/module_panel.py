"""Renders one Chamber as a 480x320 panel.

Given a Chamber and a destination surface, paints the nest interior,
brood, queen, and workers using the shared sprite/palette data. All
coordinates are converted from grid cells to pixel space.
"""

import pygame

import config as C
from rendering import palette as P
from rendering import sprites as S
from sim import brood as brood_mod
from sim.lil_guy import TEND_BROOD, TEND_QUEEN, TO_HOME


# Pre-baked pygame Surfaces for each sprite, built lazily.
_sprite_cache = {}


def _surface_for(sprite_name, sprite):
    cached = _sprite_cache.get(sprite_name)
    if cached is not None:
        return cached
    w, h = S.size(sprite)
    surf = pygame.Surface((w, h), pygame.SRCALPHA)
    surf.fill((0, 0, 0, 0))
    for x, y, rgb in S.decode(sprite):
        surf.set_at((x, y), rgb)
    _sprite_cache[sprite_name] = surf
    return surf


def _cell_to_px(cx, cy):
    return cx * C.CELL_SIZE, cy * C.CELL_SIZE


def _blit_centered(dest, surf, cx, cy, origin_x=0, origin_y=0):
    px, py = _cell_to_px(cx, cy)
    sw, sh = surf.get_size()
    dest.blit(surf, (origin_x + px - sw // 2 + C.CELL_SIZE // 2,
                     origin_y + py - sh // 2 + C.CELL_SIZE // 2))


def _blit_centered_px(dest, surf, fx, fy, origin_x=0, origin_y=0):
    """Blit with float position (cell centers at cx+0.5, cy+0.5).
    No CELL_SIZE/2 offset — the +0.5 in float position already
    places the worker at the pixel center of its cell."""
    px = fx * C.CELL_SIZE
    py = fy * C.CELL_SIZE
    sw, sh = surf.get_size()
    dest.blit(surf, (origin_x + int(px - sw / 2),
                     origin_y + int(py - sh / 2)))


def draw_chamber(dest, chamber, origin_x=0, origin_y=0, lerp_t=1.0,
                 show_direction=False):
    """Paint the whole chamber onto `dest` at (origin_x, origin_y).

    `show_direction` is the D-key debug toggle — when True, cells with
    a strongly-directional pheromone vector get a small chevron drawn
    on top of the normal scent overlay, so you can eyeball the flow
    direction of a live trail.
    """
    # Background — soil gradient: darker at edges, warmer toward queen.
    w = C.CHAMBER_WIDTH_PX
    h = C.CHAMBER_HEIGHT_PX

    dest.fill(P.SOIL_DARK, rect=pygame.Rect(origin_x, origin_y, w, h))

    # Warm inner pool only for chambers that actually have a queen.
    # Empty worker chambers look cooler and distinguishably different.
    if chamber.queen is not None:
        qpx = chamber.queen.x * C.CELL_SIZE
        qpy = chamber.queen.y * C.CELL_SIZE
        for r, colour in (
            (120, P.SOIL_MID),
            (72,  P.SOIL_LIGHT),
            (40,  P.GLOW_WARM),
        ):
            rect = pygame.Rect(origin_x + qpx - r, origin_y + qpy - r, r * 2, r * 2)
            rect = rect.clip(pygame.Rect(origin_x, origin_y, w, h))
            dest.fill(colour, rect=rect)
    else:
        # Single subtle mid-soil pool in the middle of the chamber.
        cx, cy = w // 2, h // 2
        for r, colour in ((100, P.SOIL_MID), (50, P.SOIL_LIGHT)):
            rect = pygame.Rect(origin_x + cx - r, origin_y + cy - r, r * 2, r * 2)
            rect = rect.clip(pygame.Rect(origin_x, origin_y, w, h))
            dest.fill(colour, rect=rect)

    # Pheromone overlay (drawn before borders so the border frames
    # the scent). Only cells above SENSE_FLOOR get painted.
    _draw_pheromone_overlay(dest, chamber, origin_x, origin_y)

    # Optional direction overlay (D key). Drawn after the colour
    # overlay so the chevrons sit on top of the tint, before borders
    # and sprites.
    if show_direction:
        _draw_direction_overlay(dest, chamber, origin_x, origin_y)

    # Food piles
    _draw_food_piles(dest, chamber, origin_x, origin_y)

    # Chamber border
    pygame.draw.rect(
        dest, P.WALL,
        pygame.Rect(origin_x, origin_y, w, h),
        width=2,
    )

    # Entry markers — draw a fat bar across each face so they're
    # clearly clickable. Active faces glow amber, inactive faces use
    # a dim warm outline (no neighbour attached yet).
    _draw_edge_markers(dest, chamber, origin_x, origin_y, w, h)

    # Brood (drawn first so workers appear above them)
    egg_surf   = _surface_for('egg',   S.EGG)
    larva_surf = _surface_for('larva', S.LARVA)
    pupa_surf  = _surface_for('pupa',  S.PUPA)
    for b in chamber.brood:
        if not b.alive:
            continue
        if b.stage == brood_mod.EGG:
            _blit_centered(dest, egg_surf, b.x, b.y, origin_x, origin_y)
        elif b.stage == brood_mod.LARVA:
            _blit_centered(dest, larva_surf, b.x, b.y, origin_x, origin_y)
        elif b.stage == brood_mod.PUPA:
            _blit_centered(dest, pupa_surf, b.x, b.y, origin_x, origin_y)

    # Queen
    if chamber.queen is not None and chamber.queen.alive:
        queen_surf = _surface_for('queen', S.QUEEN)
        _blit_centered(dest, queen_surf, chamber.queen.x, chamber.queen.y,
                       origin_x, origin_y)

    # Workers — interpolated between prev and current grid position.
    # Sprite chosen by role and pioneer status.
    minor_surf   = _surface_for('minor',   S.WORKER_MINOR)
    pioneer_surf = _surface_for('pioneer', S.WORKER_PIONEER)
    major_surf   = _surface_for('major',   S.MAJOR)
    t = max(0.0, min(1.0, lerp_t))
    cell = C.CELL_SIZE
    for w in chamber.workers:
        if not w.alive:
            continue
        fx = w.prev_x + (w.x - w.prev_x) * t
        fy = w.prev_y + (w.y - w.prev_y) * t

        # Pick sprite: major > pioneer > minor
        if w.role == C.ROLE_MAJOR:
            surf = major_surf
        elif w.is_pioneer:
            surf = pioneer_surf
        else:
            surf = minor_surf
        _blit_centered_px(dest, surf, fx, fy, origin_x, origin_y)

        # Pixel position of this worker (for overlays)
        apx = origin_x + int(fx * cell)
        apy = origin_y + int(fy * cell)

        # Food morsel — bright dot when carrying food
        if w.food_carried > 0:
            # Offset morsel opposite to facing direction (carried on back)
            mx = apx - int(w.facing_dx * 2)
            my = apy - int(w.facing_dy * 2)
            pygame.draw.circle(dest, P.FOOD_CARRY, (mx, my), 2)

        # Feeding indicator — small food particle between worker and
        # target when adjacent and actively tending brood or queen.
        if w.state in (TEND_BROOD, TEND_QUEEN) and w.target is not None:
            tx, ty = w.target
            import math as _m
            wcx = int(_m.floor(w.x))
            wcy = int(_m.floor(w.y))
            if abs(tx - wcx) + abs(ty - wcy) <= 1:
                tpx = origin_x + tx * cell + cell // 2
                tpy = origin_y + ty * cell + cell // 2
                mid_x = (apx + tpx) // 2
                mid_y = (apy + tpy) // 2
                pygame.draw.circle(dest, P.FOOD_CARRY, (mid_x, mid_y), 1)


def _draw_pheromone_overlay(dest, chamber, origin_x, origin_y):
    """Paint semi-transparent tints over cells carrying enough scent
    to be worth visualising. Goes cell-by-cell over the grid; at
    2400 cells with almost all empty this is cheap.
    """
    from sim.pheromones import SENSE_FLOOR
    ph = chamber.pheromones
    cell = C.CELL_SIZE
    # Build one alpha-capable overlay surface per call. Cheaper than
    # set_alpha on individual rects.
    overlay = pygame.Surface((C.CHAMBER_WIDTH_PX, C.CHAMBER_HEIGHT_PX),
                             pygame.SRCALPHA)
    overlay.fill((0, 0, 0, 0))

    home_rgb = P.HOME_SCENT
    food_rgb = P.FOOD_SCENT
    for y in range(chamber.height):
        for x in range(chamber.width):
            h_val = ph.raw_home(x, y)
            f_val = ph.raw_food(x, y)
            if h_val < SENSE_FLOOR and f_val < SENSE_FLOOR:
                continue
            px = x * cell
            py = y * cell
            # Food trail dominates — paint it on top if present.
            if f_val >= SENSE_FLOOR:
                alpha = int(min(220, 60 + f_val * 40))
                pygame.draw.rect(overlay, (*food_rgb, alpha),
                                 pygame.Rect(px, py, cell, cell))
            elif h_val >= SENSE_FLOOR:
                alpha = int(min(160, 30 + h_val * 30))
                pygame.draw.rect(overlay, (*home_rgb, alpha),
                                 pygame.Rect(px, py, cell, cell))

    dest.blit(overlay, (origin_x, origin_y))


def _draw_direction_overlay(dest, chamber, origin_x, origin_y):
    """Debug overlay: draw a small chevron inside each cell that
    carries a strongly-directional pheromone vector.

    Filter: only cells with magnitude > 1.0 AND direction length
    > 0.5 get drawn — weaker cells are too noisy to be useful. Food
    flow is drawn in amber, home flow in blue, matching the colour
    overlay. Food is drawn second so it sits on top when both layers
    are active on the same cell (consistent with the scent overlay).
    """
    ph = chamber.pheromones
    cell = C.CELL_SIZE
    half = cell // 2
    # Arrow length — keep inside the cell with a little margin.
    arm  = max(2, half - 1)
    home_rgb = P.HOME_SCENT
    food_rgb = P.FOOD_SCENT

    for y in range(chamber.height):
        for x in range(chamber.width):
            # Home layer
            h_mag, h_dx, h_dy = ph.vector_home(x, y)
            if h_mag > 1.0:
                h_len = (h_dx * h_dx + h_dy * h_dy) ** 0.5
                if h_len > 0.5:
                    cx = origin_x + x * cell + half
                    cy = origin_y + y * cell + half
                    ex = cx + int(round((h_dx / h_len) * arm))
                    ey = cy + int(round((h_dy / h_len) * arm))
                    pygame.draw.line(dest, home_rgb, (cx, cy), (ex, ey), 1)
                    # Arrowhead dot at the tip so direction reads clearly
                    dest.set_at((ex, ey), home_rgb)

            # Food layer (drawn on top of home for the same cell)
            f_mag, f_dx, f_dy = ph.vector_food(x, y)
            if f_mag > 1.0:
                f_len = (f_dx * f_dx + f_dy * f_dy) ** 0.5
                if f_len > 0.5:
                    cx = origin_x + x * cell + half
                    cy = origin_y + y * cell + half
                    ex = cx + int(round((f_dx / f_len) * arm))
                    ey = cy + int(round((f_dy / f_len) * arm))
                    pygame.draw.line(dest, food_rgb, (cx, cy), (ex, ey), 1)
                    dest.set_at((ex, ey), food_rgb)


def _draw_food_piles(dest, chamber, origin_x, origin_y):
    """Draw each food pile as scattered seed-like shapes. Small piles
    are a single seed; larger piles add more granules spreading outward."""
    cell = C.CELL_SIZE
    half = cell // 2
    for (fx, fy), amount in chamber.food_cells.items():
        if amount <= 0:
            continue
        cx = origin_x + fx * cell + half
        cy = origin_y + fy * cell + half
        if amount <= 15:
            # Single seed — small elongated shape
            pygame.draw.ellipse(dest, P.FOOD_LIGHT,
                                (cx - 1, cy - 1, 3, 2))
        elif amount <= 50:
            # Small cluster — 2-3 seeds
            pygame.draw.ellipse(dest, P.FOOD_DARK,
                                (cx - 2, cy - 1, 3, 2))
            pygame.draw.ellipse(dest, P.FOOD_LIGHT,
                                (cx,     cy,     3, 2))
        elif amount <= 150:
            # Medium pile — scattered seeds
            pygame.draw.ellipse(dest, P.FOOD_DARK,
                                (cx - 3, cy - 2, 3, 2))
            pygame.draw.ellipse(dest, P.FOOD_LIGHT,
                                (cx - 1, cy,     4, 2))
            pygame.draw.ellipse(dest, P.FOOD_DARK,
                                (cx + 1, cy - 1, 3, 2))
            pygame.draw.ellipse(dest, P.FOOD_LIGHT,
                                (cx - 2, cy + 1, 3, 2))
        else:
            # Large pile — dense cluster
            pygame.draw.circle(dest, P.FOOD_DARK,  (cx, cy), 4)
            pygame.draw.ellipse(dest, P.FOOD_LIGHT,
                                (cx - 3, cy - 2, 4, 2))
            pygame.draw.ellipse(dest, P.FOOD_DARK,
                                (cx,     cy - 1, 3, 2))
            pygame.draw.ellipse(dest, P.FOOD_LIGHT,
                                (cx - 2, cy + 1, 4, 2))
            pygame.draw.ellipse(dest, P.FOOD_DARK,
                                (cx + 1, cy,     3, 2))
            pygame.draw.ellipse(dest, P.FOOD_LIGHT,
                                (cx - 1, cy - 3, 3, 2))


def _draw_edge_markers(dest, chamber, origin_x, origin_y, w, h):
    """Draw a fat bar across each face showing active/inactive state.

    Inactive faces get a dim warm outline that says "click here to
    attach a neighbour". Active faces become a solid amber bar with
    a gap in the middle representing the open passage.
    """
    bar_thick = 6                  # logical pixels
    bar_length = 96                # logical pixels (~12 cells)
    gap = 20                       # central passage gap on active faces

    faces = {
        'N': pygame.Rect(origin_x + (w - bar_length) // 2,
                         origin_y,
                         bar_length, bar_thick),
        'S': pygame.Rect(origin_x + (w - bar_length) // 2,
                         origin_y + h - bar_thick,
                         bar_length, bar_thick),
        'W': pygame.Rect(origin_x,
                         origin_y + (h - bar_length) // 2,
                         bar_thick, bar_length),
        'E': pygame.Rect(origin_x + w - bar_thick,
                         origin_y + (h - bar_length) // 2,
                         bar_thick, bar_length),
    }

    for face, rect in faces.items():
        active = chamber.entries.get(face) is not None
        if active:
            # Solid amber bar with a gap punched in the middle.
            pygame.draw.rect(dest, P.ENTRY_ACTIVE, rect)
            if face in ('N', 'S'):
                gap_rect = pygame.Rect(rect.centerx - gap // 2, rect.y,
                                       gap, rect.height)
            else:
                gap_rect = pygame.Rect(rect.x, rect.centery - gap // 2,
                                       rect.width, gap)
            pygame.draw.rect(dest, P.SOIL_DARK, gap_rect)
        else:
            # Dashed / dim outline — inactive but clickable.
            pygame.draw.rect(dest, P.ENTRY_IDLE, rect, width=1)
