/* ============================================================================
 * obj3_bench.c --- Objective 3 driver
 *
 * Compares three strategies for building a B+ tree index on the
 * student.txt roll-no field, then runs the same probe workload against
 * each tree and reports build time, query time, and pages accessed
 * (logical and physical) in results/obj3.csv.
 *
 *   (A) "fileinsert"  : load all students into the SP file first, then
 *                       open a fresh tree and AM_Insert one key at a time
 *                       in arrival order.
 *   (B) "incremental" : do (record-insert, key-insert) interleaved, like
 *                       an OLTP load.
 *   (C) "bulkload"    : sort (key,RID) pairs and call AM_BulkLoad.
 *
 *  Author : Daksh Jain  (B24bs1114)
 * ==========================================================================*/
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "pf.h"
#include "slotted.h"
#include "am.h"

#define POOL 64
#define NPROBE 5000


/* parse roll-no = 1st field of semicolon-separated record */
static int parse_roll(const char *line) {
    return (int)strtol(line, NULL, 10);
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* read all student rows; returns count, fills *lines (heap), *lens */
static int load_lines(char ***out_lines, int **out_lens) {
    FILE *f = fopen("data/student.txt", "r");
    if (!f) { perror("data/student.txt"); exit(1); }
    int cap = 4096, n = 0;
    char **lines = malloc(sizeof(char *) * cap);
    int   *lens  = malloc(sizeof(int) * cap);
    char buf[256]; int line = 0;
    while (fgets(buf, sizeof(buf), f)) {
        line++;
        if (line == 1) continue;
        int len = (int)strlen(buf);
        while (len > 0 && (buf[len-1]=='\n' || buf[len-1]=='\r')) buf[--len] = 0;
        if (!len) continue;
        if (n == cap) { cap *= 2; lines = realloc(lines, sizeof(char *)*cap); lens = realloc(lens, sizeof(int)*cap); }
        lines[n] = malloc(len + 1); memcpy(lines[n], buf, len + 1);
        lens[n] = len;
        n++;
    }
    fclose(f);
    *out_lines = lines; *out_lens = lens;
    return n;
}

typedef struct { int key; RID rid; } KV;
static int cmp_kv(const void *a, const void *b) {
    const KV *x = a, *y = b;
    return (x->key > y->key) - (x->key < y->key);
}

/* generate a list of probe keys (existing keys, randomised order) */
static int *make_probes(const int *all_keys, int n, int nprobe, unsigned seed) {
    int *p = malloc(sizeof(int) * nprobe);
    for (int i = 0; i < nprobe; ++i) {
        int idx = (int)((unsigned)(seed = seed*1103515245u+12345u) % (unsigned)n);
        p[i] = all_keys[idx];
    }
    return p;
}


/* run NPROBE point queries on `idx`, return stats delta + elapsed seconds */
static void probe_index(AM_Tree *idx, const int *probes, int nprobe,
                        FILE *out, const char *tag, int height,
                        double build_secs, long build_phys_reads,
                        long build_phys_writes, long build_log_reads,
                        long build_log_writes)
{
    PF_ResetStats(AM_GetPF(idx));
    double t0 = now_s();
    int hits = 0;
    for (int i = 0; i < nprobe; ++i) {
        RID r;
        if (AM_Search(idx, probes[i], &r) == PFE_OK) hits++;
    }
    double dt = now_s() - t0;
    const PF_Stats *s = PF_GetStats(AM_GetPF(idx));
    fprintf(out, "%s,%d,%d,%.6f,%ld,%ld,%ld,%ld,%.6f,%ld,%ld,%ld,%ld\n",
            tag, height, hits, build_secs,
            build_log_reads, build_log_writes,
            build_phys_reads, build_phys_writes,
            dt,
            s->logical_reads, s->logical_writes,
            s->phys_reads, s->phys_writes);
    fflush(out);
}

int main(void) {
    mkdir("results", 0755);
    FILE *out = fopen("results/obj3.csv", "w");
    if (!out) { perror("results/obj3.csv"); return 1; }
    fprintf(out,
        "method,height,probe_hits,build_secs,"
        "build_log_reads,build_log_writes,build_phys_reads,build_phys_writes,"
        "probe_secs,probe_log_reads,probe_log_writes,probe_phys_reads,probe_phys_writes\n");

    char **lines; int *lens;
    int n = load_lines(&lines, &lens);
    fprintf(stderr, "[obj3] loaded %d students\n", n);

    /* gather all roll numbers (some rows have non-numeric first field?
     * parse_roll returns 0 in that case — drop those) */
    int *keys = malloc(sizeof(int) * n);
    int  m = 0;
    int *idx_in_lines = malloc(sizeof(int) * n);
    for (int i = 0; i < n; ++i) {
        int k = parse_roll(lines[i]);
        if (k > 0) { keys[m] = k; idx_in_lines[m] = i; m++; }
    }
    fprintf(stderr, "[obj3] %d valid roll numbers\n", m);

    /* unify probe set */
    int *probes = make_probes(keys, m, NPROBE, 0xBADC0DE);


    /* ====================================================================
     * (A) "fileinsert" : build SP file fully first, then index from it
     * ================================================================*/
    {
        const char *spname = "/tmp/idb_obj3_a_sp.dat";
        const char *amname = "/tmp/idb_obj3_a_am.dat";
        unlink(spname); unlink(amname);
        SP_File *sp = SP_Create(spname, PF_REPL_LRU, POOL);
        RID *rids = malloc(sizeof(RID) * m);
        for (int i = 0; i < m; ++i)
            SP_Insert(sp, lines[idx_in_lines[i]], lens[idx_in_lines[i]], &rids[i]);
        PF_FlushAll(SP_GetPF(sp));

        AM_Tree *t = AM_Create(amname, PF_REPL_LRU, POOL);
        PF_ResetStats(AM_GetPF(t));
        double t0 = now_s();
        for (int i = 0; i < m; ++i) AM_Insert(t, keys[i], rids[i]);
        PF_FlushAll(AM_GetPF(t));
        double bt = now_s() - t0;
        const PF_Stats *bs = PF_GetStats(AM_GetPF(t));
        long blr = bs->logical_reads, blw = bs->logical_writes;
        long bpr = bs->phys_reads,    bpw = bs->phys_writes;
        int  ht  = AM_Height(t);

        probe_index(t, probes, NPROBE, out, "fileinsert", ht, bt,
                    bpr, bpw, blr, blw);
        AM_Close(t);
        SP_Close(sp);
        free(rids);
    }

    /* ====================================================================
     * (B) "incremental" : insert record + index entry interleaved
     * ================================================================*/
    {
        const char *spname = "/tmp/idb_obj3_b_sp.dat";
        const char *amname = "/tmp/idb_obj3_b_am.dat";
        unlink(spname); unlink(amname);
        SP_File *sp = SP_Create(spname, PF_REPL_LRU, POOL);
        AM_Tree *t  = AM_Create(amname, PF_REPL_LRU, POOL);
        PF_ResetStats(AM_GetPF(t));
        double t0 = now_s();
        for (int i = 0; i < m; ++i) {
            RID r;
            SP_Insert(sp, lines[idx_in_lines[i]], lens[idx_in_lines[i]], &r);
            AM_Insert(t, keys[i], r);
        }
        PF_FlushAll(AM_GetPF(t));
        double bt = now_s() - t0;
        const PF_Stats *bs = PF_GetStats(AM_GetPF(t));
        long blr = bs->logical_reads, blw = bs->logical_writes;
        long bpr = bs->phys_reads,    bpw = bs->phys_writes;
        int  ht  = AM_Height(t);

        probe_index(t, probes, NPROBE, out, "incremental", ht, bt,
                    bpr, bpw, blr, blw);
        AM_Close(t);
        SP_Close(sp);
    }

    /* ====================================================================
     * (C) "bulkload" : sort then bottom-up build
     * ================================================================*/
    {
        const char *spname = "/tmp/idb_obj3_c_sp.dat";
        const char *amname = "/tmp/idb_obj3_c_am.dat";
        unlink(spname); unlink(amname);
        SP_File *sp = SP_Create(spname, PF_REPL_LRU, POOL);
        KV *kv = malloc(sizeof(KV) * m);
        for (int i = 0; i < m; ++i) {
            RID r;
            SP_Insert(sp, lines[idx_in_lines[i]], lens[idx_in_lines[i]], &r);
            kv[i].key = keys[i]; kv[i].rid = r;
        }
        PF_FlushAll(SP_GetPF(sp));

        double t0 = now_s();
        qsort(kv, m, sizeof(KV), cmp_kv);
        int *sk  = malloc(sizeof(int) * m);
        RID *sv  = malloc(sizeof(RID) * m);
        for (int i = 0; i < m; ++i) { sk[i] = kv[i].key; sv[i] = kv[i].rid; }
        AM_Tree *t = AM_BulkLoad(amname, sk, sv, m, /*fill=*/0.7,
                                 PF_REPL_LRU, POOL);
        PF_FlushAll(AM_GetPF(t));
        double bt = now_s() - t0;
        const PF_Stats *bs = PF_GetStats(AM_GetPF(t));
        long blr = bs->logical_reads, blw = bs->logical_writes;
        long bpr = bs->phys_reads,    bpw = bs->phys_writes;
        int  ht  = AM_Height(t);

        probe_index(t, probes, NPROBE, out, "bulkload", ht, bt,
                    bpr, bpw, blr, blw);
        AM_Close(t);
        SP_Close(sp);
        free(kv); free(sk); free(sv);
    }

    fclose(out);
    puts("[obj3] wrote results/obj3.csv");
    return 0;
}
