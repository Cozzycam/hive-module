/* Host stubs for the firmware's hardware/mesh edges, so the Coordinator +
 * persistence + journal + api_json compile and link off-device.
 *
 * A phone module is a SINGLE queen with NO SD card and NO ESP-NOW peers, so:
 *   - sd_card_init() returns false  -> persistence takes its in-RAM fresh path
 *   - all topology (ESP-NOW) calls are inert -> no neighbours, no handoffs
 *   - chores (async SD/HTTP worker) is inert -> the main loop's sync path runs
 * Everything here is a safe no-op; the real behaviour lives on hardware. */
#include "sd_card.h"
#include "chores.h"
#include "topology.h"
#include "config.h"
#include <SD_MMC.h>
#include <Wire.h>

// ---- SD_MMC global (unused: no card) ----
SDMMCFS SD_MMC;
// ---- Wire (I2C) global — only the HUD battery gauge would use it (skipped) ----
TwoWire Wire;

// ---- sd_card ----
// No card: the colony runs in PERSIST_DEGRADED. Since the firmware now assigns
// in-RAM identities even when degraded, conkers still get names/personalities
// (the roster works); only cross-reboot flushes are skipped. Founding runs on
// the fresh-colony path (Case A). Cross-reload persistence = later, via IDBFS.
bool     sd_card_init() { return false; }
SdState  sd_card_state() { return SD_NOT_MOUNTED; }
void     sd_remove_recursive(const char*, int) {}
uint64_t sd_card_total_bytes() { return 0; }
uint64_t sd_card_used_bytes() { return 0; }
void     sd_card_health_tick() {}
void     sd_card_write_failed() {}
void     sd_card_write_ok() {}
bool     sd_card_remount() { return false; }

// ---- chores (async worker) ----
void chores_begin() {}
bool chores_ready() { return false; }                  // -> synchronous SD path (also inert)
bool chores_submit_http(uint8_t, uint32_t, const char*, char*, size_t, const char*, const char*) { return false; }
bool chores_submit_sd_write(uint32_t, const char*, char*, size_t) { return false; }
bool chores_poll_result(ChoreResult&) { return false; }
void chores_drain(uint32_t) {}
void chores_sd_lock() {}
void chores_sd_unlock() {}

// ---- topology (ESP-NOW mesh) — single module, no neighbours ----
void     topology_init(TopologyCallback) {}
void     topology_poll() {}
uint16_t topology_my_id() { return 1; }
const FaceState& topology_face(Face) { static FaceState s{}; return s; }
const Neighbour& topology_neighbour(Face) { static Neighbour s{}; return s; }
bool     topology_pop_sync_fresh(Face) { return false; }
void     topology_set_wifi_channel(uint8_t) {}
uint8_t  topology_current_channel() { return 0; }
bool     topology_send_to_face(Face, const uint8_t*, int) { return false; }
int      topology_drain_handoffs(PendingHandoff*, int) { return 0; }
int      topology_drain_handoff_acks(PendingAck*, int) { return 0; }
int      topology_drain_death_syncs(PendingDeathSync*, int) { return 0; }
int      topology_drain_journal_relays(PendingJournalRelay*, int) { return 0; }
bool     topology_has_gather(GatherSyncMessage*) { return false; }
uint16_t topology_remote_population(Face) { return 0; }
uint16_t topology_remote_gatherers(Face) { return 0; }
uint8_t  topology_remote_role(Face) { return 0; }
uint32_t topology_remote_tint(Face) { return 0; }
bool     topology_remote_gardener_wanted(Face) { return false; }
bool     topology_has_set_role(SetRoleMessage*) { return false; }
bool     topology_has_set_tint(SetTintMessage*) { return false; }
const BoundaryPheroData& topology_boundary_phero(Face) { static BoundaryPheroData s{}; return s; }
bool     topology_has_state_sync() { return false; }
uint32_t topology_state_sync_age_ms() { return 0xFFFFFFFFu; }
void     topology_set_accept_state_sync(bool) {}
bool     topology_has_announce(AnnounceMessage*) { return false; }
bool     topology_has_wifi_creds(WifiCredsMessage*) { return false; }
bool     topology_has_gift_food(GiftFoodMessage*) { return false; }
bool     topology_broadcast(const uint8_t*, int) { return false; }
bool     topology_ota_check(uint32_t*) { return false; }
bool     topology_ota_server(OtaReadyMessage*) { return false; }
void     topology_draw_overlay(void*) {}
void     topology_status() {}
void     topology_set_wifi_active(bool) {}
