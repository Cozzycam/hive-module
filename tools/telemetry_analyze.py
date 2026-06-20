#!/usr/bin/env python3
"""Stats over pulled telemetry CSVs, with day/night phase breakdown.
Usage: telemetry_analyze.py f1.csv f2.csv ..."""
import sys, csv, statistics as st
from collections import Counter, defaultdict

NEEDS = ("boredom", "tiredness", "loneliness")
PHASES = ("day", "dawn", "dusk", "night")

for path in sys.argv[1:]:
    rows = []
    with open(path, newline="") as fh:
        rows = list(csv.DictReader(fh))
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

    print(f"\n=== {path}  module {module} ===")
    print(f"rows {len(rows)} | span {span_h:.2f}h | conkers {len(conkers)}: {', '.join(conkers)}")
    print(f"phase coverage: {dict(phases)}")
    print(f"overall mood:   {dict(Counter(r['mood'] for r in rows).most_common())}")

    # sleeping fraction overall + by phase
    slp = [r for r in rows if r.get("sleeping") == "1"]
    print(f"sleeping: {len(slp)/len(rows)*100:.1f}% of all rows", end="")
    by_ph = Counter(r["phase"] for r in slp)
    print("  | by phase: " + ", ".join(f"{p} {by_ph.get(p,0)/max(1,phases.get(p,0))*100:.0f}%" for p in PHASES if phases.get(p)))

    # per-need stats, overall + per phase mean
    print("need        overall: min/mean/max  %@0  %>=.55 %>=.85 |  mean by phase (day/dawn/dusk/night)")
    for need in NEEDS:
        vals = [fnum(r, need) for r in rows]
        vals = [v for v in vals if v is not None]
        if not vals: continue
        at0  = sum(1 for v in vals if v <= 0.001)/len(vals)*100
        r55  = sum(1 for v in vals if v >= 0.55)/len(vals)*100
        r85  = sum(1 for v in vals if v >= 0.85)/len(vals)*100
        ph_means = []
        for p in PHASES:
            pv = [fnum(r,need) for r in rows if r["phase"]==p]
            pv = [v for v in pv if v is not None]
            ph_means.append(f"{st.mean(pv):.2f}" if pv else "-")
        print(f"  {need:10s} {min(vals):.2f}/{st.mean(vals):.2f}/{max(vals):.2f}  {at0:4.0f} {r55:5.1f} {r85:5.1f}  |  {' / '.join(ph_means)}")

    hunger = [fnum(r,"hunger") for r in rows if fnum(r,"hunger") is not None]
    if hunger:
        print(f"  hunger     {min(hunger):.1f}/{st.mean(hunger):.1f}/{max(hunger):.1f}")

    # mood distribution by phase (esp. night — are they sleepy?)
    print("mood by phase:")
    for p in PHASES:
        if not phases.get(p): continue
        mc = Counter(r["mood"] for r in rows if r["phase"]==p)
        tot = sum(mc.values())
        print(f"  {p:6s} " + ", ".join(f"{m} {c/tot*100:.0f}%" for m,c in mc.most_common()))

    # per-conker peak needs
    print("per-conker max (bore/tired/lonely):")
    by = defaultdict(lambda: {n:0 for n in NEEDS})
    for r in rows:
        for n in NEEDS:
            v = fnum(r,n)
            if v is not None and v > by[r["name"]][n]: by[r["name"]][n] = v
    for name in sorted(by):
        m = by[name]
        print(f"    {name:12s} {m['boredom']:.2f} / {m['tiredness']:.2f} / {m['loneliness']:.2f}")
