/* Coordinator — orchestrates chambers and colony-wide state. */
#include "coordinator.h"
#include "topology.h"
#include "transport.h"
#include "time_of_day.h"
#include "rng.h"
#include <cmath>

#ifdef ARDUINO
#include <Preferences.h>
#endif

static const char* role_str(ModuleRole r) {
    switch (r) {
        case MODULE_QUEEN:     return "queen";
        case MODULE_SATELLITE: return "satellite";
        default:               return "unconfigured";
    }
}

static const char* face_letter(int f) {
    static const char* F[] = {"N","S","W","E"};
    return (f >= 0 && f < FACE_COUNT) ? F[f] : "?";
}

void Coordinator::init() {
#ifdef ARDUINO
    // Read persisted role from NVS
    Preferences prefs;
    prefs.begin("hive", true);  // read-only
    role = static_cast<ModuleRole>(prefs.getUChar("module_role", MODULE_UNCONFIGURED));
    prefs.end();

    if (role == MODULE_UNCONFIGURED) {
        Serial.println("[coord] module unconfigured -- falling back to queen mode for development");
    } else {
        Serial.printf("[coord] module role: %s\n", role_str(role));
    }
#else
    role = MODULE_UNCONFIGURED;  // desktop builds default to queen
#endif

    bool queen = is_queen();
    colony = ColonyState();
    chamber.init(&colony, queen);  // sets food_total for queen
}

void Coordinator::set_role_nvs(ModuleRole r) {
#ifdef ARDUINO
    Preferences prefs;
    prefs.begin("hive", false);
    prefs.putUChar("module_role", static_cast<uint8_t>(r));
    prefs.end();
    Serial.printf("[coord] role written to NVS: %s\n", role_str(r));
#else
    (void)r;
#endif
}

void Coordinator::tick(float dt, EventBus& bus, uint32_t tick_num) {
    // Wire transient per-tick state
    chamber.event_bus = &bus;
    chamber.tick_num  = tick_num;

    // Sync topology neighbour state into chamber entries
    _sync_topology_to_chamber();

    // ---- Centralized food drain (real-time) ----
    float burn_this_tick = colony.daily_burn() / Cfg::SECS_PER_DAY * dt;

    // During founding, queen eats from reserves
    if (chamber.has_queen &&
        !chamber.queen_obj.founding_done && chamber.queen_obj.reserves > 0) {
        float queen_portion = Cfg::QUEEN_FOOD_PER_DAY / Cfg::SECS_PER_DAY * dt;
        float from_reserves = fminf(queen_portion, chamber.queen_obj.reserves);
        chamber.queen_obj.reserves -= from_reserves;
        burn_this_tick -= from_reserves;
        if (burn_this_tick < 0) burn_this_tick = 0;
    }

    colony.food_store = fmaxf(0.0f, colony.food_store - burn_this_tick);

    // ---- Credit larva food investment ----
    if (colony.food_store > 0.0f ||
        (chamber.has_queen &&
         !chamber.queen_obj.founding_done && chamber.queen_obj.reserves > 0)) {
        float per_larva = Cfg::LARVA_FOOD_PER_DAY / Cfg::SECS_PER_DAY * dt;
        for (int i = 0; i < chamber.brood_count; i++) {
            if (chamber.brood[i].stage == STAGE_LARVA && chamber.brood[i].alive()) {
                chamber.brood[i].food_invested += per_larva;
            }
        }
    }

    // ---- Receive incoming handoffs (workers arriving from other modules) ----
    _receive_handoffs(bus, tick_num);

    // ---- Apply neighbour boundary pheromones (before chamber tick so workers sense them) ----
    _apply_boundary_pheromones();

    // ---- Chamber tick (movement, behavior, lifecycle events) ----
    chamber.tick(dt);

    // ---- Edge crossing detection + outgoing handoff ----
    _check_edge_crossings(bus, tick_num);

    // ---- Send our boundary pheromones to neighbours ----
    _send_boundary_pheromones(tick_num);

    // ---- Satellite: broadcast local population to neighbours ----
    _broadcast_population();

    // ---- Queen: broadcast colony state (tod + stats) to satellites ----
    _broadcast_state();

    // ---- Aggregate colony stats ----
    _aggregate_colony_stats();
}

void Coordinator::_aggregate_colony_stats() {
    // Local population
    uint16_t local_pop = chamber.lil_guy_count;

    // Queen: add remote satellite populations
    uint16_t remote_pop = 0;
#ifdef ARDUINO
    if (is_queen()) {
        for (int f = 0; f < FACE_COUNT; f++) {
            remote_pop += topology_remote_population(static_cast<Face>(f));
        }
    }
#endif
    colony.population = local_pop + remote_pop;

    int gatherers = 0;
    for (int i = 0; i < chamber.lil_guy_count; i++) {
        auto& w = chamber.lil_guys[i];
        if (w.state == STATE_TO_FOOD
                || (w.state == STATE_TO_HOME && w.food_carried > 0))
            gatherers++;
    }
    colony.gatherer_count = gatherers;

    uint16_t eggs, larvae, pupae;
    chamber.count_brood(eggs, larvae, pupae);
    colony.brood_egg   = eggs;
    colony.brood_larva = larvae;
    colony.brood_pupa  = pupae;

    float queen_reserves = 0.0f;
    if (chamber.has_queen && chamber.queen_obj.alive)
        queen_reserves = chamber.queen_obj.reserves;
    colony.food_total = colony.food_store + queen_reserves;

    colony.update_recovery_boost();
}

void Coordinator::_broadcast_population() {
#ifdef ARDUINO
    if (is_queen()) return;  // only satellites broadcast

    uint32_t now = millis();
    if (now - _last_pop_broadcast_ms < 1000) return;
    _last_pop_broadcast_ms = now;

    PopSyncMessage msg;
    msg.msg_type   = TOPO_POP_SYNC;
    msg.sender_id  = topology_my_id();
    msg.population = chamber.lil_guy_count;

    for (int f = 0; f < FACE_COUNT; f++) {
        if (chamber.entries[f] >= 0) {
            topology_send_to_face(static_cast<Face>(f),
                                  (const uint8_t*)&msg, sizeof(msg));
        }
    }
#endif
}

void Coordinator::_broadcast_state() {
#ifdef ARDUINO
    if (!is_queen()) return;  // only queen broadcasts state

    uint32_t now = millis();
    if (now - _last_state_broadcast_ms < 5000) return;
    _last_state_broadcast_ms = now;

    StateSyncMessage msg;
    msg.msg_type     = TOPO_STATE_SYNC;
    msg.sender_id    = topology_my_id();
    msg.night_factor = g_tod.night_factor;
    msg.day_progress = g_tod.day_progress;
    msg.phase        = g_tod.phase;
    msg.local_hour   = g_tod.local_hour;
    msg.local_minute = g_tod.local_minute;

    for (int f = 0; f < FACE_COUNT; f++) {
        if (chamber.entries[f] >= 0) {
            topology_send_to_face(static_cast<Face>(f),
                                  (const uint8_t*)&msg, sizeof(msg));
        }
    }
#endif
}

void Coordinator::on_topology_change(Face face, bool connected, uint16_t module_id) {
#ifdef ARDUINO
    if (is_queen() && connected) {
        // Queen: announce chamber to new satellite
        uint8_t satellite_home = Cfg::FACE_OPPOSITE[face];

        AnnounceMessage ann;
        ann.msg_type       = TOPO_ANNOUNCE;
        ann.parent_id      = topology_my_id();
        ann.parent_face    = face;
        ann.your_id        = module_id;
        ann.your_home_face = satellite_home;

        topology_send_to_face(face, (const uint8_t*)&ann, sizeof(ann));

        Serial.printf("[coord] announcing chamber on face %s -> module 0x%04X, home_face=%s\n",
            face_letter(face), module_id, face_letter(satellite_home));

        // Update topology graph
        // Slot 0 = queen (self)
        if (topo_module_count == 0) {
            topo_graph[0].module_id = topology_my_id();
            topo_graph[0].present = true;
            for (int f = 0; f < FACE_COUNT; f++) topo_graph[0].neighbours[f] = -1;
            topo_module_count = 1;
        }
        // Find or add satellite
        int sat_idx = -1;
        for (int i = 1; i < topo_module_count; i++) {
            if (topo_graph[i].module_id == module_id) { sat_idx = i; break; }
        }
        if (sat_idx < 0 && topo_module_count < MAX_MODULES) {
            sat_idx = topo_module_count++;
            topo_graph[sat_idx] = {};
            topo_graph[sat_idx].module_id = module_id;
            topo_graph[sat_idx].present = true;
            for (int f = 0; f < FACE_COUNT; f++) topo_graph[sat_idx].neighbours[f] = -1;
        }
        if (sat_idx >= 0) {
            topo_graph[0].neighbours[face] = sat_idx;
            topo_graph[sat_idx].neighbours[satellite_home] = 0;
        }
    }

    if (is_queen() && !connected) {
        // Clear graph link
        if (topo_module_count > 0) {
            topo_graph[0].neighbours[face] = -1;
            for (int i = 1; i < topo_module_count; i++) {
                if (topo_graph[i].module_id == module_id) {
                    topo_graph[i].present = false;
                    for (int f = 0; f < FACE_COUNT; f++) topo_graph[i].neighbours[f] = -1;
                    break;
                }
            }
        }
        Serial.printf("[coord] module 0x%04X disconnected from face %s\n",
            module_id, face_letter(face));
    }
#endif
}

void Coordinator::print_topology() const {
#ifdef ARDUINO
    Serial.println("=== Topology Graph ===");
    for (int i = 0; i < topo_module_count; i++) {
        const auto& e = topo_graph[i];
        if (!e.present && i > 0) continue;
        Serial.printf("  M%d (0x%04X)%s:", i, e.module_id, i == 0 ? " [queen]" : "");
        for (int f = 0; f < FACE_COUNT; f++) {
            if (e.neighbours[f] >= 0) {
                Serial.printf(" %s->M%d(0x%04X)", face_letter(f),
                    e.neighbours[f], topo_graph[e.neighbours[f]].module_id);
            }
        }
        Serial.println();
    }
    Serial.println("=====================");
#endif
}

// Helper: number of cells in a face's boundary (column for E/W, row for N/S)
static int boundary_cell_count(int face) {
    return (face == FACE_E || face == FACE_W) ? Cfg::GRID_HEIGHT : Cfg::GRID_WIDTH;
}

// Helper: get x,y for the i-th cell along a face's boundary
static void boundary_cell_xy(int face, int i, int& x, int& y) {
    switch (face) {
        case FACE_E: x = Cfg::GRID_WIDTH - 1; y = i; break;
        case FACE_W: x = 0;                   y = i; break;
        case FACE_N: x = i; y = 0;                   break;
        case FACE_S: x = i; y = Cfg::GRID_HEIGHT - 1; break;
    }
}

void Coordinator::_send_boundary_pheromones(uint32_t tick_num) {
#ifdef ARDUINO
    // Send every 4 ticks (~500ms at 8tps)
    if ((tick_num & 3) != 0) return;

    for (int f = 0; f < FACE_COUNT; f++) {
        if (chamber.entries[f] < 0) continue;

        int n = boundary_cell_count(f);
        PheroSyncMessage msg = {};
        msg.msg_type   = TOPO_PHERO_SYNC;
        msg.sender_id  = topology_my_id();
        msg.face       = f;
        msg.cell_count = n;

        for (int i = 0; i < n; i++) {
            int bx, by;
            boundary_cell_xy(f, i, bx, by);
            float h = chamber.pheromones.raw_home(bx, by);
            float fd = chamber.pheromones.raw_food(bx, by);
            msg.data[i * 2]     = static_cast<uint8_t>(fminf(255.0f, h * 12.75f));
            msg.data[i * 2 + 1] = static_cast<uint8_t>(fminf(255.0f, fd * 12.75f));
        }

        int payload_size = 5 + n * 2;
        topology_send_to_face(static_cast<Face>(f),
                              (const uint8_t*)&msg, payload_size);
    }
#endif
}

void Coordinator::_apply_boundary_pheromones() {
#ifdef ARDUINO
    for (int f = 0; f < FACE_COUNT; f++) {
        if (chamber.entries[f] < 0) continue;

        const BoundaryPheroData& bd = topology_boundary_phero(static_cast<Face>(f));
        if (bd.cell_count == 0) continue;

        // Apply neighbour's boundary values to our boundary column/row
        // using deposit (max semantics) so local values aren't reduced
        int n = boundary_cell_count(f);
        if (bd.cell_count < n) n = bd.cell_count;

        for (int i = 0; i < n; i++) {
            int bx, by;
            boundary_cell_xy(f, i, bx, by);
            if (bd.home[i] > 0.0f)
                chamber.pheromones.deposit_home(bx, by, bd.home[i]);
            if (bd.food[i] > 0.0f)
                chamber.pheromones.deposit_food(bx, by, bd.food[i]);
        }
    }
#endif
}

void Coordinator::_sync_topology_to_chamber() {
#ifdef ARDUINO
    for (int f = 0; f < FACE_COUNT; f++) {
        const Neighbour& nb = topology_neighbour(static_cast<Face>(f));
        chamber.entries[f] = nb.present ? static_cast<int8_t>(nb.module_id & 0x7F) : -1;
    }

    // Satellite: process chamber announcement from queen
    if (!is_queen()) {
        AnnounceMessage ann;
        if (topology_has_announce(&ann)) {
            chamber.home_face = ann.your_home_face;
            // Clear stale workers/brood from a previous colony.
            // After the queen reboots it starts a fresh colony — any workers
            // still on this satellite are orphans that would inflate population.
            if (chamber.lil_guy_count > 0 || chamber.brood_count > 0) {
                Serial.printf("[coord] clearing %d stale workers + %d brood (queen reconnected)\n",
                    chamber.lil_guy_count, chamber.brood_count);
                chamber.lil_guy_count = 0;
                chamber.brood_count = 0;
                chamber.food_pile_count = 0;
                colony.population = 0;
            }
            Serial.printf("[coord] received announce: home_face=%s from queen 0x%04X\n",
                face_letter(ann.your_home_face), ann.parent_id);
        }
    }
#endif
}

void Coordinator::_check_edge_crossings(EventBus& bus, uint32_t tick_num) {
#ifdef ARDUINO
    // Scan workers for edge crossings — iterate backwards since we may remove
    for (int i = chamber.lil_guy_count - 1; i >= 0; i--) {
        LilGuy& w = chamber.lil_guys[i];

        // Skip stacked workers — their bottom worker handles the whole stack
        if (w.stack_on >= 0) continue;

        int cx = w.cell_x();
        int cy = w.cell_y();
        int face = chamber._entry_face_at(cx, cy);
        if (face < 0 || chamber.entries[face] < 0) continue;

        // Anti-bounce: empty TO_FOOD workers can't retreat through home_face
        if (w.state == STATE_TO_FOOD && w.food_carried <= 0.0f
            && face == chamber.home_face) continue;

        const Neighbour& nb = topology_neighbour(static_cast<Face>(face));
        if (!nb.present) continue;

        uint8_t arrival_face = Cfg::FACE_OPPOSITE[face];

        // Collect the entire stack: this worker (bottom) + anyone stacked on it
        int stack[16];
        int stack_count = 0;
        stack[stack_count++] = i;
        {
            int cur = i;
            bool more = true;
            while (more && stack_count < 16) {
                more = false;
                for (int j = 0; j < chamber.lil_guy_count; j++) {
                    if (chamber.lil_guys[j].alive && chamber.lil_guys[j].stack_on == cur) {
                        stack[stack_count++] = j;
                        cur = j;
                        more = true;
                        break;
                    }
                }
            }
        }

        // Compute entry offset: distance from face center along the entry zone
        // N/S faces: offset = cx - ENTRY_X, W/E faces: offset = cy - ENTRY_Y
        int8_t entry_offset = (Cfg::FACE_DY[face] != 0)
            ? (int8_t)(cx - Cfg::ENTRY_X[face])
            : (int8_t)(cy - Cfg::ENTRY_Y[face]);

        // Send all stack members (bottom first)
        int sent_count = 0;
        for (int s = 0; s < stack_count; s++) {
            LilGuyTransfer payload;
            lil_guy_to_transfer(chamber.lil_guys[stack[s]], payload,
                                TOPO_HANDOFF, topology_my_id(), arrival_face, entry_offset);
            if (topology_send_to_face(static_cast<Face>(face),
                                      (const uint8_t*)&payload, sizeof(payload)))
                sent_count++;
        }

        if (sent_count == 0) continue;

        if (stack_count > 1)
            Serial.printf("[handoff] OUT stack of %d via face %s to 0x%04X\n",
                sent_count, face_letter(face), nb.module_id);
        else
            Serial.printf("[handoff] OUT worker %d via face %s to 0x%04X (state=%d food=%.1f)\n",
                i, face_letter(face), nb.module_id, w.state, w.food_carried);

        // Phantom pheromone deposit
        int ex = Cfg::ENTRY_X[face], ey = Cfg::ENTRY_Y[face];
        if (w.state == STATE_TO_HOME && w.food_carried > 0)
            chamber.pheromones.deposit_food(ex, ey, Cfg::BASE_MARKER_INTENSITY * 0.5f);
        else
            chamber.pheromones.deposit_home(ex, ey, Cfg::BASE_MARKER_INTENSITY * 0.5f);

        // Fire events
        for (int s = 0; s < sent_count; s++) {
            Event ev;
            ev.type = EVT_HANDOFF_OUTGOING;
            ev.tick = tick_num;
            ev.handoff = { static_cast<uint8_t>(stack[s]),
                           static_cast<uint8_t>(face), nb.module_id };
            bus.emit(ev);
        }

        // Remove all stack members — sort indices descending for safe removal
        for (int a = 0; a < sent_count - 1; a++)
            for (int b = a + 1; b < sent_count; b++)
                if (stack[a] < stack[b]) { int t = stack[a]; stack[a] = stack[b]; stack[b] = t; }
        for (int s = 0; s < sent_count; s++)
            chamber.remove_lil_guy(stack[s]);
    }
#endif
}

void Coordinator::_place_arrival(const LilGuyTransfer& t, EventBus& bus,
                                  uint32_t tick_num, int* first_idx) {
    if (chamber.lil_guy_count >= Cfg::MAX_LIL_GUYS) return;

    uint8_t af = t.arrival_face;
    if (af >= FACE_COUNT) return;

    int8_t off = t.entry_offset;
    float ex = static_cast<float>(Cfg::ENTRY_X[af])
             + ((Cfg::FACE_DY[af] != 0) ? off : 0);
    float ey = static_cast<float>(Cfg::ENTRY_Y[af])
             + ((Cfg::FACE_DY[af] == 0) ? off : 0);
    float fdx = static_cast<float>(-Cfg::FACE_DX[af]);
    float fdy = static_cast<float>(-Cfg::FACE_DY[af]);

    int idx = chamber.lil_guy_count;
    chamber.lil_guy_count++;
    LilGuy& w = chamber.lil_guys[idx];
    transfer_to_lil_guy(t, w, ex, ey, fdx, fdy);

    // Auto-stack: multiple arrivals on the same face in the same tick
    if (first_idx[af] >= 0) {
        int top = first_idx[af];
        for (int k = first_idx[af] + 1; k < idx; k++) {
            if (chamber.lil_guys[k].stack_on == top) top = k;
        }
        w.stack_on = top;
        w.stack_hop_remaining = 6;
        w.state = STATE_IDLE;
        w.has_target = false;
        w.has_target_cell = false;
        w.idle_ticks_remaining = g_rng.rand_int(60, 120);
    } else {
        first_idx[af] = idx;
    }

    int entry_x = Cfg::ENTRY_X[af], entry_y = Cfg::ENTRY_Y[af];
    if (w.state == STATE_TO_HOME && w.food_carried > 0)
        chamber.pheromones.deposit_food(entry_x, entry_y, Cfg::BASE_MARKER_INTENSITY * 0.5f);
    else
        chamber.pheromones.deposit_home(entry_x, entry_y, Cfg::BASE_MARKER_INTENSITY * 0.5f);

    Serial.printf("[handoff] IN slot %d from 0x%04X face %s (state=%d food=%.1f%s)\n",
        idx, t.sender_id, face_letter(af), t.state, t.food_carried,
        w.stack_on >= 0 ? " stacked" : "");

    Event ev;
    ev.type = EVT_HANDOFF_INCOMING;
    ev.tick = tick_num;
    ev.handoff = { static_cast<uint8_t>(idx), af, t.sender_id };
    bus.emit(ev);
}

void Coordinator::_receive_handoffs(EventBus& bus, uint32_t tick_num) {
#ifdef ARDUINO
    uint32_t now = millis();

    // Drain ESP-NOW handoffs into the tunnel delay buffer
    PendingHandoff pending[4];
    int n = topology_drain_handoffs(pending, 4);
    for (int h = 0; h < n; h++) {
        if (pending[h].len < (int)sizeof(LilGuyTransfer)) continue;
        const LilGuyTransfer& t = *reinterpret_cast<const LilGuyTransfer*>(pending[h].data);
        if (t.msg_type != TOPO_HANDOFF) continue;
        if (t.arrival_face >= FACE_COUNT) continue;

        // Find a free slot in the tunnel buffer
        for (int i = 0; i < MAX_TUNNEL_PENDING; i++) {
            if (!_tunnel_pending[i].active) {
                _tunnel_pending[i].transfer = t;
                _tunnel_pending[i].appear_at_ms = now + TUNNEL_TRAVEL_MS;
                _tunnel_pending[i].active = true;
                Serial.printf("[handoff] IN tunnel (appear in %lums)\n",
                    (unsigned long)TUNNEL_TRAVEL_MS);
                break;
            }
        }
    }

    // Place workers whose tunnel travel time has elapsed
    int first_idx[FACE_COUNT] = {-1, -1, -1, -1};
    for (int i = 0; i < MAX_TUNNEL_PENDING; i++) {
        if (_tunnel_pending[i].active && now >= _tunnel_pending[i].appear_at_ms) {
            _place_arrival(_tunnel_pending[i].transfer, bus, tick_num, first_idx);
            _tunnel_pending[i].active = false;
        }
    }
#endif
}
