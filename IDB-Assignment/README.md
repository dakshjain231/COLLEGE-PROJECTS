# DBMS Assignment - toyDB Extensions

**Student:** Daksh Jain
**Roll No.:** B24bs1114
**Course:** Database Management Systems
**Instructor:** Nitin Awathare

---

## What this project contains

This repository implements all three objectives of the assignment on top of a
clean, modern C99 re-implementation of the toyDB Paged-File and Access-Method
layers.  The original toyDB sources (which are written in K&R-style C and no
longer compile cleanly with modern compilers) are preserved in `reference/`
for inspection.

```
IDB-Assignment/
├── include/                 # public headers
│   ├── pf.h                 # Paged-File layer with LRU/MRU buffer
│   ├── slotted.h            # variable-length slotted record store
│   ├── static_rec.h         # fixed-length record store (baseline)
│   └── am.h                 # B+ tree access-method layer
├── src/                     # implementation
│   ├── pf.c
│   ├── slotted.c
│   ├── static_rec.c
│   └── am.c
├── bench/
│   ├── obj1_bench.c         # Objective 1 driver  -> results/obj1.csv
│   ├── obj2_bench.c         # Objective 2 driver  -> results/obj2.csv
│   ├── obj3_bench.c         # Objective 3 driver  -> results/obj3.csv
│   ├── plot_obj1.py         # produces results/obj1.png
│   └── smoke.c              # quick correctness self-check
├── data/                    # student.txt and other sample tables
├── reference/               # original toyDB sources for reference only
├── results/                 # produced CSVs and the obj1 plot
├── docs/DBMS_Assignment.pdf # original assignment statement
└── Makefile
```



## How to build and run

Requirements: a C99 compiler (gcc or clang), GNU make, and Python 3 with
matplotlib if you want to regenerate the Objective-1 plot.

```bash
# from inside IDB-Assignment/
make all          # builds bench/obj1_bench, obj2_bench, obj3_bench
make run          # runs all three benchmarks; writes results/*.csv and obj2.txt
make plot         # produces results/obj1.png from results/obj1.csv
```

Quick sanity check (PF + Slotted + B+tree round-trip):

```bash
cc -O2 -std=c99 -Iinclude bench/smoke.c src/*.c -o /tmp/smoke && /tmp/smoke
```

---

## Objective 1 - Page Buffering with LRU and MRU

`include/pf.h` declares a Paged-File API that is similar to the original
toyDB PF interface but adds the features the assignment requires:

* **Selectable replacement policy.**  Pass `PF_REPL_LRU` or `PF_REPL_MRU` to
  `PF_OpenFile()`.  MRU is helpful for sequential scans larger than the pool;
  LRU is better for skewed workloads.
* **Configurable buffer-pool size.**  Last argument of `PF_OpenFile()`.
* **Explicit dirty bit.**  `PF_MarkDirty(pf, pagenum)` is the only thing that
  marks a page as modified - we do not assume any write happens just because
  the user got hold of the page.
* **Statistics.** `PF_GetStats()` returns logical reads/writes,
  physical reads/writes, hits/misses, evictions, and dirty evictions.

The bench driver `bench/obj1_bench.c` populates a 200-page file, then issues
5000 page accesses with the read-fraction varying from 0% to 100% under both
policies.  Page IDs come from a Zipf-like skewed distribution (80% of the
hits land in the hot 25% of pages) so the cache behaviour is workload
sensitive.  Results are written to `results/obj1.csv` and plotted to
`results/obj1.png`.

The plot has the read fraction on the X-axis and operation counts on the
Y-axis, exactly as required by the assignment.

---

## Objective 2 - Slotted Pages vs Static Records

`include/slotted.h` (`src/slotted.c`) lays out variable-length records on
each page using the classic slotted-page directory:

```
  PageHdr | slot[0] slot[1] ... | <free> | ... record bytes ...
                                                     (grows from EOP)
```

Records are addressed by `RID = (page, slot)`.  Deleted slots leave a
tombstone (`len = -1`) so existing RIDs do not move.  When the directory
plus payload no longer fits, the page is compacted in place.

`include/static_rec.h` (`src/static_rec.c`) is a straw-man "static" record
manager: every record is padded out to a fixed `MAX_LEN`, and a bit-map at
the head of every page tracks the live slots.

`bench/obj2_bench.c` loads the supplied `data/student.txt` (17 813 records,
average 98.7 bytes/record, max 109 bytes) into:

* a slotted file
* five static files with `MAX_LEN` = 110, 120, 150, 200, 256

and reports space utilisation in `results/obj2.csv`.  Insert / Delete /
Sequential-scan correctness is demonstrated by inserting a synthetic
"Daksh Jain - B24bs1114" record, reading it back, deleting it, and then
walking the file end-to-end (`results/obj2.txt`).

Representative numbers from a run:

| scheme          | max_len | utilisation |
|-----------------|---------|-------------|
| slotted (var.)  | -       | **0.987**   |
| static          | 110     | 0.891       |
| static          | 120     | 0.820       |
| static          | 150     | 0.651       |
| static          | 200     | 0.482       |
| static          | 256     | 0.362       |

The slotted page wastes almost no space on this dataset because every record
is laid out at exactly its true length plus a 4-byte slot.  Static records
waste `MAX_LEN - len` per row, which is small at `MAX_LEN = 110` but
catastrophic at 256.

---

## Objective 3 - B+ Tree Index Construction

`include/am.h` (`src/am.c`) is a simple B+ tree on `int` keys (roll number)
and `RID` values.  Leaf fan-out is 340 entries/page, internal fan-out is
510 entries/page, both with 16-byte node headers.

The bench `bench/obj3_bench.c` builds the index three different ways on the
17 813 student rolls and reports build time, query time and pages accessed
during 5 000 random point queries:

| method        | description                                              |
|---------------|----------------------------------------------------------|
| `fileinsert`  | load all records first, then insert keys one-by-one      |
| `incremental` | interleave SP_Insert and AM_Insert (OLTP-style)          |
| `bulkload`    | sort `(key, RID)` pairs, build leaves bottom-up at 70%   |

A sample run produced (lower is better):

|             | build time | build phys reads | build phys writes | probe phys reads | tree height |
|-------------|-----------:|-----------------:|------------------:|-----------------:|------------:|
| fileinsert  |    1.35 ms |                5 |               109 |             1767 |           2 |
| incremental |    3.57 ms |                5 |               109 |             1767 |           2 |
| bulkload    |    1.08 ms |                1 |                79 |              781 |           2 |

The bulk-loaded tree is markedly more I/O-friendly during point lookups
(~55% fewer physical reads) because its leaves are densely packed and laid
out contiguously, so the OS page-cache behaviour during queries is much
better than the leaves a series of `AM_Insert` calls produces.

---

## Notes / Limitations

* The PF layer treats a freshly extended page (after `PF_AllocPage` /
  `ftruncate`) as all-zero.  This makes `phys_read` of a brand-new page
  cheap and keeps the on-disk layout simple - but it means we do not
  implement free-list reuse for disposed pages.  Adding it would only
  require a small change in `PF_AllocPage` plus a per-page header flag.
* The B+ tree treats keys as unique (the assignment says "use roll-no
  as the key").  Re-inserting the same key just overwrites the existing
  RID rather than rejecting the operation.
* The original toyDB sources in `reference/` are kept verbatim for
  inspection; they are *not* used by the build.

---
