/* API JSON serialization — wire-protocol payloads for Phase 7 HTTP server. */
#include "api_json.h"
#include "coordinator.h"
#include "time_of_day.h"
#include "topology.h"
#include "renderer.h"
#include "names.h"
#include "world_condition.h"

#include <Arduino.h>
#include <ArduinoJson.h>

// ---- Trait bit to string helpers ----

static const char* TRAIT_STRINGS[] = {
    "Pioneer", "Elder", "Bonded",
    "Survived Heatwave", "Survived Cold Snap", "Survived Drought", "Survived Storm"
};

static void add_traits_array(JsonArray& arr, uint32_t traits) {
    for (int i = 0; i < 7; i++) {
        if (traits & (1u << i)) arr.add(TRAIT_STRINGS[i]);
    }
}

// Live behaviour state for a worker, looked up in the local chamber.
// Workers currently on another module aren't in our chamber — report "away".
static const char* conker_state_str(Coordinator& coord, uint32_t id) {
    for (int i = 0; i < coord.chamber.conker_count; i++) {
        const Conker& c = coord.chamber.conkers[i];
        if (c.id != id) continue;
        switch (c.state) {
            case STATE_IDLE:        return c.sleeping ? "sleeping" : "idle";
            case STATE_TEND_QUEEN:  return "tending_queen";
            case STATE_TEND_BROOD:  return "tending_brood";
            case STATE_TO_FOOD:     return "gathering";
            case STATE_TO_HOME:     return "homebound";
            case STATE_CANNIBALIZE: return "cannibalizing";
            case STATE_ZOOMIES:     return "zoomies";
            case STATE_EATING:      return "eating";
            case STATE_MOURNING:    return "mourning";
            default:                return "idle";
        }
    }
    return "away";
}

static const char* phase_str(DayPhase p) {
    switch (p) {
        case PHASE_DAY:   return "day";
        case PHASE_NIGHT: return "night";
        case PHASE_DAWN:  return "dawn";
        case PHASE_DUSK:  return "dusk";
        default:          return "unknown";
    }
}

static const char* season_str(Season s) {
    switch (s) {
        case SEASON_SPRING: return "spring";
        case SEASON_SUMMER: return "summer";
        case SEASON_AUTUMN: return "autumn";
        case SEASON_WINTER: return "winter";
        default:            return "indeterminate";
    }
}

static const char* challenge_str(uint8_t t) {
    switch (t) {
        case CHALLENGE_HEATWAVE:  return "heatwave";
        case CHALLENGE_COLD_SNAP: return "cold_snap";
        case CHALLENGE_DROUGHT:   return "drought";
        case CHALLENGE_STORM:     return "storm";
        default:                  return "unknown";
    }
}

// ---- GET /api/v1/colony ----

size_t api_colony_json(Coordinator& coord, char* buf, size_t buflen) {
    JsonDocument doc;
    doc["schema"] = 1;
    doc["colony_id"] = coord.registry.manifest().colony_id;
    doc["queen_name"] = coord.registry.manifest().queen_name;
    doc["fw_version"] = FW_VERSION;
    doc["founded_unix"] = coord.registry.manifest().founded_unix;
    doc["now_unix"] = g_tod.unix_time;

    // World
    JsonObject w = doc["world"].to<JsonObject>();
    JsonObject tod = w["tod"].to<JsonObject>();
    tod["phase"] = phase_str(coord.world.tod.phase);
    tod["night_factor"] = coord.world.tod.night_factor;
    tod["day_progress"] = coord.world.tod.day_progress;
    w["day_of_year"] = coord.world.day_of_year;
    w["temperature_c"] = coord.world.temperature_c;
    w["humidity_pct"] = coord.world.humidity_pct;
    static const char* wx_str[] = {"clear","partly_cloudy","overcast","fog","drizzle","rain","heavy_rain","snow","thunderstorm"};
    w["weather"] = wx_str[coord.world.weather < 9 ? coord.world.weather : 0];
    w["season"] = season_str(coord.world.season);
    JsonArray challenges = w["active_challenges"].to<JsonArray>();
    for (int i = 0; i < coord.world.active_count; i++) {
        JsonObject c = challenges.add<JsonObject>();
        c["type"] = challenge_str(coord.world.active[i].type);
        c["severity"] = coord.world.active[i].severity;
    }

    // Population (count from registry for consistency across modules)
    JsonObject pop = doc["population"].to<JsonObject>();
    pop["alive"] = coord.colony.population;
    pop["dead_total"] = coord.registry.manifest().total_workers_died;
    JsonObject by_role = pop["by_role"].to<JsonObject>();
    by_role["queen"] = coord.chamber.has_queen ? 1 : 0;
    int conkers_count = coord.registry.living_count();
    int founders = 0;
    IdentityRecord* recs = coord.registry.living_records();
    for (int i = 0; i < conkers_count; i++) {
        if (recs[i].is_founder) founders++;
    }
    by_role["conker"] = conkers_count;
    by_role["founder"] = founders;
    by_role["brood_egg"] = coord.colony.brood_egg;
    by_role["brood_seed"] = coord.colony.brood_seed;

    // Living conkers roster (compact — name, age, founder flag, traits)
    JsonArray lilguys = doc["lilguys"].to<JsonArray>();
    for (int i = 0; i < conkers_count; i++) {
        IdentityRecord& r = recs[i];
        JsonObject lg = lilguys.add<JsonObject>();
        lg["id"] = r.id;
        lg["name"] = r.name;
        lg["age_days"] = (r.born_unix > 0 && g_tod.unix_time > r.born_unix)
            ? (g_tod.unix_time - r.born_unix) / 86400.0f
            : r.lived_ms / (86400.0f * 1000.0f);
        if (r.is_founder) lg["founder"] = true;
        if (r.traits) {
            JsonArray tr = lg["traits"].to<JsonArray>();
            add_traits_array(tr, r.traits);
        }
        // Appearance
        lg["scale_factor"] = r.scale_factor;
        lg["tint_seed"] = r.tint_seed;
        // Personality (available via VPS, no LAN needed)
        JsonObject pers = lg["personality"].to<JsonObject>();
        pers["work_tempo"] = r.personality[0];
        pers["exploration"] = r.personality[1];
        pers["route_stickiness"] = r.personality[2];
        pers["social_frequency"] = r.personality[3];
        pers["food_preference"] = r.personality[4];
        pers["hardiness"] = r.personality[5];
        pers["learning_rate"] = r.personality[6];
        // Formed bonds: who this conker is close to, strongest first
        {
            BondEntry bonds[BondStore::PER_OWNER_CAP];
            int nb = coord.bonds.get_bonds(r.id, bonds, BondStore::PER_OWNER_CAP);
            JsonArray ba;
            for (int b = 0; b < nb; b++) {
                if (!bonds[b].formed) continue;
                IdentityRecord* other = coord.registry.get(bonds[b].target);
                if (!other) continue;
                if (ba.isNull()) ba = lg["bonds"].to<JsonArray>();
                JsonObject bo = ba.add<JsonObject>();
                bo["id"] = bonds[b].target;
                bo["name"] = other->name;
                bo["strength"] = bonds[b].strength;
            }
        }
    }

    // Food
    JsonObject food = doc["food"].to<JsonObject>();
    food["store"] = coord.colony.food_store;
    float burn = coord.colony.daily_burn();
    food["daily_burn"] = burn;
    food["days_remaining"] = (burn > 0) ? coord.colony.food_total / burn : 999;

    // Modules
    JsonArray modules = doc["modules"].to<JsonArray>();
    {
        JsonObject queen_mod = modules.add<JsonObject>();
        char id_str[8];
        snprintf(id_str, sizeof(id_str), "%04X", topology_my_id());
        queen_mod["id"] = id_str;
        queen_mod["role"] = "queen";
        queen_mod["online"] = true;
        uint32_t qt = renderer_get_floor_tint();
        if (qt) {
            char tint_str[8];
            snprintf(tint_str, sizeof(tint_str), "#%06lX", (unsigned long)qt);
            queen_mod["tint"] = tint_str;
        }
        JsonObject faces = queen_mod["faces"].to<JsonObject>();
        for (int f = 0; f < FACE_COUNT; f++) {
            const Neighbour& nb = topology_neighbour(static_cast<Face>(f));
            if (nb.present) {
                const char* face_names[] = {"north", "south", "west", "east"};
                char nb_str[8];
                snprintf(nb_str, sizeof(nb_str), "%04X", nb.module_id);
                faces[face_names[f]] = nb_str;
            }
        }
    }
    // Add connected satellites
    for (int f = 0; f < FACE_COUNT; f++) {
        const Neighbour& nb = topology_neighbour(static_cast<Face>(f));
        if (nb.present) {
            JsonObject sat = modules.add<JsonObject>();
            char id_str[8];
            snprintf(id_str, sizeof(id_str), "%04X", nb.module_id);
            sat["id"] = id_str;
            // Actual role echoed via pop sync; fall back until first sync arrives.
            // A foreign queen never pop-syncs — name her for what she is.
            if (coord.foreign_face[f]) {
                sat["role"] = "foreign_queen";
            } else {
                uint8_t rr = topology_remote_role(static_cast<Face>(f));
                sat["role"] = rr ? module_role_str(rr) : "satellite";
            }
            uint32_t st = topology_remote_tint(static_cast<Face>(f));
            if (st) {
                char tint_str[8];
                snprintf(tint_str, sizeof(tint_str), "#%06lX", (unsigned long)st);
                sat["tint"] = tint_str;
            }
            sat["online"] = true;
            JsonObject faces = sat["faces"].to<JsonObject>();
            const char* face_names[] = {"north", "south", "west", "east"};
            char q_str[8];
            snprintf(q_str, sizeof(q_str), "%04X", topology_my_id());
            faces[face_names[Cfg::FACE_OPPOSITE[f]]] = q_str;
        }
    }

    return serializeJson(doc, buf, buflen);
}

// ---- GET /api/v1/lilguys ----

size_t api_lilguys_json(Coordinator& coord, char* buf, size_t buflen,
                        int limit, int offset) {
    JsonDocument doc;
    doc["schema"] = 1;

    int total = coord.registry.living_count();
    doc["total"] = total;

    JsonArray results = doc["results"].to<JsonArray>();
    IdentityRecord* recs = coord.registry.living_records();

    int start = (offset < total) ? offset : total;
    int end = (start + limit < total) ? start + limit : total;

    for (int i = start; i < end; i++) {
        IdentityRecord& r = recs[i];
        JsonObject entry = results.add<JsonObject>();
        entry["id"] = r.id;
        entry["name"] = r.name;
        entry["role"] = "conker";
        if (r.is_founder) entry["founder"] = true;

        float age_days = (r.born_unix > 0 && g_tod.unix_time > r.born_unix)
            ? (g_tod.unix_time - r.born_unix) / 86400.0f
            : r.lived_ms / (86400.0f * 1000.0f);
        entry["age_days"] = age_days;

        JsonArray traits = entry["traits"].to<JsonArray>();
        add_traits_array(traits, r.traits);

        entry["scale_factor"] = r.scale_factor;
        entry["tint_seed"] = r.tint_seed;
        entry["state"] = conker_state_str(coord, r.id);
    }

    return serializeJson(doc, buf, buflen);
}

// ---- GET /api/v1/lilguys/<id> ----

size_t api_lilguy_detail_json(Coordinator& coord, uint32_t id,
                              char* buf, size_t buflen) {
    IdentityRecord* rec = coord.registry.get(id);
    if (!rec) return 0;

    JsonDocument doc;
    doc["schema"] = 1;
    doc["id"] = rec->id;
    doc["name"] = rec->name;
    doc["role"] = "conker";
    if (rec->is_founder) doc["founder"] = true;
    doc["born_unix"] = rec->born_unix;
    doc["age_days"] = (rec->born_unix > 0 && g_tod.unix_time > rec->born_unix)
        ? (g_tod.unix_time - rec->born_unix) / 86400.0f
        : rec->lived_ms / (86400.0f * 1000.0f);
    if (rec->died_unix == 0)
        doc["died_unix"] = nullptr;
    else
        doc["died_unix"] = rec->died_unix;

    // Appearance
    doc["scale_factor"] = rec->scale_factor;
    doc["tint_seed"] = rec->tint_seed;

    doc["state"] = conker_state_str(coord, id);

    // Tended by
    if (rec->tended_by != 0) {
        JsonObject tb = doc["tended_by"].to<JsonObject>();
        tb["id"] = rec->tended_by;
        IdentityRecord* carer = coord.registry.get(rec->tended_by);
        tb["name"] = carer ? carer->name : "unknown";
    } else {
        doc["tended_by"] = nullptr;
    }

    // Personality
    JsonObject pers = doc["personality"].to<JsonObject>();
    pers["work_tempo"] = rec->personality[0];
    pers["exploration"] = rec->personality[1];
    pers["route_stickiness"] = rec->personality[2];
    pers["social_frequency"] = rec->personality[3];
    pers["food_preference"] = rec->personality[4];
    pers["hardiness"] = rec->personality[5];
    pers["learning_rate"] = rec->personality[6];

    // Traits
    JsonArray traits = doc["traits"].to<JsonArray>();
    add_traits_array(traits, rec->traits);

    // Bonds
    JsonArray bonds_arr = doc["bonds"].to<JsonArray>();
    BondEntry worker_bonds[16];
    int bc = coord.bonds.get_bonds(id, worker_bonds, 16);
    for (int i = 0; i < bc; i++) {
        JsonObject b = bonds_arr.add<JsonObject>();
        JsonObject to = b["to"].to<JsonObject>();
        to["id"] = worker_bonds[i].target;
        char name_buf[20];
        name_from_id(worker_bonds[i].target, name_buf, sizeof(name_buf));
        to["name"] = name_buf;
        b["strength"] = worker_bonds[i].strength;
    }

    // Position
    JsonObject pos = doc["last_pos"].to<JsonObject>();
    char mod_str[8];
    snprintf(mod_str, sizeof(mod_str), "%04X", topology_my_id());
    pos["module"] = mod_str;
    pos["x"] = rec->last_x;
    pos["y"] = rec->last_y;

    // Event count (approximate — would need journal scan for exact)
    doc["event_count"] = 0;  // placeholder, Phase 7 can compute

    return serializeJson(doc, buf, buflen);
}

// ---- GET /api/v1/health ----

size_t api_health_json(Coordinator& coord, char* buf, size_t buflen) {
    JsonDocument doc;
    doc["schema"] = 1;
    doc["status"] = "ok";
    doc["uptime_s"] = millis() / 1000;

    const char* ps[] = {"uninit", "ok", "degraded", "error"};
    doc["persistence"] = ps[coord.registry.state()];

    return serializeJson(doc, buf, buflen);
}
