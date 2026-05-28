#!/usr/bin/env python3
"""plot_obj1.py - Render results/obj1.png from results/obj1.csv

Required graph for Objective 1:
    X-axis: % of read queries in the workload mix
    Y-axis: I/O counts (logical & physical, both reads & writes)

Author: Daksh Jain (B24bs1114)
"""
import csv, os, sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSV  = os.path.join(ROOT, "results", "obj1.csv")
OUT  = os.path.join(ROOT, "results", "obj1.png")

if not os.path.exists(CSV):
    sys.exit(f"missing {CSV}; run ./bench/obj1_bench first")

rows = list(csv.DictReader(open(CSV)))
mixes = sorted({int(r["read_pct"]) for r in rows})
fig, axes = plt.subplots(1, 2, figsize=(12, 5), sharex=True)

for ax, policy in zip(axes, ("LRU", "MRU")):
    sub = [r for r in rows if r["policy"] == policy]
    sub.sort(key=lambda r: int(r["read_pct"]))
    x = [int(r["read_pct"]) for r in sub]
    ax.plot(x, [int(r["logical_reads"])  for r in sub], "o-",  label="logical reads")
    ax.plot(x, [int(r["logical_writes"]) for r in sub], "s-",  label="logical writes")
    ax.plot(x, [int(r["phys_reads"])     for r in sub], "o--", label="physical reads")
    ax.plot(x, [int(r["phys_writes"])    for r in sub], "s--", label="physical writes")
    ax.set_title(f"Policy = {policy}")
    ax.set_xlabel("Read % in workload mix")
    ax.set_ylabel("Operations (out of 5000 total)")
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8)

fig.suptitle("Objective 1 - PF buffer I/O vs read/write mix "
             "(pool=16, 200 pages, 5000 ops, hot working-set)",
             fontsize=11)
fig.tight_layout()
fig.savefig(OUT, dpi=120)
print(f"wrote {OUT}")
