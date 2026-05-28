/* ============================================================================
 * am.h --- Access Method Layer: B+ Tree Index (Objective 3)
 *
 * Builds an int-key/RID-value B+ tree on top of a PF file. Keys are unique
 * integer roll numbers, values are RIDs into the slotted student store.
 *
 * Three index-construction strategies are exercised by the bench driver:
 *   (a) "fileinsert"  : open existing record file, walk it once, insert into
 *                       a fresh tree (one operation overall).
 *   (b) "incremental" : start with empty record file & empty tree; insert
 *                       both record and key one at a time.
 *   (c) "bulkload"    : sort keys first, then build leaf level + internal
 *                       levels bottom-up with a configurable fill-factor.
 *
 *  Author : Daksh Jain  (Roll No. B24bs1114)
 * ==========================================================================*/
#ifndef AM_H
#define AM_H

#include "pf.h"
#include "slotted.h"

typedef struct AM_Tree AM_Tree;

AM_Tree *AM_Create (const char *name, PF_ReplPolicy, int pool);
AM_Tree *AM_Open   (const char *name, PF_ReplPolicy, int pool);
int      AM_Close  (AM_Tree *t);

int      AM_Insert (AM_Tree *t, int key, RID value);
int      AM_Search (AM_Tree *t, int key, RID *value_out);

/* Bulk-load from sorted (key,value) arrays. Creates a brand-new file.   */
AM_Tree *AM_BulkLoad(const char *name, const int *keys, const RID *vals,
                     int n, double fill_factor,
                     PF_ReplPolicy, int pool);

const PF_Stats *AM_GetStats(AM_Tree *t);
PF_File        *AM_GetPF   (AM_Tree *t);
int             AM_Height  (AM_Tree *t);

#endif
