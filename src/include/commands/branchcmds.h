/*-------------------------------------------------------------------------
 * branchcmds.h
 *    Native database branch lifecycle and session state.
 *-------------------------------------------------------------------------
 */
#ifndef BRANCHCMDS_H
#define BRANCHCMDS_H

#include "executor/tuptable.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "storage/lockdefs.h"
#include "utils/relcache.h"
#include "utils/branchcoord.h"

#define BRANCH_ROWID_ATTRIBUTE_NAME "__pg_branch_rowid"
#define BRANCH_LOW_ATTRIBUTE_NAME "__pg_branch_low"
#define BRANCH_HIGH_ATTRIBUTE_NAME "__pg_branch_high"
#define BRANCH_WRITER_ATTRIBUTE_NAME "__pg_branch_writer"
#define BRANCH_DELETED_ATTRIBUTE_NAME "__pg_branch_deleted"
#define BRANCH_HIDDEN_ATTRIBUTE_COUNT 5

extern char *branch_name;
/* Optional per-session allocator hint used by topology-aware clients. */
extern int branch_interval_fanout;
extern Oid MyBranchId;
extern Oid MyBranchSegmentId;
extern BranchCoordinate MyBranchLow;
extern BranchCoordinate MyBranchHigh;
extern BranchCoordinate MyBranchPoint;

extern void BranchEnsureSession(void);
extern bool BranchDatabaseIsEnabled(void);
extern int64 BranchNextRowId(void);
extern void BranchAcquireActivationLock(LOCKMODE lockmode);
extern void BranchAcquireLock(LOCKMODE lockmode);
extern void BranchPrepareExecutorLocks(PlannedStmt *plannedstmt);
extern List *BranchGetActivePoints(void);
extern bool BranchGetAttributeNumbers(Relation relation,
									 AttrNumber attnums[BRANCH_HIDDEN_ATTRIBUTE_COUNT]);
extern bool BranchInitializeTupleDescMetadata(TupleDesc desc, Datum *values,
												   bool *nulls);
extern bool BranchRelationIsVersioned(Relation relation);
extern bool BranchRelationCanModifyInPlace(Relation relation);
extern bool BranchSchemaCopyInProgress(void);
extern void BranchCreateStorageIndexes(Oid relid);
extern bool BranchTupleSlotIsVisible(Relation relation, TupleTableSlot *slot);
extern int64 BranchTupleRowId(Relation relation, TupleTableSlot *slot);
extern void BranchLockRowIdentity(Relation relation, int64 rowid);
extern bool BranchFindVisibleTuple(Relation relation, int64 rowid,
									TupleTableSlot *destslot);
extern bool BranchResolveTupleForUpdate(Relation relation, ItemPointer tid,
									   TupleTableSlot *slot,
									   bool *relocated);
extern bool BranchResolveTupleForLock(Relation relation, ItemPointer tid,
									 TupleTableSlot *slot,
									 LockTupleMode tuple_lockmode,
									 LockWaitPolicy wait_policy,
									 bool *relocated, bool *would_block);
extern void BranchRestoreInsertMetadata(Relation relation,
										TupleTableSlot *slot, int64 rowid);
extern void BranchTruncateRelation(Relation relation);
extern Oid BranchResolveRelationOid(Oid relid);
extern Oid BranchLogicalRelationOid(Oid relid);
extern Datum pg_branch_enable(PG_FUNCTION_ARGS);
extern Datum pg_branch_logical_relation(PG_FUNCTION_ARGS);
extern Datum pg_branch_relation_is_current(PG_FUNCTION_ARGS);
extern void BranchPrepareAlterTable(AlterTableStmt *stmt, LOCKMODE lockmode);
extern PGDLLEXPORT void BranchIndexWorkerMain(Datum main_arg);
extern PGDLLEXPORT void BranchGcWorkerMain(Datum main_arg);
extern void CreateBranch(CreateBranchStmt *stmt);
extern void DropBranch(DropBranchStmt *stmt);

#endif /* BRANCHCMDS_H */
