/* ============================================================================
 * obj1_bench.c --- Objective 1 driver
 *
 * Workload generator that issues a controllable mix of read/write page
 * accesses against a PF file pre-populated with 200 pages.  Page IDs are
 * drawn from a Zipf-ish skewed distribution so the buffer behaviour
 * differs meaningfully between LRU and MRU.
 *
 * For each (policy, read-fraction) combination it prints a CSV row to
 * results/obj1.csv:
 *
 *   policy,read_pct,buf,logical_reads,logical_writes,phys_reads,
 *   phys_writes,buffer_hits,buffer_misses,evictions,dirty_evictions
 *
 *  Author : Daksh Jain  (B24bs1114)
 * ==========================================================================*/
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "pf.h"

#define DATA_PAGES   200
#define NOPS         5000
#define POOL         16


/* simple deterministic skewed page-id generator: pick from [0,DATA_PAGES)
 * with probability falling off linearly. */
static int pick_page(unsigned *seed) {
    /* bias toward small page ids (working set ~ 25% of file) */
    int hot = DATA_PAGES / 4;
    int r = rand_r(seed) % 100;
    if (r < 80) return rand_r(seed) % hot;
    return rand_r(seed) % DATA_PAGES;
}

static void prepopulate(const char *fname) {
    unlink(fname);
    PF_CreateFile(fname);
    PF_File *f = PF_OpenFile(fname, PF_REPL_LRU, 4);
    int p; char *buf;
    for (int i = 0; i < DATA_PAGES; ++i) {
        PF_AllocPage(f, &p, &buf);
        memset(buf, (char)(i & 0xff), PF_PAGE_SIZE);
        PF_MarkDirty(f, p);
        PF_UnpinPage(f, p);
    }
    PF_FlushAll(f);
    PF_CloseFile(f);
}

static void run_mix(FILE *out, PF_ReplPolicy pol, const char *pol_name,
                    int read_pct)
{
    const char *fname = "/tmp/idb_obj1.dat";
    PF_File *f = PF_OpenFile(fname, pol, POOL);
    if (!f) { fprintf(stderr, "open failed\n"); return; }
    PF_ResetStats(f);

    unsigned seed = 0xC0FFEE ^ (read_pct << 8) ^ (pol == PF_REPL_MRU ? 1 : 0);
    for (int i = 0; i < NOPS; ++i) {
        int p = pick_page(&seed);
        char *buf;
        if (PF_GetPage(f, p, &buf) != PFE_OK) continue;
        int is_read = (int)(rand_r(&seed) % 100) < read_pct;
        if (!is_read) {
            buf[(unsigned)rand_r(&seed) % PF_PAGE_SIZE] ^= (char)i;
            PF_MarkDirty(f, p);
        }
        PF_UnpinPage(f, p);
    }
    PF_FlushAll(f);
    const PF_Stats *s = PF_GetStats(f);
    fprintf(out, "%s,%d,%d,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld\n",
            pol_name, read_pct, POOL,
            s->logical_reads, s->logical_writes,
            s->phys_reads, s->phys_writes,
            s->buffer_hits, s->buffer_misses,
            s->evictions, s->dirty_evictions);
    fflush(out);
    PF_CloseFile(f);
}

int main(void) {
    mkdir("results", 0755);
    FILE *out = fopen("results/obj1.csv", "w");
    if (!out) { perror("results/obj1.csv"); return 1; }
    fprintf(out, "policy,read_pct,pool,logical_reads,logical_writes,"
                 "phys_reads,phys_writes,buf_hits,buf_misses,"
                 "evictions,dirty_evictions\n");

    int mixes[] = { 0, 10, 25, 50, 75, 90, 100 };
    for (int m = 0; m < (int)(sizeof(mixes) / sizeof(mixes[0])); ++m) {
        int rp = mixes[m];
        prepopulate("/tmp/idb_obj1.dat");
        run_mix(out, PF_REPL_LRU, "LRU", rp);
        prepopulate("/tmp/idb_obj1.dat");
        run_mix(out, PF_REPL_MRU, "MRU", rp);
    }
    fclose(out);
    puts("[obj1] wrote results/obj1.csv");
    return 0;
}
