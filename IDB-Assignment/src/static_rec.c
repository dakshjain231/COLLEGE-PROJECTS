/* static_rec.c --- Fixed-length record store on top of PF.
 * Author : Daksh Jain  (B24bs1114)
 *
 * Page layout :  [u16 nslots][bitmap (ceil(nslots/8) bytes)] [recs ...]
 * Records are padded with zero bytes up to max_len, so any record shorter
 * than max_len wastes (max_len - real_len) bytes - this is exactly the
 * inefficiency the assignment asks us to expose.
 */
#include "static_rec.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct ST_File {
    PF_File *pf;
    int      max_len;
    int      records_per_page;
    int      bitmap_bytes;
    int      hdr_bytes;
    long     real_bytes;        /* sum of actual record lengths inserted */
    int      num_records;
};


static void compute_geom(ST_File *st) {
    /* Solve for largest n such that 2 + ceil(n/8) + n*max_len <= PAGE_SIZE */
    int n = 0;
    for (int try = 1; try <= 4096; ++try) {
        int bm = (try + 7) / 8;
        if (2 + bm + try * st->max_len <= PF_PAGE_SIZE) n = try;
        else break;
    }
    st->records_per_page = n;
    st->bitmap_bytes     = (n + 7) / 8;
    st->hdr_bytes        = 2 + st->bitmap_bytes;
}

static ST_File *st_alloc(const char *name, int max_len,
                         PF_ReplPolicy pol, int pool, int create)
{
    if (create && PF_CreateFile(name) != PFE_OK) return NULL;
    PF_File *pf = PF_OpenFile(name, pol, pool);
    if (!pf) return NULL;
    ST_File *st = calloc(1, sizeof(*st));
    st->pf = pf;
    st->max_len = max_len;
    compute_geom(st);
    return st;
}

ST_File *ST_Create(const char *name, int max_len, PF_ReplPolicy p, int pool) {
    return st_alloc(name, max_len, p, pool, 1);
}
ST_File *ST_Open  (const char *name, int max_len, PF_ReplPolicy p, int pool) {
    return st_alloc(name, max_len, p, pool, 0);
}
int      ST_Close (ST_File *st) {
    if (!st) return PFE_FD;
    PF_FlushAll(st->pf);
    int e = PF_CloseFile(st->pf);
    free(st);
    return e;
}


int ST_Insert(ST_File *st, const void *rec, int len) {
    if (len > st->max_len) len = st->max_len;
    int np = PF_NumPages(st->pf);
    char *buf;
    int pagenum = -1;

    /* try last page first */
    if (np > 0) {
        if (PF_GetPage(st->pf, np - 1, &buf) == PFE_OK) {
            uint16_t used = *(uint16_t *)buf;
            if (used < (uint16_t)st->records_per_page) pagenum = np - 1;
            else PF_UnpinPage(st->pf, np - 1);
        }
    }
    if (pagenum < 0) {
        if (PF_AllocPage(st->pf, &pagenum, &buf) != PFE_OK) return PFE_UNIX;
        memset(buf, 0, PF_PAGE_SIZE);
    }
    uint16_t *used = (uint16_t *)buf;
    uint8_t  *bm   = (uint8_t  *)(buf + 2);
    int slot = -1;
    for (int i = 0; i < st->records_per_page; ++i) {
        if (!(bm[i / 8] & (1u << (i & 7)))) { slot = i; break; }
    }
    if (slot < 0) { PF_UnpinPage(st->pf, pagenum); return PFE_NOMEM; }

    bm[slot / 8] |= (uint8_t)(1u << (slot & 7));
    (*used)++;
    char *dst = buf + st->hdr_bytes + slot * st->max_len;
    memset(dst, 0, st->max_len);
    memcpy(dst, rec, len);

    PF_MarkDirty(st->pf, pagenum);
    PF_UnpinPage(st->pf, pagenum);
    st->real_bytes += len;
    st->num_records++;
    return PFE_OK;
}

int    ST_NumRecords(ST_File *st) { return st ? st->num_records : 0; }
double ST_SpaceUtilization(ST_File *st) {
    int np = PF_NumPages(st->pf);
    if (np == 0) return 0.0;
    /* Only the actual record bytes are "useful"; everything else (padding,
     * bitmap, header, partially-empty pages) is overhead. */
    return (double)st->real_bytes / ((double)np * PF_PAGE_SIZE);
}
