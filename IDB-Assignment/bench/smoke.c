/* smoke test for the PF / SP / AM layers */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pf.h"
#include "slotted.h"
#include "am.h"

int main(void) {
    unlink("/tmp/idb_smoke.pf");
    assert(PF_CreateFile("/tmp/idb_smoke.pf") == PFE_OK);
    PF_File *f = PF_OpenFile("/tmp/idb_smoke.pf", PF_REPL_LRU, 4);
    assert(f);
    int p; char *b;
    for (int i = 0; i < 20; ++i) {
        assert(PF_AllocPage(f, &p, &b) == PFE_OK);
        snprintf(b, PF_PAGE_SIZE, "page-%d", i);
        PF_MarkDirty(f, p);
        PF_UnpinPage(f, p);
    }
    PF_FlushAll(f);
    for (int i = 0; i < 20; ++i) {
        assert(PF_GetPage(f, i, &b) == PFE_OK);
        char want[32]; snprintf(want, sizeof(want), "page-%d", i);
        assert(strncmp(b, want, strlen(want)) == 0);
        PF_UnpinPage(f, i);
    }
    const PF_Stats *s = PF_GetStats(f);
    printf("PF stats : reads=%ld phys_reads=%ld phys_writes=%ld evictions=%ld\n",
           s->logical_reads, s->phys_reads, s->phys_writes, s->evictions);
    PF_CloseFile(f);

    /* slotted page */
    unlink("/tmp/idb_smoke.sp");
    SP_File *sp = SP_Create("/tmp/idb_smoke.sp", PF_REPL_LRU, 8);
    assert(sp);
    RID rids[100];
    for (int i = 0; i < 100; ++i) {
        char rec[80];
        int n = snprintf(rec, sizeof(rec), "record-%03d-payload", i);
        assert(SP_Insert(sp, rec, n, &rids[i]) == PFE_OK);
    }
    char tmp[200]; int len;
    for (int i = 0; i < 100; ++i) {
        assert(SP_Get(sp, rids[i], tmp, &len) == PFE_OK);
        char want[80]; int wn = snprintf(want, sizeof(want), "record-%03d-payload", i);
        assert(len == wn && memcmp(tmp, want, wn) == 0);
    }
    printf("SP utilization=%.3f\n", SP_SpaceUtilization(sp));
    SP_Close(sp);

    /* AM B+ tree */
    unlink("/tmp/idb_smoke.am");
    AM_Tree *t = AM_Create("/tmp/idb_smoke.am", PF_REPL_LRU, 16);
    assert(t);
    for (int i = 0; i < 5000; ++i) {
        RID v = { i % 50, i % 100 };
        assert(AM_Insert(t, i * 7, v) == PFE_OK);
    }
    for (int i = 0; i < 5000; ++i) {
        RID v;
        assert(AM_Search(t, i * 7, &v) == PFE_OK);
        assert(v.page == i % 50 && v.slot == i % 100);
    }
    printf("AM height=%d\n", AM_Height(t));
    AM_Close(t);
    puts("SMOKE OK");
    return 0;
}
