#!/usr/bin/env python3
"""Stats over pulled bond CSVs. Usage: bonds_analyze.py f1_bonds.csv ..."""
import sys, csv, statistics as st
from collections import defaultdict

for path in sys.argv[1:]:
    with open(path, newline="") as fh:
        rows = [r for r in csv.DictReader(fh)
                if r.get("strength") and r["strength"].replace(".","",1).isdigit()
                and r.get("formed") in ("0","1")]
    if not rows:
        print(f"\n=== {path}: EMPTY ==="); continue

    module = rows[0]["module"]
    unix = [int(r["unix"]) for r in rows if r["unix"].isdigit() and int(r["unix"])>0]
    span_h = (max(unix)-min(unix))/3600.0 if len(unix)>1 else 0

    # group by sample (unix,uptime) to get a timeline
    samples = defaultdict(list)
    for r in rows:
        samples[(r["unix"], r["uptime_ms"])].append(r)
    times = sorted(samples)

    def pair(r): return (r["owner_name"], r["target_name"])
    # final-sample state
    last = samples[times[-1]]
    first = samples[times[0]]

    formed_last = [r for r in last if r["formed"]=="1"]
    recip_last  = [r for r in last if r["recip"]=="1"]
    # best-friend pairs = both directions formed; recip flag marks them, count unordered
    bf = set(frozenset(pair(r)) for r in recip_last)

    print(f"\n=== {path}  module {module} ===")
    print(f"rows {len(rows)} | samples {len(times)} | span {span_h:.2f}h")
    print(f"first sample: {len(first)} bonds | last sample: {len(last)} bonds, "
          f"{len(formed_last)} formed (>=0.1), {len(bf)} mutual best-friend pair(s)")

    # peak strength ever seen per directed pair + whether it ever formed
    peak = defaultdict(float); everformed=set()
    for r in rows:
        s=float(r["strength"]); p=pair(r)
        if s>peak[p]: peak[p]=s
        if r["formed"]=="1": everformed.add(p)
    strong = sorted(peak.items(), key=lambda kv:-kv[1])
    print(f"distinct directed pairs ever: {len(peak)} | ever crossed FORM(0.1): {len(everformed)}")
    print("strongest bonds (peak strength):")
    for (o,t),s in strong[:12]:
        print(f"    {o:12s} -> {t:12s}  peak {s:.3f}{'  [formed]' if (o,t) in everformed else ''}")

    # mutual best friends with strengths at last sample
    if bf:
        print("mutual best friends (last sample):")
        smap = {pair(r): float(r['strength']) for r in last}
        for fs in bf:
            a,b = tuple(fs)
            print(f"    {a} <-> {b}   ({smap.get((a,b),0):.3f} / {smap.get((b,a),0):.3f})")
