/* First-boot setup wizard — runs when the module has no role in NVS.
 *
 * Blocking, touch-driven, pre-sim. Two paths:
 *   Found a colony → SoftAP captive portal for WiFi (password typed on the
 *                    phone), role=queen, then a founding ceremony after the
 *                    colony is created.
 *   Join a colony  → role=satellite, module waits to be snapped onto a queen.
 *
 * Re-trigger by clearing NVS ("factory" serial command).
 */
#pragma once

class Arduino_Canvas;

enum SetupChoice : unsigned char {
    SETUP_NONE = 0,    // wizard didn't run (role already configured)
    SETUP_FOUNDED,     // user chose to found a new colony (role=queen written)
    SETUP_JOINED,      // user chose add-on module (role=satellite written)
};

// True when NVS has no module role — a factory-fresh module.
bool setup_wizard_required();

// Run the blocking wizard. Writes role (and WiFi creds on the found path)
// to NVS before returning.
SetupChoice setup_wizard_run(Arduino_Canvas* gfx);

// Founding ceremony — call after sim.init() has created the colony so the
// names exist. Shows colony name, queen name, and the app URL.
void setup_wizard_ceremony(Arduino_Canvas* gfx, const char* colony_id,
                           const char* queen_name);
