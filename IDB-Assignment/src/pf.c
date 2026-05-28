/* ============================================================================
 * pf.c --- Paged File Layer implementation
 *
 *  Disk layout for every file managed by PF:
 *
 *     +---------------------------+
 *     |  FILE HEADER (16 bytes)   |   magic + numpages
 *     +---------------------------+
 *     |  PAGE 0   (4096 bytes)    |
 *     +---------------------------+
 *     |  PAGE 1                   |
 *     +---------------------------+
 *               ....
 *
 *  Buffer Pool (per open file) is an array of `pool_size` frames threaded
 *  into a doubly-linked list ordered by recency-of-use (head = MRU,
 *  tail = LRU).  The replacement policy chosen at open() time decides
 *  whether the eviction victim is taken from the head (MRU policy) or
 *  the tail (LRU policy), skipping pinned frames.
 *
 *  All counters are accumulated in a PF_Stats struct so the upper-level
 *  benchmark drivers can report logical-vs-physical I/O for any work-load
 *  mix of reads and writes.
 *
 *  Author : Daksh Jain  (Roll No. B24bs1114)
 * ==========================================================================*/

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "pf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ----------------------------------------------------------------------- */
/*                            on-disk header                               */
/* ----------------------------------------------------------------------- */
#define PF_MAGIC    0x50466631u      /* "PFf1" */
#define PF_HDR_SIZE 16

typedef struct {
    uint32_t magic;
    int32_t  numpages;
    int32_t  pad0;
    int32_t  pad1;
} PF_DiskHdr;

/* ----------------------------------------------------------------------- */
/*                        in-memory buffer frame                            */
/* ----------------------------------------------------------------------- */
typedef struct PF_Frame {
    int              pagenum;        /* -1 if frame empty   */
    int              pin_count;
    int              dirty;
    char            *data;           /* PF_PAGE_SIZE bytes  */
    struct PF_Frame *prev, *next;    /* recency list        */
} PF_Frame;

/* ----------------------------------------------------------------------- */
/*                    public-but-opaque PF_File struct                     */
/* ----------------------------------------------------------------------- */
struct PF_File {
    int            unix_fd;
    char          *fname;
    PF_DiskHdr     hdr;
    int            hdr_dirty;

    /* buffer pool */
    PF_ReplPolicy  policy;
    int            pool_size;
    PF_Frame      *frames;           /* array */
    PF_Frame      *head, *tail;      /* recency list ends   */

    /* simple open-addressed page->frame map (linear probe). */
    int           *map_page;         /* parallel arrays     */
    int           *map_frame;
    int            map_cap;

    PF_Stats       stats;
};

/* error message table */
static const char *PF_msg[] = {
    "ok",
    "out of memory",
    "buffer pool exhausted (all frames pinned)",
    "page already fixed",
    "page not in buffer",
    "unix system error",
    "", "", "", "",
    "invalid page number",
    "file already open / open error",
    "", "",
    "invalid file descriptor",
    "end of file",
};

const char *PF_StrError(int err) {
    int i = -err;
    if (i < 0 || (size_t)i >= sizeof(PF_msg) / sizeof(PF_msg[0]))
        return "unknown error";
    return PF_msg[i];
}

/* ----------------------------------------------------------------------- */
/*                          tiny page->frame map                           */
/* ----------------------------------------------------------------------- */
static int map_find(PF_File *f, int page) {
    int n = f->map_cap;
    int h = ((unsigned)page * 2654435761u) % n;
    for (int i = 0; i < n; ++i) {
        int j = (h + i) % n;
        if (f->map_page[j] == -1) return -1;       /* empty -> miss */
        if (f->map_page[j] == page) return f->map_frame[j];
    }
    return -1;
}
static void map_insert(PF_File *f, int page, int frame) {
    int n = f->map_cap;
    int h = ((unsigned)page * 2654435761u) % n;
    for (int i = 0; i < n; ++i) {
        int j = (h + i) % n;
        if (f->map_page[j] == -1 || f->map_page[j] == page) {
            f->map_page[j]  = page;
            f->map_frame[j] = frame;
            return;
        }
    }
}
static void map_remove(PF_File *f, int page) {
    int n = f->map_cap;
    int h = ((unsigned)page * 2654435761u) % n;
    for (int i = 0; i < n; ++i) {
        int j = (h + i) % n;
        if (f->map_page[j] == -1) return;
        if (f->map_page[j] == page) {
            f->map_page[j] = -1;
            f->map_frame[j] = -1;
            /* re-hash the remainder of the cluster (linear probing). */
            int k = (j + 1) % n;
            while (f->map_page[k] != -1) {
                int p = f->map_page[k], fr = f->map_frame[k];
                f->map_page[k] = -1;
                f->map_frame[k] = -1;
                map_insert(f, p, fr);
                k = (k + 1) % n;
            }
            return;
        }
    }
}

/* ----------------------------------------------------------------------- */
/*                       recency-list maintenance                          */
/* ----------------------------------------------------------------------- */
static void list_unlink(PF_File *f, PF_Frame *fr) {
    if (fr->prev) fr->prev->next = fr->next;
    else          f->head        = fr->next;
    if (fr->next) fr->next->prev = fr->prev;
    else          f->tail        = fr->prev;
    fr->prev = fr->next = NULL;
}
static void list_push_head(PF_File *f, PF_Frame *fr) {
    fr->prev = NULL;
    fr->next = f->head;
    if (f->head) f->head->prev = fr;
    f->head = fr;
    if (!f->tail) f->tail = fr;
}
static void list_touch(PF_File *f, PF_Frame *fr) {
    list_unlink(f, fr);
    list_push_head(f, fr);
}

/* ----------------------------------------------------------------------- */
/*                   physical disk read / write                            */
/* ----------------------------------------------------------------------- */
static off_t page_offset(int pagenum) {
    return (off_t)PF_HDR_SIZE + (off_t)pagenum * (off_t)PF_PAGE_SIZE;
}

static int phys_read(PF_File *f, int pagenum, char *dst) {
    if (lseek(f->unix_fd, page_offset(pagenum), SEEK_SET) < 0) return PFE_UNIX;
    ssize_t r = read(f->unix_fd, dst, PF_PAGE_SIZE);
    if (r != PF_PAGE_SIZE) {
        /* For freshly-allocated pages on disk that have been seek-extended
         * but not yet written, treat partial/zero read as "all zeros". */
        if (r >= 0) memset(dst + (r > 0 ? r : 0), 0, PF_PAGE_SIZE - (r > 0 ? r : 0));
        else        return PFE_UNIX;
    }
    f->stats.phys_reads++;
    return PFE_OK;
}

static int phys_write(PF_File *f, int pagenum, const char *src) {
    if (lseek(f->unix_fd, page_offset(pagenum), SEEK_SET) < 0) return PFE_UNIX;
    if (write(f->unix_fd, src, PF_PAGE_SIZE) != PF_PAGE_SIZE)  return PFE_UNIX;
    f->stats.phys_writes++;
    return PFE_OK;
}

static int phys_write_hdr(PF_File *f) {
    if (lseek(f->unix_fd, 0, SEEK_SET) < 0) return PFE_UNIX;
    if (write(f->unix_fd, &f->hdr, sizeof(f->hdr)) != sizeof(f->hdr))
        return PFE_UNIX;
    f->hdr_dirty = 0;
    return PFE_OK;
}

/* ----------------------------------------------------------------------- */
/*                           victim selection                              */
/* ----------------------------------------------------------------------- */
static PF_Frame *find_victim(PF_File *f) {
    PF_Frame *fr = (f->policy == PF_REPL_MRU) ? f->head : f->tail;
    while (fr && fr->pin_count > 0)
        fr = (f->policy == PF_REPL_MRU) ? fr->next : fr->prev;
    return fr;
}

/* ----------------------------------------------------------------------- */
/*                              fault-in                                   */
/* ----------------------------------------------------------------------- */
static int fault_in(PF_File *f, int pagenum, int read_existing,
                    PF_Frame **out)
{
    /* 1. find a free frame, else evict */
    PF_Frame *fr = NULL;
    for (int i = 0; i < f->pool_size; ++i)
        if (f->frames[i].pagenum == -1) { fr = &f->frames[i]; break; }

    if (!fr) {
        fr = find_victim(f);
        if (!fr) return PFE_NOBUF;
        if (fr->dirty) {
            int e = phys_write(f, fr->pagenum, fr->data);
            if (e != PFE_OK) return e;
            f->stats.dirty_evictions++;
        }
        f->stats.evictions++;
        map_remove(f, fr->pagenum);
        list_unlink(f, fr);
        fr->pagenum = -1;
        fr->dirty = 0;
    }

    if (read_existing) {
        int e = phys_read(f, pagenum, fr->data);
        if (e != PFE_OK) return e;
    } else {
        memset(fr->data, 0, PF_PAGE_SIZE);
    }
    fr->pagenum = pagenum;
    fr->pin_count = 0;
    fr->dirty = 0;
    list_push_head(f, fr);
    map_insert(f, pagenum, (int)(fr - f->frames));
    *out = fr;
    return PFE_OK;
}

/* ----------------------------------------------------------------------- */
/*                           public API : files                            */
/* ----------------------------------------------------------------------- */
int PF_CreateFile(const char *name) {
    int fd = open(name, O_CREAT | O_EXCL | O_RDWR, 0664);
    if (fd < 0) return PFE_UNIX;
    PF_DiskHdr h = { PF_MAGIC, 0, 0, 0 };
    if (write(fd, &h, sizeof(h)) != (ssize_t)sizeof(h)) {
        close(fd); unlink(name); return PFE_UNIX;
    }
    close(fd);
    return PFE_OK;
}

int PF_DestroyFile(const char *name) {
    return (unlink(name) == 0) ? PFE_OK : PFE_UNIX;
}

PF_File *PF_OpenFile(const char *name, PF_ReplPolicy policy, int pool_size) {
    if (pool_size < 2) pool_size = 2;

    PF_File *f = calloc(1, sizeof(*f));
    if (!f) return NULL;

    f->unix_fd = open(name, O_RDWR);
    if (f->unix_fd < 0) { free(f); return NULL; }

    if (read(f->unix_fd, &f->hdr, sizeof(f->hdr)) != (ssize_t)sizeof(f->hdr)
        || f->hdr.magic != PF_MAGIC) {
        close(f->unix_fd); free(f); return NULL;
    }

    f->fname     = strdup(name);
    f->policy    = policy;
    f->pool_size = pool_size;
    f->frames    = calloc(pool_size, sizeof(PF_Frame));
    f->map_cap   = pool_size * 4 + 7;
    f->map_page  = malloc(sizeof(int) * f->map_cap);
    f->map_frame = malloc(sizeof(int) * f->map_cap);
    if (!f->frames || !f->map_page || !f->map_frame) {
        PF_CloseFile(f); return NULL;
    }
    for (int i = 0; i < pool_size; ++i) {
        f->frames[i].pagenum = -1;
        f->frames[i].data = malloc(PF_PAGE_SIZE);
        if (!f->frames[i].data) { PF_CloseFile(f); return NULL; }
    }
    for (int i = 0; i < f->map_cap; ++i) {
        f->map_page[i] = -1;
        f->map_frame[i] = -1;
    }
    return f;
}

int PF_CloseFile(PF_File *f) {
    if (!f) return PFE_FD;
    PF_FlushAll(f);
    if (f->hdr_dirty) phys_write_hdr(f);
    if (f->unix_fd >= 0) close(f->unix_fd);
    if (f->frames) {
        for (int i = 0; i < f->pool_size; ++i)
            free(f->frames[i].data);
        free(f->frames);
    }
    free(f->map_page);
    free(f->map_frame);
    free(f->fname);
    free(f);
    return PFE_OK;
}

/* ----------------------------------------------------------------------- */
/*                          public API : pages                              */
/* ----------------------------------------------------------------------- */
int PF_AllocPage(PF_File *f, int *pagenum, char **buf) {
    int p = f->hdr.numpages;
    /* Extend the file on disk so phys_read of this fresh page is well-defined. */
    if (ftruncate(f->unix_fd, page_offset(p + 1)) < 0) return PFE_UNIX;
    f->hdr.numpages = p + 1;
    f->hdr_dirty = 1;

    PF_Frame *fr = NULL;
    int e = fault_in(f, p, /*read_existing=*/0, &fr);
    if (e != PFE_OK) return e;

    fr->pin_count = 1;
    fr->dirty = 1;
    f->stats.logical_writes++;
    *pagenum = p;
    *buf = fr->data;
    return PFE_OK;
}

int PF_GetPage(PF_File *f, int pagenum, char **buf) {
    if (pagenum < 0 || pagenum >= f->hdr.numpages) return PFE_INVALIDPAGE;

    f->stats.logical_reads++;
    int idx = map_find(f, pagenum);
    PF_Frame *fr = NULL;

    if (idx >= 0) {
        fr = &f->frames[idx];
        f->stats.buffer_hits++;
        list_touch(f, fr);
    } else {
        f->stats.buffer_misses++;
        int e = fault_in(f, pagenum, /*read_existing=*/1, &fr);
        if (e != PFE_OK) return e;
    }
    fr->pin_count++;
    *buf = fr->data;
    return PFE_OK;
}

int PF_GetFirstPage(PF_File *f, int *pagenum, char **buf) {
    *pagenum = -1;
    return PF_GetNextPage(f, pagenum, buf);
}
int PF_GetNextPage(PF_File *f, int *pagenum, char **buf) {
    int p = *pagenum + 1;
    if (p >= f->hdr.numpages) return PFE_EOF;
    *pagenum = p;
    return PF_GetPage(f, p, buf);
}

int PF_MarkDirty(PF_File *f, int pagenum) {
    int idx = map_find(f, pagenum);
    if (idx < 0) return PFE_PAGENOTINBUF;
    PF_Frame *fr = &f->frames[idx];
    if (fr->pin_count <= 0) return PFE_PAGENOTINBUF;
    fr->dirty = 1;
    f->stats.logical_writes++;
    list_touch(f, fr);
    return PFE_OK;
}

int PF_UnpinPage(PF_File *f, int pagenum) {
    int idx = map_find(f, pagenum);
    if (idx < 0) return PFE_PAGENOTINBUF;
    PF_Frame *fr = &f->frames[idx];
    if (fr->pin_count <= 0) return PFE_PAGENOTINBUF;
    fr->pin_count--;
    return PFE_OK;
}

int PF_FlushAll(PF_File *f) {
    if (!f) return PFE_FD;
    for (int i = 0; i < f->pool_size; ++i) {
        PF_Frame *fr = &f->frames[i];
        if (fr->pagenum != -1 && fr->dirty) {
            int e = phys_write(f, fr->pagenum, fr->data);
            if (e != PFE_OK) return e;
            fr->dirty = 0;
        }
    }
    if (f->hdr_dirty) phys_write_hdr(f);
    return PFE_OK;
}

int             PF_NumPages (PF_File *f)         { return f ? f->hdr.numpages : 0; }
const PF_Stats *PF_GetStats (PF_File *f)         { return f ? &f->stats : NULL; }
void            PF_ResetStats(PF_File *f)        { if (f) memset(&f->stats, 0, sizeof(f->stats)); }
