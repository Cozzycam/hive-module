# Recon: Lilguy Interaction System Audit

**Date:** 2026-04-23
**Scope:** Event system, state machine, pheromone trails, colony pressure, idle internals, queen, renderer animation hooks
**Purpose:** Inform implementation brief for proximity greetings, grooming, nighttime huddling, and queen retinue

---

## 1. Event System

### Emission points

**EVT_INTERACTION_STARTED + EVT_INTERACTION_ENDED** (paired):

| Trigger | File | Lines | Condition | Throttling |
|---------|------|-------|-----------|------------|
| Tending brood | `firmware/src/lil_guy.cpp` | 462–468 | Worker reaches target larva (≤1 cell), larva is STAGE_LARVA, needs feeding, colony has food | None — fires once on arrival (state transition gates it) |
| Tending queen | `firmware/src/lil_guy.cpp` | 493–499 | Worker reaches queen (≤1 cell), queen.needs_feeding() (hunger > 0.3), colony has food | None — same gate |
| Proximity: food sharing | `firmware/src/chamber.cpp` | 170–179, 212–221 | One worker has food, other doesn't. 15% chance per frame (`PROXIMITY_FOOD_SHARE_CHANCE`) | Probabilistic per-frame |
| Proximity: greeting | `firmware/src/chamber.cpp` | 183–192, 225–234 | Two workers same cell or adjacent. 35% chance per frame (`PROXIMITY_GREETING_CHANCE`) | Probabilistic per-frame |

**EVT_FOOD_DELIVERED** — `firmware/src/lil_guy.cpp:409–414`. Worker in STATE_TO_HOME with food reaches queen (≤1 cell). Sets `ch.food_delivery_signal = 200` ticks as recruitment broadcast.

### Consumption

- **Renderer** (`firmware/src/renderer.cpp:1140–1189`): Only `EVT_FOOD_DELIVERED` spawns a visual animation (`ANIM_FOOD_DELIVER`). **Interaction events are emitted but have zero visual feedback** — logged to serial only.
- **No per-lilguy cooldown.** Proximity interactions use probabilistic per-frame gating (35%, 15%), which prevents spam but isn't a true cooldown. A worker can greet every frame if the dice keep rolling.

> **Flag:** Interaction events exist and fire but produce no on-screen effect. The infrastructure is there — just needs renderer-side handling.

---

## 2. Lilguy State Machine

### All states (`firmware/include/config.h:12–15`)

| State | Value | Description |
|-------|-------|-------------|
| `STATE_IDLE` | 0 | Resting with micro-behaviors (hold/drift/reface) |
| `STATE_TEND_QUEEN` | 1 | Moving to queen to feed her |
| `STATE_TEND_BROOD` | 2 | Moving to target larva to feed it |
| `STATE_TO_FOOD` | 3 | Scouting/gathering: following pheromone trails or exploring |
| `STATE_TO_HOME` | 4 | Returning toward queen (with or without food) |
| `STATE_CANNIBALIZE` | 5 | Crisis: moving to least-invested larva to consume |

### Trail-following vs exploring

**No discrete distinction at the state level.** `STATE_TO_FOOD` covers both. The difference is internal to `_do_to_food` (`firmware/src/lil_guy.cpp:373–400`):
- Trail following: `_sample_markers(ch, true, dx, dy)` returns true → waypoint set toward gradient
- Exploring: `_sample_markers` returns false → `_explore_or_wander` chooses random movement

**Cleanest predicate for "on-job":** `w.state == STATE_TO_FOOD || (w.state == STATE_TO_HOME && w.food_carried > 0)`. Both legs of the foraging trip should be protected from interruption.

### Per-lilguy tick function

**`LilGuy::tick(Chamber& ch, float dt)`** — `firmware/src/lil_guy.cpp:63–130`

Phases:
1. **Housekeeping** (64–89): death check, hunger/food management
2. **Return-home timer** (91–103): if away > 960 ticks, force STATE_TO_HOME
3. **Movement** (106): `_advance_toward_target(ch)` — smooth sub-cell movement
4. **Decision** (109–129): only when `!has_target_cell` (arrived at waypoint). If idle and resting, runs `_tick_idle(ch)`. Otherwise dispatches to per-state behavior function.

**Insertion point for new micro-behaviors:** The `_tick_idle` function (`firmware/src/lil_guy.cpp:548–612`) is the natural hook. It manages microstate cycling and repoll-for-work checks.

---

## 3. Pheromone / Trail-Following Logic

### Pheromone deposition (`firmware/src/lil_guy.cpp:183–193`)

- **Outbound (STATE_TO_FOOD):** Deposits **home** markers. Intensity = `10.0 * exp(-0.02 * steps_walked)`. Deposits on cell entry only.
- **Return with food (STATE_TO_HOME, food > 0):** Deposits **food** markers. Same decay formula. This is what creates the trail other workers follow.

### Trail state in code

When on a food trail:
- `state == STATE_TO_FOOD`
- `steps_walked < SCOUT_PATIENCE_TICKS (3000)`
- `_sample_markers(ch, true, dx, dy)` found a food pheromone gradient

There is **no stored flag** like `on_trail`. Trail detection is ephemeral — re-evaluated each time the worker needs a new waypoint.

### Returning home with vs without food

Both are `STATE_TO_HOME`. Distinguished solely by `food_carried > 0`:
- With food: deposits food pheromone, delivers to colony store on reaching queen
- Without food: follows home pheromone back, no deposit

---

## 4. Colony Pressure System

### Idle budget function (`firmware/src/lil_guy.cpp:637–643`)

```cpp
float LilGuy::_colony_idle_budget(Chamber& ch) {
    if (ch.colony->population < 10) return 0.0f;         // founding: everyone works
    if (ch.colony->food_pressure() > 0.9f) return 0.0f;  // famine: everyone works
    float nf = g_tod.night_factor;
    return 0.70f + nf * (0.95f - 0.70f);                 // day: 70% idle, night: 95% idle
}
```

### food_pressure scalar (`firmware/src/colony_state.cpp:10–17`)

**Yes, it's a colony-level method** — `ColonyState::food_pressure()`:

```
burn    = queen_daily + population * worker_daily * metabolic_scale + larvae * larva_daily
target  = burn * 2.0  (buffer days)
pressure = 1.0 - min(1.0, food_total / target / 2.0)
```

Result: **0.0** (food ≥ 4 days of burn) → **1.0** (zero food).

### Thresholds used elsewhere

| Threshold | Constant | Effect |
|-----------|----------|--------|
| 0.8 | `FAMINE_BROOD_CULL_PRESSURE` | Hungry larvae die |
| 0.9 | `FAMINE_SLOWDOWN_PRESSURE` | Idle budget → 0, queen-feeding override |
| 0.95 | `BROOD_CANNIBALISM_PRESSURE` | Cannibalism enabled |

**Predicate for gating luxury behaviors:** `ch.colony->food_pressure() < 0.9f && ch.colony->population >= 10`. Same gate the idle budget already uses.

---

## 5. Idle State Internals

### Idle target position

No fixed position. Each microstate transition picks dynamically:
- **Hold (microstate 0):** No target cell — stays in place
- **Drift (microstate 1):** Random cardinal direction, 1 cell, at 0.1 cells/tick (slow)
- **Reface (microstate 2):** No movement — just changes `facing_dx/dy`

### Micro-behavior implementation (`firmware/src/lil_guy.cpp:614–635`)

`_pick_idle_microstate(Chamber& ch)`:
- Roll random float `r`
- `r < 0.60` → hold (microstate 0)
- `r < 0.85` → drift (microstate 1)
- else → reface (microstate 2)
- Duration: random 16–80 ticks per microstate

### Adding a fourth behavior

**Straightforward.** The microstate picker is a simple weighted random branch. To add "drift toward nearest idler" or "drift toward queen":
1. Add weight constant (e.g., `IDLE_HUDDLE_WEIGHT = 0.15`)
2. Reduce existing weights to keep sum at 1.0
3. Add `else if` branch setting `idle_microstate = 3` and computing target cell
4. Handle microstate 3 in `_do_idle` (which dispatches on `idle_microstate`)

~10–15 lines of new code. No architectural changes needed.

> **Flag:** There's already a 30% bias toward queen in idle wandering (`firmware/src/lil_guy.cpp:530–532`). A "huddle toward queen" microstate would complement this.

---

## 6. Queen

### Type

**Separate struct, NOT a LilGuy.** Defined in `firmware/include/queen.h`, implemented in `firmware/src/queen.cpp`. Has position (x, y), reserves, hunger, eggs_laid, alive flag. No state machine — just founding vs established phase.

### Position access

Workers access queen position directly via `ch.queen_obj.x`, `ch.queen_obj.y`. The chamber stores her as a public member: `Queen queen_obj` (`firmware/include/chamber.h:23`).

### Queen-aware worker behaviors

| Behavior | Location | Condition |
|----------|----------|-----------|
| Famine override: feed queen first | `lil_guy.cpp:225–235` | pressure > 0.9, queen hungry, has food, in range |
| Normal queen feeding | `lil_guy.cpp:261–268` | queen.needs_feeding(), has food, in range |
| Idle bias toward queen | `lil_guy.cpp:530–532` | 30% chance during idle drift |
| Return-home navigates to queen | `lil_guy.cpp:405–428` | STATE_TO_HOME, direct path to queen cell |

---

## 7. Renderer Animation Hooks

### Breathing bob

**Computed from frame counter and position, not stored per-lilguy** (`firmware/src/renderer.cpp:945–955`, now inside `_build_agent_sprites`):

```cpp
// Resting: phase = (_frame + i * 7) % 60, bob = phase < 30 ? 0 : -1
// Moving:  bob derived from float position (walk cycle)
```

Worker index `i * 7` desynchronizes breathing across workers. No per-lilguy animation state exists.

### Adding a short per-lilguy animation

**Two approaches:**

1. **Per-lilguy fields** — Add `anim_type`, `anim_age`, `anim_duration` to `LilGuy`. Set in behavior code, decrement in `tick()`, read in renderer. This is the right approach for animations tied to behavior (pause-and-twitch during greeting).

2. **Renderer animation pool** — The existing `Anim` system (`renderer.h:34–40`) has a 32-slot pool with spawn/draw/age lifecycle. Good for position-anchored effects (sparkles, rings) but not for animations that track a moving entity.

**For interaction animations (greetings, grooming):** Approach 1 is better. The renderer already reads worker state for bob animation — reading an `anim_type` field is the same pattern.

### Existing animation scaffolding

The `Anim` pool is complete:
- `_spawn_anim(type, px, py, duration)` — add to pool with LRU eviction
- `_draw_anims()` / `_draw_one_anim()` — draw active anims each frame
- Types: TAP_RING, FOOD_DELIVER, EGG_LAID, HATCH, DEATH_WORKER, DEATH_YOUNG

Adding a new type: add enum value, add draw logic in `_draw_one_anim` switch, trigger via `receive_events`.

---

## Summary: Key Predicates for Implementation

| Question | Answer |
|----------|--------|
| Is this lilguy on a food trail? | `w.state == STATE_TO_FOOD \|\| (w.state == STATE_TO_HOME && w.food_carried > 0)` |
| Where to hook new idle micro-behavior? | `_pick_idle_microstate()` at `lil_guy.cpp:614–635` — weighted random branch |
| What gates luxury behaviors under food pressure? | `ch.colony->food_pressure() < 0.9f && ch.colony->population >= 10` |
| Does the renderer know how to play one-shot animations? | Yes — 32-slot `Anim` pool, but position-anchored. Per-lilguy animations need new fields on `LilGuy`. |
| Can workers find the queen? | Yes — `ch.queen_obj.x`, `ch.queen_obj.y` directly accessible |
| Is there already a greeting system? | Yes — proximity greeting events fire at 35%/frame, but produce no visual effect |
