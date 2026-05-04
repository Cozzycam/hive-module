/* Coordinator — orchestrates chambers and colony-wide state. */
#include "coordinator.h"
#include "topology.h"
#include "transport.h"
#include "time_of_day.h"
#include "rng.h"
#include "sd_card.h"
#include "journal.h"
#include <cmath>

#ifdef ARDUINO
#include <Preferences.h>
#endif

// Death counters (defined in main.cpp)
extern uint16_t g_deaths_starved;
extern uint16_t g_deaths_old_age;

// Handoff counters (defined in main.cpp)
extern uint32_t g_handoffs_out;
extern uint32_t g_handoffs_in;
extern uint32_t g_handoffs_dropped;

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

    boot_id = static_cast<uint16_t>(esp_random() & 0xFFFF);
    Serial.printf("[coord] boot_id: 0x%04X\n", boot_id);

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

    // ---- Receive incoming handoffs (workers arriving from other modules) ----
    _receive_handoffs(bus, tick_num);

    // ---- Service pending outgoing handoffs (ACK processing, retries, timeouts) ----
    _service_pending_handoffs();

    // ---- Apply neighbour boundary pheromones (before chamber tick so workers sense them) ----
    _apply_boundary_pheromones();

    // ---- Chamber tick (movement, behavior, lifecycle events) ----
    chamber.tick(dt);

    // ---- Edge crossing detection + outgoing handoff ----
    _check_edge_crossings(bus, tick_num);
    _service_departures(bus, tick_num);

    // ---- Send our boundary pheromones to neighbours ----
    _send_boundary_pheromones(tick_num);

    // ---- Satellite: broadcast local population to neighbours ----
    _broadcast_population();

    // ---- Queen: broadcast colony state (tod + stats) to satellites ----
    _broadcast_state();

    // ---- Aggregate colony stats ----
    _aggregate_colony_stats();

    // ---- Persistence: assign IDs, process hatches/deaths, periodic flush ----
    if (is_queen()) _persist_tick(tick_num);

    // ---- Journal: flush buffered events to disk periodically ----
    if (is_queen()) journal.tick();
}

void Coordinator::_aggregate_colony_stats() {
    // Local population (departing workers are still in chamber, counted naturally)
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

    // Throttle population display updates to avoid transit fluctuation
    uint16_t new_pop = local_pop + remote_pop;
    static uint32_t _last_pop_update_ms = 0;
    uint32_t now = millis();
    if (colony.population == 0 || now - _last_pop_update_ms >= 5000) {
        colony.population = new_pop;
        _last_pop_update_ms = now;
    }

    int gatherers = 0;
    for (int i = 0; i < chamber.lil_guy_count; i++) {
        auto& w = chamber.lil_guys[i];
        if (w.state == STATE_TO_FOOD
                || (w.state == STATE_TO_HOME && w.food_carried > 0))
            gatherers++;
    }
    // Departing workers are still in chamber.lil_guys, counted naturally above
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
    if (now - _last_pop_broadcast_ms < 5000) return;
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
        ann.boot_id        = boot_id;

        topology_send_to_face(face, (const uint8_t*)&ann, sizeof(ann));

        Serial.printf("[coord] announcing chamber on face %s -> module 0x%04X, home_face=%s\n",
            face_letter(face), module_id, face_letter(satellite_home));

        // Journal: module connected
        JournalEntry je = {};
        je.tick = 0;  // no tick context in topology callback
        je.unix_time = g_tod.unix_time;
        je.type = JEVT_COLONY_EVENT;
        je.lilguy_id = 0;
        je.colony_event.kind = COLONY_MODULE_CONNECTED;
        je.colony_event.module_id = module_id;
        journal.emit(je);

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

        // Journal: module disconnected
        JournalEntry je = {};
        je.tick = 0;
        je.unix_time = g_tod.unix_time;
        je.type = JEVT_COLONY_EVENT;
        je.lilguy_id = 0;
        je.colony_event.kind = COLONY_MODULE_DISCONNECTED;
        je.colony_event.module_id = module_id;
        journal.emit(je);
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
            float fd = chamber.pheromones.raw_food(bx, by);
            msg.data[i * 2]     = 0;  // home slot unused
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
            // Only clear workers if the queen rebooted (new boot_id).
            // A simple reconnect after radio glitch keeps workers alive.
            if (ann.boot_id != _last_queen_boot_id && _last_queen_boot_id != 0) {
                if (chamber.lil_guy_count > 0 || chamber.brood_count > 0) {
                    Serial.printf("[coord] queen rebooted (boot_id 0x%04X -> 0x%04X) -- clearing %d workers + %d brood\n",
                        _last_queen_boot_id, ann.boot_id,
                        chamber.lil_guy_count, chamber.brood_count);
                    chamber.lil_guy_count = 0;
                    chamber.brood_count = 0;
                    chamber.food_pile_count = 0;
                    colony.population = 0;
                }
            }
            _last_queen_boot_id = ann.boot_id;
            Serial.printf("[coord] received announce: home_face=%s from queen 0x%04X boot_id=0x%04X\n",
                face_letter(ann.your_home_face), ann.parent_id, ann.boot_id);
        }
    }
#endif
}

void Coordinator::_check_edge_crossings(EventBus& bus, uint32_t tick_num) {
#ifdef ARDUINO
    // Scan workers for edge crossings — mark as departing (sprite vanishes, still counted here)
    for (int i = 0; i < chamber.lil_guy_count; i++) {
        LilGuy& w = chamber.lil_guys[i];

        if (!w.alive || w.departing) continue;

        // Skip stacked workers — their bottom worker handles the whole stack
        if (w.stack_on >= 0) continue;

        int cx = w.cell_x();
        int cy = w.cell_y();
        int face = chamber._entry_face_at(cx, cy);
        if (face < 0 || chamber.entries[face] < 0) continue;

        // Anti-bounce: recently arrived workers can't exit through arrival face (2s window)
        if (w.arrival_face >= 0 && face == w.arrival_face
            && (millis() - w.arrival_ms) < 2000) continue;

        const Neighbour& nb = topology_neighbour(static_cast<Face>(face));
        if (!nb.present) continue;

        // Mark this worker (and any stacked on it) as departing
        w.departing = true;
        w.depart_at_ms = millis() + DEPART_DELAY_MS;
        w.depart_face = static_cast<int8_t>(face);

        // Also mark stacked workers
        int cur = i;
        bool more = true;
        while (more) {
            more = false;
            for (int j = 0; j < chamber.lil_guy_count; j++) {
                if (chamber.lil_guys[j].alive && chamber.lil_guys[j].stack_on == cur) {
                    chamber.lil_guys[j].departing = true;
                    chamber.lil_guys[j].depart_at_ms = w.depart_at_ms;
                    chamber.lil_guys[j].depart_face = w.depart_face;
                    cur = j;
                    more = true;
                    break;
                }
            }
        }

        // Phantom pheromone deposit — food trail only
        if (w.state == STATE_TO_HOME && w.food_carried > 0) {
            int ex = Cfg::ENTRY_X[face], ey = Cfg::ENTRY_Y[face];
            chamber.pheromones.deposit_food(ex, ey, Cfg::BASE_MARKER_INTENSITY * 0.5f);
        }
    }
#endif
}

void Coordinator::_service_departures(EventBus& bus, uint32_t tick_num) {
#ifdef ARDUINO
    uint32_t now = millis();

    // Find departing workers whose delay has elapsed — send them out
    for (int i = chamber.lil_guy_count - 1; i >= 0; i--) {
        LilGuy& w = chamber.lil_guys[i];
        if (!w.departing || w.stack_on >= 0) continue;  // only process bottom of stack
        if (now < w.depart_at_ms) continue;

        int face = w.depart_face;
        const Neighbour& nb = topology_neighbour(static_cast<Face>(face));

        // If neighbour disconnected while waiting, cancel departure
        if (!nb.present) {
            w.departing = false;
            w.depart_face = -1;
            // Unmark stacked workers too
            for (int j = 0; j < chamber.lil_guy_count; j++) {
                if (chamber.lil_guys[j].departing && chamber.lil_guys[j].depart_face == face)
                    chamber.lil_guys[j].departing = false;
            }
            continue;
        }

        uint8_t arrival_face = Cfg::FACE_OPPOSITE[face];

        // Collect the stack
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

        // Check pending out slots
        int free_slots = 0;
        for (int j = 0; j < MAX_PENDING_OUT; j++)
            if (!_pending_out[j].active) free_slots++;
        if (free_slots < stack_count) continue;  // wait for slots

        // Compute entry offset
        int cx = w.cell_x(), cy = w.cell_y();
        int8_t entry_offset = (Cfg::FACE_DY[face] != 0)
            ? (int8_t)(cx - Cfg::ENTRY_X[face])
            : (int8_t)(cy - Cfg::ENTRY_Y[face]);

        // Serialize, store in pending out, and send
        int sent_count = 0;
        for (int s = 0; s < stack_count; s++) {
            LilGuyTransfer payload;
            lil_guy_to_transfer(chamber.lil_guys[stack[s]], payload,
                                TOPO_HANDOFF, topology_my_id(), arrival_face, entry_offset);
            payload.seq = _handoff_seq++;

            int slot = -1;
            for (int j = 0; j < MAX_PENDING_OUT; j++) {
                if (!_pending_out[j].active) { slot = j; break; }
            }
            if (slot < 0) break;

            _pending_out[slot].payload = payload;
            _pending_out[slot].sent_ms = now;
            _pending_out[slot].retries = 0;
            _pending_out[slot].face = static_cast<uint8_t>(face);
            _pending_out[slot].active = true;

            topology_send_to_face(static_cast<Face>(face),
                                  (const uint8_t*)&payload, sizeof(payload));
            sent_count++;
            g_handoffs_out++;
        }

        if (sent_count == 0) continue;

        if (stack_count > 1)
            Serial.printf("[handoff] OUT stack of %d via face %s to 0x%04X\n",
                sent_count, face_letter(face), nb.module_id);
        else
            Serial.printf("[handoff] OUT id=%lu via face %s to 0x%04X (state=%d food=%.1f)\n",
                (unsigned long)w.id, face_letter(face), nb.module_id, w.state, w.food_carried);

        // Fire events + journal
        for (int s = 0; s < sent_count; s++) {
            Event ev;
            ev.type = EVT_HANDOFF_OUTGOING;
            ev.tick = tick_num;
            ev.handoff = { static_cast<uint8_t>(stack[s]),
                           static_cast<uint8_t>(face), nb.module_id };
            bus.emit(ev);

            JournalEntry je = {};
            je.tick = tick_num;
            je.unix_time = g_tod.unix_time;
            je.type = JEVT_CHAMBER_CROSSING;
            je.lilguy_id = chamber.lil_guys[stack[s]].id;
            je.crossing.from_module = topology_my_id();
            je.crossing.to_module = nb.module_id;
            je.crossing.face = static_cast<uint8_t>(face);
            journal.emit(je);
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
    w.arrival_ms = millis();

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

    // Phantom pheromone deposit — food trail only
    if (w.state == STATE_TO_HOME && w.food_carried > 0) {
        int entry_x = Cfg::ENTRY_X[af], entry_y = Cfg::ENTRY_Y[af];
        chamber.pheromones.deposit_food(entry_x, entry_y, Cfg::BASE_MARKER_INTENSITY * 0.5f);
    }

    g_handoffs_in++;

    Serial.printf("[handoff] IN id=%lu from 0x%04X face %s (state=%d food=%.1f%s)\n",
        (unsigned long)w.id, t.sender_id, face_letter(af), t.state, t.food_carried,
        w.stack_on >= 0 ? " stacked" : "");

    Event ev;
    ev.type = EVT_HANDOFF_INCOMING;
    ev.tick = tick_num;
    ev.handoff = { static_cast<uint8_t>(idx), af, t.sender_id };
    bus.emit(ev);
}

void Coordinator::_receive_handoffs(EventBus& bus, uint32_t tick_num) {
#ifdef ARDUINO
    // Drain ESP-NOW handoffs and place workers immediately
    PendingHandoff pending[16];
    int n = topology_drain_handoffs(pending, 16);
    int first_idx[FACE_COUNT] = {-1, -1, -1, -1};

    for (int h = 0; h < n; h++) {
        if (pending[h].len < (int)sizeof(LilGuyTransfer)) continue;
        const LilGuyTransfer& t = *reinterpret_cast<const LilGuyTransfer*>(pending[h].data);
        if (t.msg_type != TOPO_HANDOFF) continue;
        if (t.arrival_face >= FACE_COUNT) continue;

        // Find which face this sender is connected to (for dedup + ACK)
        int src_face = -1;
        for (int f = 0; f < FACE_COUNT; f++) {
            const Neighbour& nb = topology_neighbour(static_cast<Face>(f));
            if (nb.present && nb.module_id == t.sender_id) {
                src_face = f;
                break;
            }
        }

        // Dedup: if seq matches last seen from this face, re-send ACK but skip placement
        if (src_face >= 0 && t.seq == _last_seen_seq[src_face]) {
            HandoffAck ack;
            ack.msg_type = TOPO_HANDOFF_ACK;
            ack.acker_id = topology_my_id();
            ack.seq = t.seq;
            topology_send_to_face(static_cast<Face>(src_face),
                                  (const uint8_t*)&ack, sizeof(ack));
            continue;
        }

        // Place worker immediately (visual delay is on sender side)
        if (chamber.lil_guy_count >= Cfg::MAX_LIL_GUYS) {
            g_handoffs_dropped++;
            Serial.println("[handoff] chamber full -- dropped");
            continue;  // don't ACK — sender will retry then restore
        }

        _place_arrival(t, bus, tick_num, first_idx);

        // Send ACK to sender
        if (src_face >= 0) {
            _last_seen_seq[src_face] = t.seq;
            HandoffAck ack;
            ack.msg_type = TOPO_HANDOFF_ACK;
            ack.acker_id = topology_my_id();
            ack.seq = t.seq;
            topology_send_to_face(static_cast<Face>(src_face),
                                  (const uint8_t*)&ack, sizeof(ack));
        }

        // Force immediate PopSync so queen sees new count quickly
        _last_pop_broadcast_ms = 0;
    }
#endif
}

void Coordinator::_service_pending_handoffs() {
#ifdef ARDUINO
    // 1. Drain ACK buffer — mark matched pending entries as done
    PendingAck acks[16];
    int ack_count = topology_drain_handoff_acks(acks, 16);
    for (int i = 0; i < ack_count; i++) {
        if (acks[i].len < (int)sizeof(HandoffAck)) continue;
        const HandoffAck& ack = *reinterpret_cast<const HandoffAck*>(acks[i].data);
        if (ack.msg_type != TOPO_HANDOFF_ACK) continue;
        for (int j = 0; j < MAX_PENDING_OUT; j++) {
            if (_pending_out[j].active && _pending_out[j].payload.seq == ack.seq) {
                _pending_out[j].active = false;
                break;
            }
        }
    }

    // 2. Handle retries and timeouts
    uint32_t now = millis();
    for (int i = 0; i < MAX_PENDING_OUT; i++) {
        if (!_pending_out[i].active) continue;
        if (now - _pending_out[i].sent_ms < 500) continue;

        if (_pending_out[i].retries < 3) {
            // Resend
            topology_send_to_face(static_cast<Face>(_pending_out[i].face),
                                  (const uint8_t*)&_pending_out[i].payload,
                                  sizeof(LilGuyTransfer));
            _pending_out[i].retries++;
            _pending_out[i].sent_ms = now;
        } else {
            // Timeout — restore ant to chamber
            int f = _pending_out[i].face;
            float ex = static_cast<float>(Cfg::ENTRY_X[f]);
            float ey = static_cast<float>(Cfg::ENTRY_Y[f]);
            float fdx = static_cast<float>(-Cfg::FACE_DX[f]);
            float fdy = static_cast<float>(-Cfg::FACE_DY[f]);

            if (chamber.lil_guy_count < Cfg::MAX_LIL_GUYS) {
                int idx = chamber.lil_guy_count++;
                LilGuy& w = chamber.lil_guys[idx];
                transfer_to_lil_guy(_pending_out[i].payload, w, ex, ey, fdx, fdy);
                w.arrival_face = static_cast<int8_t>(f);  // prevent immediate re-exit
                w.arrival_ms = millis();
                Serial.printf("[handoff] TIMEOUT -- restoring id=%lu\n",
                              (unsigned long)w.id);
            } else {
                Serial.println("[handoff] TIMEOUT -- chamber full, worker lost");
            }

            _pending_out[i].active = false;
            g_handoffs_dropped++;
        }
    }
#endif
}

// ================================================================
//  Journal: convert EventBus events to journal entries
// ================================================================

void Coordinator::_journal_from_bus_events(const Event* events, int count,
                                            uint32_t tick_num) {
    if (journal.pending_count() < 0) return;  // not active

    for (int i = 0; i < count; i++) {
        const Event& ev = events[i];
        JournalEntry je = {};
        je.tick = ev.tick;
        je.unix_time = g_tod.unix_time;

        switch (ev.type) {
        case EVT_FOOD_DELIVERED:
            if (ev.food_delivered.amount > 0.01f) {
                je.type = JEVT_FOOD_DELIVERED;
                je.lilguy_id = 0;
                je.food_delivered.amount = ev.food_delivered.amount;
                journal.emit(je);
            }
            break;

        case EVT_PILE_DISCOVERED:
            // Not currently emitted with enough data — skip for now
            break;

        default:
            // Other events handled directly (hatch, death, crossing, food_tap)
            break;
        }
    }
}

// ================================================================
//  Persistence integration
// ================================================================

void Coordinator::_persist_tick(uint32_t tick_num) {
    if (registry.state() == PERSIST_DEGRADED || registry.state() == PERSIST_UNINIT)
        return;

    // Assign IDs to newly-laid brood (id == 0)
    _persist_assign_new_brood_ids();

    // Process hatches — create IdentityRecords, remove BroodRecords
    _persist_process_hatches();

    // Process deaths — mark_dead in registry
    _persist_process_deaths();

    // Assign IDs to workers that somehow have id==0 (handoff from old firmware, etc.)
    for (int i = 0; i < chamber.lil_guy_count; i++) {
        if (chamber.lil_guys[i].id == 0) {
            chamber.lil_guys[i].id = registry.allocate_id();
            // Create a record for this worker
            IdentityRecord rec;
            rec.id = chamber.lil_guys[i].id;
            snprintf(rec.name, sizeof(rec.name), "LilGuy_%lu",
                     (unsigned long)rec.id);
            rec.role = chamber.lil_guys[i].role;
            rec.is_pioneer = chamber.lil_guys[i].is_pioneer;
            rec.born_unix = g_tod.unix_time;
            rec.lifespan_ms = chamber.lil_guys[i].lifespan_ms;
            rec.last_x = chamber.lil_guys[i].x;
            rec.last_y = chamber.lil_guys[i].y;
            rec.last_state = chamber.lil_guys[i].state;
            registry.create(rec);
        }
    }

    // Milestone tracking
    static uint16_t _last_milestone_born = 0;
    uint16_t born = colony.total_workers_born;
    if (born > 0 && born != _last_milestone_born) {
        // Check round numbers: 10, 25, 50, 100, 200, 500, 1000...
        bool hit = (born == 10 || born == 25 || born == 50 ||
                    (born >= 100 && born % 100 == 0));
        if (hit) {
            JournalEntry je = {};
            je.tick = tick_num;
            je.unix_time = g_tod.unix_time;
            je.type = JEVT_MILESTONE;
            je.lilguy_id = 0;
            je.milestone.kind = MILE_WORKERS_BORN;
            je.milestone.value = born;
            journal.emit(je);
        }
        _last_milestone_born = born;
    }

    // SD card health check
    sd_card_health_tick();

    // Periodic flush (every 30s)
    uint32_t now = millis();
    if (now - _last_persist_flush_ms >= 30000) {
        _last_persist_flush_ms = now;
        _persist_update_positions();
        _persist_sync_colony_state(tick_num);
        registry.flush();
    }
}

void Coordinator::_persist_assign_new_brood_ids() {
    for (int i = 0; i < chamber.brood_count; i++) {
        if (chamber.brood[i].id == 0) {
            chamber.brood[i].id = registry.allocate_id();
            BroodRecord rec;
            rec.id = chamber.brood[i].id;
            rec.stage = chamber.brood[i].stage;
            rec.role = chamber.brood[i].role;
            rec.x = chamber.brood[i].x;
            rec.y = chamber.brood[i].y;
            rec.hunger = chamber.brood[i].hunger;
            rec.food_invested = chamber.brood[i].food_invested;
            rec.stage_start_ms = chamber.brood[i].stage_start_ms;
            rec.born_unix = g_tod.unix_time;
            registry.create_brood(rec);
        }
    }
}

void Coordinator::_persist_process_hatches() {
    for (int h = 0; h < chamber.hatch_count; h++) {
        uint32_t id = chamber.hatch_ids[h];
        // Remove brood record
        registry.remove_brood(id);

        // Find the worker with this ID and create its IdentityRecord
        for (int i = 0; i < chamber.lil_guy_count; i++) {
            if (chamber.lil_guys[i].id == id) {
                IdentityRecord rec;
                rec.id = id;
                snprintf(rec.name, sizeof(rec.name), "LilGuy_%lu",
                         (unsigned long)id);
                rec.role = chamber.lil_guys[i].role;
                rec.is_pioneer = chamber.lil_guys[i].is_pioneer;
                rec.born_unix = g_tod.unix_time;
                rec.lifespan_ms = chamber.lil_guys[i].lifespan_ms;
                rec.last_x = chamber.lil_guys[i].x;
                rec.last_y = chamber.lil_guys[i].y;
                rec.last_state = chamber.lil_guys[i].state;
                registry.create(rec);

                // Journal: hatch event
                JournalEntry je = {};
                je.tick = chamber.tick_num;
                je.unix_time = g_tod.unix_time;
                je.type = JEVT_HATCH;
                je.lilguy_id = id;
                je.hatch.role = chamber.lil_guys[i].role;
                je.hatch.is_pioneer = chamber.lil_guys[i].is_pioneer;
                je.hatch.from_brood_id = id;
                journal.emit(je);
                break;
            }
        }
    }
}

void Coordinator::_persist_process_deaths() {
    for (int d = 0; d < chamber.death_count; d++) {
        uint32_t id = chamber.deaths[d].id;
        uint8_t cause = chamber.deaths[d].cause;
        registry.mark_dead(id, g_tod.unix_time);
        registry.manifest().total_workers_died++;

        // Journal: death event
        JournalEntry je = {};
        je.tick = chamber.tick_num;
        je.unix_time = g_tod.unix_time;
        je.type = JEVT_DEATH;
        je.lilguy_id = id;
        je.death.cause = cause;
        journal.emit(je);
    }
}

void Coordinator::_persist_update_positions() {
    for (int i = 0; i < chamber.lil_guy_count; i++) {
        LilGuy& w = chamber.lil_guys[i];
        if (w.id == 0) continue;
        IdentityRecord* rec = registry.get(w.id);
        if (!rec) continue;
        rec->last_x = w.x;
        rec->last_y = w.y;
        rec->last_state = w.state;
        if (rec->lifespan_ms == 0) rec->lifespan_ms = w.lifespan_ms;
        rec->dirty = true;
    }

    // Also update brood positions/state
    for (int i = 0; i < chamber.brood_count; i++) {
        Brood& b = chamber.brood[i];
        if (b.id == 0) continue;
        BroodRecord* rec = registry.get_brood(b.id);
        if (!rec) continue;
        rec->stage = b.stage;
        rec->hunger = b.hunger;
        rec->food_invested = b.food_invested;
        rec->stage_start_ms = b.stage_start_ms;
    }
}

void Coordinator::_persist_sync_colony_state(uint32_t tick_num) {
    ColonyManifest& m = registry.manifest();
    m.last_tick = tick_num;
    m.food_store = colony.food_store;
    m.food_total = colony.food_total;
    m.total_workers_born = colony.total_workers_born;
    m.worker_census = colony.worker_census;
    m.module_role = role;

    // Queen state
    if (chamber.has_queen) {
        m.queen_state.reserves = chamber.queen_obj.reserves;
        m.queen_state.hunger = chamber.queen_obj.hunger;
        m.queen_state.founding_done = chamber.queen_obj.founding_done;
        m.queen_state.eggs_laid = chamber.queen_obj.eggs_laid;
        m.queen_state.egg_accum = chamber.queen_obj.egg_accum;
        m.queen_state.x = chamber.queen_obj.x;
        m.queen_state.y = chamber.queen_obj.y;
    }
}

void Coordinator::_persist_migrate_live_colony() {
    // Case B: first boot with SD — migrate existing live colony to persistence.
    // Called when SD is available but no manifest exists.
    Serial.println("[persist] migrating live colony to persistence (Case B)...");

    ColonyManifest& m = registry.manifest();
    m.schema = 1;
    registry.generate_colony_id();

    // Read founding time from HUD's NVS if available
    Preferences prefs;
    prefs.begin("hive", true);
    m.founded_unix = prefs.getULong("founded", g_tod.unix_time);
    prefs.end();
    if (m.founded_unix == 0) m.founded_unix = g_tod.unix_time;

    m.module_role = role;
    m.food_store = colony.food_store;
    m.food_total = colony.food_total;
    m.total_workers_born = colony.total_workers_born;
    m.worker_census = colony.worker_census;

    // Queen gets ID 0 (special)
    if (chamber.has_queen) {
        chamber.queen_obj.id = registry.allocate_id();
        m.queen_state.reserves = chamber.queen_obj.reserves;
        m.queen_state.hunger = chamber.queen_obj.hunger;
        m.queen_state.founding_done = chamber.queen_obj.founding_done;
        m.queen_state.eggs_laid = chamber.queen_obj.eggs_laid;
        m.queen_state.egg_accum = chamber.queen_obj.egg_accum;
        m.queen_state.x = chamber.queen_obj.x;
        m.queen_state.y = chamber.queen_obj.y;
    }

    // Migrate all living workers
    for (int i = 0; i < chamber.lil_guy_count; i++) {
        LilGuy& w = chamber.lil_guys[i];
        w.id = registry.allocate_id();
        IdentityRecord rec;
        rec.id = w.id;
        snprintf(rec.name, sizeof(rec.name), "LilGuy_%lu", (unsigned long)rec.id);
        rec.role = w.role;
        rec.is_pioneer = w.is_pioneer;
        rec.born_unix = m.founded_unix;  // approximate
        rec.lifespan_ms = w.lifespan_ms;
        rec.last_x = w.x;
        rec.last_y = w.y;
        rec.last_state = w.state;
        registry.create(rec);
    }

    // Migrate brood
    for (int i = 0; i < chamber.brood_count; i++) {
        Brood& b = chamber.brood[i];
        b.id = registry.allocate_id();
        BroodRecord rec;
        rec.id = b.id;
        rec.stage = b.stage;
        rec.role = b.role;
        rec.x = b.x;
        rec.y = b.y;
        rec.hunger = b.hunger;
        rec.food_invested = b.food_invested;
        rec.stage_start_ms = b.stage_start_ms;
        rec.born_unix = m.founded_unix;
        registry.create_brood(rec);
    }

    registry.flush();

    // Journal: colony founded event
    JournalEntry je = {};
    je.tick = 0;
    je.unix_time = m.founded_unix;
    je.type = JEVT_COLONY_EVENT;
    je.lilguy_id = 0;
    je.colony_event.kind = COLONY_FOUNDED;
    je.colony_event.module_id = 0;
    journal.emit(je);
    journal.flush();

    Serial.printf("[persist] migration complete — %d workers, %d brood, colony_id=%s\n",
                  chamber.lil_guy_count, chamber.brood_count, m.colony_id);
}

void Coordinator::_persist_restore_from_disk() {
    // Case C: manifest exists — restore colony from disk.
    Serial.println("[persist] restoring colony from disk (Case C)...");

    ColonyManifest& m = registry.manifest();

    // Restore colony state
    colony.food_store = m.food_store;
    colony.food_total = m.food_total;
    colony.total_workers_born = m.total_workers_born;
    colony.worker_census = m.worker_census;

    // Restore queen
    if (chamber.has_queen) {
        chamber.queen_obj.id = 1;  // queen is always first allocated
        chamber.queen_obj.reserves = m.queen_state.reserves;
        chamber.queen_obj.hunger = m.queen_state.hunger;
        chamber.queen_obj.founding_done = m.queen_state.founding_done;
        chamber.queen_obj.eggs_laid = m.queen_state.eggs_laid;
        chamber.queen_obj.egg_accum = m.queen_state.egg_accum;
        chamber.queen_obj.x = m.queen_state.x;
        chamber.queen_obj.y = m.queen_state.y;
    }

    // Restore workers from living records
    IdentityRecord* recs = registry.living_records();
    int rec_count = registry.living_count();
    chamber.lil_guy_count = 0;

    for (int i = 0; i < rec_count && chamber.lil_guy_count < Cfg::MAX_LIL_GUYS; i++) {
        IdentityRecord& r = recs[i];
        int idx = chamber.lil_guy_count;
        chamber.lil_guys[idx].init(
            static_cast<int8_t>(r.last_x),
            static_cast<int8_t>(r.last_y),
            static_cast<Role>(r.role),
            r.is_pioneer
        );
        chamber.lil_guys[idx].id = r.id;
        // Restore float position (init sets int pos, we want exact float)
        chamber.lil_guys[idx].x = r.last_x;
        chamber.lil_guys[idx].y = r.last_y;
        chamber.lil_guys[idx].prev_x = r.last_x;
        chamber.lil_guys[idx].prev_y = r.last_y;
        // Restore lifespan (init randomizes it — override with persisted value)
        if (r.lifespan_ms > 0)
            chamber.lil_guys[idx].lifespan_ms = r.lifespan_ms;
        // born_at_ms: approximate from born_unix vs current time
        if (r.born_unix > 0 && g_tod.unix_time > r.born_unix) {
            uint32_t age_secs = g_tod.unix_time - r.born_unix;
            chamber.lil_guys[idx].born_at_ms = millis() - (age_secs * 1000);
        }
        chamber.lil_guy_count++;
    }

    // Restore brood
    BroodRecord* brecs = registry.brood_records();
    int brood_count = registry.brood_count();
    chamber.brood_count = 0;

    for (int i = 0; i < brood_count && chamber.brood_count < Cfg::MAX_BROOD; i++) {
        BroodRecord& br = brecs[i];
        int idx = chamber.brood_count;
        chamber.brood[idx].init(br.x, br.y, static_cast<Role>(br.role));
        chamber.brood[idx].id = br.id;
        chamber.brood[idx].stage = static_cast<BroodStage>(br.stage);
        chamber.brood[idx].hunger = br.hunger;
        chamber.brood[idx].food_invested = br.food_invested;
        // stage_start_ms already converted from elapsed to absolute in _load_brood_records
        chamber.brood[idx].stage_start_ms = br.stage_start_ms;
        chamber.brood_count++;
    }

    colony.population = chamber.lil_guy_count;

    Serial.printf("[persist] restored %d workers, %d brood, food=%.0f\n",
                  chamber.lil_guy_count, chamber.brood_count, colony.food_store);
}
