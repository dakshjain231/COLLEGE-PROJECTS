/* ============================================================================
 * am.c --- B+ Tree access method built on top of PF.
 *
 *   Page 0 of the index file is the meta-page (magic, root_page, height).
 *   Every other page is either a leaf or an internal node; the type byte
 *   in the first 4 bytes distinguishes them.
 *
 *  Author : Daksh Jain (B24bs1114)
 * ==========================================================================*/
#include "am.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AM_MAGIC        0x414D3031u   /* 'AM01' */
#define AM_LEAF         1
#define AM_INTERNAL     2

#define MAX_LEAF        340           /* (4096-16)/12  */
#define MAX_INTERNAL    509           /* (4096-16)/8   */

#pragma pack(push, 1)
typedef struct { uint32_t magic; int32_t root; int32_t height; int32_t pad; } AM_Meta;
typedef struct { int32_t type; int32_t nkeys; int32_t next_leaf; int32_t _pad; } LeafHdr;
typedef struct { int32_t key; int32_t page; int32_t slot; } LeafEnt;
typedef struct { int32_t type; int32_t nkeys; int32_t child0; int32_t _pad; } IntHdr;
typedef struct { int32_t key; int32_t child; } IntEnt;
#pragma pack(pop)

struct AM_Tree { PF_File *pf; AM_Meta meta; };


/* -------- meta-page helpers ------------------------------------------------ */
static int meta_load(AM_Tree *t) {
    char *buf;
    int e = PF_GetPage(t->pf, 0, &buf);
    if (e != PFE_OK) return e;
    memcpy(&t->meta, buf, sizeof(t->meta));
    PF_UnpinPage(t->pf, 0);
    return PFE_OK;
}
static int meta_store(AM_Tree *t) {
    char *buf;
    int e = PF_GetPage(t->pf, 0, &buf);
    if (e != PFE_OK) return e;
    memcpy(buf, &t->meta, sizeof(t->meta));
    PF_MarkDirty(t->pf, 0);
    PF_UnpinPage(t->pf, 0);
    return PFE_OK;
}

/* -------- node access helpers --------------------------------------------- */
static LeafEnt *leaf_ents(char *buf) { return (LeafEnt *)(buf + sizeof(LeafHdr)); }
static IntEnt  *int_ents (char *buf) { return (IntEnt  *)(buf + sizeof(IntHdr )); }

/* binary-search-the-largest-i such that ents[i].key <= key (for internals) */
static int int_locate(const IntEnt *e, int n, int key) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (e[mid].key <= key) lo = mid + 1; else hi = mid;
    }
    return lo - 1;   /* -1 means: descend into child0 */
}

static int leaf_locate(const LeafEnt *e, int n, int key) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (e[mid].key < key) lo = mid + 1; else hi = mid;
    }
    return lo;
}


/* -------- creation / open / close ---------------------------------------- */
AM_Tree *AM_Create(const char *name, PF_ReplPolicy pol, int pool) {
    if (PF_CreateFile(name) != PFE_OK) return NULL;
    PF_File *pf = PF_OpenFile(name, pol, pool);
    if (!pf) return NULL;
    /* allocate meta-page = page 0 */
    int p; char *buf;
    if (PF_AllocPage(pf, &p, &buf) != PFE_OK) { PF_CloseFile(pf); return NULL; }
    AM_Meta m = { AM_MAGIC, -1, 0, 0 };
    memcpy(buf, &m, sizeof(m));
    PF_MarkDirty(pf, p);
    PF_UnpinPage(pf, p);
    PF_FlushAll(pf);

    AM_Tree *t = calloc(1, sizeof(*t));
    t->pf = pf;
    t->meta = m;
    return t;
}

AM_Tree *AM_Open(const char *name, PF_ReplPolicy pol, int pool) {
    PF_File *pf = PF_OpenFile(name, pol, pool);
    if (!pf) return NULL;
    AM_Tree *t = calloc(1, sizeof(*t));
    t->pf = pf;
    if (meta_load(t) != PFE_OK || t->meta.magic != AM_MAGIC) {
        PF_CloseFile(pf); free(t); return NULL;
    }
    return t;
}

int AM_Close(AM_Tree *t) {
    if (!t) return PFE_FD;
    meta_store(t);
    PF_FlushAll(t->pf);
    int e = PF_CloseFile(t->pf);
    free(t);
    return e;
}


/* -------- search ---------------------------------------------------------- */
int AM_Search(AM_Tree *t, int key, RID *out) {
    if (t->meta.root < 0) return PFE_EOF;
    int p = t->meta.root;
    char *buf;
    while (1) {
        if (PF_GetPage(t->pf, p, &buf) != PFE_OK) return PFE_UNIX;
        int32_t type = *(int32_t *)buf;
        if (type == AM_LEAF) {
            LeafHdr *h = (LeafHdr *)buf;
            LeafEnt *e = leaf_ents(buf);
            int i = leaf_locate(e, h->nkeys, key);
            int found = (i < h->nkeys && e[i].key == key);
            if (found && out) { out->page = e[i].page; out->slot = e[i].slot; }
            PF_UnpinPage(t->pf, p);
            return found ? PFE_OK : PFE_EOF;
        } else {
            IntHdr *h = (IntHdr *)buf;
            IntEnt *e = int_ents(buf);
            int idx = int_locate(e, h->nkeys, key);
            int next = (idx < 0) ? h->child0 : e[idx].child;
            PF_UnpinPage(t->pf, p);
            p = next;
        }
    }
}


/* -------- internal: split helpers ---------------------------------------- */
typedef struct { int promoted; int sep_key; int new_child; } InsRes;

static int leaf_split(AM_Tree *t, int p, char *buf,
                      int ins_pos, int key, RID v, InsRes *r)
{
    LeafHdr *h = (LeafHdr *)buf;
    LeafEnt *e = leaf_ents(buf);
    LeafEnt tmp[MAX_LEAF + 1];
    int n = h->nkeys;
    memcpy(tmp, e, n * sizeof(LeafEnt));
    /* shift up */
    for (int i = n; i > ins_pos; --i) tmp[i] = tmp[i - 1];
    tmp[ins_pos].key = key; tmp[ins_pos].page = v.page; tmp[ins_pos].slot = v.slot;
    n++;

    int half = n / 2;

    /* allocate the new right-sibling leaf */
    int q; char *qbuf;
    if (PF_AllocPage(t->pf, &q, &qbuf) != PFE_OK) return PFE_UNIX;
    memset(qbuf, 0, PF_PAGE_SIZE);
    LeafHdr *qh = (LeafHdr *)qbuf;
    qh->type = AM_LEAF;
    qh->nkeys = n - half;
    qh->next_leaf = h->next_leaf;
    memcpy(leaf_ents(qbuf), tmp + half, (n - half) * sizeof(LeafEnt));
    PF_MarkDirty(t->pf, q);
    PF_UnpinPage(t->pf, q);

    /* rewrite original leaf with first half */
    h->nkeys = half;
    h->next_leaf = q;
    memcpy(e, tmp, half * sizeof(LeafEnt));
    PF_MarkDirty(t->pf, p);

    r->promoted = 1;
    r->sep_key  = tmp[half].key;
    r->new_child = q;
    return PFE_OK;
}

static int int_split(AM_Tree *t, int p, char *buf,
                     int ins_pos, int sep, int new_child, InsRes *r)
{
    IntHdr *h = (IntHdr *)buf;
    IntEnt *e = int_ents(buf);
    IntEnt tmp[MAX_INTERNAL + 1];
    int n = h->nkeys;
    memcpy(tmp, e, n * sizeof(IntEnt));
    for (int i = n; i > ins_pos; --i) tmp[i] = tmp[i - 1];
    tmp[ins_pos].key = sep;
    tmp[ins_pos].child = new_child;
    n++;

    int mid = n / 2;
    int promoted_key = tmp[mid].key;

    /* new right internal node: child0 = tmp[mid].child, then tmp[mid+1..n-1] */
    int q; char *qbuf;
    if (PF_AllocPage(t->pf, &q, &qbuf) != PFE_OK) return PFE_UNIX;
    memset(qbuf, 0, PF_PAGE_SIZE);
    IntHdr *qh = (IntHdr *)qbuf;
    qh->type = AM_INTERNAL;
    qh->nkeys = n - mid - 1;
    qh->child0 = tmp[mid].child;
    memcpy(int_ents(qbuf), tmp + mid + 1, (n - mid - 1) * sizeof(IntEnt));
    PF_MarkDirty(t->pf, q);
    PF_UnpinPage(t->pf, q);

    /* rewrite original */
    h->nkeys = mid;
    memcpy(e, tmp, mid * sizeof(IntEnt));
    PF_MarkDirty(t->pf, p);

    r->promoted = 1;
    r->sep_key  = promoted_key;
    r->new_child = q;
    return PFE_OK;
}


/* -------- recursive insert ----------------------------------------------- */
static int do_insert(AM_Tree *t, int p, int key, RID v, InsRes *r) {
    char *buf;
    if (PF_GetPage(t->pf, p, &buf) != PFE_OK) return PFE_UNIX;
    int32_t type = *(int32_t *)buf;

    if (type == AM_LEAF) {
        LeafHdr *h = (LeafHdr *)buf;
        LeafEnt *e = leaf_ents(buf);
        int pos = leaf_locate(e, h->nkeys, key);
        if (pos < h->nkeys && e[pos].key == key) {
            /* duplicate -> overwrite value (treat key as unique) */
            e[pos].page = v.page; e[pos].slot = v.slot;
            PF_MarkDirty(t->pf, p); PF_UnpinPage(t->pf, p);
            r->promoted = 0;
            return PFE_OK;
        }
        if (h->nkeys < MAX_LEAF) {
            for (int i = h->nkeys; i > pos; --i) e[i] = e[i - 1];
            e[pos].key = key; e[pos].page = v.page; e[pos].slot = v.slot;
            h->nkeys++;
            PF_MarkDirty(t->pf, p); PF_UnpinPage(t->pf, p);
            r->promoted = 0;
            return PFE_OK;
        }
        int rc = leaf_split(t, p, buf, pos, key, v, r);
        PF_UnpinPage(t->pf, p);
        return rc;
    } else {
        IntHdr *h = (IntHdr *)buf;
        IntEnt *e = int_ents(buf);
        int idx = int_locate(e, h->nkeys, key);
        int child = (idx < 0) ? h->child0 : e[idx].child;
        PF_UnpinPage(t->pf, p);

        InsRes sub = {0};
        int rc = do_insert(t, child, key, v, &sub);
        if (rc != PFE_OK) return rc;
        if (!sub.promoted) { r->promoted = 0; return PFE_OK; }

        /* re-fetch parent and insert separator */
        if (PF_GetPage(t->pf, p, &buf) != PFE_OK) return PFE_UNIX;
        h = (IntHdr *)buf; e = int_ents(buf);
        int pos = idx + 1;
        if (h->nkeys < MAX_INTERNAL) {
            for (int i = h->nkeys; i > pos; --i) e[i] = e[i - 1];
            e[pos].key = sub.sep_key; e[pos].child = sub.new_child;
            h->nkeys++;
            PF_MarkDirty(t->pf, p); PF_UnpinPage(t->pf, p);
            r->promoted = 0; return PFE_OK;
        }
        rc = int_split(t, p, buf, pos, sub.sep_key, sub.new_child, r);
        PF_UnpinPage(t->pf, p);
        return rc;
    }
}

/* -------- public insert -------------------------------------------------- */
int AM_Insert(AM_Tree *t, int key, RID v) {
    if (t->meta.root < 0) {
        int p; char *buf;
        if (PF_AllocPage(t->pf, &p, &buf) != PFE_OK) return PFE_UNIX;
        memset(buf, 0, PF_PAGE_SIZE);
        LeafHdr *h = (LeafHdr *)buf;
        h->type = AM_LEAF; h->nkeys = 1; h->next_leaf = -1;
        leaf_ents(buf)[0] = (LeafEnt){ key, v.page, v.slot };
        PF_MarkDirty(t->pf, p); PF_UnpinPage(t->pf, p);
        t->meta.root = p; t->meta.height = 1;
        meta_store(t);
        return PFE_OK;
    }

    InsRes r = {0};
    int rc = do_insert(t, t->meta.root, key, v, &r);
    if (rc != PFE_OK) return rc;
    if (!r.promoted) return PFE_OK;

    /* root split: create a new root */
    int newroot; char *buf;
    if (PF_AllocPage(t->pf, &newroot, &buf) != PFE_OK) return PFE_UNIX;
    memset(buf, 0, PF_PAGE_SIZE);
    IntHdr *h = (IntHdr *)buf;
    h->type = AM_INTERNAL; h->nkeys = 1; h->child0 = t->meta.root;
    int_ents(buf)[0].key = r.sep_key;
    int_ents(buf)[0].child = r.new_child;
    PF_MarkDirty(t->pf, newroot); PF_UnpinPage(t->pf, newroot);

    t->meta.root = newroot; t->meta.height++;
    meta_store(t);
    return PFE_OK;
}


/* -------- bulk-load: build leaf level then internal levels bottom-up ----- */
AM_Tree *AM_BulkLoad(const char *name, const int *keys, const RID *vals,
                     int n, double fill, PF_ReplPolicy pol, int pool)
{
    if (fill < 0.05 || fill > 1.0) fill = 0.7;
    if (n <= 0) return AM_Create(name, pol, pool);

    AM_Tree *t = AM_Create(name, pol, pool);
    if (!t) return NULL;

    int per_leaf = (int)(MAX_LEAF * fill); if (per_leaf < 1) per_leaf = 1;
    int per_int  = (int)(MAX_INTERNAL * fill); if (per_int < 1) per_int = 1;

    /* --- emit leaf level --- */
    int *level_pages = malloc(sizeof(int) * (n / per_leaf + 4));
    int *level_keys  = malloc(sizeof(int) * (n / per_leaf + 4));
    int  level_n     = 0;

    int prev_leaf = -1;
    int i = 0;
    while (i < n) {
        int take = (i + per_leaf <= n) ? per_leaf : (n - i);
        int p; char *buf;
        if (PF_AllocPage(t->pf, &p, &buf) != PFE_OK) { AM_Close(t); return NULL; }
        memset(buf, 0, PF_PAGE_SIZE);
        LeafHdr *h = (LeafHdr *)buf;
        h->type = AM_LEAF; h->nkeys = take; h->next_leaf = -1;
        LeafEnt *e = leaf_ents(buf);
        for (int j = 0; j < take; ++j) {
            e[j].key = keys[i + j];
            e[j].page = vals[i + j].page;
            e[j].slot = vals[i + j].slot;
        }
        PF_MarkDirty(t->pf, p); PF_UnpinPage(t->pf, p);

        /* link previous leaf -> p */
        if (prev_leaf >= 0) {
            char *pb;
            PF_GetPage(t->pf, prev_leaf, &pb);
            ((LeafHdr *)pb)->next_leaf = p;
            PF_MarkDirty(t->pf, prev_leaf);
            PF_UnpinPage(t->pf, prev_leaf);
        }
        prev_leaf = p;

        level_pages[level_n] = p;
        level_keys [level_n] = keys[i];   /* first key of leaf becomes separator */
        level_n++;
        i += take;
    }

    /* If only one leaf, it's the root */
    if (level_n == 1) {
        t->meta.root = level_pages[0];
        t->meta.height = 1;
        meta_store(t); PF_FlushAll(t->pf);
        free(level_pages); free(level_keys);
        return t;
    }

    int height = 1;
    while (level_n > 1) {
        int *next_pages = malloc(sizeof(int) * (level_n / per_int + 4));
        int *next_keys  = malloc(sizeof(int) * (level_n / per_int + 4));
        int  next_n     = 0;

        int j = 0;
        while (j < level_n) {
            int take = (j + per_int + 1 <= level_n) ? per_int + 1 : (level_n - j);
            /* a node has 'take' children and 'take-1' separator keys */
            int p; char *buf;
            if (PF_AllocPage(t->pf, &p, &buf) != PFE_OK) {
                AM_Close(t); free(level_pages); free(level_keys);
                free(next_pages); free(next_keys); return NULL;
            }
            memset(buf, 0, PF_PAGE_SIZE);
            IntHdr *h = (IntHdr *)buf;
            h->type = AM_INTERNAL;
            h->nkeys = take - 1;
            h->child0 = level_pages[j];
            IntEnt *e = int_ents(buf);
            for (int k = 1; k < take; ++k) {
                e[k - 1].key = level_keys[j + k];
                e[k - 1].child = level_pages[j + k];
            }
            PF_MarkDirty(t->pf, p); PF_UnpinPage(t->pf, p);
            next_pages[next_n] = p;
            next_keys [next_n] = level_keys[j];
            next_n++;
            j += take;
        }
        free(level_pages); free(level_keys);
        level_pages = next_pages; level_keys = next_keys; level_n = next_n;
        height++;
    }

    t->meta.root = level_pages[0];
    t->meta.height = height;
    meta_store(t); PF_FlushAll(t->pf);
    free(level_pages); free(level_keys);
    return t;
}

const PF_Stats *AM_GetStats(AM_Tree *t) { return PF_GetStats(t->pf); }
PF_File        *AM_GetPF   (AM_Tree *t) { return t->pf; }
int             AM_Height  (AM_Tree *t) { return t->meta.height; }
