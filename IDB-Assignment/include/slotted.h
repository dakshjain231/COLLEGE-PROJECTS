/* ============================================================================
 * slotted.h --- Variable-length record manager built on top of PF (Objective 2)
 *
 * Page layout (4096 bytes):
 *
 *   +-----------------------------------+  byte 0
 *   |  PageHdr  : nslots(2)  free_end(2)|
 *   +-----------------------------------+  byte 4
 *   |  slot[0] = (off:2, len:2)         |
 *   |  slot[1] = (off:2, len:2)         |
 *   |   ...   slots grow downward       |
 *   +-----------------------------------+
 *   |          free space               |
 *   +-----------------------------------+  free_end
 *   |          record bytes ...         |
 *   |  records grow upward from EOP     |
 *   +-----------------------------------+  byte 4096
 *
 * A record-id (RID) = (page, slot).  Deleted slots have len = -1 so the
 * slot index space is stable (no shifting of RIDs).
 *
 *  Author : Daksh Jain  (Roll No. B24bs1114)
 * ==========================================================================*/
#ifndef SLOTTED_H
#define SLOTTED_H

#include "pf.h"

typedef struct { int page; int slot; } RID;

#define RID_INVALID ((RID){-1,-1})

typedef struct SP_File SP_File;

SP_File *SP_Create(const char *name, PF_ReplPolicy policy, int pool);
SP_File *SP_Open  (const char *name, PF_ReplPolicy policy, int pool);
int      SP_Close (SP_File *sp);

int  SP_Insert  (SP_File *sp, const void *rec, int len, RID *rid_out);
int  SP_Delete  (SP_File *sp, RID rid);
int  SP_Get     (SP_File *sp, RID rid, void *buf, int *len);

/* sequential scan of all live records */
int  SP_ScanFirst(SP_File *sp, RID *rid, void *buf, int *len);
int  SP_ScanNext (SP_File *sp, RID *rid, void *buf, int *len);

/* introspection */
int             SP_NumRecords (SP_File *sp);
double          SP_SpaceUtilization(SP_File *sp);  /* useful_bytes / page_bytes */
const PF_Stats *SP_GetStats   (SP_File *sp);
PF_File        *SP_GetPF      (SP_File *sp);

#endif
