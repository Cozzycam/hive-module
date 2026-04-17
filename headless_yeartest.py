"""Headless 365-day simulation — run the colony for a full year and
log key metrics every sim-day to diagnose the food economy.

Usage:  python headless_yeartest.py
Output: prints a CSV-like table to stdout + a summary at the end.
"""

import sys, os
sys.path.insert(0, os.path.dirname(__file__))

import config as C
from sim.coordinator import Coordinator

DAYS = 365
TICKS = DAYS * C.TICKS_PER_SIM_DAY  # 73,000
SAMPLE_EVERY = C.TICKS_PER_SIM_DAY  # once per sim-day

# Replenish food in the outworld every N sim-days, simulating
# a steady seed source.  Real Messor colonies gather from scattered
# seed caches; we model this as a pile that refills periodically.
FOOD_REPLENISH_INTERVAL = C.TICKS_PER_SIM_DAY       # every day
FOOD_REPLENISH_AMOUNT   = 80.0    # steady daily seed availability


def run():
    coord = Coordinator()

    # Attach an outworld chamber to the south of the founding chamber.
    ok, outworld_id = coord.announce_module('M0', 'S')
    if not ok:
        print(f"Failed to attach outworld: {outworld_id}")
        return
    outworld = coord.chambers[outworld_id]

    # Outworld food appears once the first workers hatch (not during
    # founding — queen is sealed in and nobody gathers).
    ox, oy = C.GRID_WIDTH // 2, C.GRID_HEIGHT // 2
    food_started = False

    # Tracking
    peak_food = 0.0
    min_food_post_founding = 999999.0
    cannibalism_events = 0
    peak_pop = 0
    total_deaths = 0

    # Count cannibalism by watching brood counts drop in ways that
    # aren't explained by hatching.
    prev_brood_total = 0

    header = (
        f"{'day':>5} {'pop':>5} {'store':>8} {'piles':>7} {'total':>8} "
        f"{'eggs':>4} {'larv':>4} {'pupa':>4} "
        f"{'gthr':>4} {'pres':>6}"
    )
    print(header)
    print("-" * len(header))

    prev_store = 0.0
    for tick in range(1, TICKS + 1):
        pre_store = coord.colony.food_store
        coord.tick()
        post_store = coord.colony.food_store
        delta = post_store - pre_store

        # Start food once first workers hatch (no gathering during founding).
        if not food_started and coord.colony.population > 0:
            food_started = True
            outworld.add_food(ox, oy, FOOD_REPLENISH_AMOUNT)

        # Replenish outworld food.
        if food_started and tick % FOOD_REPLENISH_INTERVAL == 0:
            outworld.add_food(ox, oy, FOOD_REPLENISH_AMOUNT)

        # Per-tick tracking for crash analysis — print when store
        # drops significantly in a single tick.
        day_f = tick / C.TICKS_PER_SIM_DAY
        if delta < -50 and day_f > 50:
            print(f"  !! tick {tick} (day {day_f:.1f}): "
                  f"store {pre_store:.1f} -> {post_store:.1f} "
                  f"(delta {delta:+.1f})")

        # Sample once per sim-day.
        if tick % SAMPLE_EVERY == 0:
            day = tick // C.TICKS_PER_SIM_DAY
            col = coord.colony
            food = col.food_total
            pop = col.population
            bc = col.brood_counts
            pressure = col.food_pressure()

            # Sum food in all chamber piles (not in food_store)
            pile_food = sum(ch.total_food() for ch in coord.chambers.values())
            # Food carried by all workers
            carried = sum(w.food_carried for ch in coord.chambers.values()
                          for w in ch.workers)

            peak_food = max(peak_food, food)
            if day > 56:  # post-founding
                min_food_post_founding = min(min_food_post_founding, food)
            peak_pop = max(peak_pop, pop)

            # Print every 5 days to keep output manageable
            if day % 5 == 0 or day <= 60:
                print(
                    f"{day:>5} {pop:>5} {col.food_store:>8.1f} "
                    f"{pile_food:>7.1f} {food:>8.1f} "
                    f"{bc['egg']:>4} {bc['larva']:>4} {bc['pupa']:>4} "
                    f"{col.gatherer_count:>4} {pressure:>6.3f}"
                )

    # Final summary
    col = coord.colony
    print()
    print("=" * 60)
    print("YEAR-END SUMMARY")
    print("=" * 60)
    print(f"  Final population:    {col.population}")
    print(f"  Peak population:     {peak_pop}")
    print(f"  Final food:          {col.food_total:.1f}")
    print(f"  Peak food:           {peak_food:.1f}")
    print(f"  Min food (post-d56): {min_food_post_founding:.1f}")
    print(f"  Final brood:         eggs={col.brood_counts['egg']} "
          f"larvae={col.brood_counts['larva']} "
          f"pupae={col.brood_counts['pupa']}")
    print(f"  Food pressure:       {col.food_pressure():.3f}")
    print(f"  Queen alive:         ", end="")
    for ch in coord.chambers.values():
        if ch.queen is not None:
            print("YES" if ch.queen.alive else "NO")
            break
    print(f"  Eggs laid total:     ", end="")
    for ch in coord.chambers.values():
        if ch.queen is not None:
            print(ch.queen.eggs_laid)
            break

    # Expectation check
    print()
    if 50 <= col.population <= 200:
        print("Population in expected Messor range (50-200). GOOD")
    elif col.population < 50:
        print(f"Population {col.population} is BELOW expected Messor range (50-200)")
    else:
        print(f"Population {col.population} is ABOVE expected Messor range (50-200)")

    if min_food_post_founding > 20:
        print("Food never crashed to near-zero post-founding. GOOD")
    else:
        print(f"Food dropped to {min_food_post_founding:.1f} post-founding. "
              "STILL CRASHING")


if __name__ == '__main__':
    run()
