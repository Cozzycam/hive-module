/* Coordinator — orchestrates chambers and colony-wide state. */
#include "coordinator.h"
#include "chores.h"
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
    chamber.bonds = &bonds;        // let behaviour read friendships

#ifdef ARDUINO
    // Restore the followed list (app pins → on-glass stars)
    {
        Preferences fp;
        fp.begin("hive", true);
        size_t len = fp.getBytes("followed", chamber.followed,
                                 sizeof(chamber.followed));
        fp.end();
        chamber.followed_count = (uint8_t)(len / sizeof(uint32_t));
    }
#endif
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
    chamber.is_garden = (role == MODULE_GARDEN);
    _bus              = &bus;

    // Sync topology neighbour state into chamber entries
    _sync_topology_to_chamber();

    // ---- Receive incoming handoffs (workers arriving from other modules) ----
    _receive_handoffs(bus, tick_num);

    // ---- Receive death syncs from satellites (queen only) ----
    if (is_queen()) _receive_death_syncs();

    // ---- Receive satellite story beats for the journal (queen only) ----
    if (is_queen()) _receive_journal_relays();

    // ---- Anniversary visits to memorials (all modules) ----
    _anniversary_tick(tick_num);

    // ---- Fill a vacant garden post next door (queen only) ----
    if (is_queen()) _gardener_summon_tick();

    // ---- Garden staffs itself from passing traffic (garden satellite) ----
    _garden_draft_tick();

    // ---- Receive care packages from neighbouring kingdoms (queen only) ----
    if (is_queen()) _receive_gifts();

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
    if (is_queen()) _world_tick(tick_num);

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

    // Pooled population cap: POP_CAP_PER_MODULE living workers per connected
    // (non-foreign) module. Only the queen lays/hatches, so this is computed and
    // enforced here; the chamber gates hatching on colony.population >= pop_cap.
    int modules = 1;
#ifdef ARDUINO
    if (is_queen()) {
        for (int f = 0; f < FACE_COUNT; f++)
            if (topology_neighbour(static_cast<Face>(f)).present && !foreign_face[f]) modules++;
    }
#endif
    colony.pop_cap = (uint16_t)(Cfg::POP_CAP_PER_MODULE * modules);

    bool dormant = (colony.population >= colony.pop_cap)
                && (colony.brood_egg + colony.brood_seed > 0);
    if (dormant && !colony.eggs_dormant) {
        // Rising edge — journal a colony event so the app can surface the notice.
        JournalEntry je = {};
        je.tick = 0;
        je.unix_time = g_tod.unix_time;
        je.type = JEVT_COLONY_EVENT;
        je.lilguy_id = 0;
        je.colony_event.kind = COLONY_EGGS_DORMANT;
        je.colony_event.module_id = 0;
        journal.emit(je);
    }
    colony.eggs_dormant = dormant;

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
    // Garden post vacancy — daytime only; the queen answers with a green thumb
    msg.gardener_wanted = (role == MODULE_GARDEN && g_tod.phase == PHASE_DAY
                           && !chamber.garden_post_filled()) ? 1 : 0;
    {
        uint32_t tint = renderer_get_floor_tint();
        msg.tint_r = (tint >> 16) & 0xFF;
        msg.tint_g = (tint >> 8) & 0xFF;
        msg.tint_b = tint & 0xFF;
    }

    // Beacon to every present neighbour, not just open entries — pop sync is
    // the satellite-hood proof that opens a default-closed face in the first
    // place, so it must flow before the face is open.
    for (int f = 0; f < FACE_COUNT; f++) {
        if (topology_neighbour(static_cast<Face>(f)).present) {
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
    msg.unix_time    = g_tod.unix_time;

    for (int f = 0; f < FACE_COUNT; f++) {
        if (chamber.entries[f] >= 0) {
            topology_send_to_face(static_cast<Face>(f),
                                  (const uint8_t*)&msg, sizeof(msg));
        }
    }
#endif
}

void Coordinator::_send_announce(Face face, uint16_t module_id) {
#ifdef ARDUINO
    AnnounceMessage ann;
    ann.msg_type       = TOPO_ANNOUNCE;
    ann.parent_id      = topology_my_id();
    ann.parent_face    = face;
    ann.your_id        = module_id;
    ann.your_home_face = Cfg::FACE_OPPOSITE[face];
    ann.boot_id        = boot_id;
    topology_send_to_face(face, (const uint8_t*)&ann, sizeof(ann));
#endif
}

void Coordinator::on_topology_change(Face face, bool connected, uint16_t module_id) {
#ifdef ARDUINO
    if (is_queen() && connected) {
        // Queen: announce chamber to new satellite
        uint8_t satellite_home = Cfg::FACE_OPPOSITE[face];

        _send_announce(face, module_id);

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

    // Any disconnect reopens the face for whoever connects next
    if (!connected && face >= 0 && face < FACE_COUNT) foreign_face[face] = false;

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
    // Default-closed borders: presence alone never opens a face. A queen
    // opens a face only once the neighbour has proven itself a satellite
    // (pop sync beacon — foreign queens never send one). A satellite opens
    // its home face (proven by ANNOUNCE) and satellite-satellite faces on
    // the same pop-sync proof.
    for (int f = 0; f < FACE_COUNT; f++) {
        const Neighbour& nb = topology_neighbour(static_cast<Face>(f));
        bool open = nb.present && !foreign_face[f];
        if (open) {
            if (is_queen())
                open = topology_pop_sync_fresh(static_cast<Face>(f));
            else
                open = (f == chamber.home_face)
                    || topology_pop_sync_fresh(static_cast<Face>(f));
        }
        chamber.entries[f] = open ? static_cast<int8_t>(nb.module_id & 0x7F) : -1;
    }

    // Re-announce to every connected face every 5s. The connect-time ANNOUNCE
    // is a single packet; if it's lost, a satellite never learns its home face
    // and a neighbouring queen is never recognised as foreign. Idempotent on
    // the receiver (same boot_id), so repetition is safe.
    if (is_queen()) {
        uint32_t now = millis();
        if (now - _last_announce_refresh_ms >= 5000) {
            _last_announce_refresh_ms = now;
            for (int f = 0; f < FACE_COUNT; f++) {
                const Neighbour& nb = topology_neighbour(static_cast<Face>(f));
                if (nb.present) _send_announce(static_cast<Face>(f), nb.module_id);
            }
        }
    }

    // Queen: an ANNOUNCE can only come from another queen claiming us as
    // her satellite. We are nobody's satellite — recognise the neighbouring
    // kingdom and close the border before any workers emigrate.
    if (is_queen()) {
        AnnounceMessage ann;
        if (topology_has_announce(&ann)) {
            for (int f = 0; f < FACE_COUNT; f++) {
                const Neighbour& nb = topology_neighbour(static_cast<Face>(f));
                if (nb.present && nb.module_id == ann.parent_id
                        && !foreign_face[f]) {
                    foreign_face[f] = true;
                    chamber.entries[f] = -1;
                    Serial.printf("[coord] neighbouring queen 0x%04X on face %s"
                                  " — border closed (two sovereign colonies)\r\n",
                                  ann.parent_id, face_letter(f));
                    JournalEntry je = {};
                    je.tick = chamber.tick_num;
                    je.unix_time = g_tod.unix_time;
                    je.type = JEVT_COLONY_EVENT;
                    je.colony_event.kind = COLONY_FOREIGN_QUEEN;
                    je.colony_event.module_id = ann.parent_id;
                    journal.emit(je);
                }
            }
        }
    }

    // Satellite: process chamber announcement from queen
    if (!is_queen()) {
        AnnounceMessage ann;
        if (topology_has_announce(&ann)) {
            // Hijack guard: while our queen's face is live, refuse a claim
            // from any other module (a foreign queen announces to anything
            // that docks with her).
            bool home_live = chamber.home_face >= 0 && chamber.home_face < FACE_COUNT
                && topology_neighbour(static_cast<Face>(chamber.home_face)).present;
            uint16_t home_id = home_live
                ? topology_neighbour(static_cast<Face>(chamber.home_face)).module_id : 0;
            if (home_live && ann.parent_id != home_id) {
                Serial.printf("[coord] ignoring announce from 0x%04X — already serving queen 0x%04X\r\n",
                              ann.parent_id, home_id);
                return;
            }
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

        // Also mark stacked workers (hop-bounded against stack_on cycles)
        int cur = i;
        bool more = true;
        int hops = 0;
        while (more && ++hops <= Cfg::MAX_CONKERS) {
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

        // If the neighbour disconnected — or the border closed (foreign queen
        // recognised mid-delay) — cancel the departure
        if (!nb.present || chamber.entries[face] < 0) {
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

    // Returning hat-wearer: the world may have moved on while they were
    // away. Queen-only (bonds + registry live here; a satellite would
    // misread its empty registry as "maker gone"). Maker gone from the
    // living roster = they died while the wearer was away — the bond can
    // no longer break, the hat becomes a memorial. Bond formally broken
    // while away = the hat comes off quietly (no banner — it happened
    // out of sight).
    if (is_queen() && w.accessory != 0 && !w.accessory_memorial
            && w.accessory_from != 0) {
        if (!registry.get(w.accessory_from)) {
            w.accessory_memorial = true;
        } else if (!bonds.is_formed(w.id, w.accessory_from)) {
            w.accessory = 0; w.accessory_tint = 0; w.accessory_from = 0;
        }
        // registry copy of the wearer syncs on the next persist tick
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

        // Border guard: never place a worker from an unknown sender or across
        // a foreign border. No ACK — the sender times out and restores the
        // worker at home, so nobody is lost or duplicated.
        if (src_face < 0 || foreign_face[src_face]) {
            g_handoffs_dropped++;
            Serial.printf("[handoff] REJECTED id=%lu from 0x%04X (%s)\r\n",
                          (unsigned long)t.conker_id, t.sender_id,
                          src_face < 0 ? "unknown sender" : "foreign border");
            continue;
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
    // Seed tiredness from time of day, jittered per conker so a night reboot
    // doesn't bunch everyone at identical max-tired (they wake staggered).
    w.needs[NEED_REST] = g_tod.night_factor * (0.8f + 0.2f * g_rng.rand_float());
    // Heal a nameless record before copying (see restore path) — else the
    // respawned conker renders "???" and never gets named again.
    if (rec->name[0] == '\0') {
        registry.pick_unique_name(rec->name, sizeof(rec->name));
        rec->dirty = true;
        Serial.printf("[reconcile] id=%lu record had no name — healed to %s\r\n",
                      (unsigned long)id, rec->name);
    }
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
    w.speed        = w.base_speed();

    // Records have been seen with lifespan_ms == 0 or lived >= lifespan (the
    // boot-restore path guards this too). A respawn must never materialise a
    // conker that is instantly past its lifespan — that reads as a spurious
    // "old age" death seconds after the respawn (killed lilguys 3 & 6,
    // 2026-06-12). Heal the record, don't just the copy.
    if (rec->lifespan_ms == 0) {
        float days = g_rng.rand_gaussian(Cfg::CONKER_LIFESPAN_MEAN,
                                         Cfg::CONKER_LIFESPAN_SD);
        if (days < 1.0f) days = 1.0f;
        rec->lifespan_ms = static_cast<uint32_t>(days * Cfg::SECS_PER_DAY * 1000.0f);
        rec->dirty = true;
        Serial.printf("[reconcile] id=%lu record had no lifespan — drew %.1f days\r\n",
                      (unsigned long)id, days);
    }
    if (rec->lived_ms >= rec->lifespan_ms) {
        Serial.printf("[reconcile] id=%lu lived %.2fd >= lifespan %.2fd — clamping (corrupt record?)\r\n",
                      (unsigned long)id,
                      rec->lived_ms / 86400000.0f, rec->lifespan_ms / 86400000.0f);
        rec->lived_ms = static_cast<uint32_t>(rec->lifespan_ms * 0.95f);
        rec->dirty = true;
    }
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
            // Boredom makes conkers play constantly now — journaling every
            // parade floods the diary. Keep an occasional "they're playing"
            // note (≤ 1 per 30 min) instead of one per romp.
            static uint32_t _last_play_journal_ms = 0;
            uint32_t now_ms = millis();
            bool cooled = (_last_play_journal_ms == 0
                           || now_ms - _last_play_journal_ms >= 1800000);
            int li = ev.parade.leader_idx;
            if (cooled && li < chamber.conker_count && chamber.conkers[li].alive) {
                _last_play_journal_ms = now_ms;
                je.type = JEVT_PLAY;
                je.lilguy_id = chamber.conkers[li].id;
                strlcpy(je.who, chamber.conkers[li].name, sizeof(je.who));
                je.play.kind = 0;  // parade
                je.play.participants = ev.parade.participants;
                journal.emit(je);
            }
            break;
        }

        case EVT_DISCOVERY: {
            // Throttle so frequent finds (e.g. firefly catches at night) don't
            // flood the diary or spam phone notifications — ≤1 per 10 min.
            static uint32_t _last_discovery_journal_ms = 0;
            uint32_t now_ms = millis();
            bool cooled = (_last_discovery_journal_ms == 0
                           || now_ms - _last_discovery_journal_ms >= 600000);
            int fi = ev.discovery.finder_idx;
            if (cooled && fi < chamber.conker_count && chamber.conkers[fi].alive) {
                _last_discovery_journal_ms = now_ms;
                je.type = JEVT_DISCOVERY;
                je.lilguy_id = chamber.conkers[fi].id;
                strlcpy(je.who, chamber.conkers[fi].name, sizeof(je.who));
                je.discovery.critter = ev.discovery.kind;
                journal.emit(je);
            }
            break;
        }

        case EVT_CRAFTED:
            je.type = JEVT_CRAFTED;
            je.lilguy_id = ev.crafted.maker_id;
            strlcpy(je.who, ev.crafted.who, sizeof(je.who));
            je.crafted.kind = ev.crafted.kind;
            je.crafted.context = ev.crafted.context;
            je.crafted.motif = ev.crafted.motif;
            strlcpy(je.crafted.honoree, ev.crafted.honoree,
                    sizeof(je.crafted.honoree));
            journal.emit(je);
            break;

        case EVT_ART_WEATHERED:
            je.type = JEVT_ART_WEATHERED;
            je.lilguy_id = 0;
            strlcpy(je.who, ev.crafted.who, sizeof(je.who));
            je.crafted.kind = ev.crafted.kind;
            journal.emit(je);
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
    if (registry.state() == PERSIST_UNINIT)
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
        registry.pick_unique_name(rec.name, sizeof(rec.name));
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

    // Everything above is in-RAM (identities, names, personalities) and runs
    // even without a card — so a no-SD colony (e.g. the phone-as-module build)
    // still shows named conkers in the app. Below here writes to the card;
    // skip it in degraded mode (nothing to flush to).
    if (registry.state() == PERSIST_DEGRADED)
        return;

    // SD card health check
    sd_card_health_tick();

    // Manifest + positions + bonds flush (every 30s — two file writes)
    uint32_t now = millis();
    static uint32_t _last_manifest_ms = 0;
    if (now - _last_manifest_ms >= 30000) {
        _last_manifest_ms = now;
        _persist_update_positions();
        _persist_sync_brood();
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

// Brood records were written once at laying and never again — so a larva that
// advanced past the egg stage reverted to an egg on every reboot (the stale
// on-disk record still said STAGE_EGG). Mirror each live brood's developmental
// state back into its record when it has materially changed. Runs on the 30s
// manifest cadence; brood stages last hours, so 30s of slack is harmless.
void Coordinator::_persist_sync_brood() {
    for (int i = 0; i < chamber.brood_count; i++) {
        Brood& b = chamber.brood[i];
        if (b.id == 0) continue;  // not yet assigned a record this tick
        if (!b.alive()) continue;
        BroodRecord* rec = registry.get_brood(b.id);
        if (!rec) continue;

        // Re-persist every live brood on the cadence — do NOT gate on a
        // material change. The on-disk stage_elapsed_ms is recomputed at write
        // time, and in a steady stage (an egg incubating, or a fully-fed larva
        // waiting out its timer) NONE of stage/stage_start_ms/hunger/food change,
        // so a change-gate would never refresh the elapsed. The record would
        // keep its lay-time value (~0) and the stage clock would reset to the
        // start on every reboot — which stranded COM4's egg at the egg stage for
        // days, since that module reboots within the ~16h egg window.
        BroodRecord updated = *rec;
        updated.stage          = static_cast<uint8_t>(b.stage);
        updated.stage_start_ms = b.stage_start_ms;
        updated.hunger         = b.hunger;
        updated.food_invested  = b.food_invested;
        updated.x              = b.x;
        updated.y              = b.y;
        registry.update_brood(updated);
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
                } else if (registry.manifest().queen_imported) {
                    // Gateway coronation: untended hatches (the founding
                    // cohort) take after an imported queen — the colony
                    // inherits the character she grew on the phone. Same
                    // blend as carer inheritance; identity, not power.
                    for (int d = 0; d < PERS_COUNT; d++) {
                        float random_part = chamber.conkers[i].personality[d];
                        chamber.conkers[i].personality[d] =
                            0.7f * random_part
                            + 0.3f * registry.manifest().q_pers[d];
                    }
                }

                IdentityRecord rec;
                rec.id = id;
                registry.pick_unique_name(rec.name, sizeof(rec.name));
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

// Buy a piece of decor with bugs she's caught. Cosmetic only — identity, not
// power. Placed in her room (the whole grid on a colony), and marked CTX_BOUGHT
// so the density rules never weather it away.
bool Coordinator::cmd_buy_decor(uint8_t item_id) {
    if (item_id >= Cfg::SHOP_ITEM_COUNT) return false;
    const Cfg::ShopItem& it = Cfg::SHOP_ITEMS[item_id];
    const uint32_t bit = 1u << item_id;
    const bool already_owned = (colony.owned_items & bit) != 0;

    // You pay once. Without this, buying a second keepsake meant losing the
    // first — she carries one at a time, so swapping back charged you again for
    // something you'd already bought.
    if (!already_owned && colony.bugs < it.price) {
        Serial.printf("[shop] '%s' costs %u, purse has %u\r\n",
                      it.name, (unsigned)it.price, (unsigned)colony.bugs);
        return false;
    }

    // Something she carries rather than something that stands in the room.
    // She holds one at a time, so this swaps whatever she had — a keeper who
    // paid for it should be able to change her mind freely (unlike the crafted
    // gifts, which are deliberately one-per-conker and stay put).
    if (it.wear != 0) {
        if (chamber.conker_count == 0) return false;
        Conker& her = chamber.conkers[0];
        if (!already_owned) colony.bugs -= it.price;
        colony.owned_items |= bit;
        her.accessory       = it.wear;
        her.accessory_tint  = it.tint;
        her.accessory_from  = 0;          // from you, not a maker
        her.accessory_memorial = false;
        IdentityRecord* rec = registry.get(her.id);
        if (rec) {
            rec->accessory      = it.wear;
            rec->accessory_tint = it.tint;
            rec->dirty = true;            // survives a reload
        }
        Serial.printf("[shop] %s '%s' — purse now %u\r\n",
                      already_owned ? "carrying" : "bought", it.name, (unsigned)colony.bugs);
        return true;
    }

    // Already standing in her room — nothing to buy, nothing to charge.
    if (already_owned) {
        Serial.printf("[shop] '%s' is already in her room\r\n", it.name);
        return false;
    }

    // Somewhere clear to stand it. Try a few spots, then give up rather than
    // drop it on top of her or on another piece.
    int px = -1, py = -1;
    for (int tries = 0; tries < 24; tries++) {
        int cx = g_rng.rand_int(chamber.room_x0() + 1, chamber.room_x1() - 1);
        int cy = g_rng.rand_int(chamber.room_y0() + 1, chamber.room_y1() - 1);
        if (!chamber.artwork_spot_free(cx, cy)) continue;
        px = cx; py = cy; break;
    }
    if (px < 0) {
        Serial.println("[shop] nowhere clear to put it");
        return false;
    }

    colony.bugs -= it.price;
    colony.owned_items |= bit;

    Artwork piece;
    piece.active       = true;
    piece.kind         = it.kind;
    piece.motif        = it.motif;
    piece.x            = static_cast<int8_t>(px);
    piece.y            = static_cast<int8_t>(py);
    piece.maker_id     = 0;                 // the keeper, not a conker
    strlcpy(piece.maker_name, "you", sizeof(piece.maker_name));
    piece.maker_tint   = it.tint;
    piece.created_unix = g_tod.unix_time;
    piece.context      = CTX_BOUGHT;

    Artwork weathered;
    chamber.place_artwork(piece, &weathered);
    Serial.printf("[shop] bought '%s' for %u — purse now %u\r\n",
                  it.name, (unsigned)it.price, (unsigned)colony.bugs);
    return true;
}

// Put down whatever she's carrying (the shop lets you take it off again — you
// keep the item, so it can be picked back up for nothing).
bool Coordinator::cmd_unequip() {
    if (chamber.conker_count == 0) return false;
    Conker& her = chamber.conkers[0];
    her.accessory = 0;
    her.accessory_tint = 0;
    her.accessory_from = 0;
    IdentityRecord* rec = registry.get(her.id);
    if (rec) { rec->accessory = 0; rec->accessory_tint = 0; rec->dirty = true; }
    return true;
}

// Name the colony (keeper feedback #40: "the colony title should be chooseable
// and show up when connected to other colonies"). This is a DISPLAY name only —
// colony_id stays the immutable identity/API key, so renaming can never orphan
// a colony's snapshots, events or the app's connection. Empty title = show the id.
bool Coordinator::cmd_set_colony_title(const char* title) {
    char clean[25] = {};
    int j = 0;
    for (int i = 0; title && title[i] && j < 24; i++) {
        char ch = title[i];
        if (ch >= 32 && ch < 127) clean[j++] = ch;   // printable ASCII, as names are
    }
    // Trim trailing spaces so " " can't masquerade as a title and hide the id.
    while (j > 0 && clean[j - 1] == ' ') clean[--j] = '\0';

    strlcpy(registry.manifest().title, clean, sizeof(registry.manifest().title));
    registry.flush_manifest();   // a name the keeper chose must survive a power cut
    Serial.printf("[cmd] colony title set to '%s'\r\n", clean[0] ? clean : "(cleared)");
    return true;
}

// Recolour a conker (keeper feedback #43/#49: "customise the first baby",
// "being able to change the colour"). tint_seed is the hue the renderer's
// luma-key remap keys off, so this is purely cosmetic — identity, not power.
// 0 is reserved for "unset" (init would reroll it), so it's clamped to 1-255.
bool Coordinator::cmd_set_conker_tint(uint32_t id, uint8_t tint_seed) {
    if (tint_seed == 0) tint_seed = 1;

    IdentityRecord* rec = registry.get(id);
    if (!rec) return false;

    rec->tint_seed = tint_seed;
    rec->dirty = true;

    for (int i = 0; i < chamber.conker_count; i++) {
        if (chamber.conkers[i].id == id) {
            chamber.conkers[i].tint_seed = tint_seed;
            break;
        }
    }
    Serial.printf("[cmd] conker %lu recoloured (tint %u)\r\n",
                  (unsigned long)id, (unsigned)tint_seed);
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

bool Coordinator::cmd_gift_care_package(uint16_t target_id) {
#ifdef ARDUINO
    // Gifts only cross a recognised sovereign border — the one sanctioned
    // exception to the closed-border rule. target_id 0 = any neighbour.
    int face = -1;
    for (int f = 0; f < FACE_COUNT; f++) {
        const Neighbour& nb = topology_neighbour(static_cast<Face>(f));
        if (nb.present && foreign_face[f]
            && (target_id == 0 || nb.module_id == target_id)) {
            face = f;
            break;
        }
    }
    if (face < 0) {
        Serial.println("[cmd] gift: no neighbouring kingdom at the border");
        return false;
    }

    // Separate cooldown from the home care package (same 6h span, own timer)
    Preferences prefs;
    prefs.begin("hive", false);
    uint32_t last = prefs.getULong("gift_unix", 0);
    if (last != 0 && g_tod.unix_time > last
        && g_tod.unix_time - last < 6UL * 3600UL) {
        prefs.end();
        Serial.println("[cmd] gift: caravan still resting (cooldown)");
        return false;
    }
    prefs.putULong("gift_unix", g_tod.unix_time);
    prefs.end();

    GiftFoodMessage msg;
    msg.msg_type  = TOPO_GIFT_FOOD;
    msg.sender_id = topology_my_id();
    topology_send_to_face(static_cast<Face>(face),
                          (const uint8_t*)&msg, sizeof(msg));

    uint16_t nb_id = topology_neighbour(static_cast<Face>(face)).module_id;
    Serial.printf("[cmd] care package sent to neighbouring queen 0x%04X\r\n", nb_id);

    JournalEntry je = {};
    je.tick = chamber.tick_num;
    je.unix_time = g_tod.unix_time;
    je.type = JEVT_COLONY_EVENT;
    je.colony_event.kind = COLONY_GIFT_SENT;
    je.colony_event.module_id = nb_id;
    journal.emit(je);
    return true;
#else
    (void)target_id;
    return false;
#endif
}

// The keeper answers a wish: the effect lands on that ONE conker (a
// personal kindness, not a colony broadcast), it's journaled, and the
// glass announces it.
bool Coordinator::cmd_grant_wish(uint32_t id) {
    if (wish.kind == 0 || wish.conker_id != id) return false;
    for (int i = 0; i < chamber.conker_count; i++) {
        Conker& w = chamber.conkers[i];
        if (w.id != id || !w.alive) continue;

        if (wish.kind == 1) {           // a treat, hand-delivered
            w.hunger -= 50.0f;
            if (w.hunger < 0.0f) w.hunger = 0.0f;
        } else {                        // a boop from beyond the glass
            w.needs[NEED_BOREDOM] -= Cfg::BOREDOM_BOOP_RELIEF;
            if (w.needs[NEED_BOREDOM] < 0.0f) w.needs[NEED_BOREDOM] = 0.0f;
            if (w.state == STATE_IDLE && w.stack_on < 0
                    && w.anim_remaining_ticks == 0) {
                w.anim_type = LG_ANIM_NOTICE;
                w.anim_remaining_ticks = 12;
                w.facing_dx = 0.0f; w.facing_dy = 1.0f;
                w.last_dx = 0.0f;   w.last_dy = 1.0f;
                w.has_target_cell = false;
            }
        }
        w.afterglow_ticks = Cfg::AFTERGLOW_TICKS;

        JournalEntry je = {};
        je.tick = chamber.tick_num;
        je.unix_time = g_tod.unix_time;
        je.type = JEVT_WISH_GRANTED;
        je.lilguy_id = w.id;
        strlcpy(je.who, w.name, sizeof(je.who));
        je.wish.kind = wish.kind;
        journal.emit(je);

        Serial.printf("[wish] granted: %s (%s)\r\n", w.name,
                      wish.kind == 1 ? "treat" : "boop");
        wish = Wish{};
        return true;
    }
    return false;
}

// App pins → followed list. The renderer draws a small star over these
// conkers so a favourite stays findable at a glance (readability lever as
// the population grows). Persisted so it survives reboots.
void Coordinator::cmd_set_followed(const uint32_t* ids, int n) {
    if (n > Chamber::MAX_FOLLOWED) n = Chamber::MAX_FOLLOWED;
    chamber.followed_count = (uint8_t)n;
    for (int i = 0; i < n; i++) chamber.followed[i] = ids[i];

    Preferences prefs;
    prefs.begin("hive", false);
    prefs.putBytes("followed", chamber.followed, n * sizeof(uint32_t));
    prefs.end();
    Serial.printf("[cmd] following %d conker(s)\r\n", n);
}

// Satellite side: forward diary-worthy bus events to the queen. The
// journal (and the VPS push) live with her — without this relay the most
// story-rich module would be mute in the chronicle.
void Coordinator::_relay_bus_events(const Event* events, int count) {
#ifdef ARDUINO
    if (chamber.home_face < 0) return;
    for (int i = 0; i < count; i++) {
        const Event& ev = events[i];
        JournalRelayMessage msg = {};
        msg.msg_type  = TOPO_JOURNAL_RELAY;
        msg.sender_id = topology_my_id();

        if (ev.type == EVT_CROP_SOWN) {
            msg.jtype     = JEVT_CROP_SOWN;
            msg.lilguy_id = ev.crop.sower_id;
            msg.extra     = ev.crop.plot;
            strlcpy(msg.who, ev.crop.who, sizeof(msg.who));
        } else if (ev.type == EVT_DISCOVERY) {
            int fi = ev.discovery.finder_idx;
            if (fi < 0 || fi >= chamber.conker_count) continue;
            msg.jtype     = JEVT_DISCOVERY;
            msg.lilguy_id = chamber.conkers[fi].id;
            msg.extra     = ev.discovery.kind;
            strlcpy(msg.who, chamber.conkers[fi].name, sizeof(msg.who));
        } else if (ev.type == EVT_CRAFTED) {
            msg.jtype     = JEVT_CRAFTED;
            msg.lilguy_id = ev.crafted.maker_id;
            msg.extra     = (uint8_t)(ev.crafted.kind | (ev.crafted.context << 4));
            msg.motif     = ev.crafted.motif;
            strlcpy(msg.who, ev.crafted.who, sizeof(msg.who));
            strlcpy(msg.honoree, ev.crafted.honoree, sizeof(msg.honoree));
        } else if (ev.type == EVT_ART_WEATHERED) {
            msg.jtype = JEVT_ART_WEATHERED;
            msg.extra = ev.crafted.kind;
            strlcpy(msg.who, ev.crafted.who, sizeof(msg.who));
        } else {
            continue;
        }
        topology_send_to_face(static_cast<Face>(chamber.home_face),
                              (const uint8_t*)&msg, sizeof(msg));
    }
#endif
}

// Queen side: satellite story beats land in the colony journal (throttled
// like local ones so a busy garden doesn't flood the diary).
void Coordinator::_receive_journal_relays() {
#ifdef ARDUINO
    PendingJournalRelay pending[8];
    int n = topology_drain_journal_relays(pending, 8);
    for (int i = 0; i < n; i++) {
        // motif was appended later — accept the shorter pre-motif layout too
        if (pending[i].len < (int)offsetof(JournalRelayMessage, motif)) continue;
        const JournalRelayMessage& msg =
            *reinterpret_cast<const JournalRelayMessage*>(pending[i].data);
        if (msg.msg_type != TOPO_JOURNAL_RELAY) continue;
        bool has_motif = pending[i].len >= (int)sizeof(JournalRelayMessage);

        if (msg.jtype == JEVT_DISCOVERY) {
            // Same cadence as local discoveries — ≤1 per 10 min
            static uint32_t _last_relay_discovery_ms = 0;
            uint32_t now_ms = millis();
            if (_last_relay_discovery_ms != 0
                    && now_ms - _last_relay_discovery_ms < 600000) continue;
            _last_relay_discovery_ms = now_ms;
        }

        JournalEntry je = {};
        je.tick = chamber.tick_num;
        je.unix_time = g_tod.unix_time;
        je.type = msg.jtype;
        je.lilguy_id = msg.lilguy_id;
        strlcpy(je.who, msg.who, sizeof(je.who));
        if (msg.jtype == JEVT_DISCOVERY) je.discovery.critter = msg.extra;
        if (msg.jtype == JEVT_MOURNING) {
            strlcpy(je.mourning.dead_name, msg.honoree,
                    sizeof(je.mourning.dead_name));
            je.mourning.anniversary = msg.extra;
        }
        // Crafted/weathered are NOT journaled here: they're re-emitted onto
        // the queen's bus below, and the bus→journal bridge writes them —
        // one path for local and relayed alike (no double entries).
        if (msg.jtype != JEVT_CRAFTED && msg.jtype != JEVT_ART_WEATHERED)
            journal.emit(je);

        // The queen's glass narrates the garden's news too
        if (_bus && msg.jtype == JEVT_CROP_SOWN) {
            Event ev = {};
            ev.type = EVT_CROP_SOWN;
            ev.tick = chamber.tick_num;
            ev.crop.plot = msg.extra;
            ev.crop.sower_id = msg.lilguy_id;
            strlcpy(ev.crop.who, msg.who, sizeof(ev.crop.who));
            _bus->emit(ev);
        }
        if (_bus && msg.jtype == JEVT_CRAFTED) {
            Event ev = {};
            ev.type = EVT_CRAFTED;
            ev.tick = chamber.tick_num;
            ev.crafted.kind = msg.extra & 0x0F;
            ev.crafted.context = (msg.extra >> 4) & 0x0F;
            ev.crafted.motif = has_motif ? msg.motif : 0;
            ev.crafted.maker_id = msg.lilguy_id;
            strlcpy(ev.crafted.who, msg.who, sizeof(ev.crafted.who));
            strlcpy(ev.crafted.honoree, msg.honoree, sizeof(ev.crafted.honoree));
            _bus->emit(ev);
        }
        if (_bus && msg.jtype == JEVT_ART_WEATHERED) {
            Event ev = {};
            ev.type = EVT_ART_WEATHERED;
            ev.tick = chamber.tick_num;
            ev.crafted.kind = msg.extra;
            strlcpy(ev.crafted.who, msg.who, sizeof(ev.crafted.who));
            _bus->emit(ev);
        }
    }
#endif
}

void Coordinator::_receive_gifts() {
#ifdef ARDUINO
    GiftFoodMessage gm;
    if (!topology_has_gift_food(&gm)) return;

    // Receiver sizes its own gift — a day's table for this colony, exactly
    // like the home care package (larder cap still applies in _world_tick)
    float amount = colony.daily_burn();
    if (amount <= 0.0f) amount = Cfg::QUEEN_FOOD_PER_DAY;
    if (amount > 100.0f) amount = 100.0f;

    int cx = Cfg::GRID_WIDTH / 2 + g_rng.rand_int(-5, 5);
    int cy = Cfg::GRID_HEIGHT / 2 + g_rng.rand_int(-4, 4);
    chamber.add_food(cx, cy, amount);
    Serial.printf("[gift] care package from neighbouring queen 0x%04X: %.0fu at (%d,%d)\r\n",
                  gm.sender_id, amount, cx, cy);

    JournalEntry je = {};
    je.tick = chamber.tick_num;
    je.unix_time = g_tod.unix_time;
    je.type = JEVT_COLONY_EVENT;
    je.colony_event.kind = COLONY_GIFT_RECEIVED;
    je.colony_event.module_id = gm.sender_id;
    journal.emit(je);
#endif
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
            if (foreign_face[f]) {
                Serial.println("[cmd] set_role: that module is a foreign queen");
                return false;
            }
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
            if (foreign_face[f]) {
                Serial.println("[cmd] set_tint: that module is a foreign queen");
                return false;
            }
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

// Event-driven personality drift (v1): a major life event nudges one
// dimension, permanently. Updates the live conker AND its record (the app's
// petals chart reads pushed values live), clamps to [0,1], marks the record
// dirty so it persists. Returns the delta actually applied after clamping —
// grief banks that as restore-credit.
static float _drift_personality(Conker& w, IdentityRecord* rec,
                                uint8_t dim, float delta) {
    float before = w.personality[dim];
    float v = before + delta;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    w.personality[dim] = v;
    if (rec) {
        rec->personality[dim] = v;
        rec->dirty = true;
    }
    return v - before;
}

void Coordinator::_persist_process_deaths() {
    for (int d = 0; d < chamber.death_count; d++) {
        uint32_t id = chamber.deaths[d].id;
        uint8_t cause = chamber.deaths[d].cause;
        char attended_by[16] = {};   // the friend at their side, if any

        // Dead conker's bonds — fetched before remove_owner() erases them.
        // Shared by the grief drift below and the vigil loop.
        BondEntry bs[BondStore::PER_OWNER_CAP];
        int bc = bonds.get_bonds(id, bs, BondStore::PER_OWNER_CAP);

        // Personality drift: losing a BEST friend (mutual — both directions
        // formed) quiets the survivor for good. Felt even when the colony is
        // too stressed to hold a vigil; a later NEW best friend restores half
        // (credit banked on the record).
        for (int b = 0; b < bc; b++) {
            if (!bs[b].formed || !bonds.is_formed(bs[b].target, id)) continue;
            for (int i = 0; i < chamber.conker_count; i++) {
                Conker& w = chamber.conkers[i];
                if (w.id != bs[b].target || !w.alive) continue;
                IdentityRecord* rec = registry.get(w.id);
                float ds = _drift_personality(w, rec, PERS_SOCIAL_FREQUENCY,
                                              Cfg::DRIFT_GRIEF_SOCIAL);
                float dp = _drift_personality(w, rec, PERS_PLAYFULNESS,
                                              Cfg::DRIFT_GRIEF_PLAY);
                if (rec) {
                    rec->grief_social += -ds;   // applied deltas are <= 0
                    rec->grief_play   += -dp;
                }
                break;
            }
        }

        // Mourning: bonded partners pay respects at the husk. Must run
        // before bonds.remove_owner() erases the relationships. Skipped
        // when the colony is stressed — survival first.
        if (colony.food_pressure() <= Cfg::FLAIR_MAX_PRESSURE) {
            // Dead conker's name for the vigil banner (registry record still
            // live — mark_dead runs after this loop)
            const IdentityRecord* dead_rec = registry.get(id);
            const char* dead_name = dead_rec ? dead_rec->name : "";
            int8_t hx = -1, hy = -1;
            for (int h = chamber.husk_count - 1; h >= 0; h--) {
                if (chamber.husks[h].conker_id == id) {
                    hx = chamber.husks[h].x; hy = chamber.husks[h].y;
                    break;
                }
            }
            int mourners = 0;
            for (int b = 0; b < bc && hx >= 0
                    && mourners < Cfg::MOURN_MAX_PARTNERS; b++) {
                if (bs[b].strength < Cfg::MOURN_MIN_BOND) continue;
                for (int i = 0; i < chamber.conker_count; i++) {
                    Conker& w = chamber.conkers[i];
                    if (w.id != bs[b].target || !w.alive) continue;
                    // Was this friend already at their side when they went?
                    // (within a couple of cells of where they fell)
                    if (!attended_by[0]
                            && abs(w.cell_x() - hx) + abs(w.cell_y() - hy) <= 3)
                        strlcpy(attended_by, w.name, sizeof(attended_by));
                    // Never interrupt a carrier or a departing worker
                    if (w.food_carried > 0 || w.departing) break;
                    w.state = STATE_MOURNING;
                    w.target_x = hx; w.target_y = hy;
                    w.has_target = true;
                    w.has_target_cell = false;
                    strlcpy(w.mourning_for, dead_name, sizeof(w.mourning_for));
                    // Best friends (the bond was mutual — BOTH directions
                    // formed, bs[b] is dead->w) hold a full vigil; a one-way
                    // attachment grieves only briefly.
                    bool mutual = bs[b].formed && bonds.is_formed(w.id, id);
                    w.zoomie_ticks = mutual ? Cfg::MOURN_DURATION_TICKS
                                            : Cfg::MOURN_ONEWAY_TICKS;
                    w.zoomie_target = -1;
                    w.sleeping = false;
                    w.stack_on = -1;
                    w.idle_ticks_remaining = 0;
                    w.anim_type = LG_ANIM_NONE;
                    w.anim_remaining_ticks = 0;
                    w.speed = w.base_speed();
                    mourners++;

                    JournalEntry jm = {};
                    jm.tick = chamber.tick_num;
                    jm.unix_time = g_tod.unix_time;
                    jm.type = JEVT_MOURNING;
                    jm.lilguy_id = w.id;
                    strlcpy(jm.who, w.name, sizeof(jm.who));
                    jm.mourning.dead_id = id;
                    strlcpy(jm.mourning.dead_name, dead_name,
                            sizeof(jm.mourning.dead_name));
                    journal.emit(jm);

                    // Display bus: vigil banner ("X stands vigil for Y")
                    if (_bus) {
                        Event ev = {};
                        ev.type = EVT_MOURNING;
                        ev.tick = chamber.tick_num;
                        ev.mourning.mourner_id = w.id;
                        strlcpy(ev.mourning.mourner_name, w.name,
                                sizeof(ev.mourning.mourner_name));
                        strlcpy(ev.mourning.dead_name, dead_name,
                                sizeof(ev.mourning.dead_name));
                        _bus->emit(ev);
                    }
                    break;
                }
            }
        }

        // Hats made by the departed: if the friendship still held, the hat
        // becomes a memorial — worn for life, never taken off. If the bond
        // had already faded below formed, it comes off with the news. Must
        // run before bonds.remove_owner() erases the relationship (and
        // before mark_dead drops the maker's name from the living roster).
        {
            const IdentityRecord* dr = registry.get(id);
            const char* dead_maker_name = dr ? dr->name : "";
            for (int c = 0; c < chamber.conker_count; c++) {
                Conker& w = chamber.conkers[c];
                if (!w.alive || w.accessory == 0 || w.accessory_memorial
                        || w.accessory_from != id) continue;
                IdentityRecord* rw = registry.get(w.id);
                if (bonds.is_formed(w.id, id)) {
                    w.accessory_memorial = true;
                    if (rw) { rw->accessory_memorial = true; rw->dirty = true; }
                    if (_bus) {
                        Event ev = {};
                        ev.type = EVT_KEEPSAKE_VOW;
                        ev.tick = chamber.tick_num;
                        ev.keepsake.kind = w.accessory;
                        strlcpy(ev.keepsake.who, w.name, sizeof(ev.keepsake.who));
                        strlcpy(ev.keepsake.maker, dead_maker_name,
                                sizeof(ev.keepsake.maker));
                        _bus->emit(ev);
                    }
                } else {
                    uint8_t kind = w.accessory;
                    w.accessory = 0; w.accessory_tint = 0; w.accessory_from = 0;
                    if (rw) {
                        rw->accessory = 0; rw->accessory_tint = 0;
                        rw->accessory_from = 0; rw->dirty = true;
                    }
                    if (_bus) {
                        Event ev = {};
                        ev.type = EVT_KEEPSAKE_OFF;
                        ev.tick = chamber.tick_num;
                        ev.keepsake.kind = kind;
                        strlcpy(ev.keepsake.who, w.name, sizeof(ev.keepsake.who));
                        _bus->emit(ev);
                    }
                }
            }
        }

        registry.mark_dead(id, g_tod.unix_time);
        registry.manifest().total_workers_died++;
        bonds.remove_owner(id);

        // Journal: death event (name rides along — the roster forgets the
        // dead; attended_by records the friend at their side, if any)
        JournalEntry je = {};
        je.tick = chamber.tick_num;
        je.unix_time = g_tod.unix_time;
        je.type = JEVT_DEATH;
        strlcpy(je.death.attended_by, attended_by, sizeof(je.death.attended_by));
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
            // v151: snapshot needs so they survive a reboot (piggybacks the
            // existing 30s dirty-flush — no extra writes). Mood re-derives.
            rec->last_boredom    = w.needs[NEED_BOREDOM];
            rec->last_tiredness  = w.needs[NEED_REST];
            rec->last_loneliness = w.needs[NEED_SOCIAL];
            rec->last_hunger     = w.hunger;
            rec->keeper_bond     = w.keeper_bond;  // incubation: bond survives reloads
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
    m.bugs = colony.bugs;                      // shop purse survives a power cut
    m.owned_items = colony.owned_items;
    m.bond_peak_seen = colony.bond_peak_seen;
    for (int i = 0; i < 3; i++) m.last_interact[i] = colony.last_interact[i];
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

// Gateway coronation: if a summoned queen was staged across the reset reboot
// (vps_push summon_queen → NVS "handoff"/"queen"), found with HER instead of
// a random queen — her name, her identity, the colony_id the app is already
// watching, and her personality as the inheritance source for untended
// hatches. One-shot: the staged blob is consumed (success or not) so a bad
// payload can never wedge founding. Returns true when she took the throne.
static bool _summon_staged_apply(ColonyManifest& m) {
    Preferences prefs;
    prefs.begin("handoff", false);
    // isKey first — a bare getString on a missing key makes the ESP32
    // Preferences lib log a scary "nvs_get_str len fail" line every founding.
    String blob = prefs.isKey("queen") ? prefs.getString("queen", "") : String();
    if (blob.length() > 0) prefs.remove("queen");
    prefs.end();
    if (blob.length() == 0) return false;

    JsonDocument doc;
    const char* qname = "";
    if (deserializeJson(doc, blob.c_str()) == DeserializationError::Ok)
        qname = doc["name"] | "";
    if (!qname[0]) {
        Serial.println("[summon] staged queen unreadable — founding fresh instead");
        return false;
    }
    strlcpy(m.queen_name, qname, sizeof(m.queen_name));
    const char* keep_id = doc["colony_id"] | "";
    if (keep_id[0]) strlcpy(m.colony_id, keep_id, sizeof(m.colony_id));
    m.queen_imported = true;
    JsonArray pers = doc["personality"];
    for (int i = 0; i < 8 && i < (int)pers.size(); i++)
        m.q_pers[i] = pers[i] | 0.5f;
    m.q_tint      = doc["tint_seed"] | 0;
    m.q_bond      = doc["keeper_bond"] | 0.0f;
    m.q_born_unix = doc["born_unix"] | 0;
    m.q_traits    = doc["traits"] | 0;
    m.q_catches   = doc["catches"] | 0;
    strlcpy(m.q_from, doc["from_colony"] | "", sizeof(m.q_from));
    Serial.printf("[summon] crowned Queen %s — raised on %s, founding her colony\r\n",
                  m.queen_name, m.q_from[0] ? m.q_from : "an unknown chamber");
    return true;
}

// ---- Verdant chamber (Gateway coronation) ----

// Marker staged across the `reset verdant` wipe/reboot (NVS survives the
// colony wipe). One-shot.
bool Coordinator::consume_verdant_flag() {
    Preferences prefs;
    prefs.begin("handoff", false);
    bool flagged = prefs.getBool("verdant", false);
    if (flagged) prefs.remove("verdant");
    prefs.end();
    return flagged;
}

void Coordinator::_verdant_create_manifest() {
    ColonyManifest& m = registry.manifest();
    m.schema = 1;
    registry.generate_colony_id();
    m.queen_name[0] = '\0';
    m.awaiting = true;
    m.founded_unix = 0;   // nothing has been founded
    m.module_role = role;
    registry.flush_manifest();
    Serial.printf("[verdant] chamber %s minted — awaiting opportunity\r\n",
                  m.colony_id);
}

void Coordinator::_verdant_boot() {
    // The chamber was init'd with a queen (queen-role default); un-seat her.
    // The world still runs — weather, day/night, critters, fireflies — it's
    // a living chamber with nobody home yet.
    chamber.has_queen = false;
    colony = ColonyState();
    colony.population = 0;
    colony.worker_census = 0;
    colony.food_store = 0;
    colony.food_total = 0;
    Serial.printf("[verdant] empty chamber %s awaiting its queen\r\n",
                  registry.manifest().colony_id);
}

void Coordinator::_persist_migrate_live_colony() {
    // Case B: first boot with SD — migrate existing live colony to persistence.
    // Called when SD is available but no manifest exists.
    Serial.println("[persist] migrating live colony to persistence (Case B)...");

    ColonyManifest& m = registry.manifest();
    m.schema = 1;
    registry.generate_colony_id();
    name_random(m.queen_name, sizeof(m.queen_name));
    if (_summon_staged_apply(m))
        chamber.queen_tint = m.q_tint;   // she arrives wearing her colour
    else
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
    m.bugs = colony.bugs;                      // shop purse survives a power cut
    m.owned_items = colony.owned_items;
    m.bond_peak_seen = colony.bond_peak_seen;
    for (int i = 0; i < 3; i++) m.last_interact[i] = colony.last_interact[i];
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
        registry.pick_unique_name(rec.name, sizeof(rec.name));
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

    // An imported (app-raised) queen keeps wearing her colour after reboots
    if (m.queen_imported) chamber.queen_tint = m.q_tint;

    // Restore colony state
    colony.food_store = m.food_store;
    colony.food_total = m.food_total;
    colony.bugs = m.bugs;
    colony.owned_items = m.owned_items;
    colony.bond_peak_seen = m.bond_peak_seen;
    for (int i = 0; i < 3; i++) colony.last_interact[i] = m.last_interact[i];
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
        if (r.lifespan_ms > 0) {
            chamber.conkers[idx].lifespan_ms = r.lifespan_ms;
        } else {
            // Heal the RECORD, not just the conker: a zero-lifespan record
            // otherwise lives forever (the conker gets a fresh random draw
            // each boot that is never written back) and kills its owner the
            // next time the respawn path copies it verbatim.
            r.lifespan_ms = chamber.conkers[idx].lifespan_ms;
            r.dirty = true;
            Serial.printf("[persist] id=%lu record had no lifespan — healed to %.1fd\r\n",
                          (unsigned long)r.id, r.lifespan_ms / 86400000.0f);
        }
        // Restore lived_ms (ageing source of truth — no wall-clock reconstruction)
        chamber.conkers[idx].lived_ms = r.lived_ms;
        // Restore scale_factor (init randomizes it — override with persisted value)
        if (r.scale_factor > 0.0f)
            chamber.conkers[idx].scale_factor = r.scale_factor;
        // Restore tint_seed (init randomizes it — override with persisted value)
        if (r.tint_seed > 0)
            chamber.conkers[idx].tint_seed = r.tint_seed;
        // Restore cumulative catch count (Catcher trait progress)
        chamber.conkers[idx].catches = r.catches;
        chamber.conkers[idx].accessory = r.accessory;
        chamber.conkers[idx].accessory_tint = r.accessory_tint;
        chamber.conkers[idx].accessory_from = r.accessory_from;
        chamber.conkers[idx].accessory_memorial = r.accessory_memorial;
        // Restore name from registry. Heal the RECORD if it has no name (a
        // partial/malformed write — same family as the zero-lifespan records):
        // otherwise the conker renders "???" forever, and _persist_tick won't
        // fix it because the record exists (only record-LESS conkers get named).
        if (r.name[0] == '\0') {
            registry.pick_unique_name(r.name, sizeof(r.name));
            r.dirty = true;
            Serial.printf("[persist] id=%lu record had no name — healed to %s\r\n",
                          (unsigned long)r.id, r.name);
        }
        strncpy(chamber.conkers[idx].name, r.name, sizeof(chamber.conkers[idx].name) - 1);
        // Restore personality (init randomizes it — override with persisted value)
        memcpy(chamber.conkers[idx].personality, r.personality,
               sizeof(chamber.conkers[idx].personality));
        // v151: restore needs so mood/needs survive a reboot (mood re-derives
        // from these on the first tick).
        chamber.conkers[idx].needs[NEED_BOREDOM] = r.last_boredom;
        chamber.conkers[idx].needs[NEED_REST]    = r.last_tiredness;
        chamber.conkers[idx].needs[NEED_SOCIAL]  = r.last_loneliness;
        chamber.conkers[idx].hunger              = r.last_hunger;
        chamber.conkers[idx].keeper_bond         = r.keeper_bond;  // incubation
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

        // A broken friendship takes the hat with it: if either side of the
        // pair wears a hat MADE by the other, it comes off. Memorial hats
        // (maker died a friend) are worn for life. A wearer away on another
        // module is caught by the arrival check in _place_arrival instead.
        for (int i = 0; i < broken_count; i++) {
            for (int c = 0; c < chamber.conker_count; c++) {
                Conker& w = chamber.conkers[c];
                if (!w.alive || w.accessory == 0 || w.accessory_memorial
                        || w.accessory_from == 0) continue;
                bool made_by_lost_friend =
                       (w.id == broken_owners[i]  && w.accessory_from == broken_targets[i])
                    || (w.id == broken_targets[i] && w.accessory_from == broken_owners[i]);
                if (!made_by_lost_friend) continue;
                uint8_t kind = w.accessory;
                w.accessory = 0; w.accessory_tint = 0; w.accessory_from = 0;
                IdentityRecord* rw = registry.get(w.id);
                if (rw) {
                    rw->accessory = 0; rw->accessory_tint = 0;
                    rw->accessory_from = 0; rw->accessory_memorial = false;
                    rw->dirty = true;
                }
                if (_bus) {
                    Event ev = {};
                    ev.type = EVT_KEEPSAKE_OFF;
                    ev.tick = tick_num;
                    ev.keepsake.kind = kind;
                    strlcpy(ev.keepsake.who, w.name, sizeof(ev.keepsake.who));
                    _bus->emit(ev);
                }
            }
        }
    }

    // Proximity-based bond formation every 200 ticks (~25s)
    if (tick_num - _bond_proximity_tick >= 200) {
        _bond_proximity_tick = tick_num;
        _bond_detect_proximity(tick_num);
    }
}

// How readily a conker forms proximity friendships, by sociability. Almost
// everyone makes friends — only the rare loner (bottom ~10%, social < 0.22 on
// the 0.5-centred triangular spread) keeps entirely to itself. Above that the
// factor scales how QUICKLY a friendship forms: the more sociable, the faster.
// Directed: each side of a pair scales by its OWN sociability, so a butterfly
// bonds fast to a quiet neighbour who's slow (or never) to reciprocate — and a
// bond only becomes a best-friendship once it's mutual.
static inline float _bond_social_factor(const Conker& c) {
    float f = (c.personality[PERS_SOCIAL_FREQUENCY] - Cfg::BOND_LONER_FLOOR) * Cfg::BOND_SOCIAL_GAIN;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.5f) f = 1.5f;
    return f;
}

void Coordinator::_bond_detect_proximity(uint32_t tick_num) {
    // Scan for worker pairs within 3 cells that are both active (non-idle)
    for (int i = 0; i < chamber.conker_count; i++) {
        Conker& a = chamber.conkers[i];
        if (a.id == 0 || a.departing || !a.alive) continue;

        for (int j = i + 1; j < chamber.conker_count; j++) {
            Conker& b = chamber.conkers[j];
            if (b.id == 0 || b.departing || !b.alive) continue;
            // v150: sleeping together no longer bonds. The all-night huddle used to
            // be the main driver and it saturated the whole colony to best friends.
            // Friendship is now earned through shared WAKING activity (co-tend,
            // co-forage, awake idle); the night is for rest, not networking.
            if (a.sleeping || b.sleeping) continue;

            int dist = abs(a.cell_x() - b.cell_x()) + abs(a.cell_y() - b.cell_y());
            if (dist > 3) continue;

            // Base accrual for the kind of time they're sharing. Sociability
            // (applied per-direction below) gates how much of it actually sticks,
            // and gentle decay lets a real friendship last for days.
            float base = 0.0f;

            // v152: passive proximity is now just a slow trickle — friendships
            // are mostly made by DOING things together (the activity bonuses in
            // chamber.cpp). These values are ~1/3 of the old ones so co-presence
            // alone no longer saturates the colony into one big clique.
            // Co-tending: both in TEND_BROOD targeting same cell
            if (a.state == STATE_TEND_BROOD && b.state == STATE_TEND_BROOD
                && a.target_x == b.target_x && a.target_y == b.target_y) {
                base = 0.012f;
            }
            // Co-foraging: both in TO_FOOD or TO_HOME (actively working)
            else if ((a.state == STATE_TO_FOOD || a.state == STATE_TO_HOME)
                  && (b.state == STATE_TO_FOOD || b.state == STATE_TO_HOME)) {
                base = 0.006f;
            }
            // Awake idle proximity — lounging near each other by day (sleepers are
            // already excluded above, so this is genuine waking company now).
            else if (a.state == STATE_IDLE && b.state == STATE_IDLE) {
                base = 0.003f;
            }

            if (base > 0.0f) {
                float inc_ab = base * _bond_social_factor(a);
                float inc_ba = base * _bond_social_factor(b);
                bool formed_ab = (inc_ab > 0.0f) && bonds.increment(a.id, b.id, inc_ab);
                bool formed_ba = (inc_ba > 0.0f) && bonds.increment(b.id, a.id, inc_ba);

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
                // Display bus: subtle hearts between the pair (no banner —
                // one-way bonds form often enough to spam it)
                if ((formed_ab || formed_ba) && _bus) {
                    Event ev = {};
                    ev.type = EVT_BOND_FORMED;
                    ev.tick = tick_num;
                    ev.bond.a_id = a.id;
                    ev.bond.b_id = b.id;
                    strlcpy(ev.bond.a_name, a.name, sizeof(ev.bond.a_name));
                    strlcpy(ev.bond.b_name, b.name, sizeof(ev.bond.b_name));
                    _bus->emit(ev);
                }

                // Reciprocation: the moment both directions are formed, the pair
                // become best friends. Fires once, on the tick the second side
                // crosses (one of formed_ab/formed_ba is what completed it).
                if ((formed_ab || formed_ba)
                        && bonds.is_formed(a.id, b.id)
                        && bonds.is_formed(b.id, a.id)) {
                    // Personality drift: a NEW best friend restores half of
                    // what grief took; the banked credit is spent either way
                    // (the other half is gone for good).
                    Conker* pair[2] = { &a, &b };
                    for (int p = 0; p < 2; p++) {
                        IdentityRecord* rec = registry.get(pair[p]->id);
                        if (!rec || (rec->grief_social <= 0.0f
                                  && rec->grief_play <= 0.0f)) continue;
                        _drift_personality(*pair[p], rec, PERS_SOCIAL_FREQUENCY,
                                           rec->grief_social * 0.5f);
                        _drift_personality(*pair[p], rec, PERS_PLAYFULNESS,
                                           rec->grief_play * 0.5f);
                        rec->grief_social = 0.0f;
                        rec->grief_play   = 0.0f;
                        rec->dirty = true;
                    }

                    JournalEntry je = {};
                    je.tick = tick_num;
                    je.unix_time = g_tod.unix_time;
                    je.type = JEVT_BOND_MUTUAL;
                    je.lilguy_id = a.id;
                    strlcpy(je.who, a.name, sizeof(je.who));
                    je.bond.target_id = b.id;
                    strlcpy(je.bond.target_name, b.name, sizeof(je.bond.target_name));
                    journal.emit(je);

                    // Display bus: best friends is a headline moment —
                    // big hearts + HUD banner
                    if (_bus) {
                        Event ev = {};
                        ev.type = EVT_BOND_MUTUAL;
                        ev.tick = tick_num;
                        ev.bond.a_id = a.id;
                        ev.bond.b_id = b.id;
                        strlcpy(ev.bond.a_name, a.name, sizeof(ev.bond.a_name));
                        strlcpy(ev.bond.b_name, b.name, sizeof(ev.bond.b_name));
                        _bus->emit(ev);
                    }
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

    // Atomic write, off-loop via the chores worker (part of the 30s persist
    // stall — issue #56). Fire-and-forget: bonds re-save every 30s anyway.
    if (chores_ready()) {
        chores_submit_sd_write(0, "/colony/bonds.json", buf, len);  // takes buf
        return;
    }
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
    // Heap-temp staging: a static here stayed resident for the whole run
    // (~6KB) for a buffer used once at boot.
    BondEntry* entries = (BondEntry*)malloc(sizeof(BondEntry) * BondStore::POOL_CAP);
    if (!entries) return;
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
    free(entries);
    Serial.printf("[bonds] loaded %d bonds\r\n", n);
}

// ================================================================
//  WorldCondition + Traits + Challenges
// ================================================================

void Coordinator::_world_tick(uint32_t tick_num) {
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

    // Real weather starts/ends survival challenges
    _weather_challenge_tick(tick_num);

    // Challenges bite: stores burn faster in hard conditions (severity-scaled)
    float mult = 1.0f;
    for (int i = 0; i < world.active_count; i++)
        mult = fmaxf(mult, 1.0f + Cfg::WX_CHALLENGE_BURN_SCALE * world.active[i].severity);
    colony.challenge_burn_mult = mult;
}

// Classify current weather into a challenge candidate (or CHALLENGE_NONE).
uint8_t Coordinator::_classify_weather_challenge(float* sev) const {
    *sev = 0.0f;
    if (!g_weather.valid) return CHALLENGE_NONE;

    // Storm: dangerous wind or a thunderstorm overhead
    if (g_weather.wind >= WIND_HIGH || g_weather.condition == WX_THUNDERSTORM) {
        float s = (g_weather.wind_speed_kmh - 62.0f) / 60.0f;   // 62..122 km/h → 0..1
        if (g_weather.condition == WX_THUNDERSTORM && s < 0.7f) s = 0.7f;
        *sev = fmaxf(0.3f, fminf(1.0f, s));
        return CHALLENGE_STORM;
    }
    if (g_weather.temp >= TEMP_HOT) {
        *sev = fmaxf(0.3f, fminf(1.0f, (g_weather.temperature_c - 28.0f) / 14.0f));
        return CHALLENGE_HEATWAVE;
    }
    if (g_weather.temp == TEMP_FREEZING) {
        *sev = fmaxf(0.3f, fminf(1.0f, (0.0f - g_weather.temperature_c) / 12.0f));
        return CHALLENGE_COLD_SNAP;
    }
    // Drought: hot-dry clear spell (rare here — and so all the more storied)
    if (g_weather.humidity_pct < 30.0f && g_weather.temp >= TEMP_WARM
            && g_weather.condition <= WX_PARTLY_CLOUDY) {
        *sev = fmaxf(0.3f, fminf(0.9f, (30.0f - g_weather.humidity_pct) / 25.0f));
        return CHALLENGE_DROUGHT;
    }
    return CHALLENGE_NONE;
}

void Coordinator::_weather_challenge_tick(uint32_t tick_num) {
    uint32_t now = millis();
    if (now - _wx_last_check_ms < Cfg::WX_CHALLENGE_CHECK_MS) return;
    _wx_last_check_ms = now;

    float sev = 0.0f;
    uint8_t current = _classify_weather_challenge(&sev);

    // One at a time: while any challenge runs (auto or serial-started),
    // we only watch for its end
    if (world.active_count > 0) {
        uint8_t active_type = world.active[world.active_count - 1].type;
        if (current == active_type) {
            _wx_clear_obs = 0;
        } else if (now - _wx_started_ms >= Cfg::WX_CHALLENGE_MIN_MS) {
            if (++_wx_clear_obs >= Cfg::WX_CHALLENGE_CLEAR_OBS) {
                _wx_clear_obs = 0;
                _wx_cooldown_until_ms[active_type] =
                    now + Cfg::WX_CHALLENGE_COOLDOWN_MS;
                challenge_end(tick_num);
            }
        }
        return;
    }

    if (current == CHALLENGE_NONE) {
        _wx_pending_type = CHALLENGE_NONE;
        _wx_pending_obs = 0;
        return;
    }
    if (_wx_cooldown_until_ms[current] != 0
            && now < _wx_cooldown_until_ms[current]) return;

    if (current == _wx_pending_type) {
        _wx_pending_obs++;
    } else {
        _wx_pending_type = current;
        _wx_pending_obs = 1;
    }
    if (_wx_pending_obs >= Cfg::WX_CHALLENGE_START_OBS) {
        _wx_pending_type = CHALLENGE_NONE;
        _wx_pending_obs = 0;
        _wx_started_ms = now;
        challenge_start(current, sev, tick_num);
    }
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

        // Elder: lived past 80% of lifespan. Uses lived_ms — the same clock
        // as the old-age death check (born_at_ms is another module's millis
        // domain after a handoff) — and 64-bit math: lifespan_ms * 4
        // overflows uint32 for lifespans past ~12.4 days, which is how a
        // 1.8-day-old Lupin got an Elder badge. Self-heal: a conker clearly
        // young (<50% lived) loses a wrongly-awarded badge; genuine elders
        // (>80%, and lived only rises) never flap.
        if (w.lifespan_ms > 0) {
            uint64_t threshold = static_cast<uint64_t>(w.lifespan_ms) * 4 / 5;
            if (w.lived_ms >= threshold)
                rec->traits |= TRAIT_ELDER;
            else if (w.lived_ms < w.lifespan_ms / 2)
                rec->traits &= ~TRAIT_ELDER;
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

        // Catcher ("Bug Hunter"): keep the persisted catch count in step with
        // the live conker. Sync UP only — a live count below the bank means
        // the live copy lost history (pre-v179 crossings zeroed it), so
        // restore from the bank instead of nuking it.
        if (w.catches > rec->catches) { rec->catches = w.catches; rec->dirty = true; }
        else if (w.catches < rec->catches) w.catches = rec->catches;

        // Worn accessory: gifts persist within the minute
        if (w.accessory != rec->accessory
                || w.accessory_tint != rec->accessory_tint
                || w.accessory_from != rec->accessory_from
                || w.accessory_memorial != rec->accessory_memorial) {
            rec->accessory          = w.accessory;
            rec->accessory_tint     = w.accessory_tint;
            rec->accessory_from     = w.accessory_from;
            rec->accessory_memorial = w.accessory_memorial;
            rec->dirty = true;
        }

        // Emit trait_earned for newly set bits
        uint32_t new_bits = rec->traits & ~prev_traits;
        if (new_bits) {
            for (int bit = 0; bit < 8; bit++) {
                if (new_bits & (1u << bit)) {
                    JournalEntry je = {};
                    je.tick = tick_num;
                    je.unix_time = g_tod.unix_time;
                    je.type = JEVT_TRAIT_EARNED;
                    je.lilguy_id = w.id;
                    strlcpy(je.who, w.name, sizeof(je.who));
                    je.trait.trait_bit = (1u << bit);
                    journal.emit(je);
                    _emit_trait_bus(w.id, w.name, 1u << bit, tick_num);
                }
            }
        }
        // Persist clears too (bonded hysteresis, elder self-heal)
        if (rec->traits != prev_traits) rec->dirty = true;
    }

    // Deflation: the tally is unbounded on a uint8 — a sunny week pegs the
    // whole roster at 255, the +5 bar lands above the cap, and the title
    // freezes (the v199 pathology, banks re-pegging within days at ~4
    // counting finds/hour). When the leader nears the cap, halve EVERYONE
    // so ordering survives but headroom returns. Only when every living
    // conker is home (usually overnight): the catch sync above is
    // bidirectional-max, so a commuter's un-halved live copy would
    // re-inflate its bank the moment it crossed back.
    {
        IdentityRecord* recs = registry.living_records();
        int rn = registry.living_count();
        uint8_t peak = 0;
        for (int i = 0; i < rn; i++)
            if (recs[i].catches > peak) peak = recs[i].catches;
        if (peak > 200) {
            bool all_home = true;
            for (int i = 0; i < rn && all_home; i++) {
                bool here = false;
                for (int c = 0; c < chamber.conker_count; c++) {
                    if (chamber.conkers[c].alive && chamber.conkers[c].id == recs[i].id) {
                        here = true;
                        break;
                    }
                }
                if (!here) all_home = false;
            }
            if (all_home) {
                for (int i = 0; i < rn; i++) {
                    if (!recs[i].catches) continue;
                    recs[i].catches /= 2;
                    recs[i].dirty = true;
                }
                for (int c = 0; c < chamber.conker_count; c++)
                    chamber.conkers[c].catches /= 2;
                Serial.println("[trait] catch tallies halved (cap deflation)");
            }
        }
    }

    _catcher_resolve(tick_num);

    // Wishes: at most one active colony-wide. A conker with a genuinely
    // unmet need sometimes wishes for something the keeper can grant —
    // the app surfaces it, the keeper answers it, the diary remembers it.
    {
        uint32_t now_ms = millis();
        // Expire an unanswered wish after 2h (they get over it)
        if (wish.kind != 0 && now_ms - wish.set_ms > 7200000UL) {
            wish = Wish{};
        }
        if (wish.kind == 0) {
            for (int i = 0; i < chamber.conker_count; i++) {
                Conker& w = chamber.conkers[i];
                if (w.id == 0 || !w.alive) continue;
                uint8_t kind = 0;
                if (w.hunger > 55.0f) kind = 1;                          // a treat
                else if (w.needs[NEED_BOREDOM] > 0.75f) kind = 2;        // a boop
                if (kind != 0 && g_rng.rand_float() < 0.25f) {
                    wish.conker_id = w.id;
                    wish.kind = kind;
                    wish.set_ms = now_ms;
                    break;
                }
            }
        } else {
            // The wisher may have died or crossed away — let it lapse
            bool present = false;
            for (int i = 0; i < chamber.conker_count; i++)
                if (chamber.conkers[i].id == wish.conker_id
                        && chamber.conkers[i].alive) { present = true; break; }
            if (!present) wish = Wish{};
        }
    }

    // Milestone decor unlocks — monotonic during runtime; recomputed from
    // living records on boot (a keepsake held only by the departed can
    // lapse across a reboot — manifest persistence can come later)
    uint8_t mb = _milestone_bits;
    if (colony.total_workers_born >= 25) mb |= 0x01;
    constexpr uint32_t SURV = TRAIT_SURVIVED_HEATWAVE | TRAIT_SURVIVED_COLD_SNAP
                            | TRAIT_SURVIVED_DROUGHT | TRAIT_SURVIVED_STORM;
    for (int i = 0; i < chamber.conker_count; i++) {
        const Conker& w = chamber.conkers[i];
        if (w.id == 0 || !w.alive) continue;
        const IdentityRecord* rec = registry.get(w.id);
        if (!rec) continue;
        if (rec->traits & SURV)          mb |= 0x02;
        if (rec->traits & TRAIT_BONDED)  mb |= 0x04;
        if (rec->traits & TRAIT_CATCHER) mb |= 0x08;
    }
    _milestone_bits = mb;
}

// Anniversary visits: on the weekly anniversary of a memorial's making,
// its maker sets down whatever they're doing (if idle) and returns to the
// stone for a quiet visit. "Moss visited Fern's stone today" — the single
// most carable behaviour in the codebase. Runs on all modules.
void Coordinator::_anniversary_tick(uint32_t tick_num) {
    uint32_t now_ms = millis();
    if (now_ms - _last_anniv_check_ms < 600000UL) return;   // every 10 min
    _last_anniv_check_ms = now_ms;
    if (g_tod.unix_time == 0) return;

    for (int i = 0; i < Cfg::MAX_ARTWORKS; i++) {
        Artwork& a = chamber.artworks[i];
        if (!a.active || a.kind != ART_MEMORIAL) continue;
        uint32_t age = g_tod.unix_time - a.created_unix;
        if (age < 6UL * 86400UL) continue;          // grief is still fresh
        if ((age / 86400UL) % 7 != 0) continue;     // anniversary days only
        if (a.last_visited_unix != 0
                && g_tod.unix_time - a.last_visited_unix < 3UL * 86400UL)
            continue;                               // already visited this one

        for (int c = 0; c < chamber.conker_count; c++) {
            Conker& w = chamber.conkers[c];
            if (w.id != a.maker_id || !w.alive) continue;
            if (w.state != STATE_IDLE || w.food_carried > 0) break;  // catch them later
            w.state = STATE_MOURNING;
            w.target_x = a.x;
            w.target_y = a.y;
            w.has_target = true;
            w.has_target_cell = false;
            strlcpy(w.mourning_for, a.honoree, sizeof(w.mourning_for));
            w.zoomie_ticks = Cfg::MOURN_ONEWAY_TICKS;   // a shorter, quieter visit
            w.zoomie_target = -1;
            w.sleeping = false;
            w.stack_on = -1;
            w.idle_ticks_remaining = 0;
            a.last_visited_unix = g_tod.unix_time;

            JournalEntry jm = {};
            jm.tick = tick_num;
            jm.unix_time = g_tod.unix_time;
            jm.type = JEVT_MOURNING;
            jm.lilguy_id = w.id;
            strlcpy(jm.who, w.name, sizeof(jm.who));
            strlcpy(jm.mourning.dead_name, a.honoree,
                    sizeof(jm.mourning.dead_name));
            jm.mourning.anniversary = 1;
            if (is_queen()) {
                journal.emit(jm);
            } else if (chamber.home_face >= 0) {
                JournalRelayMessage msg = {};
                msg.msg_type  = TOPO_JOURNAL_RELAY;
                msg.sender_id = topology_my_id();
                msg.jtype     = JEVT_MOURNING;
                msg.lilguy_id = w.id;
                msg.extra     = 1;   // anniversary flag
                strlcpy(msg.who, w.name, sizeof(msg.who));
                strlcpy(msg.honoree, a.honoree, sizeof(msg.honoree));
                topology_send_to_face(static_cast<Face>(chamber.home_face),
                                      (const uint8_t*)&msg, sizeof(msg));
            }
            if (_bus) {
                Event ev = {};
                ev.type = EVT_MOURNING;
                ev.tick = tick_num;
                ev.mourning.mourner_id = w.id;
                strlcpy(ev.mourning.mourner_name, w.name,
                        sizeof(ev.mourning.mourner_name));
                strlcpy(ev.mourning.dead_name, a.honoree,
                        sizeof(ev.mourning.dead_name));
                _bus->emit(ev);
            }
            break;
        }
    }
}

// Garden post staffing (queen only): a garden satellite whose post sits
// vacant says so in its pop sync; the queen answers by sending across her
// best available green thumb. "Available" honours the job ladder — carrying
// food and fulfilling a need both outrank gardening, so haulers, eaters,
// sleepers, mourners, crafters and players are never drafted; idlers and
// empty-pawed foragers are. Daytime only, and famine keeps every hand home.
void Coordinator::_gardener_summon_tick() {
#ifdef ARDUINO
    uint32_t now_ms = millis();
    if (now_ms - _last_gardener_summon_ms < Cfg::GARDENER_SUMMON_COOLDOWN_MS)
        return;
    if (g_tod.phase != PHASE_DAY) return;
    if (colony.food_pressure() > Cfg::FAMINE_SLOWDOWN_PRESSURE) return;

    // A gardener already on their way covers any vacancy
    for (int i = 0; i < chamber.conker_count; i++) {
        const Conker& w = chamber.conkers[i];
        if (w.alive && w.state == STATE_TO_GARDEN) return;
    }

    // Find a garden face asking for hands
    int garden_face = -1;
    for (int f = 0; f < FACE_COUNT; f++) {
        if (!topology_neighbour(static_cast<Face>(f)).present) continue;
        if (chamber.entries[f] < 0) continue;
        if (!topology_pop_sync_fresh(static_cast<Face>(f))) continue;
        if (topology_remote_role(static_cast<Face>(f)) != MODULE_GARDEN) continue;
        if (!topology_remote_gardener_wanted(static_cast<Face>(f))) continue;
        garden_face = f;
        break;
    }
    if (garden_face < 0) return;

    // Best available green thumb takes the walk. A company-seeker is mid-need
    // (fulfilling need > gardening) — and the seek tick force-resets state to
    // IDLE, which silently killed every draft in v188/189 one tick after the
    // summon, so seekers are both ineligible and defensively cleared below.
    int best = -1;
    float best_gt = 0.0f;
    for (int i = 0; i < chamber.conker_count; i++) {
        const Conker& w = chamber.conkers[i];
        if (!w.alive || w.departing || w.sleeping || w.seeking_company) continue;
        if (w.food_carried > 0) continue;
        if (w.state != STATE_IDLE && w.state != STATE_TO_FOOD) continue;
        float gt = w.green_thumb();
        if (gt < Cfg::GREEN_THUMB_MIN) continue;
        if (gt > best_gt) { best_gt = gt; best = i; }
    }
    if (best < 0) return;

    Conker& w = chamber.conkers[best];
    w.state = STATE_TO_GARDEN;
    w.zoomie_target = static_cast<int16_t>(garden_face);  // repurposed: face
    w.zoomie_ticks = Cfg::TO_GARDEN_MAX_TICKS;
    w.has_target = false;
    w.has_target_cell = false;
    w.sleeping = false;
    w.seeking_company = false;
    w.seek_ticks = 0;
    w.stack_on = -1;
    w.idle_ticks_remaining = 0;
    _last_gardener_summon_ms = now_ms;
    Serial.printf("[garden] post vacant — %s (green thumb %.2f) sent across\r\n",
                  w.name, best_gt);
#endif
}

// Garden self-staffing (satellite side): when the post sits vacant by day
// and a qualified green thumb is already on the module — usually a forager
// passing through, who would never re-run task selection here — draft them
// on the spot. Their next task pick claims the post and heads for a plot.
// Keeps the garden staffed even when the queen's summon channel is flaky.
void Coordinator::_garden_draft_tick() {
    if (is_queen() || role != MODULE_GARDEN) return;
    uint32_t now_ms = millis();
    if (now_ms - _last_garden_draft_ms < 2000) return;
    _last_garden_draft_ms = now_ms;
    if (g_tod.phase != PHASE_DAY) return;
    if (chamber.garden_post_filled()) return;

    int best = -1;
    float best_gt = 0.0f;
    for (int i = 0; i < chamber.conker_count; i++) {
        const Conker& w = chamber.conkers[i];
        if (!w.alive || w.departing || w.sleeping || w.seeking_company) continue;
        if (w.food_carried > 0) continue;
        if (w.state != STATE_IDLE && w.state != STATE_TO_FOOD
                && w.state != STATE_TO_HOME) continue;
        float gt = w.green_thumb();
        if (gt < Cfg::GREEN_THUMB_MIN) continue;
        if (gt > best_gt) { best_gt = gt; best = i; }
    }
    if (best < 0) return;

    Conker& w = chamber.conkers[best];
    w.state = STATE_IDLE;
    w.has_target = false;
    w.has_target_cell = false;
    w.stack_on = -1;
    w.idle_ticks_remaining = 0;
    w.idle_repoll_tick = 0;
    Serial.printf("[garden] %s drafted off the trail to keep the garden\r\n",
                  w.name);
}

// Display-bus mirror of JEVT_TRAIT_EARNED — sparkle + HUD banner on the glass.
void Coordinator::_emit_trait_bus(uint32_t id, const char* name,
                                  uint32_t trait_bit, uint32_t tick_num) {
    if (!_bus) return;
    Event ev = {};
    ev.type = EVT_TRAIT_EARNED;
    ev.tick = tick_num;
    ev.trait.conker_id = id;
    ev.trait.trait_bit = trait_bit;
    strlcpy(ev.trait.who, name, sizeof(ev.trait.who));
    _bus->emit(ev);
}

// "Bug Hunter" is a single, colony-wide title that scales in steps of 5
// catches. The first conker to reach 5 wins it; to take it from the holder a
// challenger must reach the next 5-multiple STRICTLY above the holder's bank
// (holder at 10 → challenger needs 15, not 11 — beating them by one doesn't
// unseat a champion). The holder defends automatically: as their own count
// climbs, the bar climbs with them. Only living conkers compete for the live
// title; a deceased champion keeps the badge as a keepsake (same as Bonded).
void Coordinator::_catcher_resolve(uint32_t tick_num) {
    // Identify the current living holder (heal any duplicates by keeping the
    // strongest — there should only ever be one).
    int holder_idx = -1;
    uint8_t holder_catches = 0;
    for (int i = 0; i < chamber.conker_count; i++) {
        Conker& w = chamber.conkers[i];
        if (w.id == 0 || !w.alive) continue;
        IdentityRecord* rec = registry.get(w.id);
        if (!rec || !(rec->traits & TRAIT_CATCHER)) continue;
        if (holder_idx < 0 || w.catches > holder_catches) {
            holder_idx = i;
            holder_catches = w.catches;
        }
    }

    // The champion may be alive but AWAY — commuting to the garden next door.
    // They still hold the title. Without this, every crossing by the champion
    // read as a vacancy (bar resets to 5) and the badge was re-awarded to
    // whoever stood nearest a snail — ~7 journal handoffs an hour, and the
    // v196 firefly fix couldn't touch it because the tally wasn't the driver.
    if (holder_idx < 0) {
        IdentityRecord* recs = registry.living_records();
        int n = registry.living_count();
        for (int i = 0; i < n; i++) {
            if (recs[i].traits & TRAIT_CATCHER) return;  // held in absentia
        }
    }

    // Enforce the single-holder invariant: clear the badge from every other
    // living conker. Heals the pre-v131 legacy where Bug Hunter was handed to
    // anyone with >=5 catches, so a whole colony could carry it. The rightful
    // champion is the strongest living holder; deceased champions keep theirs
    // as a keepsake (they're not in this living-only scan).
    for (int i = 0; i < chamber.conker_count; i++) {
        if (i == holder_idx) continue;
        Conker& w = chamber.conkers[i];
        if (w.id == 0 || !w.alive) continue;
        IdentityRecord* rec = registry.get(w.id);
        if (rec && (rec->traits & TRAIT_CATCHER)) {
            rec->traits &= ~TRAIT_CATCHER;
            rec->dirty = true;
        }
    }

    // The crown sits for at least a day: challenges resolve at most once per
    // 24h. Persisted — the reboot shuffle used to hand the badge out for
    // free while the roster was still re-homing. Death is exempt: the hold
    // only applies while a living holder is defending (holder_idx >= 0), so
    // a vacated title refills immediately.
    static uint32_t _last_award_unix = UINT32_MAX;   // UINT32_MAX = not yet loaded
    if (_last_award_unix == UINT32_MAX) {
        Preferences prefs;
        prefs.begin("hive", true);
        _last_award_unix = prefs.getULong("catcher_unix", 0);
        prefs.end();
    }
    if (_last_award_unix == 0 && g_tod.unix_time > 1000000) {
        // First run under this rule: start the clock now so a freshly
        // flashed board doesn't hand the title out in its boot shuffle.
        _last_award_unix = g_tod.unix_time;
        Preferences prefs;
        prefs.begin("hive", false);
        prefs.putULong("catcher_unix", _last_award_unix);
        prefs.end();
    }
    if (holder_idx >= 0 && _last_award_unix != 0
        && g_tod.unix_time > _last_award_unix
        && g_tod.unix_time - _last_award_unix < 24UL * 3600UL) {
        return;
    }

    // The bar is the next 5-multiple strictly above the holder's banked
    // catches (so the holder never qualifies as their own challenger), or 5
    // when the title is currently vacant.
    uint16_t bar = (holder_idx >= 0) ? (uint16_t)((holder_catches / 5 + 1) * 5) : 5;

    // Strongest living challenger that has reached the bar.
    int best_idx = -1;
    uint8_t best_catches = 0;
    for (int i = 0; i < chamber.conker_count; i++) {
        Conker& w = chamber.conkers[i];
        if (w.id == 0 || !w.alive) continue;
        if (w.catches < bar) continue;
        if (best_idx < 0 || w.catches > best_catches) {
            best_idx = i;
            best_catches = w.catches;
        }
    }

    if (best_idx < 0 || best_idx == holder_idx) return;  // title unchanged

    // Hand the badge over: strip it from the previous living holder, award the
    // challenger, and announce it in the journal.
    if (holder_idx >= 0) {
        IdentityRecord* old = registry.get(chamber.conkers[holder_idx].id);
        if (old && (old->traits & TRAIT_CATCHER)) {
            old->traits &= ~TRAIT_CATCHER;
            old->dirty = true;
        }
    }
    Conker& champ = chamber.conkers[best_idx];
    IdentityRecord* rec = registry.get(champ.id);
    if (rec) {
        rec->traits |= TRAIT_CATCHER;
        rec->dirty = true;
        // Personality drift: the crown emboldens — champions range wider.
        _drift_personality(champ, rec, PERS_EXPLORATION,
                           Cfg::DRIFT_CROWN_EXPLORATION);
        if (g_tod.unix_time > 1000000) {
            _last_award_unix = g_tod.unix_time;
            Preferences prefs;
            prefs.begin("hive", false);
            prefs.putULong("catcher_unix", _last_award_unix);
            prefs.end();
        }
        JournalEntry je = {};
        je.tick = tick_num;
        je.unix_time = g_tod.unix_time;
        je.type = JEVT_TRAIT_EARNED;
        je.lilguy_id = champ.id;
        strlcpy(je.who, champ.name, sizeof(je.who));
        je.trait.trait_bit = TRAIT_CATCHER;
        journal.emit(je);
        _emit_trait_bus(champ.id, champ.name, TRAIT_CATCHER, tick_num);
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

    // Display bus: the glass announces the weather turning
    if (_bus) {
        Event ev = {};
        ev.type = EVT_CHALLENGE_STARTED;
        ev.tick = tick_num;
        ev.challenge.challenge_type = type;
        ev.challenge.severity = severity;
        _bus->emit(ev);
    }

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

    // Display bus first, so the "it passed" banner precedes the survivor
    // trait banners in the queue
    if (_bus) {
        Event ev = {};
        ev.type = EVT_CHALLENGE_ENDED;
        ev.tick = tick_num;
        ev.challenge.challenge_type = ended.type;
        ev.challenge.severity = ended.severity;
        _bus->emit(ev);
    }

    // Award survival trait to all currently alive workers
    uint32_t survival_bit = challenge_survival_trait(ended.type);
    if (survival_bit == 0) return;

    int survivors = 0;
    for (int i = 0; i < chamber.conker_count; i++) {
        Conker& w = chamber.conkers[i];
        if (w.id == 0 || !w.alive) continue;
        IdentityRecord* rec = registry.get(w.id);
        if (!rec) continue;
        // Personality drift: every survival steels them a little — not just
        // the first of each challenge type (the trait below fires once).
        _drift_personality(w, rec, PERS_BRAVERY, Cfg::DRIFT_SURVIVE_BRAVERY);
        _drift_personality(w, rec, PERS_HARDINESS, Cfg::DRIFT_SURVIVE_HARDINESS);
        if (!(rec->traits & survival_bit)) {
            rec->traits |= survival_bit;
            rec->dirty = true;
            survivors++;

            JournalEntry te = {};
            te.tick = tick_num;
            te.unix_time = g_tod.unix_time;
            te.type = JEVT_TRAIT_EARNED;
            te.lilguy_id = w.id;
            strlcpy(te.who, w.name, sizeof(te.who));
            te.trait.trait_bit = survival_bit;
            journal.emit(te);
            _emit_trait_bus(w.id, w.name, survival_bit, tick_num);
        }
    }

    Serial.printf("[challenge] ended type=%d, %d survivors earned trait\r\n",
                  ended.type, survivors);
}
