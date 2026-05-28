/* static_rec.h --- Fixed-length record store, used for the space-utilization
 * baseline that the assignment asks us to compare against the slotted-page
 * design (Objective 2).  Records are padded out to MAX_LEN; a 1-bit slot
 * directory at the head of every page tells which slots are alive.
 *
 * Author : Daksh Jain  (Roll No. B24bs1114)
 */
#ifndef STATIC_REC_H
#define STATIC_REC_H

#include "pf.h"

typedef struct ST_File ST_File;

ST_File *ST_Create(const char *name, int max_len, PF_ReplPolicy, int pool);
ST_File *ST_Open  (const char *name, int max_len, PF_ReplPolicy, int pool);
int      ST_Close (ST_File *st);

int      ST_Insert(ST_File *st, const void *rec, int len);
int      ST_NumRecords(ST_File *st);
double   ST_SpaceUtilization(ST_File *st);

#endif
