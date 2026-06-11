/* Coordinator — orchestrates chambers and colony-wide state. */
#include "coordinator.h"
#include "topology.h"
#include "renderer.h"
#include "transport.h"
#include "time_of_day.h"
#include "weather.h"
#include "rng.h"
#include "sd_card.h"
#include "journal.h"
#include "bonds.h"
#include "names.h"
#include <SD_MMC.h>
#include <ArduinoJson.h>
#include <cmath>
#include <cstring>

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

const char* module_role_str(uint8_t r) {
    switch (r) {
        case MODULE_QUEEN:      return "queen";
        case MODULE_SATELLITE:  return "satellite";
        case MODULE_GARDEN:     return "garden";
        case MODULE_FOOD_STORE: return "food_store";
        case MODULE_HEART_TREE: return "heart_tree";
        default:                return "unconfigured";
    }
}

int module_role_from_str(const char* s) {
    for (uint8_t r = MODULE_QUEEN; r < MODULE_ROLE_COUNT; r++) {
        if (strcmp(s, module_role_str(r)) == 0) return r;
    }
    return -1;
}

static const char* role_str(ModuleRole r) { return module_role_str(r); }

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
        Serial.printf("[coord] module role: %s\r\n", role_str(role));
    }
#else
    role = MODULE_UNCONFIGURED;  // desktop builds default to queen
#endif

    boot_id = static_cast<uint16_t>(esp_random() & 0xFFFF);
    Serial.printf("[coord] boot_id: 0x%04X\r\n", boot_id);

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
    Serial.printf("[coord] role written to NVS: %s\r\n", role_str(r));
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

    // ---- Receive death syncs from satellites (queen only) ----
    if (is_queen()) _receive_death_syncs();

    // ---- Service pending outgoing handoffs (ACK processing, retries, timeouts) ----
    _service_pending_handoffs();

    // ---- Apply neighbour boundary pheromones (before chamber tick so workers sense them) ----
    _apply_boundary_pheromones();

    // ---- Chamber tick (movement, behavior, lifecycle events) ----
    // Gather state is wired from Sim by main.cpp before tick()
    chamber.tick(dt);

    // ---- Satellite: send death syncs to queen ----
    if (!is_queen()) _send_death_syncs();

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

    // ---- Queen: reconcile registry vs chamber — respawn lost workers ----
    if (is_queen()) _reconcile_workers();

    // ---- WorldCondition: sync time ----
    if (is_queen()) _world_tick();

    // ---- Bonds: decay + proximity detection ----
    if (is_queen()) _bond_tick(tick_num);

    // ---- Traits: periodic check for Elder, Bonded ----
    if (is_queen()) _trait_tick(tick_num);

    // ---- Journal: flush buffered events to disk periodically ----
    if (is_queen()) journal.tick();
}

void Coordinator::_aggregate_colony_stats() {
    // Local population (departing workers are still in chamber, counted naturally)
    uint16_t local_pop = chamber.conker_count;

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

    // Empty-handed homebound workers still count — they hold their forage
    // slot until they arrive, preventing over-recruitment while in transit.
    int gatherers = 0;
    for (int i = 0; i < chamber.conker_count; i++) {
        auto& w = chamber.conkers[i];
        if (w.state == STATE_TO_FOOD || w.state == STATE_TO_HOME)
            gatherers++;
    }
#ifdef ARDUINO
    // Queen: add gatherers currently on satellite modules (mirrors population)
    if (is_queen()) {
        for (int f = 0; f < FACE_COUNT; f++) {
            gatherers += topology_remote_gatherers(static_cast<Face>(f));
        }
    }
#endif
    colony.gatherer_count = gatherers;

    uint16_t eggs, seeds;
    chamber.count_brood(eggs, seeds);
    colony.brood_egg   = eggs;
    colony.brood_seed  = seeds;
    colony.brood_larva = seeds;  // legacy alias
    colony.brood_pupa  = 0;

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
    msg.population = chamber.conker_count;
    msg.gatherers  = colony.gatherer_count;  // local count (satellites don't aggregate)
    msg.role       = static_cast<uint8_t>(role);
    {
        uint32_t tint = renderer_get_floor_tint();
        msg.tint_r = (tint >> 16) & 0xFF;
        msg.tint_g = (tint >> 8) & 0xFF;
        msg.tint_b = tint & 0xFF;
    }

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
    msg.weather      = g_weather.valid ? g_weather.condition : 0;
    msg.temperature_c = g_weather.temperature_c;

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

        // Send WiFi credentials so satellite can sync when solo
        WifiCredsMessage wcm;
        wcm.msg_type  = TOPO_WIFI_CREDS;
        wcm.sender_id = topology_my_id();
        memset(wcm.ssid, 0, sizeof(wcm.ssid));
        memset(wcm.pass, 0, sizeof(wcm.pass));
        tod_wifi_get_creds(wcm.ssid, sizeof(wcm.ssid), wcm.pass, sizeof(wcm.pass));
        topology_send_to_face(face, (const uint8_t*)&wcm, sizeof(wcm));

        Serial.printf("[coord] announcing chamber on face %s -> module 0x%04X, home_face=%s\r\n",
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
        Serial.printf("[coord] module 0x%04X disconnected from face %s\r\n",
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
                if (chamber.conker_count > 0 || chamber.brood_count > 0) {
                    Serial.printf("[coord] queen rebooted (boot_id 0x%04X -> 0x%04X) -- clearing %d workers + %d brood\r\n",
                        _last_queen_boot_id, ann.boot_id,
                        chamber.conker_count, chamber.brood_count);
                    chamber.conker_count = 0;
                    chamber.brood_count = 0;
                    chamber.food_pile_count = 0;
                    colony.population = 0;
                }
            }
            _last_queen_boot_id = ann.boot_id;
            Serial.printf("[coord] received announce: home_face=%s from queen 0x%04X boot_id=0x%04X\r\n",
                face_letter(ann.your_home_face), ann.parent_id, ann.boot_id);
        }

        // Receive WiFi credentials from queen
        WifiCredsMessage wcm;
        if (topology_has_wifi_creds(&wcm)) {
            if (wcm.ssid[0] != '\0') {
                tod_wifi_set_ssid(wcm.ssid);
                tod_wifi_set_pass(wcm.pass);
                Serial.printf("[coord] WiFi creds received from queen: '%s'\r\n", wcm.ssid);
            }
        }

        // Role assignment from queen (relayed app command). No reboot —
        // all assignable roles share satellite behaviour; the new role is
        // echoed back via pop sync within ~5s.
        SetRoleMessage srm;
        if (topology_has_set_role(&srm)) {
            if (srm.target_id == topology_my_id()
                && srm.role > MODULE_QUEEN && srm.role < MODULE_ROLE_COUNT) {
                role = static_cast<ModuleRole>(srm.role);
                set_role_nvs(role);
                Serial.printf("[coord] role assigned by queen: %s\r\n",
                              module_role_str(srm.role));
            }
        }

        // Floor tint from queen (relayed app command)
        SetTintMessage stm;
        if (topology_has_set_tint(&stm)) {
            if (stm.target_id == topology_my_id())
                renderer_set_floor_tint(stm.r, stm.g, stm.b, true);
        }
    }
#endif
}

void Coordinator::_check_edge_crossings(EventBus& bus, uint32_t tick_num) {
#ifdef ARDUINO
    // Scan workers for edge crossings — mark as departing (sprite vanishes, still counted here)
    for (int i = 0; i < chamber.conker_count; i++) {
        Conker& w = chamber.conkers[i];

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
            for (int j = 0; j < chamber.conker_count; j++) {
                if (chamber.conkers[j].alive && chamber.conkers[j].stack_on == cur) {
                    chamber.conkers[j].departing = true;
                    chamber.conkers[j].depart_at_ms = w.depart_at_ms;
                    chamber.conkers[j].depart_face = w.depart_face;
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
    for (int i = chamber.conker_count - 1; i >= 0; i--) {
        Conker& w = chamber.conkers[i];
        if (!w.departing || w.stack_on >= 0) continue;  // only process bottom of stack
        if (now < w.depart_at_ms) continue;

        int face = w.depart_face;
        const Neighbour& nb = topology_neighbour(static_cast<Face>(face));

        // If neighbour disconnected while waiting, cancel departure
        if (!nb.present) {
            w.departing = false;
            w.depart_face = -1;
            // Unmark stacked workers too
            for (int j = 0; j < chamber.conker_count; j++) {
                if (chamber.conkers[j].departing && chamber.conkers[j].depart_face == face)
                    chamber.conkers[j].departing = false;
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
                for (int j = 0; j < chamber.conker_count; j++) {
                    if (chamber.conkers[j].alive && chamber.conkers[j].stack_on == cur) {
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
            ConkerTransfer payload;
            conker_to_transfer(chamber.conkers[stack[s]], payload,
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
            Serial.printf("[handoff] OUT stack of %d via face %s to 0x%04X\r\n",
                sent_count, face_letter(face), nb.module_id);
        else
            Serial.printf("[handoff] OUT id=%lu via face %s to 0x%04X (state=%d food=%.1f)\r\n",
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
            je.lilguy_id = chamber.conkers[stack[s]].id;
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
            chamber.remove_conker(stack[s]);
    }
#endif
}

void Coordinator::_place_arrival(const ConkerTransfer& t, EventBus& bus,
                                  uint32_t tick_num, int* first_idx) {
    if (chamber.conker_count >= Cfg::MAX_CONKERS) return;

    uint8_t af = t.arrival_face;
    if (af >= FACE_COUNT) return;

    int8_t off = t.entry_offset;
    float ex = static_cast<float>(Cfg::ENTRY_X[af])
             + ((Cfg::FACE_DY[af] != 0) ? off : 0);
    float ey = static_cast<float>(Cfg::ENTRY_Y[af])
             + ((Cfg::FACE_DY[af] == 0) ? off : 0);
    float fdx = static_cast<float>(-Cfg::FACE_DX[af]);
    float fdy = static_cast<float>(-Cfg::FACE_DY[af]);

    int idx = chamber.conker_count;
    chamber.conker_count++;
    Conker& w = chamber.conkers[idx];
    transfer_to_conker(t, w, ex, ey, fdx, fdy);
    w.arrival_ms = millis();

    // Clear stale cross-module target — conker will recompute on next tick
    w.has_target = false;

    // Auto-stack: multiple arrivals on the same face in the same tick
    if (first_idx[af] >= 0) {
        int top = first_idx[af];
        for (int k = first_idx[af] + 1; k < idx; k++) {
            if (chamber.conkers[k].stack_on == top) top = k;
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

    Serial.printf("[handoff] IN id=%lu from 0x%04X face %s (state=%d food=%.1f%s)\r\n",
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
        if (pending[h].len < (int)sizeof(ConkerTransfer)) continue;
        const ConkerTransfer& t = *reinterpret_cast<const ConkerTransfer*>(pending[h].data);
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
        if (chamber.conker_count >= Cfg::MAX_CONKERS) {
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
                                  sizeof(ConkerTransfer));
            _pending_out[i].retries++;
            _pending_out[i].sent_ms = now;
        } else {
            // Timeout — restore ant to chamber
            int f = _pending_out[i].face;
            float ex = static_cast<float>(Cfg::ENTRY_X[f]);
            float ey = static_cast<float>(Cfg::ENTRY_Y[f]);
            float fdx = static_cast<float>(-Cfg::FACE_DX[f]);
            float fdy = static_cast<float>(-Cfg::FACE_DY[f]);

            if (chamber.conker_count < Cfg::MAX_CONKERS) {
                int idx = chamber.conker_count++;
                Conker& w = chamber.conkers[idx];
                transfer_to_conker(_pending_out[i].payload, w, ex, ey, fdx, fdy);
                w.arrival_face = static_cast<int8_t>(f);  // prevent immediate re-exit
                w.arrival_ms = millis();
                Serial.printf("[handoff] TIMEOUT -- restoring id=%lu\r\n",
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
//  Death sync + missing worker reconciliation
// ================================================================

void Coordinator::_send_death_syncs() {
#ifdef ARDUINO
    if (chamber.death_count == 0) return;
    for (int d = 0; d < chamber.death_count; d++) {
        DeathSyncMessage msg;
        msg.msg_type  = TOPO_DEATH_SYNC;
        msg.sender_id = topology_my_id();
        msg.conker_id = chamber.deaths[d].id;
        msg.cause     = chamber.deaths[d].cause;
        if (chamber.home_face >= 0) {
            topology_send_to_face(static_cast<Face>(chamber.home_face),
                                  (const uint8_t*)&msg, sizeof(msg));
            Serial.printf("[death-sync] sent id=%lu cause=%d to queen\r\n",
                          (unsigned long)msg.conker_id, msg.cause);
        }
    }
#endif
}

void Coordinator::_receive_death_syncs() {
#ifdef ARDUINO
    PendingDeathSync pending[8];
    int n = topology_drain_death_syncs(pending, 8);
    for (int i = 0; i < n; i++) {
        if (pending[i].len < (int)sizeof(DeathSyncMessage)) continue;
        const DeathSyncMessage& msg = *reinterpret_cast<const DeathSyncMessage*>(pending[i].data);
        if (msg.msg_type != TOPO_DEATH_SYNC) continue;

        registry.mark_dead(msg.conker_id, g_tod.unix_time);
        registry.manifest().total_workers_died++;
        bonds.remove_owner(msg.conker_id);

        JournalEntry je = {};
        je.tick = chamber.tick_num;
        je.unix_time = g_tod.unix_time;
        je.type = JEVT_DEATH;
        je.lilguy_id = msg.conker_id;
        je.death.cause = msg.cause;
        journal.emit(je);

        Serial.printf("[death-sync] received: id=%lu cause=%d — marked dead\r\n",
                      (unsigned long)msg.conker_id, msg.cause);
    }
#endif
}

void Coordinator::_reconcile_workers() {
#ifdef ARDUINO
    uint32_t now = millis();

    // Run every 10 seconds
    if (now - _last_reconcile_ms < 10000) return;
    _last_reconcile_ms = now;

    // Deduplicate: if two conkers share the same ID, remove the later one
    for (int i = 0; i < chamber.conker_count; i++) {
        if (!chamber.conkers[i].alive || chamber.conkers[i].id == 0) continue;
        for (int j = i + 1; j < chamber.conker_count; j++) {
            if (chamber.conkers[j].id == chamber.conkers[i].id) {
                Serial.printf("[reconcile] duplicate id=%lu — removing index %d\r\n",
                              (unsigned long)chamber.conkers[j].id, j);
                chamber.remove_conker(j);
                j--;  // recheck this index after swap-with-last
            }
        }
    }

    // For each alive conker in the registry, check if it's in the local chamber.
    // Account for satellite populations: up to remote_pop conkers may be away legitimately.
    int remote_pop = 0;
    for (int f = 0; f < FACE_COUNT; f++)
        remote_pop += topology_remote_population(static_cast<Face>(f));

    IdentityRecord* recs = registry.living_records();
    int alive_count = registry.living_count();

    // Count how many alive conkers are NOT in the local chamber
    int away_count = 0;

    for (int r = 0; r < alive_count; r++) {
        uint32_t id = recs[r].id;
        if (id == 0 || recs[r].died_unix != 0) continue;

        // Check if in local chamber
        bool in_chamber = false;
        for (int c = 0; c < chamber.conker_count; c++) {
            if (chamber.conkers[c].id == id) { in_chamber = true; break; }
        }
        if (in_chamber) {
            // Clear from missing list if present
            for (int m = 0; m < MAX_MISSING; m++) {
                if (_missing_workers[m].active && _missing_workers[m].id == id)
                    _missing_workers[m].active = false;
            }
            continue;
        }

        // Not in local chamber — could be on a satellite or lost
        away_count++;
        if (away_count <= remote_pop) continue;  // accounted for by satellite headcount

        // Unaccounted — check if already in missing list
        int slot = -1;
        bool already_tracked = false;
        for (int m = 0; m < MAX_MISSING; m++) {
            if (_missing_workers[m].active && _missing_workers[m].id == id) {
                already_tracked = true;
                // Check timeout
                if (now - _missing_workers[m].missing_since_ms >= 30000) {
                    // 30s missing — respawn
                    _missing_workers[m].active = false;
                    _respawn_worker(id, &recs[r]);
                }
                break;
            }
            if (!_missing_workers[m].active && slot < 0) slot = m;
        }
        if (!already_tracked && slot >= 0) {
            _missing_workers[slot] = {id, now, true};
            Serial.printf("[reconcile] id=%lu not in chamber or satellite — tracking\r\n",
                          (unsigned long)id);
        }
    }
#endif
}

void Coordinator::_respawn_worker(uint32_t id, IdentityRecord* rec) {
    if (chamber.conker_count >= Cfg::MAX_CONKERS) {
        Serial.printf("[reconcile] id=%lu — chamber full, cannot respawn\r\n",
                      (unsigned long)id);
        return;
    }

    int8_t qx = chamber.queen_obj.x;
    int8_t qy = chamber.queen_obj.y;
    int idx = chamber.conker_count++;
    Conker& w = chamber.conkers[idx];
    w = Conker{};
    w.id = id;
    strncpy(w.name, rec->name, sizeof(w.name) - 1);
    w.alive = true;
    w.x = static_cast<float>(qx) + 0.5f;
    w.y = static_cast<float>(qy) + 0.5f;
    w.prev_x = w.x;
    w.prev_y = w.y;
    w.role = static_cast<Role>(rec->role);
    w.is_founder = rec->is_founder;
    w.scale_factor = rec->scale_factor;
    w.tint_seed = rec->tint_seed;
    w.born_at_ms = rec->born_unix > 0 ? millis() - (g_tod.unix_time - rec->born_unix) * 1000 : millis();
    for (int p = 0; p < PERS_COUNT && p < 8; p++)
        w.personality[p] = rec->personality[p];
    w.move_ticks   = Cfg::ROLE_PARAMS[w.role].move_ticks;
    w.sense_radius = Cfg::ROLE_PARAMS[w.role].sense_radius;
    w.carry_amount = Cfg::ROLE_PARAMS[w.role].carry_amount;
    w.speed        = Cfg::ROLE_PARAMS[w.role].speed;
    w.lifespan_ms  = rec->lifespan_ms;
    w.lived_ms     = rec->lived_ms;

    Serial.printf("[reconcile] respawned id=%lu at queen\r\n", (unsigned long)id);
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

        case EVT_PARADE_STARTED: {
            int li = ev.parade.leader_idx;
            if (li < chamber.conker_count && chamber.conkers[li].alive) {
                je.type = JEVT_PLAY;
                je.lilguy_id = chamber.conkers[li].id;
                strlcpy(je.who, chamber.conkers[li].name, sizeof(je.who));
                je.play.kind = 0;  // parade
                je.play.participants = ev.parade.participants;
                journal.emit(je);
            }
            break;
        }

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

    // Ensure every worker in the chamber has an IdentityRecord.
    // Catches: id==0 (unassigned), or non-zero id with no record (handoff arrival
    // from satellite that had no persistence, or record lost on disk).
    for (int i = 0; i < chamber.conker_count; i++) {
        Conker& w = chamber.conkers[i];
        bool needs_record = (w.id == 0) || (w.id != 0 && !registry.get(w.id));
        if (!needs_record) continue;

        if (w.id == 0)
            w.id = registry.allocate_id();

        IdentityRecord rec;
        rec.id = w.id;
        name_random(rec.name, sizeof(rec.name));
        strncpy(w.name, rec.name, sizeof(w.name) - 1);
        rec.role = w.role;
        rec.is_pioneer = w.is_pioneer;
        rec.is_founder = w.is_founder;
        rec.born_unix = g_tod.unix_time;
        rec.lifespan_ms = w.lifespan_ms;
        rec.lived_ms = w.lived_ms;
        rec.scale_factor = w.scale_factor;
        rec.tint_seed = w.tint_seed;
        memcpy(rec.personality, w.personality, sizeof(rec.personality));
        rec.last_x = w.x;
        rec.last_y = w.y;
        rec.last_state = w.state;
        registry.create(rec);
        Serial.printf("[persist] created missing record for id=%lu\r\n",
                      (unsigned long)w.id);
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

    // Manifest + positions + bonds flush (every 30s — two file writes)
    uint32_t now = millis();
    static uint32_t _last_manifest_ms = 0;
    if (now - _last_manifest_ms >= 30000) {
        _last_manifest_ms = now;
        _persist_update_positions();
        _persist_sync_colony_state(tick_num);
        registry.flush_manifest();
        _bond_persist();
    }

    // Record flush: only processes records dirtied by lifecycle events (rare)
    if (now - _last_persist_flush_ms >= 5000) {
        _last_persist_flush_ms = now;
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
            rec.total_duration_ms = chamber.brood[i].total_duration_ms;
            rec.born_unix = g_tod.unix_time;
            registry.create_brood(rec);
        }
    }
}

void Coordinator::_persist_process_hatches() {
    for (int h = 0; h < chamber.hatch_count; h++) {
        uint32_t id = chamber.hatch_ids[h];
        uint32_t carer_id = chamber.hatch_tended_by[h];

        // Remove brood record
        registry.remove_brood(id);

        // Find the worker with this ID and create its IdentityRecord
        for (int i = 0; i < chamber.conker_count; i++) {
            if (chamber.conkers[i].id == id) {
                // Personality inheritance from carer (0.7 random + 0.3 carer)
                if (carer_id != 0) {
                    IdentityRecord* carer_rec = registry.get(carer_id);
                    if (carer_rec) {
                        for (int d = 0; d < PERS_COUNT; d++) {
                            float random_part = chamber.conkers[i].personality[d];
                            chamber.conkers[i].personality[d] =
                                0.7f * random_part + 0.3f * carer_rec->personality[d];
                        }
                    }
                    // Initial bond: child → carer at 0.3
                    bonds.set(id, carer_id, 0.3f);
                }

                IdentityRecord rec;
                rec.id = id;
                name_random(rec.name, sizeof(rec.name));
                strncpy(chamber.conkers[i].name, rec.name, sizeof(chamber.conkers[i].name) - 1);
                rec.role = chamber.conkers[i].role;
                rec.is_pioneer = chamber.conkers[i].is_pioneer;
                rec.is_founder = chamber.conkers[i].is_founder;
                rec.born_unix = g_tod.unix_time;
                rec.lifespan_ms = chamber.conkers[i].lifespan_ms;
                rec.lived_ms = chamber.conkers[i].lived_ms;
                rec.scale_factor = chamber.conkers[i].scale_factor;
                rec.tint_seed = chamber.conkers[i].tint_seed;
                rec.tended_by = carer_id;
                memcpy(rec.personality, chamber.conkers[i].personality, sizeof(rec.personality));
                rec.last_x = chamber.conkers[i].x;
                rec.last_y = chamber.conkers[i].y;
                rec.last_state = chamber.conkers[i].state;
                registry.create(rec);

                // Journal: hatch event
                JournalEntry je = {};
                je.tick = chamber.tick_num;
                je.unix_time = g_tod.unix_time;
                je.type = JEVT_HATCH;
                je.lilguy_id = id;
                strlcpy(je.who, chamber.conkers[i].name, sizeof(je.who));
                je.hatch.role = chamber.conkers[i].role;
                je.hatch.is_pioneer = chamber.conkers[i].is_founder;
                je.hatch.from_brood_id = id;
                journal.emit(je);
                break;
            }
        }
    }
}

// ================================================================
//  Remote commands (companion app -> VPS queue -> queen)
// ================================================================

bool Coordinator::cmd_rename_conker(uint32_t id, const char* new_name) {
    if (!new_name || !new_name[0]) return false;

    // Sanitize: printable ASCII only, cap to record size
    char clean[16] = {};
    int j = 0;
    for (int i = 0; new_name[i] && j < 15; i++) {
        char ch = new_name[i];
        if (ch >= 32 && ch < 127) clean[j++] = ch;
    }
    if (j == 0) return false;

    IdentityRecord* rec = registry.get(id);
    if (!rec) return false;

    strlcpy(rec->name, clean, sizeof(rec->name));
    rec->dirty = true;

    // Live conker in this chamber too (satellite copies pick it up on handoff)
    for (int i = 0; i < chamber.conker_count; i++) {
        if (chamber.conkers[i].id == id) {
            strlcpy(chamber.conkers[i].name, clean, sizeof(chamber.conkers[i].name));
            break;
        }
    }
    Serial.printf("[cmd] conker %lu renamed to '%s'\r\n", (unsigned long)id, clean);
    return true;
}

bool Coordinator::cmd_feed_colony(float amount) {
    // A care package is a day's table for the whole colony — sized here,
    // not by the client, so it scales with the family. (Client amount is
    // ignored; kept in the signature for command compatibility.)
    (void)amount;
    amount = colony.daily_burn();
    if (amount <= 0.0f) amount = Cfg::QUEEN_FOOD_PER_DAY;
    if (amount > 100.0f) amount = 100.0f;  // sanity rail

    // A gift from above: drop a visible pile near the chamber centre —
    // foragers will discover it and lay trails like any other find
    int cx = Cfg::GRID_WIDTH / 2 + g_rng.rand_int(-5, 5);
    int cy = Cfg::GRID_HEIGHT / 2 + g_rng.rand_int(-4, 4);
    chamber.add_food(cx, cy, amount);
    Serial.printf("[cmd] care package: %.0fu at (%d,%d)\r\n", amount, cx, cy);

    JournalEntry je = {};
    je.tick = chamber.tick_num;
    je.unix_time = g_tod.unix_time;
    je.type = JEVT_FOOD_TAP;
    je.food_tap = {static_cast<int8_t>(cx), static_cast<int8_t>(cy), amount};
    journal.emit(je);
    return true;
}

bool Coordinator::cmd_set_module_role(uint16_t target_id, uint8_t new_role) {
    // Queen role is never assignable remotely — demoting the queen would
    // orphan the colony. Queen<->satellite transitions stay on serial.
    if (new_role <= MODULE_QUEEN || new_role >= MODULE_ROLE_COUNT) {
        Serial.printf("[cmd] set_role: role %d not assignable\r\n", new_role);
        return false;
    }
#ifdef ARDUINO
    if (target_id == topology_my_id()) {
        Serial.println("[cmd] set_role: refusing to re-role the queen");
        return false;
    }

    for (int f = 0; f < FACE_COUNT; f++) {
        const Neighbour& nb = topology_neighbour(static_cast<Face>(f));
        if (nb.present && nb.module_id == target_id) {
            SetRoleMessage msg;
            msg.msg_type  = TOPO_SET_ROLE;
            msg.sender_id = topology_my_id();
            msg.target_id = target_id;
            msg.role      = new_role;
            bool ok = topology_send_to_face(static_cast<Face>(f),
                                            (const uint8_t*)&msg, sizeof(msg));
            Serial.printf("[cmd] set_role: 0x%04X -> %s (%s)\r\n",
                          target_id, module_role_str(new_role),
                          ok ? "sent" : "send failed");
            return ok;
        }
    }
    Serial.printf("[cmd] set_role: module 0x%04X not connected\r\n", target_id);
#endif
    return false;
}

bool Coordinator::cmd_set_floor_tint(uint16_t target_id, uint8_t r, uint8_t g, uint8_t b) {
#ifdef ARDUINO
    if (target_id == topology_my_id()) {
        renderer_set_floor_tint(r, g, b, true);
        return true;
    }
    for (int f = 0; f < FACE_COUNT; f++) {
        const Neighbour& nb = topology_neighbour(static_cast<Face>(f));
        if (nb.present && nb.module_id == target_id) {
            SetTintMessage msg;
            msg.msg_type  = TOPO_SET_TINT;
            msg.sender_id = topology_my_id();
            msg.target_id = target_id;
            msg.r = r; msg.g = g; msg.b = b;
            bool ok = topology_send_to_face(static_cast<Face>(f),
                                            (const uint8_t*)&msg, sizeof(msg));
            Serial.printf("[cmd] set_tint: 0x%04X -> #%02X%02X%02X (%s)\r\n",
                          target_id, r, g, b, ok ? "sent" : "send failed");
            return ok;
        }
    }
    Serial.printf("[cmd] set_tint: module 0x%04X not connected\r\n", target_id);
#endif
    return false;
}

void Coordinator::_persist_process_deaths() {
    for (int d = 0; d < chamber.death_count; d++) {
        uint32_t id = chamber.deaths[d].id;
        uint8_t cause = chamber.deaths[d].cause;

        // Mourning: bonded partners pay respects at the husk. Must run
        // before bonds.remove_owner() erases the relationships. Skipped
        // when the colony is stressed — survival first.
        if (colony.food_pressure() <= Cfg::FLAIR_MAX_PRESSURE) {
            int8_t hx = -1, hy = -1;
            for (int h = chamber.husk_count - 1; h >= 0; h--) {
                if (chamber.husks[h].conker_id == id) {
                    hx = chamber.husks[h].x; hy = chamber.husks[h].y;
                    break;
                }
            }
            BondEntry bs[8];
            int bc = bonds.get_bonds(id, bs, 8);
            int mourners = 0;
            for (int b = 0; b < bc && hx >= 0
                    && mourners < Cfg::MOURN_MAX_PARTNERS; b++) {
                if (bs[b].strength < Cfg::MOURN_MIN_BOND) continue;
                for (int i = 0; i < chamber.conker_count; i++) {
                    Conker& w = chamber.conkers[i];
                    if (w.id != bs[b].target || !w.alive) continue;
                    // Never interrupt a carrier or a departing worker
                    if (w.food_carried > 0 || w.departing) break;
                    w.state = STATE_MOURNING;
                    w.target_x = hx; w.target_y = hy;
                    w.has_target = true;
                    w.has_target_cell = false;
                    w.zoomie_ticks = Cfg::MOURN_DURATION_TICKS;
                    w.zoomie_target = -1;
                    w.sleeping = false;
                    w.stack_on = -1;
                    w.idle_ticks_remaining = 0;
                    w.anim_type = LG_ANIM_NONE;
                    w.anim_remaining_ticks = 0;
                    w.speed = Cfg::ROLE_PARAMS[w.role].speed;
                    mourners++;

                    JournalEntry jm = {};
                    jm.tick = chamber.tick_num;
                    jm.unix_time = g_tod.unix_time;
                    jm.type = JEVT_MOURNING;
                    jm.lilguy_id = w.id;
                    strlcpy(jm.who, w.name, sizeof(jm.who));
                    jm.mourning.dead_id = id;
                    journal.emit(jm);
                    break;
                }
            }
        }

        registry.mark_dead(id, g_tod.unix_time);
        registry.manifest().total_workers_died++;
        bonds.remove_owner(id);

        // Journal: death event (name rides along — the roster forgets the dead)
        JournalEntry je = {};
        je.tick = chamber.tick_num;
        je.unix_time = g_tod.unix_time;
        je.type = JEVT_DEATH;
        je.lilguy_id = id;
        {
            IdentityRecord* rec = registry.get(id);
            if (rec) strlcpy(je.who, rec->name, sizeof(je.who));
        }
        je.death.cause = cause;
        journal.emit(je);
    }
}

void Coordinator::_persist_update_positions() {
    // Write positions into manifest's positions array (RAM only, flushed with manifest)
    ColonyManifest& m = registry.manifest();
    m.pos_count = 0;
    for (int i = 0; i < chamber.conker_count && m.pos_count < ColonyManifest::MAX_POS; i++) {
        Conker& w = chamber.conkers[i];
        if (w.id == 0) continue;
        m.positions[m.pos_count++] = {w.id, w.x, w.y};
        // Sync lived_ms to identity record (ageing source of truth)
        IdentityRecord* rec = registry.get(w.id);
        if (rec) {
            rec->lived_ms = w.lived_ms;
            if (rec->tint_seed == 0 && w.tint_seed != 0)
                rec->tint_seed = w.tint_seed;
            rec->dirty = true;
        }
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
    name_random(m.queen_name, sizeof(m.queen_name));
    Serial.printf("[persist] the queen is named %s\r\n", m.queen_name);

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
    for (int i = 0; i < chamber.conker_count; i++) {
        Conker& w = chamber.conkers[i];
        w.id = registry.allocate_id();
        IdentityRecord rec;
        rec.id = w.id;
        name_random(rec.name, sizeof(rec.name));
        strncpy(w.name, rec.name, sizeof(w.name) - 1);
        rec.role = w.role;
        rec.is_pioneer = w.is_pioneer;
        rec.born_unix = m.founded_unix;  // approximate
        rec.lifespan_ms = w.lifespan_ms;
        memcpy(rec.personality, w.personality, sizeof(rec.personality));
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
    registry.flush_manifest();  // re-save manifest with queen state set above

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

    Serial.printf("[persist] migration complete — %d workers, %d brood, colony_id=%s\r\n",
                  chamber.conker_count, chamber.brood_count, m.colony_id);
}

void Coordinator::_persist_restore_from_disk() {
    // Case C: manifest exists — restore colony from disk.
    Serial.println("[persist] restoring colony from disk (Case C)...");

    ColonyManifest& m = registry.manifest();

    // Migration: colonies founded before v98 have an unnamed queen
    if (m.queen_name[0] == '\0') {
        name_random(m.queen_name, sizeof(m.queen_name));
        registry.flush_manifest();
        Serial.printf("[persist] queen named retroactively: %s\r\n", m.queen_name);
    }

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
    chamber.conker_count = 0;

    for (int i = 0; i < rec_count && chamber.conker_count < Cfg::MAX_CONKERS; i++) {
        IdentityRecord& r = recs[i];
        int idx = chamber.conker_count;

        // Find position from manifest's live positions array
        float pos_x = r.last_x, pos_y = r.last_y;
        for (int p = 0; p < m.pos_count; p++) {
            if (m.positions[p].id == r.id) {
                pos_x = m.positions[p].x;
                pos_y = m.positions[p].y;
                break;
            }
        }

        chamber.conkers[idx].init(
            static_cast<int8_t>(pos_x),
            static_cast<int8_t>(pos_y),
            ROLE_CONKER,
            false
        );
        chamber.conkers[idx].id = r.id;
        chamber.conkers[idx].is_founder = r.is_founder;
        // Restore float position
        chamber.conkers[idx].x = pos_x;
        chamber.conkers[idx].y = pos_y;
        chamber.conkers[idx].prev_x = pos_x;
        chamber.conkers[idx].prev_y = pos_y;
        // Restore lifespan (init randomizes it — override with persisted value)
        if (r.lifespan_ms > 0)
            chamber.conkers[idx].lifespan_ms = r.lifespan_ms;
        // Restore lived_ms (ageing source of truth — no wall-clock reconstruction)
        chamber.conkers[idx].lived_ms = r.lived_ms;
        // Restore scale_factor (init randomizes it — override with persisted value)
        if (r.scale_factor > 0.0f)
            chamber.conkers[idx].scale_factor = r.scale_factor;
        // Restore tint_seed (init randomizes it — override with persisted value)
        if (r.tint_seed > 0)
            chamber.conkers[idx].tint_seed = r.tint_seed;
        // Restore name from registry
        strncpy(chamber.conkers[idx].name, r.name, sizeof(chamber.conkers[idx].name) - 1);
        // Restore personality (init randomizes it — override with persisted value)
        memcpy(chamber.conkers[idx].personality, r.personality,
               sizeof(chamber.conkers[idx].personality));
        chamber.conker_count++;
    }

    // Restore brood
    BroodRecord* brecs = registry.brood_records();
    int brood_count = registry.brood_count();
    chamber.brood_count = 0;

    for (int i = 0; i < brood_count && chamber.brood_count < Cfg::MAX_BROOD; i++) {
        BroodRecord& br = brecs[i];
        int idx = chamber.brood_count;
        chamber.brood[idx].init(br.x, br.y, ROLE_CONKER);
        chamber.brood[idx].id = br.id;
        // Map legacy STAGE_PUPA (2) to STAGE_SEED (1) for old records
        uint8_t stage_raw = br.stage;
        if (stage_raw == 2) stage_raw = 1;  // PUPA -> SEED
        chamber.brood[idx].stage = static_cast<BroodStage>(stage_raw);
        chamber.brood[idx].hunger = br.hunger;
        chamber.brood[idx].food_invested = br.food_invested;
        chamber.brood[idx].total_duration_ms = br.total_duration_ms;
        // stage_start_ms already converted from elapsed to absolute in _load_brood_records
        chamber.brood[idx].stage_start_ms = br.stage_start_ms;
        chamber.brood_count++;
    }

    colony.population = chamber.conker_count;

    Serial.printf("[persist] restored %d workers, %d brood, food=%.0f\r\n",
                  chamber.conker_count, chamber.brood_count, colony.food_store);
}

// ================================================================
//  Bonds
// ================================================================

void Coordinator::_bond_tick(uint32_t tick_num) {
    // Decay every 1000 ticks (~2 min)
    if (tick_num - _bond_decay_tick >= BondStore::DECAY_INTERVAL) {
        _bond_decay_tick = tick_num;
        uint32_t broken_owners[16], broken_targets[16];
        int broken_count = 0;
        bonds.decay(broken_owners, broken_targets, broken_count, 16);

        for (int i = 0; i < broken_count; i++) {
            JournalEntry je = {};
            je.tick = tick_num;
            je.unix_time = g_tod.unix_time;
            je.type = JEVT_BOND_BROKEN;
            je.lilguy_id = broken_owners[i];
            je.bond.target_id = broken_targets[i];
            IdentityRecord* ro = registry.get(broken_owners[i]);
            if (ro) strlcpy(je.who, ro->name, sizeof(je.who));
            IdentityRecord* rt = registry.get(broken_targets[i]);
            if (rt) strlcpy(je.bond.target_name, rt->name, sizeof(je.bond.target_name));
            journal.emit(je);
        }
    }

    // Proximity-based bond formation every 200 ticks (~25s)
    if (tick_num - _bond_proximity_tick >= 200) {
        _bond_proximity_tick = tick_num;
        _bond_detect_proximity(tick_num);
    }
}

void Coordinator::_bond_detect_proximity(uint32_t tick_num) {
    // Scan for worker pairs within 3 cells that are both active (non-idle)
    for (int i = 0; i < chamber.conker_count; i++) {
        Conker& a = chamber.conkers[i];
        if (a.id == 0 || a.departing || !a.alive) continue;

        for (int j = i + 1; j < chamber.conker_count; j++) {
            Conker& b = chamber.conkers[j];
            if (b.id == 0 || b.departing || !b.alive) continue;

            int dist = abs(a.cell_x() - b.cell_x()) + abs(a.cell_y() - b.cell_y());
            if (dist > 3) continue;

            float increment = 0.0f;

            // Co-tending: both in TEND_BROOD targeting same cell
            if (a.state == STATE_TEND_BROOD && b.state == STATE_TEND_BROOD
                && a.target_x == b.target_x && a.target_y == b.target_y) {
                increment = 0.04f;
            }
            // Co-foraging: both in TO_FOOD or TO_HOME (actively working)
            else if ((a.state == STATE_TO_FOOD || a.state == STATE_TO_HOME)
                  && (b.state == STATE_TO_FOOD || b.state == STATE_TO_HOME)) {
                increment = 0.01f;
            }
            // Idle proximity
            else if (a.state == STATE_IDLE && b.state == STATE_IDLE) {
                increment = 0.003f;
            }

            if (increment > 0) {
                bool formed_ab = bonds.increment(a.id, b.id, increment);
                bool formed_ba = bonds.increment(b.id, a.id, increment);

                if (formed_ab) {
                    JournalEntry je = {};
                    je.tick = tick_num;
                    je.unix_time = g_tod.unix_time;
                    je.type = JEVT_BOND_FORMED;
                    je.lilguy_id = a.id;
                    strlcpy(je.who, a.name, sizeof(je.who));
                    je.bond.target_id = b.id;
                    strlcpy(je.bond.target_name, b.name, sizeof(je.bond.target_name));
                    journal.emit(je);
                }
                if (formed_ba) {
                    JournalEntry je = {};
                    je.tick = tick_num;
                    je.unix_time = g_tod.unix_time;
                    je.type = JEVT_BOND_FORMED;
                    je.lilguy_id = b.id;
                    strlcpy(je.who, b.name, sizeof(je.who));
                    je.bond.target_id = a.id;
                    strlcpy(je.bond.target_name, a.name, sizeof(je.bond.target_name));
                    journal.emit(je);
                }
            }
        }
    }
}

void Coordinator::_bond_persist() {
    if (sd_card_state() != SD_OK || bonds.count() == 0) return;

    // Write all bonds to a single file (avoids per-worker file stutter)
    JsonDocument doc;
    doc["schema"] = 1;
    JsonArray arr = doc["bonds"].to<JsonArray>();
    const BondEntry* pool = bonds.pool();
    for (int i = 0; i < bonds.count(); i++) {
        JsonObject b = arr.add<JsonObject>();
        b["o"] = pool[i].owner;
        b["t"] = pool[i].target;
        b["s"] = pool[i].strength;
        b["f"] = pool[i].formed;
    }

    size_t buf_size = 128 + bonds.count() * 32;
    char* buf = (char*)malloc(buf_size);
    if (!buf) return;
    size_t len = serializeJson(doc, buf, buf_size);

    // Atomic write
    File f = SD_MMC.open("/colony/bonds.json.tmp", FILE_WRITE);
    if (f) {
        f.write((const uint8_t*)buf, len);
        f.flush();
        f.close();
        if (SD_MMC.exists("/colony/bonds.json")) SD_MMC.remove("/colony/bonds.json");
        SD_MMC.rename("/colony/bonds.json.tmp", "/colony/bonds.json");
        sd_card_write_ok();
    }
    free(buf);
}

void Coordinator::_bond_load() {
    File f = SD_MMC.open("/colony/bonds.json", FILE_READ);
    if (!f) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return;

    JsonArray arr = doc["bonds"];
    int n = 0;
    static BondEntry entries[BondStore::POOL_CAP];
    for (size_t i = 0; i < arr.size() && n < BondStore::POOL_CAP; i++) {
        JsonObject b = arr[i];
        float strength = b["s"] | 0.0f;
        entries[n++] = {
            b["o"] | (uint32_t)0,
            b["t"] | (uint32_t)0,
            strength,
            // Older files lack "f" — heal by treating strong bonds as formed
            b["f"] | (strength >= BondStore::FORM_THRESHOLD)
        };
    }
    bonds.load(entries, n);
    Serial.printf("[bonds] loaded %d bonds\r\n", n);
}

// ================================================================
//  WorldCondition + Traits + Challenges
// ================================================================

void Coordinator::_world_tick() {
    // Larder cap: the colony can only store so much — excess spoils.
    // Single chokepoint for every food source (taps, packages, restores).
    float larder_cap = colony.daily_burn() * Cfg::LARDER_CAP_DAYS;
    if (larder_cap > 0.0f) {
        if (colony.food_store > larder_cap) colony.food_store = larder_cap;
        if (colony.food_total > larder_cap) colony.food_total = larder_cap;
    }

    world.tod = g_tod;
    world.day_of_year = g_tod.day_of_year;

    // Wire real weather data into world condition
    if (g_weather.valid) {
        world.temperature_c = g_weather.temperature_c;
        world.humidity_pct = g_weather.humidity_pct;
        world.weather = g_weather.condition;
    }

    // Season from day of year (Northern hemisphere, Scotland)
    int doy = g_tod.day_of_year;
    if (doy >= 60 && doy < 152)       world.season = SEASON_SPRING;   // ~Mar 1 – May 31
    else if (doy >= 152 && doy < 244) world.season = SEASON_SUMMER;   // ~Jun 1 – Aug 31
    else if (doy >= 244 && doy < 335) world.season = SEASON_AUTUMN;   // ~Sep 1 – Nov 30
    else                                world.season = SEASON_WINTER;   // ~Dec 1 – Feb 28
}

void Coordinator::_trait_tick(uint32_t tick_num) {
    // Check every 500 ticks (~1 min) to avoid per-tick overhead
    if (tick_num - _trait_check_tick < 500) return;
    _trait_check_tick = tick_num;

    for (int i = 0; i < chamber.conker_count; i++) {
        Conker& w = chamber.conkers[i];
        if (w.id == 0 || !w.alive) continue;

        IdentityRecord* rec = registry.get(w.id);
        if (!rec) continue;

        uint32_t prev_traits = rec->traits;

        // Pioneer: set if is_pioneer flag (already determined at hatch)
        if (w.is_pioneer) rec->traits |= TRAIT_PIONEER;

        // Elder: age > 80% of typical lifespan
        if (w.lifespan_ms > 0) {
            uint32_t age_ms = millis() - w.born_at_ms;
            if (age_ms > w.lifespan_ms * 4 / 5)
                rec->traits |= TRAIT_ELDER;
        }

        // Bonded: any bond >= 0.6 (hysteresis: clear below 0.4)
        BondEntry worker_bonds[16];
        int bond_count = bonds.get_bonds(w.id, worker_bonds, 16);
        bool has_strong_bond = false;
        for (int b = 0; b < bond_count; b++) {
            if (worker_bonds[b].strength >= 0.6f) {
                has_strong_bond = true;
                break;
            }
        }
        if (has_strong_bond) {
            rec->traits |= TRAIT_BONDED;
        } else if (rec->traits & TRAIT_BONDED) {
            // Check hysteresis: clear only if all below 0.4
            bool any_above = false;
            for (int b = 0; b < bond_count; b++) {
                if (worker_bonds[b].strength >= 0.4f) { any_above = true; break; }
            }
            if (!any_above) rec->traits &= ~TRAIT_BONDED;
        }

        // Emit trait_earned for newly set bits
        uint32_t new_bits = rec->traits & ~prev_traits;
        if (new_bits) {
            for (int bit = 0; bit < 7; bit++) {
                if (new_bits & (1u << bit)) {
                    JournalEntry je = {};
                    je.tick = tick_num;
                    je.unix_time = g_tod.unix_time;
                    je.type = JEVT_TRAIT_EARNED;
                    je.lilguy_id = w.id;
                    je.trait.trait_bit = (1u << bit);
                    journal.emit(je);
                }
            }
            rec->dirty = true;
        }
    }
}

void Coordinator::challenge_start(uint8_t type, float severity, uint32_t tick_num) {
    int idx = world.start_challenge(type, severity, tick_num, g_tod.unix_time);
    if (idx < 0) {
        Serial.println("[challenge] max active challenges reached");
        return;
    }

    // Journal event
    JournalEntry je = {};
    je.tick = tick_num;
    je.unix_time = g_tod.unix_time;
    je.type = JEVT_CHALLENGE_START;
    je.lilguy_id = 0;
    je.challenge.challenge_type = type;
    je.challenge.severity = severity;
    journal.emit(je);

    // Mark all living workers as participants (set challenge bit on traits temporarily)
    // We use the survival trait bit as a "participating" marker;
    // on end, if still alive, they keep it (survived). On death during, it's cleared.
    Serial.printf("[challenge] started type=%d severity=%.1f, %d participants\r\n",
                  type, severity, chamber.conker_count);
}

void Coordinator::challenge_end(uint32_t tick_num) {
    if (world.active_count == 0) {
        Serial.println("[challenge] no active challenge to end");
        return;
    }

    // End the most recent challenge
    ActiveChallenge ended = world.end_challenge(world.active_count - 1);

    // Journal event
    JournalEntry je = {};
    je.tick = tick_num;
    je.unix_time = g_tod.unix_time;
    je.type = JEVT_CHALLENGE_END;
    je.lilguy_id = 0;
    je.challenge.challenge_type = ended.type;
    je.challenge.severity = ended.severity;
    journal.emit(je);

    // Award survival trait to all currently alive workers
    uint32_t survival_bit = challenge_survival_trait(ended.type);
    if (survival_bit == 0) return;

    int survivors = 0;
    for (int i = 0; i < chamber.conker_count; i++) {
        Conker& w = chamber.conkers[i];
        if (w.id == 0 || !w.alive) continue;
        IdentityRecord* rec = registry.get(w.id);
        if (!rec) continue;
        if (!(rec->traits & survival_bit)) {
            rec->traits |= survival_bit;
            rec->dirty = true;
            survivors++;

            JournalEntry te = {};
            te.tick = tick_num;
            te.unix_time = g_tod.unix_time;
            te.type = JEVT_TRAIT_EARNED;
            te.lilguy_id = w.id;
            te.trait.trait_bit = survival_bit;
            journal.emit(te);
        }
    }

    Serial.printf("[challenge] ended type=%d, %d survivors earned trait\r\n",
                  ended.type, survivors);
}
