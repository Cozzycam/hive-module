/* Bonds — directed relationship strengths between Conkers.
 *
 * Stored in a flat pool. Each entry is owner→target with strength 0.0-1.0.
 * Cap: 16 bonds per owner. Pool cap: 1024 total entries.
 * Persisted to /colony/bonds.json every 60s.
 */
#pragma once
#include <cstdint>

struct BondEntry {
    uint32_t owner;
    uint32_t target;
    float    strength;
};

class BondStore {
public:
    static constexpr int POOL_CAP = 1024;
    static constexpr int PER_OWNER_CAP = 16;
    static constexpr float FORM_THRESHOLD = 0.1f;   // emit bond_formed
    static constexpr float BREAK_THRESHOLD = 0.05f; // emit bond_broken / remove
    static constexpr float DECAY_FACTOR = 0.997f;   // per 1000 ticks (~2 min)
    static constexpr int   DECAY_INTERVAL = 1000;   // ticks between decay passes

    void init();

    // Add increment to bond (owner→target). Creates if doesn't exist.
    // Returns true if bond just crossed FORM_THRESHOLD (new bond formed).
    bool increment(uint32_t owner, uint32_t target, float amount);

    // Set bond to a specific strength (e.g. initial lineage bond).
    void set(uint32_t owner, uint32_t target, float strength);

    // Decay all bonds. Call every DECAY_INTERVAL ticks.
    // broken_ids/broken_targets filled with pairs that decayed below threshold.
    void decay(uint32_t* broken_owners, uint32_t* broken_targets, int& broken_count, int max_broken);

    // Remove all bonds for a specific owner (on death).
    void remove_owner(uint32_t owner);

    // Get bonds for an owner. Returns count, fills out array.
    int get_bonds(uint32_t owner, BondEntry* out, int max_out) const;

    // Access for persistence
    int count() const { return _count; }
    const BondEntry* pool() const { return _pool; }

    // Load from array (used by persistence loader)
    void load(const BondEntry* entries, int n);

private:
    BondEntry _pool[POOL_CAP];
    int       _count = 0;

    int _find(uint32_t owner, uint32_t target) const;
    int _count_for_owner(uint32_t owner) const;
    int _weakest_for_owner(uint32_t owner) const;
    void _remove_at(int idx);
};
