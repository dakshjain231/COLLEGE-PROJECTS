/* ============================================================================
 * slotted.c --- Slotted-page record store implementation
 *
 *  Author : Daksh Jain  (Roll No. B24bs1114)
 * ==========================================================================*/
#include "slotted.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----- on-page structures (packed) ------------------------------------- */
#pragma pack(push, 1)
typedef struct { uint16_t nslots; uint16_t free_end; } PageHdr;
typedef struct { int16_t off; int16_t len; } Slot;     /* len < 0 -> deleted */
#pragma pack(pop)

#define PAGE_HDR_SZ ((int)sizeof(PageHdr))
#define SLOT_SZ     ((int)sizeof(Slot))

struct SP_File {
    PF_File *pf;
    int      num_records;
};

/* ----------------------------------------------------------------------- */
static void page_init(char *p) {
    PageHdr *h = (PageHdr *)p;
    h->nslots   = 0;
    h->free_end = PF_PAGE_SIZE;
}

/* free contiguous bytes between slot-array end and free_end. */
static int page_free(const char *p) {
    const PageHdr *h = (const PageHdr *)p;
    int slots_end = PAGE_HDR_SZ + h->nslots * SLOT_SZ;
    return (int)h->free_end - slots_end;
}

/* live bytes used for record payload + slot directory + header. */
static int page_live_bytes(const char *p) {
    const PageHdr *h = (const PageHdr *)p;
    int payload = 0;
    const Slot *s = (const Slot *)(p + PAGE_HDR_SZ);
    for (int i = 0; i < h->nslots; ++i)
        if (s[i].len >= 0) payload += s[i].len;
    return PAGE_HDR_SZ + h->nslots * SLOT_SZ + payload;
}

/* compact a page in place, removing tombstones */
static void page_compact(char *p) {
    PageHdr *h = (PageHdr *)p;
    Slot *s = (Slot *)(p + PAGE_HDR_SZ);
    char tmp[PF_PAGE_SIZE];
    int write_off = PF_PAGE_SIZE;
    for (int i = 0; i < h->nslots; ++i) {
        if (s[i].len < 0) continue;
        write_off -= s[i].len;
        memcpy(tmp + write_off, p + s[i].off, s[i].len);
        s[i].off = write_off;
    }
    memcpy(p + write_off, tmp + write_off, PF_PAGE_SIZE - write_off);
    h->free_end = write_off;
}

/* ----------------------------------------------------------------------- */
SP_File *SP_Create(const char *name, PF_ReplPolicy policy, int pool) {
    if (PF_CreateFile(name) != PFE_OK) return NULL;
    return SP_Open(name, policy, pool);
}

SP_File *SP_Open(const char *name, PF_ReplPolicy policy, int pool) {
    PF_File *pf = PF_OpenFile(name, policy, pool);
    if (!pf) return NULL;
    SP_File *sp = calloc(1, sizeof(*sp));
    sp->pf = pf;
    /* recompute num_records by scanning the slot directory of every page. */
    int n = PF_NumPages(pf);
    for (int p = 0; p < n; ++p) {
        char *buf;
        if (PF_GetPage(pf, p, &buf) != PFE_OK) continue;
        PageHdr *h = (PageHdr *)buf;
        Slot *s = (Slot *)(buf + PAGE_HDR_SZ);
        for (int i = 0; i < h->nslots; ++i)
            if (s[i].len >= 0) sp->num_records++;
        PF_UnpinPage(pf, p);
    }
    return sp;
}

int SP_Close(SP_File *sp) {
    if (!sp) return PFE_FD;
    PF_FlushAll(sp->pf);
    int e = PF_CloseFile(sp->pf);
    free(sp);
    return e;
}

/* ----------------------------------------------------------------------- */
int SP_Insert(SP_File *sp, const void *rec, int len, RID *rid_out) {
    if (len < 0 || len > PF_PAGE_SIZE - PAGE_HDR_SZ - SLOT_SZ) return PFE_NOMEM;

    char *buf = NULL;
    int   pagenum = -1;
    int   need = len + SLOT_SZ;
    int   numpages = PF_NumPages(sp->pf);

    /* try to fit in the last page first (simple "last-page" strategy). */
    if (numpages > 0) {
        int p = numpages - 1;
        if (PF_GetPage(sp->pf, p, &buf) == PFE_OK) {
            if (page_free(buf) >= need) {
                pagenum = p;
            } else {
                /* try compaction */
                int total_free = page_free(buf);
                PageHdr *h = (PageHdr *)buf;
                Slot *s = (Slot *)(buf + PAGE_HDR_SZ);
                int dead = 0;
                for (int i = 0; i < h->nslots; ++i)
                    if (s[i].len < 0) dead += 0;          /* ignore */
                /* compute actual fragmented hole */
                int payload = 0;
                for (int i = 0; i < h->nslots; ++i)
                    if (s[i].len >= 0) payload += s[i].len;
                int real_free = PF_PAGE_SIZE - PAGE_HDR_SZ - h->nslots * SLOT_SZ - payload;
                if (real_free >= need) {
                    page_compact(buf);
                    PF_MarkDirty(sp->pf, p);
                    pagenum = p;
                } else {
                    PF_UnpinPage(sp->pf, p);
                }
                (void)total_free; (void)dead;
            }
        }
    }
    if (pagenum < 0) {
        /* allocate fresh page */
        if (PF_AllocPage(sp->pf, &pagenum, &buf) != PFE_OK) return PFE_UNIX;
        page_init(buf);
        PF_MarkDirty(sp->pf, pagenum);
    }

    PageHdr *h = (PageHdr *)buf;
    Slot    *s = (Slot *)(buf + PAGE_HDR_SZ);

    /* try to reuse a tombstone slot to keep dir compact (optional). */
    int slot_idx = -1;
    for (int i = 0; i < h->nslots; ++i)
        if (s[i].len < 0) { slot_idx = i; break; }
    if (slot_idx < 0) {
        slot_idx = h->nslots;
        h->nslots++;
        /* note: nslots grew by 1, ensure free space still ok */
    }

    int new_off = h->free_end - len;
    s[slot_idx].off = (int16_t)new_off;
    s[slot_idx].len = (int16_t)len;
    h->free_end = (uint16_t)new_off;
    memcpy(buf + new_off, rec, len);

    PF_MarkDirty(sp->pf, pagenum);
    PF_UnpinPage(sp->pf, pagenum);

    sp->num_records++;
    if (rid_out) { rid_out->page = pagenum; rid_out->slot = slot_idx; }
    return PFE_OK;
}

int SP_Delete(SP_File *sp, RID rid) {
    char *buf;
    int e = PF_GetPage(sp->pf, rid.page, &buf);
    if (e != PFE_OK) return e;
    PageHdr *h = (PageHdr *)buf;
    Slot *s = (Slot *)(buf + PAGE_HDR_SZ);
    if (rid.slot < 0 || rid.slot >= h->nslots || s[rid.slot].len < 0) {
        PF_UnpinPage(sp->pf, rid.page);
        return PFE_INVALIDPAGE;
    }
    s[rid.slot].len = -1;
    sp->num_records--;
    PF_MarkDirty(sp->pf, rid.page);
    PF_UnpinPage(sp->pf, rid.page);
    return PFE_OK;
}

int SP_Get(SP_File *sp, RID rid, void *out, int *len) {
    char *buf;
    int e = PF_GetPage(sp->pf, rid.page, &buf);
    if (e != PFE_OK) return e;
    PageHdr *h = (PageHdr *)buf;
    Slot *s = (Slot *)(buf + PAGE_HDR_SZ);
    if (rid.slot < 0 || rid.slot >= h->nslots || s[rid.slot].len < 0) {
        PF_UnpinPage(sp->pf, rid.page);
        return PFE_INVALIDPAGE;
    }
    int rl = s[rid.slot].len;
    if (out) memcpy(out, buf + s[rid.slot].off, rl);
    if (len) *len = rl;
    PF_UnpinPage(sp->pf, rid.page);
    return PFE_OK;
}

/* ----------------------------------------------------------------------- */
int SP_ScanFirst(SP_File *sp, RID *rid, void *buf, int *len) {
    rid->page = 0;
    rid->slot = -1;
    return SP_ScanNext(sp, rid, buf, len);
}

int SP_ScanNext(SP_File *sp, RID *rid, void *buf, int *len) {
    int np = PF_NumPages(sp->pf);
    int p = rid->page, s_idx = rid->slot;
    while (p < np) {
        char *page;
        if (PF_GetPage(sp->pf, p, &page) != PFE_OK) return PFE_UNIX;
        PageHdr *h = (PageHdr *)page;
        Slot *s = (Slot *)(page + PAGE_HDR_SZ);
        for (int i = s_idx + 1; i < h->nslots; ++i) {
            if (s[i].len >= 0) {
                if (buf) memcpy(buf, page + s[i].off, s[i].len);
                if (len) *len = s[i].len;
                rid->page = p;
                rid->slot = i;
                PF_UnpinPage(sp->pf, p);
                return PFE_OK;
            }
        }
        PF_UnpinPage(sp->pf, p);
        p++;
        s_idx = -1;
    }
    return PFE_EOF;
}

/* ----------------------------------------------------------------------- */
int SP_NumRecords(SP_File *sp) { return sp ? sp->num_records : 0; }

double SP_SpaceUtilization(SP_File *sp) {
    int np = PF_NumPages(sp->pf);
    if (np == 0) return 0.0;
    long live = 0;
    for (int p = 0; p < np; ++p) {
        char *buf;
        if (PF_GetPage(sp->pf, p, &buf) != PFE_OK) continue;
        live += page_live_bytes(buf);
        PF_UnpinPage(sp->pf, p);
    }
    return (double)live / ((double)np * PF_PAGE_SIZE);
}

const PF_Stats *SP_GetStats(SP_File *sp) { return PF_GetStats(sp->pf); }
PF_File        *SP_GetPF   (SP_File *sp) { return sp->pf; }
