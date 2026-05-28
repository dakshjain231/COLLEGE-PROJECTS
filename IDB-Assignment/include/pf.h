/* ============================================================================
 * pf.h --- Paged File Layer with Buffered Page Cache (Objective 1)
 *
 * Re-implementation of the toyDB PF layer in modern C99.
 * Adds the features required by the assignment:
 *   * Selectable page replacement policy (LRU or MRU) per-file at open time.
 *   * Configurable buffer pool size (per file).
 *   * Per-page dirty bit, set by an explicit PF_MarkDirty() call.
 *   * Per-file statistics (logical/physical reads & writes, hits, evictions).
 *
 *  Author : Daksh Jain  (Roll No. B24bs1114)
 *  Course : Database Management Systems
 * ==========================================================================*/
#ifndef PF_H
#define PF_H

#include <stddef.h>

/* --------------------------------------------------------------------------
 * Page geometry
 * The user-visible payload of every page is exactly PF_PAGE_SIZE bytes.
 * On disk a page is preceded by a small (4-byte) header storing meta-data
 * which is hidden from upper layers.
 * ------------------------------------------------------------------------*/
#define PF_PAGE_SIZE      4096
#define PF_INVALID_PAGE   (-1)

/* Error codes (negative). */
#define PFE_OK             0
#define PFE_NOMEM         -1
#define PFE_NOBUF         -2
#define PFE_PAGEFIXED     -3
#define PFE_PAGENOTINBUF  -4
#define PFE_UNIX          -5
#define PFE_INVALIDPAGE   -10
#define PFE_FILEOPEN      -11
#define PFE_FD            -13
#define PFE_EOF           -14

/* --------------------------------------------------------------------------
 * Replacement policy.
 *   PF_REPL_LRU : evict least-recently-used unpinned page  (good for random)
 *   PF_REPL_MRU : evict most-recently-used  unpinned page  (good for scans)
 * ------------------------------------------------------------------------*/
typedef enum { PF_REPL_LRU = 0, PF_REPL_MRU = 1 } PF_ReplPolicy;

/* Statistics gathered by every open PF_File. */
typedef struct PF_Stats {
    long logical_reads;     /* PF_GetPage / PF_GetThisPage / scan calls    */
    long logical_writes;    /* PF_MarkDirty calls                          */
    long phys_reads;        /* actual read() syscalls performed            */
    long phys_writes;       /* actual write() syscalls performed           */
    long buffer_hits;       /* page found resident in pool                 */
    long buffer_misses;     /* page had to be brought in from disk         */
    long evictions;         /* victim evicted to satisfy a fault           */
    long dirty_evictions;   /* of which were dirty (forced phys_write)     */
} PF_Stats;

/* Opaque handle to an open paged file. */
typedef struct PF_File PF_File;

/* ---- File lifecycle --------------------------------------------------- */
int      PF_CreateFile (const char *name);
int      PF_DestroyFile(const char *name);
PF_File *PF_OpenFile   (const char *name, PF_ReplPolicy policy, int pool_size);
int      PF_CloseFile  (PF_File *f);

/* ---- Page operations -------------------------------------------------- */
/* Allocate a fresh page; returns its number in *pagenum and a pointer
 * to its in-memory buffer in *buf (page is pinned + marked dirty). */
int  PF_AllocPage   (PF_File *f, int *pagenum, char **buf);

/* Pin & fetch an existing page. Returned buffer is valid until UnpinPage. */
int  PF_GetPage     (PF_File *f, int pagenum, char **buf);

/* Sequential scan helpers. Pin one page at a time; caller must Unpin. */
int  PF_GetFirstPage(PF_File *f, int *pagenum, char **buf);
int  PF_GetNextPage (PF_File *f, int *pagenum, char **buf);

/* Mark a pinned page dirty (must be done BEFORE unpinning if modified). */
int  PF_MarkDirty   (PF_File *f, int pagenum);

/* Release a pin held on a page. */
int  PF_UnpinPage   (PF_File *f, int pagenum);

/* Force every dirty pinned/unpinned page back to disk. */
int  PF_FlushAll    (PF_File *f);

/* ---- Introspection / statistics --------------------------------------- */
int             PF_NumPages (PF_File *f);
const PF_Stats *PF_GetStats (PF_File *f);
void            PF_ResetStats(PF_File *f);
const char     *PF_StrError (int err);

#endif /* PF_H */
