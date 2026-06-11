/* First-boot setup wizard — see setup_wizard.h. */
#include "setup_wizard.h"
#include "touch.h"
#include "time_of_day.h"
#include "coordinator.h"
#include "sd_card.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

static constexpr int W = 480, H = 320;

// ---- Palette (warm, matches the chamber's evening tones) ----
static inline uint16_t _rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static const uint16_t COL_BG     = _rgb(24, 18, 14);    // deep bark
static const uint16_t COL_TEXT   = _rgb(235, 220, 195); // warm parchment
static const uint16_t COL_DIM    = _rgb(140, 125, 105);
static const uint16_t COL_AMBER  = _rgb(228, 160, 60);  // lantern amber
static const uint16_t COL_BTN    = _rgb(48, 38, 28);
static const uint16_t COL_BTN_HI = _rgb(70, 54, 36);
static const uint16_t COL_GREEN  = _rgb(120, 190, 90);

// ---- Drawing helpers ----

static void _text_centered(Arduino_Canvas* gfx, int y, uint8_t size,
                           uint16_t col, const char* s) {
    int w = (int)strlen(s) * 6 * size;
    gfx->setTextSize(size);
    gfx->setTextColor(col);
    gfx->setCursor((W - w) / 2, y);
    gfx->print(s);
}

struct Button { int x, y, w, h; };

static void _draw_button(Arduino_Canvas* gfx, const Button& b,
                         const char* label, uint16_t border) {
    gfx->fillRoundRect(b.x, b.y, b.w, b.h, 10, COL_BTN);
    gfx->drawRoundRect(b.x, b.y, b.w, b.h, 10, border);
    int tw = (int)strlen(label) * 6 * 2;
    gfx->setTextSize(2);
    gfx->setTextColor(COL_TEXT);
    gfx->setCursor(b.x + (b.w - tw) / 2, b.y + (b.h - 16) / 2);
    gfx->print(label);
}

static bool _hit(const Button& b, const TouchEvent& t) {
    return t.x >= b.x && t.x < b.x + b.w && t.y >= b.y && t.y < b.y + b.h;
}

// ---- NVS ----

bool setup_wizard_required() {
    Preferences prefs;
    prefs.begin("hive", true);
    uint8_t role = prefs.getUChar("module_role", MODULE_UNCONFIGURED);
    prefs.end();
    return role == MODULE_UNCONFIGURED;
}

// ---- Captive portal ----

static String _portal_page(const String& networks_html, const String& notice) {
    String page =
        "<!DOCTYPE html><html><head><meta name='viewport' "
        "content='width=device-width,initial-scale=1'>"
        "<title>Hive Setup</title><style>"
        "body{font-family:sans-serif;background:#181210;color:#ebdcc3;"
        "margin:0;padding:24px}"
        "h1{font-size:1.3em;color:#e4a03c}"
        ".n{display:block;background:#302620;border:1px solid #46362a;"
        "border-radius:10px;padding:12px 14px;margin:8px 0;color:#ebdcc3;"
        "text-decoration:none;font-size:1.05em}"
        "input{width:100%;box-sizing:border-box;padding:12px;margin:8px 0;"
        "border-radius:8px;border:1px solid #46362a;background:#241c16;"
        "color:#ebdcc3;font-size:1.1em}"
        "button{width:100%;padding:14px;border-radius:10px;border:0;"
        "background:#e4a03c;color:#181210;font-size:1.1em;font-weight:bold}"
        ".d{color:#8c7d69;font-size:.9em}"
        "</style></head><body>"
        "<h1>&#127792; Hive Module Setup</h1>";
    if (notice.length()) page += "<p style='color:#e4634a'>" + notice + "</p>";
    page += networks_html;
    page += "<p class='d'><a class='d' href='/?rescan=1'>&#8635; rescan networks</a></p>"
            "</body></html>";
    return page;
}

static String _scan_networks_html() {
    int n = WiFi.scanNetworks(false, false, false, 300);
    String html = "<p>Choose your WiFi network:</p>";
    if (n <= 0) {
        html += "<p class='d'>No networks found — try a rescan.</p>";
        return html;
    }
    // List strongest-first, dedupe SSIDs, cap at 12
    bool used[64] = {};
    int shown = 0;
    while (shown < 12) {
        int best = -1;
        for (int i = 0; i < n && i < 64; i++) {
            if (used[i] || WiFi.SSID(i).length() == 0) continue;
            if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
        }
        if (best < 0) break;
        String ssid = WiFi.SSID(best);
        for (int i = 0; i < n && i < 64; i++)
            if (WiFi.SSID(i) == ssid) used[i] = true;
        String esc = ssid;
        esc.replace("\"", "&quot;");
        html += "<a class='n' href='/pw?ssid=" + esc + "'>" + esc;
        if (WiFi.encryptionType(best) == WIFI_AUTH_OPEN) html += " <span class='d'>(open)</span>";
        html += "</a>";
        shown++;
    }
    WiFi.scanDelete();
    return html;
}

static void _portal_screen(Arduino_Canvas* gfx, const char* ap_name,
                           const char* status, const Button& skip_btn) {
    gfx->fillScreen(COL_BG);
    _text_centered(gfx, 36, 3, COL_AMBER, "WiFi setup");
    _text_centered(gfx, 92, 2, COL_TEXT, "on your phone, connect to");
    _text_centered(gfx, 122, 3, COL_TEXT, ap_name);
    _text_centered(gfx, 168, 2, COL_DIM, "a setup page will pop up");
    _text_centered(gfx, 210, 2, COL_AMBER, status);
    _draw_button(gfx, skip_btn, "skip for now", COL_DIM);
    gfx->flush();
}

// Runs the captive portal until creds are saved (returns true) or the user
// taps skip (returns false). Blocking; serves DNS + HTTP in the loop.
static bool _run_wifi_portal(Arduino_Canvas* gfx) {
    char ap_name[24];
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(ap_name, sizeof(ap_name), "Hive-Setup-%02X%02X", mac[4], mac[5]);

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(ap_name);  // open network — onboarding only, torn down after
    delay(100);
    IPAddress ap_ip = WiFi.softAPIP();

    DNSServer dns;
    dns.start(53, "*", ap_ip);
    WebServer server(80);

    String chosen_ssid;
    bool saved = false;

    server.on("/", [&]() {
        server.send(200, "text/html", _portal_page(_scan_networks_html(), ""));
    });
    server.on("/pw", [&]() {
        chosen_ssid = server.arg("ssid");
        String esc = chosen_ssid;
        esc.replace("\"", "&quot;");
        String form =
            "<p>Password for <b>" + esc + "</b>:</p>"
            "<form method='POST' action='/save'>"
            "<input type='hidden' name='ssid' value=\"" + esc + "\">"
            "<input type='password' name='pass' placeholder='WiFi password' autofocus>"
            "<button type='submit'>Connect</button></form>"
            "<p class='d'><a class='d' href='/'>&larr; back to networks</a></p>";
        server.send(200, "text/html", _portal_page(form, ""));
    });
    server.on("/save", HTTP_POST, [&]() {
        String ssid = server.arg("ssid");
        String pass = server.arg("pass");
        if (ssid.length() == 0) { server.send(400, "text/plain", "no ssid"); return; }

        char status[48];
        snprintf(status, sizeof(status), "trying %.20s...", ssid.c_str());
        Button none{-100, -100, 1, 1};
        _portal_screen(gfx, ap_name, status, none);

        WiFi.begin(ssid.c_str(), pass.c_str());
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(250);

        if (WiFi.status() == WL_CONNECTED) {
            tod_wifi_set_ssid(ssid.c_str());
            tod_wifi_set_pass(pass.c_str());
            saved = true;
            server.send(200, "text/html", _portal_page(
                "<p style='color:#78be5a;font-size:1.2em'>&#10003; Connected!</p>"
                "<p>All set — look at the module. You can close this page.</p>", ""));
        } else {
            WiFi.disconnect();
            String esc = ssid; esc.replace("\"", "&quot;");
            server.send(200, "text/html", _portal_page(
                "<p><a class='n' href='/pw?ssid=" + esc + "'>try " + esc + " again</a></p>"
                "<p class='d'><a class='d' href='/'>&larr; pick a different network</a></p>",
                "Couldn't connect — wrong password?"));
        }
    });
    // Captive-portal detection endpoints + everything else → redirect to us
    server.onNotFound([&]() {
        server.sendHeader("Location", String("http://") + ap_ip.toString() + "/", true);
        server.send(302, "text/plain", "");
    });
    server.begin();

    Button skip_btn{W - 180, H - 56, 164, 40};
    _portal_screen(gfx, ap_name, "waiting for your phone...", skip_btn);

    bool phone_seen = false;
    uint32_t done_at = 0;
    bool skipped = false;
    while (true) {
        dns.processNextRequest();
        server.handleClient();

        if (saved && done_at == 0) {
            char status[48];
            snprintf(status, sizeof(status), "connected to %.20s!", WiFi.SSID().c_str());
            Button none{-100, -100, 1, 1};
            _portal_screen(gfx, ap_name, status, none);
            done_at = millis();  // linger so the phone gets the success page
        }
        if (done_at && millis() - done_at > 2500) break;

        if (!saved && !phone_seen && WiFi.softAPgetStationNum() > 0) {
            phone_seen = true;
            _portal_screen(gfx, ap_name, "phone connected - check it!", skip_btn);
        }

        TouchEvent t;
        if (!saved && touch_poll(&t) && _hit(skip_btn, t)) { skipped = true; break; }
        delay(2);
    }

    server.stop();
    dns.stop();
    WiFi.softAPdisconnect(true);
    if (!saved) WiFi.mode(WIFI_OFF);
    return !skipped || saved;
}

// ---- Wizard ----

SetupChoice setup_wizard_run(Arduino_Canvas* gfx) {
    Button found_btn{60, 140, 360, 60};
    Button join_btn{60, 220, 360, 60};

    gfx->fillScreen(COL_BG);
    _text_centered(gfx, 50, 3, COL_AMBER, "a tiny world,");
    _text_centered(gfx, 80, 3, COL_AMBER, "waiting...");
    _draw_button(gfx, found_btn, "Found a new colony", COL_AMBER);
    _draw_button(gfx, join_btn, "Join a colony", COL_DIM);
    gfx->flush();

    SetupChoice choice = SETUP_NONE;
    while (choice == SETUP_NONE) {
        TouchEvent t;
        if (touch_poll(&t)) {
            if (_hit(found_btn, t)) choice = SETUP_FOUNDED;
            else if (_hit(join_btn, t)) choice = SETUP_JOINED;
        }
        delay(5);
    }

    if (choice == SETUP_JOINED) {
        Coordinator::set_role_nvs(MODULE_SATELLITE);
        gfx->fillScreen(COL_BG);
        _text_centered(gfx, 110, 3, COL_GREEN, "add-on module");
        _text_centered(gfx, 160, 2, COL_TEXT, "snap it onto a colony");
        _text_centered(gfx, 186, 2, COL_TEXT, "and it will join in");
        gfx->flush();
        delay(3000);
        Serial.println("[wizard] configured as add-on (satellite)");
        return SETUP_JOINED;
    }

    // Found path: WiFi first (founding needs real-world time), then queen role
    bool wifi_ok = _run_wifi_portal(gfx);
    if (!wifi_ok)
        Serial.println("[wizard] WiFi skipped — clock/weather offline until set up");

    Coordinator::set_role_nvs(MODULE_QUEEN);
    Serial.println("[wizard] configured as queen — founding on this boot");

    gfx->fillScreen(COL_BG);
    _text_centered(gfx, 140, 3, COL_AMBER, "preparing the nest...");
    gfx->flush();

    // A new founding always starts from clean ground — clears any colony
    // data a previous life of this module (or a reused SD card) left behind
    if (sd_card_state() == SD_OK) {
        sd_remove_recursive("/colony");
        Serial.println("[wizard] cleared stale colony data from SD");
    }
    return SETUP_FOUNDED;
}

void setup_wizard_ceremony(Arduino_Canvas* gfx, const char* colony_id,
                           const char* queen_name) {
    // Scene 1: the colony has a name
    gfx->fillScreen(COL_BG);
    _text_centered(gfx, 90, 2, COL_DIM, "a new colony is founded");
    _text_centered(gfx, 140, 3, COL_AMBER,
                   (colony_id && colony_id[0]) ? colony_id : "the colony");
    gfx->flush();
    delay(3500);

    // Scene 2: the queen stirs
    char line[48];
    snprintf(line, sizeof(line), "Queen %s stirs...",
             (queen_name && queen_name[0]) ? queen_name : "of the nest");
    gfx->fillScreen(COL_BG);
    _text_centered(gfx, 120, 3, COL_TEXT, line);
    _text_centered(gfx, 170, 2, COL_DIM, "her first eggs are waiting");
    gfx->flush();
    delay(3200);

    // Scene 3: where to watch over them
    gfx->fillScreen(COL_BG);
    _text_centered(gfx, 110, 2, COL_TEXT, "watch over them at");
    _text_centered(gfx, 150, 2, COL_AMBER, "hive.campbell.fish/app");
    _text_centered(gfx, 200, 2, COL_DIM, "your colony:");
    _text_centered(gfx, 226, 2, COL_TEXT,
                   (colony_id && colony_id[0]) ? colony_id : "(on screen soon)");
    gfx->flush();
    delay(4000);
    // Boot splash follows — the chamber fades in with queen and eggs
}
