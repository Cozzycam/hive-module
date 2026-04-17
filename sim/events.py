"""Typed event bus — sim emits, renderer subscribes.

Events are ephemeral and advisory. The renderer missing one must not
break the world — it just skips an animation. The bus is also the
future seam for inter-module network transport.

Events are plain dicts with a 'type' tag and a 'tick' stamp.
No classes, no dataclasses — MicroPython-safe.
"""

import config as C

# ---- event type tags ----

INTERACTION_STARTED = 'interaction_started'
INTERACTION_ENDED   = 'interaction_ended'
FOOD_DELIVERED      = 'food_delivered'
FOOD_TAPPED         = 'food_tapped'
PILE_DISCOVERED     = 'pile_discovered'
QUEEN_LAID_EGG      = 'queen_laid_egg'
YOUNG_HATCHED       = 'young_hatched'
YOUNG_DIED          = 'young_died'
LIL_GUY_DIED        = 'lil_guy_died'
HANDOFF_INCOMING    = 'handoff_incoming'
HANDOFF_OUTGOING    = 'handoff_outgoing'

# ---- interaction subtypes ----

GREETING      = 'greeting'
FOOD_SHARING  = 'food_sharing'
TENDING_YOUNG = 'tending_young'
TENDING_QUEEN = 'tending_queen'


# ---- factory functions ----

def interaction_started(tick, pair_id, kind, duration_hint=0):
    return {'type': INTERACTION_STARTED, 'tick': tick,
            'pair_id': pair_id, 'kind': kind,
            'duration_hint': duration_hint}


def interaction_ended(tick, pair_id):
    return {'type': INTERACTION_ENDED, 'tick': tick,
            'pair_id': pair_id}


def food_delivered(tick, x, y, amount):
    return {'type': FOOD_DELIVERED, 'tick': tick,
            'x': x, 'y': y, 'amount': amount}


def food_tapped(tick, x, y):
    return {'type': FOOD_TAPPED, 'tick': tick, 'x': x, 'y': y}


def pile_discovered(tick, x, y, discoverer_id):
    return {'type': PILE_DISCOVERED, 'tick': tick,
            'x': x, 'y': y, 'discoverer_id': discoverer_id}


def queen_laid_egg(tick):
    return {'type': QUEEN_LAID_EGG, 'tick': tick}


def young_hatched(tick, stage_from, stage_to):
    return {'type': YOUNG_HATCHED, 'tick': tick,
            'stage_from': stage_from, 'stage_to': stage_to}


def young_died(tick):
    return {'type': YOUNG_DIED, 'tick': tick}


def lil_guy_died(tick):
    return {'type': LIL_GUY_DIED, 'tick': tick}


def handoff_outgoing(tick, lil_guy_data):
    return {'type': HANDOFF_OUTGOING, 'tick': tick,
            'lil_guy_data': lil_guy_data}


def handoff_incoming(tick, lil_guy_data):
    return {'type': HANDOFF_INCOMING, 'tick': tick,
            'lil_guy_data': lil_guy_data}


# ---- ring buffer ----

class EventBus:
    """Fixed-size ring buffer. Overwrites oldest when full.

    Sim owns the bus and calls emit(). Renderer calls drain() once per
    frame to collect all pending events in chronological order.
    Single-threaded — no locks needed.
    """
    __slots__ = ('_buf', '_capacity', '_write', '_count', '_next_pair_id')

    def __init__(self, capacity=None):
        if capacity is None:
            capacity = C.EVENT_BUS_CAPACITY
        self._buf = [None] * capacity
        self._capacity = capacity
        self._write = 0
        self._count = 0
        self._next_pair_id = 0

    def emit(self, event):
        """Write one event. Overwrites oldest if full."""
        self._buf[self._write] = event
        self._write = (self._write + 1) % self._capacity
        if self._count < self._capacity:
            self._count += 1

    def drain(self):
        """Return all pending events as a list and clear.
        Called once per frame by the renderer."""
        if self._count == 0:
            return []
        start = (self._write - self._count) % self._capacity
        result = []
        for i in range(self._count):
            idx = (start + i) % self._capacity
            result.append(self._buf[idx])
        self._count = 0
        return result

    def next_pair_id(self):
        """Monotonic ID for correlating interaction start/end pairs."""
        pid = self._next_pair_id
        self._next_pair_id += 1
        return pid
