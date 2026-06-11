/* Bonds — directed relationship strengths. */
#include "bonds.h"
#include <cstring>

void BondStore::init() {
    _count = 0;
    memset(_pool, 0, sizeof(_pool));
}

int BondStore::_find(uint32_t owner, uint32_t target) const {
    for (int i = 0; i < _count; i++) {
        if (_pool[i].owner == owner && _pool[i].target == target)
            return i;
    }
    return -1;
}

int BondStore::_count_for_owner(uint32_t owner) const {
    int n = 0;
    for (int i = 0; i < _count; i++)
        if (_pool[i].owner == owner) n++;
    return n;
}

int BondStore::_weakest_for_owner(uint32_t owner) const {
    int weakest = -1;
    float min_str = 2.0f;
    for (int i = 0; i < _count; i++) {
        if (_pool[i].owner == owner && _pool[i].strength < min_str) {
            min_str = _pool[i].strength;
            weakest = i;
        }
    }
    return weakest;
}

void BondStore::_remove_at(int idx) {
    if (idx < 0 || idx >= _count) return;
    _pool[idx] = _pool[_count - 1];
    _count--;
}

bool BondStore::increment(uint32_t owner, uint32_t target, float amount) {
    if (owner == 0 || target == 0 || owner == target) return false;

    int idx = _find(owner, target);
    if (idx >= 0) {
        float prev = _pool[idx].strength;
        _pool[idx].strength += amount;
        if (_pool[idx].strength > 1.0f) _pool[idx].strength = 1.0f;
        bool crossed = (prev < FORM_THRESHOLD && _pool[idx].strength >= FORM_THRESHOLD);
        if (crossed) _pool[idx].formed = true;
        return crossed;
    }

    // New bond — check caps
    if (_count >= POOL_CAP) return false;
    if (_count_for_owner(owner) >= PER_OWNER_CAP) {
        int w = _weakest_for_owner(owner);
        if (w >= 0) _remove_at(w);
        else return false;
    }

    _pool[_count++] = {owner, target, amount, amount >= FORM_THRESHOLD};
    return (amount >= FORM_THRESHOLD);
}

void BondStore::set(uint32_t owner, uint32_t target, float strength) {
    if (owner == 0 || target == 0 || owner == target) return;

    int idx = _find(owner, target);
    if (idx >= 0) {
        _pool[idx].strength = strength;
        if (strength >= FORM_THRESHOLD) _pool[idx].formed = true;
        return;
    }

    if (_count >= POOL_CAP) return;
    if (_count_for_owner(owner) >= PER_OWNER_CAP) {
        int w = _weakest_for_owner(owner);
        if (w >= 0) _remove_at(w);
        else return;
    }
    _pool[_count++] = {owner, target, strength, strength >= FORM_THRESHOLD};
}

void BondStore::decay(uint32_t* broken_owners, uint32_t* broken_targets,
                       int& broken_count, int max_broken) {
    broken_count = 0;
    for (int i = _count - 1; i >= 0; i--) {
        _pool[i].strength *= DECAY_FACTOR;
        if (_pool[i].strength < BREAK_THRESHOLD) {
            // Only announce breaks for bonds that actually formed —
            // sub-threshold flings fade without a diary entry
            if (_pool[i].formed && broken_count < max_broken) {
                broken_owners[broken_count] = _pool[i].owner;
                broken_targets[broken_count] = _pool[i].target;
                broken_count++;
            }
            _remove_at(i);
        }
    }
}

void BondStore::remove_owner(uint32_t owner) {
    for (int i = _count - 1; i >= 0; i--) {
        if (_pool[i].owner == owner || _pool[i].target == owner)
            _remove_at(i);
    }
}

int BondStore::get_bonds(uint32_t owner, BondEntry* out, int max_out) const {
    int n = 0;
    for (int i = 0; i < _count && n < max_out; i++) {
        if (_pool[i].owner == owner)
            out[n++] = _pool[i];
    }
    return n;
}

void BondStore::load(const BondEntry* entries, int n) {
    _count = (n > POOL_CAP) ? POOL_CAP : n;
    memcpy(_pool, entries, _count * sizeof(BondEntry));
}
