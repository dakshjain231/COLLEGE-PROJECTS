/* ============================================================================
 * obj2_bench.c --- Objective 2 driver
 *
 * Loads the provided student.txt into both:
 *   * a slotted-page (variable-length) record file  (SP_File)
 *   * several static (fixed-length) record files     (ST_File) with
 *     different MAX_LEN values
 * and reports space utilisation in results/obj2.csv.
 *
 * Also demonstrates Insert / Delete / Sequential-Scan correctness on the
 * slotted store and writes a short demo log to results/obj2.txt.
 *
 *  Author : Daksh Jain  (B24bs1114)
 * ==========================================================================*/
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pf.h"
#include "slotted.h"
#include "static_rec.h"

#define POOL 32


/* helper: read student.txt, calling cb(line, len, ctx) for each record (skip header) */
static void each_student(const char *path,
                         void (*cb)(const char *line, int len, void *ctx),
                         void *ctx)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    char buf[256];
    int line_no = 0;
    while (fgets(buf, sizeof(buf), f)) {
        line_no++;
        if (line_no == 1) continue;            /* skip "Database dummy..." */
        int n = (int)strlen(buf);
        while (n > 0 && (buf[n-1]=='\n' || buf[n-1]=='\r')) buf[--n] = 0;
        if (n == 0) continue;
        cb(buf, n, ctx);
    }
    fclose(f);
}

struct sp_ctx { SP_File *sp; long total_bytes; int count; };
static void sp_cb(const char *line, int len, void *ctx) {
    struct sp_ctx *c = ctx;
    RID r;
    if (SP_Insert(c->sp, line, len, &r) == PFE_OK) {
        c->total_bytes += len;
        c->count++;
    }
}

struct st_ctx { ST_File *st; long total_bytes; int count; };
static void st_cb(const char *line, int len, void *ctx) {
    struct st_ctx *c = ctx;
    if (ST_Insert(c->st, line, len) == PFE_OK) {
        c->total_bytes += len;
        c->count++;
    }
}

static void run_static(FILE *out, int max_len, long student_bytes,
                       int student_count)
{
    char fname[64]; snprintf(fname, sizeof(fname), "/tmp/idb_obj2_st_%d.dat", max_len);
    unlink(fname);
    ST_File *st = ST_Create(fname, max_len, PF_REPL_LRU, POOL);
    if (!st) { fprintf(stderr, "ST_Create failed\n"); return; }
    struct st_ctx c = { st, 0, 0 };
    each_student("data/student.txt", st_cb, &c);

    int npages = PF_NumPages(((PF_File **)st)[0]); /* peek - replaced below */
    (void)npages;
    double util = ST_SpaceUtilization(st);
    /* number of pages: we expose via close path; recompute via PF_GetStats trick */
    /* simpler: since we know real bytes & utilization */
    long total_disk_bytes = (c.count > 0)
        ? (long)((double)c.total_bytes / util)
        : 0;
    fprintf(out, "static,%d,%d,%ld,%.4f,%ld\n",
            max_len, c.count, c.total_bytes, util, total_disk_bytes);
    ST_Close(st);
    unlink(fname);
}

static void run_slotted(FILE *out, FILE *demo)
{
    const char *fname = "/tmp/idb_obj2_sp.dat";
    unlink(fname);
    SP_File *sp = SP_Create(fname, PF_REPL_LRU, POOL);
    struct sp_ctx c = { sp, 0, 0 };
    each_student("data/student.txt", sp_cb, &c);

    double util = SP_SpaceUtilization(sp);
    int npages = PF_NumPages(SP_GetPF(sp));
    fprintf(out, "slotted,VARIABLE,%d,%ld,%.4f,%d\n",
            c.count, c.total_bytes, util, npages * PF_PAGE_SIZE);

    /* ---- Demonstrate Insert / Delete / Sequential Scan correctness --- */
    fprintf(demo, "=== Slotted-page demo ===\n");
    fprintf(demo, "Loaded %d student records into %d pages, util=%.3f\n",
            c.count, npages, util);

    /* Insert one new test record */
    const char *test_rec = "999999;TEST_ROLL;Daksh Jain;M;B24bs1114;DBMS;DEMO;;;;;;BTECH;;;";
    RID test_rid;
    SP_Insert(sp, test_rec, (int)strlen(test_rec), &test_rid);
    fprintf(demo, "Inserted synthetic record at RID(page=%d,slot=%d)\n",
            test_rid.page, test_rid.slot);

    /* Read it back */
    char tmp[256]; int len;
    SP_Get(sp, test_rid, tmp, &len);
    tmp[len] = 0;
    fprintf(demo, "Get returned: \"%s\"\n", tmp);

    /* Delete and verify */
    SP_Delete(sp, test_rid);
    int rc = SP_Get(sp, test_rid, tmp, &len);
    fprintf(demo, "After delete, SP_Get returned: %d (expect %d=PFE_INVALIDPAGE)\n",
            rc, PFE_INVALIDPAGE);

    /* Sequential scan: count and sample first 3 */
    RID rid; int seen = 0;
    for (int e = SP_ScanFirst(sp, &rid, tmp, &len); e == PFE_OK;
             e = SP_ScanNext (sp, &rid, tmp, &len)) {
        if (seen < 3) {
            tmp[len] = 0;
            fprintf(demo, "scan[%d] page=%d slot=%d  \"%.60s%s\"\n",
                    seen, rid.page, rid.slot, tmp,
                    len > 60 ? "..." : "");
        }
        seen++;
    }
    fprintf(demo, "Sequential scan saw %d live records (expect %d)\n",
            seen, c.count);
    SP_Close(sp);
    unlink(fname);
}


int main(void) {
    mkdir("results", 0755);
    FILE *out  = fopen("results/obj2.csv", "w");
    FILE *demo = fopen("results/obj2.txt", "w");
    if (!out || !demo) { perror("results/"); return 1; }
    fprintf(out, "scheme,max_len,records,real_bytes,utilization,disk_bytes\n");

    /* count student records once for baseline */
    long student_bytes = 0; int student_count = 0;
    {
        FILE *fp = fopen("data/student.txt", "r");
        char b[256]; int line=0;
        while (fgets(b, sizeof(b), fp)) {
            line++;
            if (line == 1) continue;
            int n = (int)strlen(b); while (n>0 && (b[n-1]=='\n'||b[n-1]=='\r')) n--;
            if (n>0) { student_bytes += n; student_count++; }
        }
        fclose(fp);
    }
    fprintf(demo, "Student dataset: %d records, %ld bytes total, avg %.1f B/record\n",
            student_count, student_bytes, (double)student_bytes/student_count);

    int lens[] = { 110, 120, 150, 200, 256 };
    for (size_t i = 0; i < sizeof(lens)/sizeof(lens[0]); ++i)
        run_static(out, lens[i], student_bytes, student_count);

    run_slotted(out, demo);

    fclose(out); fclose(demo);
    puts("[obj2] wrote results/obj2.csv and results/obj2.txt");
    return 0;
}
