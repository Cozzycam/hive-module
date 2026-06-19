#!/usr/bin/env python3
"""Quick stats over pulled telemetry CSVs. Usage: telemetry_analyze.py f1.csv f2.csv ..."""
import sys, csv, statistics as st
from collections import Counter, defaultdict

for path in sys.argv[1:]:
    rows = []
    with open(path, newline="") as fh:
        for r in csv.DictReader(fh):
            rows.append(r)
    if not rows:
        print(f"\n=== {path}: EMPTY ===")
        continue

    def fnum(r, k):
        try: return float(r[k])
        except: return None

    unix = [int(r["unix"]) for r in rows if r["unix"].isdigit() and int(r["unix"]) > 0]
    span_h = (max(unix) - min(unix)) / 3600.0 if len(unix) > 1 else 0
    module = rows[0]["module"]
    conkers = sorted(set(r["name"] for r in rows))
    phases = Counter(r["phase"] for r in rows)
    moods  = Counter(r["mood"] for r in rows)
    intents = Counter(r["intent"] for r in rows)

    print(f"\n=== {path}  module {module} ===")
    print(f"rows {len(rows)} | span {span_h:.2f}h | conkers {len(conkers)}: {', '.join(conkers)}")
    print(f"phase coverage: {dict(phases)}")
    print(f"mood dist:   {dict(moods.most_common())}")
    print(f"intent dist: {dict(intents.most_common())}")

    for need in ("boredom", "tiredness", "loneliness"):
        vals = [fnum(r, need) for r in rows]
        vals = [v for v in vals if v is not None]
        if not vals: continue
        at_floor = sum(1 for v in vals if v <= 0.001) / len(vals) * 100
        at_ceil  = sum(1 for v in vals if v >= 0.999) / len(vals) * 100
        hi       = sum(1 for v in vals if v >= 0.85) / len(vals) * 100
        print(f"  {need:11s} min {min(vals):.3f} mean {st.mean(vals):.3f} max {max(vals):.3f} "
              f"| %@0 {at_floor:5.1f} %>=.85 {hi:5.1f} %@1 {at_ceil:5.1f}")

    hunger = [fnum(r, "hunger") for r in rows]
    hunger = [v for v in hunger if v is not None]
    if hunger:
        print(f"  {'hunger':11s} min {min(hunger):.2f} mean {st.mean(hunger):.2f} max {max(hunger):.2f}")

    # Per-conker peak needs — who actually felt anything
    print("  per-conker max needs (bore/tired/lonely):")
    by = defaultdict(lambda: {"boredom":0,"tiredness":0,"loneliness":0})
    for r in rows:
        for need in ("boredom","tiredness","loneliness"):
            v = fnum(r, need)
            if v is not None and v > by[r["name"]][need]:
                by[r["name"]][need] = v
    for name in sorted(by):
        m = by[name]
        print(f"    {name:12s} {m['boredom']:.2f} / {m['tiredness']:.2f} / {m['loneliness']:.2f}")
