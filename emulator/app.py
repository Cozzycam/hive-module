"""Main emulator loop.

Runs the display at a steady DISPLAY_FPS and advances the Coordinator
using a fixed-timestep accumulator driven by the current sim speed.
Workers are rendered at their interpolated sub-tick position so
movement looks smooth at low tick rates.

Phase 2 first cut: a 3x3 grid of module slots. The founding chamber
pins to the centre. Click any outer edge of an existing chamber to
attach a new empty chamber on that face (if the adjacent grid cell is
free). Workers crossing an active edge are handed off to the
neighbour on the same tick.
"""

import pygame

import config as C
from sim.coordinator import Coordinator
from emulator.module_panel import draw_chamber
from emulator.input_handler import InputHandler
from rendering import palette as P


HUD_HEIGHT = 52


def _compute_window_size():
    panel_w = int(C.CHAMBER_WIDTH_PX  * C.PANEL_SCALE)
    panel_h = int(C.CHAMBER_HEIGHT_PX * C.PANEL_SCALE)
    window_w = C.LAYOUT_COLS * panel_w + (C.LAYOUT_COLS - 1) * C.LAYOUT_GAP_PX
    window_h = (C.LAYOUT_ROWS * panel_h
                + (C.LAYOUT_ROWS - 1) * C.LAYOUT_GAP_PX
                + HUD_HEIGHT)
    return panel_w, panel_h, window_w, window_h


def run():
    pygame.init()
    panel_w, panel_h, window_w, window_h = _compute_window_size()
    screen = pygame.display.set_mode((window_w, window_h))
    pygame.display.set_caption("Hive Module — Colony")
    clock = pygame.time.Clock()
    font       = pygame.font.SysFont('consolas', 14)
    font_small = pygame.font.SysFont('consolas', 12)

    coord = Coordinator()
    inp   = InputHandler()

    # Native-size panel we render each chamber into, then scale + blit
    # to the screen at its layout position.
    native_panel = pygame.Surface((C.CHAMBER_WIDTH_PX, C.CHAMBER_HEIGHT_PX))

    # Fixed-timestep accumulator. `lerp_t` tracks the fractional
    # progress toward the next tick so workers render between grid
    # cells at low tick rates.
    tick_accum_ms = 0.0
    lerp_t        = 1.0

    clock.tick()   # prime; discard startup delta
    running = True
    while running:
        dt = min(clock.tick(C.DISPLAY_FPS), 200)

        # Build geometry dict for this frame so the input handler can
        # do hit-testing against the current layout.
        layout = coord.layout()
        panel_geom = {
            'scale':   C.PANEL_SCALE,
            'panel_w': panel_w,
            'panel_h': panel_h,
            'gap':     C.LAYOUT_GAP_PX,
            'layout':  layout,
        }

        inp.poll(coord, panel_geom)
        if inp.quit:
            running = False

        tps = inp.sim_ticks_per_sec
        if tps > 0:
            tick_interval = 1000.0 / tps
            tick_accum_ms += dt
            max_ticks = max(1, min(200, int(tps * 2 / C.DISPLAY_FPS) + 1))
            done = 0
            while tick_accum_ms >= tick_interval and done < max_ticks:
                coord.tick()
                tick_accum_ms -= tick_interval
                done += 1
            if tick_accum_ms > tick_interval:
                tick_accum_ms = tick_interval
            lerp_t = tick_accum_ms / tick_interval
        else:
            lerp_t = 1.0

        # ---- draw ----
        screen.fill(P.WALL)

        for module_id, (col, row) in layout.items():
            chamber = coord.chambers[module_id]
            # Paint the native panel for this chamber, then scale + blit.
            native_panel.fill(P.SOIL_DARK)
            draw_chamber(native_panel, chamber, lerp_t=lerp_t,
                         show_direction=inp.show_direction)
            scaled = pygame.transform.smoothscale(native_panel, (panel_w, panel_h))
            ox = col * (panel_w + C.LAYOUT_GAP_PX)
            oy = row * (panel_h + C.LAYOUT_GAP_PX)
            screen.blit(scaled, (ox, oy))

        _draw_hud(screen, font, font_small, coord, inp, window_w, window_h)

        pygame.display.flip()

    pygame.quit()


def _draw_hud(screen, font, font_small, coord, inp, window_w, window_h):
    hud_y = window_h - HUD_HEIGHT
    hud_rect = pygame.Rect(0, hud_y, window_w, HUD_HEIGHT)
    screen.fill(P.SOIL_DARK, rect=hud_rect)
    pygame.draw.line(screen, P.WALL, (0, hud_y), (window_w, hud_y), 2)

    colony   = coord.colony
    founding = coord.chambers.get('M0')
    queen    = founding.queen if founding else None
    brood    = coord.colony.brood_counts

    q_state  = 'alive' if (queen and queen.alive) else 'DEAD'
    q_hunger = queen.hunger if queen else 0.0
    sim_day  = coord.tick_count / C.TICKS_PER_SIM_DAY
    modules  = len(coord.chambers)

    line1 = (
        f"day {sim_day:>5.1f}   tick {coord.tick_count:>6}   "
        f"{inp.speed_label:>10}   food {colony.food_total:6.1f}   "
        f"queen {q_state} (h {q_hunger:4.1f})   modules {modules}"
    )
    line2 = (
        f"workers {colony.population:>3}   "
        f"eggs {brood['egg']:>2}  larvae {brood['larva']:>2}  "
        f"pupae {brood['pupa']:>2}   "
        f"[click] attach  [F] food  [D] dir  [space] pause  [+/-] speed  [q]"
    )
    screen.blit(font.render(line1, True, P.UI_TEXT), (8, hud_y + 4))
    screen.blit(font.render(line2, True, P.UI_TEXT), (8, hud_y + 22))

    if inp.last_status:
        screen.blit(font_small.render(inp.last_status, True, P.GLOW_AMBER),
                    (8, hud_y + 38))
