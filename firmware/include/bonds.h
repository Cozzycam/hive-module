/* Bonds — directed relationship strengths between Conkers.
 *
 * Stored in a flat pool. Each entry is owner→target with strength 0.0-1.0.
 * Cap: 12 bonds per owner (a butterfly can feel bonded to lots). Pool cap: 1024.
 * A bond is one-way; when both directions have formed, the pair are best friends.
 * Persisted to /colony/bonds.json every 60s.
 */
#pragma once
#include <cstdint>

struct BondEntry {
    uint32_t owner;
    uint32_t target;
    float    strength;
    // True once strength has crossed FORM_THRESHOLD — only formed bonds
    // announce bond_broken (passing acquaintances fade silently)
    bool     formed = false;
    // Set when the bond is reinforced during a decay window; an unformed bond
    // that was touched is still actively growing, so decay() spares it from the
    // sub-threshold wipe (lets slow accruers mature instead of resetting each
    // cycle). Runtime-only transient — never serialized; reset every decay pass.
    bool     touched = false;
};

class BondStore {
public:
    static constexpr int POOL_CAP = 1024;
    static constexpr int PER_OWNER_CAP = 12;        // a butterfly can feel bonded to lots (one-way)
    static constexpr int CLOSE_FRIEND_CAP = 4;      // v150: a conker can have at most this many FORMED
                                                    // (close) friends at once. Beyond it, extra bonds stay
                                                    // capped just under the form line as acquaintances, so
                                                    // "best friend" is selective instead of a whole-colony
                                                    // clique. Decay frees slots as old friendships fade.
    static constexpr float FORM_THRESHOLD = 0.1f;   // emit bond_formed
    static constexpr float BREAK_THRESHOLD = 0.01f; // emit bond_broken / remove
                                                    // (low floor so intermittent-contact bonds
                                                    // survive the gaps between encounters and
                                                    // accumulate cumulatively, instead of being
                                                    // reset to zero before they can mature)
    static constexpr float DECAY_FACTOR = 0.997f;   // per 1000 ticks (~2 min) — a maintained friendship
                                                    // lasts days, but an UNmaintained one now sheds ~66%/day
                                                    // (was 0.999 ≈ too gentle to ever counter the nightly
                                                    // huddle, which saturated everyone to best friends)
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

    // True if owner→target exists and has crossed FORM_THRESHOLD. Used to test
    // reciprocation: a pair are best friends when both directions are formed.
    bool is_formed(uint32_t owner, uint32_t target) const;

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
    int _count_formed_for_owner(uint32_t owner) const;
    int _weakest_for_owner(uint32_t owner) const;
    // Weakest UNFORMED bond for an owner, or -1 if every slot is a formed
    // friendship. Used for cap eviction: never sacrifice a real friendship to
    // admit a nascent pairing.
    int _weakest_unformed_for_owner(uint32_t owner) const;
    void _remove_at(int idx);
};
