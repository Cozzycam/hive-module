"""Keyboard / mouse input handling for the emulator.

Phase 2 first cut: pause, speed, quit — and click-to-attach. A left
click within EDGE_CLICK_DEPTH pixels of any chamber's inactive face
calls `coordinator.announce_module()` to spawn a neighbour there.

The app hands the current panel geometry (scale, gap, layout) into
`poll()` so the handler can translate window coordinates back into
panel-local coordinates.
"""

import pygame

import config as C


class InputHandler:
    def __init__(self):
        self.paused         = False
        self.speed_index    = C.DEFAULT_SPEED_INDEX
        self.quit           = False
        self.last_status    = ''     # transient HUD message for clicks
        self.mouse_pos      = (0, 0) # updated each poll() from pygame
        self.last_added     = None   # track newest attached module for F fallback
        # Debug overlay: when True, the renderer draws a small chevron
        # inside cells that carry a strongly-directional pheromone
        # vector, to eyeball whether the field is flowing where we
        # expect it to. Toggled with the D key.
        self.show_direction = False

    @property
    def sim_ticks_per_sec(self):
        """How many sim ticks should elapse per real second at the
        current speed. Zero while paused."""
        if self.paused:
            return 0
        return C.SIM_SPEED_LEVELS[self.speed_index]

    @property
    def speed_label(self):
        if self.paused:
            return 'PAUSED'
        return f'{C.SIM_SPEED_LEVELS[self.speed_index]} tps'

    def poll(self, coordinator, panel_geom):
        """Handle all pending events.

        panel_geom is a dict built by the app each frame:
            {
              'scale':      float,     # PANEL_SCALE
              'panel_w':    int,       # scaled panel width in window pixels
              'panel_h':    int,       # scaled panel height
              'gap':        int,       # gap between panels
              'layout':     {id: (col, row)},
            }
        """
        self.mouse_pos = pygame.mouse.get_pos()
        for ev in pygame.event.get():
            if ev.type == pygame.QUIT:
                self.quit = True
            elif ev.type == pygame.KEYDOWN:
                self._on_key(ev.key, coordinator, panel_geom)
            elif ev.type == pygame.MOUSEBUTTONDOWN:
                self._on_click(ev.pos, ev.button, coordinator, panel_geom)

    def _on_key(self, key, coordinator, panel_geom):
        if key in (pygame.K_ESCAPE, pygame.K_q):
            self.quit = True
        elif key == pygame.K_SPACE:
            self.paused = not self.paused
        elif key in (pygame.K_EQUALS, pygame.K_PLUS, pygame.K_KP_PLUS):
            self.speed_index = min(len(C.SIM_SPEED_LEVELS) - 1, self.speed_index + 1)
        elif key in (pygame.K_MINUS, pygame.K_KP_MINUS):
            self.speed_index = max(0, self.speed_index - 1)
        elif key == pygame.K_f:
            self._debug_spawn_food(coordinator, panel_geom)
        elif key == pygame.K_d:
            self.show_direction = not self.show_direction
            self.last_status = ('direction overlay ON'
                                if self.show_direction
                                else 'direction overlay off')

    def _debug_spawn_food(self, coordinator, panel_geom):
        """Drop a food pile at the current mouse position. If the
        cursor is outside any chamber, fall back to the most recently
        attached chamber at its centre."""
        hit = self._hit_test_chamber_cell(self.mouse_pos, coordinator, panel_geom)
        if hit is not None:
            module_id, cx, cy = hit
        else:
            # Fallback — most recently attached non-queen chamber,
            # if any, else the founding chamber.
            module_id = self.last_added or self._last_non_queen(coordinator)
            if module_id is None:
                self.last_status = "no non-queen chamber to drop food in"
                return
            cx, cy = C.GRID_WIDTH // 2, C.GRID_HEIGHT // 2
        chamber = coordinator.chambers[module_id]
        chamber.add_food(cx, cy, C.FORAGE_DEBUG_PILE_SIZE)
        self.last_status = (f"dropped {C.FORAGE_DEBUG_PILE_SIZE:.0f} food "
                            f"at ({cx},{cy}) in {module_id}")

    def _last_non_queen(self, coordinator):
        for mid, ch in reversed(list(coordinator.chambers.items())):
            if ch.queen is None:
                return mid
        return None

    def _hit_test_chamber_cell(self, pos, coordinator, panel_geom):
        """Return (module_id, grid_x, grid_y) of the chamber cell the
        click/cursor is over, or None if it's outside any chamber."""
        wx, wy = pos
        scale   = panel_geom['scale']
        panel_w = panel_geom['panel_w']
        panel_h = panel_geom['panel_h']
        gap     = panel_geom['gap']
        layout  = panel_geom['layout']
        for module_id, (col, row) in layout.items():
            ox = col * (panel_w + gap)
            oy = row * (panel_h + gap)
            if not (ox <= wx < ox + panel_w and oy <= wy < oy + panel_h):
                continue
            lx = (wx - ox) / scale
            ly = (wy - oy) / scale
            gx = int(lx // C.CELL_SIZE)
            gy = int(ly // C.CELL_SIZE)
            if 0 <= gx < C.GRID_WIDTH and 0 <= gy < C.GRID_HEIGHT:
                return (module_id, gx, gy)
        return None

    def _on_click(self, pos, button, coordinator, panel_geom):
        if button != 1:
            return
        hit = self._hit_test_edge(pos, coordinator, panel_geom)
        if hit is None:
            return
        module_id, face = hit
        ok, result = coordinator.announce_module(attach_to=module_id, face=face)
        if ok:
            self.last_status = f"attached {result} on {face} of {module_id}"
            self.last_added = result
        else:
            self.last_status = f"can't attach: {result}"

    def _hit_test_edge(self, pos, coordinator, panel_geom):
        """Return (module_id, face) if the click falls near an inactive
        face of any existing chamber. Otherwise None."""
        wx, wy      = pos
        scale       = panel_geom['scale']
        panel_w     = panel_geom['panel_w']
        panel_h     = panel_geom['panel_h']
        gap         = panel_geom['gap']
        layout      = panel_geom['layout']

        # Native chamber dims (for edge-depth comparison in panel space)
        native_w = C.CHAMBER_WIDTH_PX
        native_h = C.CHAMBER_HEIGHT_PX
        depth    = C.EDGE_CLICK_DEPTH

        for module_id, (col, row) in layout.items():
            ox = col * (panel_w + gap)
            oy = row * (panel_h + gap)
            if not (ox <= wx < ox + panel_w and oy <= wy < oy + panel_h):
                continue

            # Convert window coords -> panel-native coords
            lx = (wx - ox) / scale
            ly = (wy - oy) / scale

            # Work out which edge (if any) the click landed near
            face = None
            if ly < depth:
                face = 'N'
            elif ly >= native_h - depth:
                face = 'S'
            elif lx < depth:
                face = 'W'
            elif lx >= native_w - depth:
                face = 'E'

            if face is None:
                return None

            chamber = coordinator.chambers[module_id]
            if chamber.entries.get(face) is not None:
                # Face already has a neighbour — don't overwrite it.
                return None
            return (module_id, face)
        return None
