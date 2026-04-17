"""Message types for inter-module communication.

In the PC emulator these are dicts passed between Chamber objects via the
Coordinator. On ESP32 hardware the same payloads will serialise onto
ESP-NOW frames. Keep the wire format small and dict-friendly.

Phase 1: only MODULE_ANNOUNCE is actually used (by a single-chamber
colony announcing itself to the coordinator). The other types are
defined now so Phase 2 can plug straight in.
"""

# ---------- message type tags ----------
HANDOFF          = 'handoff'
TOPOLOGY_UPDATE  = 'topology_update'
COLONY_STATE     = 'colony_state'
MODULE_ANNOUNCE  = 'module_announce'


def module_announce(module_id, kind, faces):
    """A module declaring itself and which of its edges are physically
    connectable (N/S/E/W). `kind` is 'chamber' | 'outworld' | 'tunnel'."""
    return {
        'type':      MODULE_ANNOUNCE,
        'module_id': module_id,
        'kind':      kind,
        'faces':     list(faces),
    }


def handoff(from_id, to_id, face, worker_state):
    """Worker crossing a module boundary. `face` is the edge it exits from
    on the source module; the receiver re-enters on the opposite face.
    `worker_state` is a dict snapshot: position, facing, task, cargo."""
    return {
        'type':      HANDOFF,
        'from':      from_id,
        'to':        to_id,
        'face':      face,
        'worker':    worker_state,
    }


def topology_update(graph):
    """Coordinator broadcast: current module adjacency graph.
    `graph` is a dict of module_id -> {face: neighbour_id}."""
    return {
        'type':  TOPOLOGY_UPDATE,
        'graph': graph,
    }


def colony_state(food, population, brood_counts):
    """Coordinator broadcast: summary of colony-wide state.
    `brood_counts` is {'egg': n, 'larva': n, 'pupa': n}."""
    return {
        'type':       COLONY_STATE,
        'food':       food,
        'population': population,
        'brood':      brood_counts,
    }
