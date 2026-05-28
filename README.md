# COLLEGE-PROJECTS

Coursework projects by **Daksh Jain (B24bs1114)**.

## Projects

* [`IDB-Assignment/`](./IDB-Assignment) - Database Management Systems
  assignment (Instructor: Nitin Awathare). Implements:
  * Page-buffering for the toyDB Paged-File layer with selectable LRU/MRU
    replacement, configurable pool size, dirty-bit tracking and full I/O
    statistics (Objective 1).
  * A slotted-page record store for variable-length records, with
    insert / delete / sequential-scan and a space-utilisation comparison
    against fixed-length static records (Objective 2).
  * A B+ tree index on the student `roll-no` field, built three different
    ways (incremental insert, file-then-index, sorted bulk-load) and
    benchmarked on query time and pages accessed (Objective 3).
  * Original toyDB sources are preserved under
    `IDB-Assignment/reference/` for inspection.

The original assignment archive `IDB.rar` is kept here for reference.
