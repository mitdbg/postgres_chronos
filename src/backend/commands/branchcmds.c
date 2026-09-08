/*-------------------------------------------------------------------------
 *
 * branchcmds.c
 *    Native database branch lifecycle and backend branch selection.
 *
 * Branch metadata is ordinary transactional catalog data.  PostgreSQL's
 * database-object locks serialize forks with writers, so the bolt-on epoch
 * and cross-store barrier protocols are intentionally absent here.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/attmap.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/tupconvert.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_branch.h"
#include "catalog/pg_branch_relversion.h"
#include "catalog/pg_branch_segment.h"
#include "catalog/pg_database.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_publication.h"
#include "commands/branchcmds.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/tablespace.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/plannodes.h"
#include "parser/parse_utilcmd.h"
#include "parser/parser.h"
#include "postmaster/bgworker.h"
#include "storage/lock.h"
#include "storage/lmgr.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/plancache.h"
#include "utils/snapmgr.h"

char	   *branch_name = NULL;
/* -1 means that the caller did not provide a fanout estimate. */
int			branch_interval_fanout = -1;
Oid			MyBranchId = InvalidOid;
Oid			MyBranchSegmentId = InvalidOid;
BranchCoordinate MyBranchLow = {0, 0};
BranchCoordinate MyBranchHigh = {0, 0};
BranchCoordinate MyBranchPoint = {0, 0};

static LocalTransactionId BranchWriteLockLxid = InvalidLocalTransactionId;
static Oid	BranchWriteLockId = InvalidOid;
static LocalTransactionId BranchRelationLockLxid = InvalidLocalTransactionId;
static List *BranchRelationWriteLocks = NIL;
static List *BranchBulkWriteLocks = NIL;

/*
 * Bound one statement's estimated row-lock footprint.  Branch visibility
 * predicates make planner row estimates deliberately conservative, so leave
 * most of the configured per-transaction lock allowance for that error and
 * for ordinary PostgreSQL locks.  The cap also protects installations with a
 * very large max_locks_per_transaction setting from an oversized estimate.
 */
#define BRANCH_ROW_LOCK_ESCALATION_CAP 1024
#define BRANCH_ROW_LOCK_BUDGET_DIVISOR 4

static HeapTuple lookup_branch(const char *name, bool missing_ok);
static void require_database_privilege(AclMode mode);
static void form_segment_tuple(Relation relation, Oid oid, Oid branchid,
							   Oid parentid, const BranchCoordinate *low,
							   const BranchCoordinate *high,
							   const BranchCoordinate *point, int32 depth,
							   char kind);
static BranchCoordinate coordinate_midpoint(const BranchCoordinate *low,
											const BranchCoordinate *high);
static BranchCoordinate choose_child_width(Oid branchid,
										   Form_pg_branch branch,
										   Form_pg_branch_segment head,
										   int32 fanout);
static void cleanup_branch_rows(List *segmentids);
static void cleanup_branch_relversions(Oid branchid);
static List *branch_child_names(Oid parentid);
static void enable_database_branching(void);
static void branch_mark_database_enabled(void);
static void branch_refresh_session_head_locked(void);
static bool branch_prepare_relation_ddl(RangeVar *relation, bool missing_ok,
										bool hold_session_lock,
										bool *copied);
static void branch_set_tuple_metadata(Relation relation, TupleTableSlot *slot,
									  const BranchCoordinate *low,
									  const BranchCoordinate *high,
									  Oid writer, bool deleted);
static Oid branch_relation_for_point(Oid logicalrelid,
									 const BranchCoordinate *point);
static bool branch_schema_version_is_private(Oid logicalrelid,
										 Oid physicalrelid);
static bool branch_physical_version_is_private(Oid logicalrelid,
										   Oid physicalrelid);
static void branch_reset_private_dml_cache(void);
static Oid branch_clone_relation(Oid logicalrelid, Oid sourcerelid,
								 Oid versionid);
static void branch_bulk_copy_relation(Relation source, Relation target);
static bool branch_clone_indexes(Oid sourcerelid, Oid targetrelid,
								 bool secondary_only);
static bool branch_has_secondary_indexes(Oid relid);
static void branch_ensure_indexes_ready(Oid physicalrelid);
static void branch_schedule_index_worker(Oid versionid, Oid ownerid);
static void branch_index_xact_callback(XactEvent event, void *arg);
static void branch_mark_index_state(Oid versionid, char state);
static void branch_schedule_gc_worker(Oid branchid);
static void branch_schedule_pending_gc_workers(void);
static void branch_reclaim(Oid branchid);
static void branch_add_storage_index(Oid relid, const char *first,
									 const char *second);

#define MAIN_BRANCH_OID 9100
#define ROOT_BRANCH_SEGMENT_OID 9101
#define BRANCH_SCHEMA_COPY_BATCH_SIZE 1000
#define BRANCH_DDL_LOCK_SUBID 3
#define BRANCH_CONCURRENT_INDEX_LOCK_SUBID 4

typedef struct BranchPendingIndexWorker
{
	Oid			dbid;
	Oid			ownerid;
	Oid			versionid;
} BranchPendingIndexWorker;

typedef struct BranchPendingGcWorker
{
	Oid			dbid;
	Oid			ownerid;
	Oid			branchid;
} BranchPendingGcWorker;

static List *BranchPendingIndexWorkers = NIL;
static List *BranchPendingGcWorkers = NIL;
static bool BranchIndexXactCallbackRegistered = false;
static Oid BranchGcRecoveryScannedDbid = InvalidOid;
static int BranchSchemaCopyDepth = 0;
static LocalTransactionId BranchPrivateDmlLxid = InvalidLocalTransactionId;
static Oid BranchPrivateDmlBranchId = InvalidOid;
static List *BranchPrivateDmlRelations = NIL;
static List *BranchSharedDmlRelations = NIL;

/*
 * The bolt-on implementation keeps the selected segment and its table map in
 * the client session.  Do the equivalent inside each PostgreSQL backend.  A
 * stable branch must not require catalog scans (or a heavyweight lock-manager
 * round trip) for every statement.
 *
 * Invalidation callbacks only mark this state stale.  Rebuilding outside the
 * callback keeps memory-context work out of the sinval dispatcher.
 */
typedef struct BranchLogicalMapEntry
{
	Oid			physicalrelid;	/* hash key */
	Oid			logicalrelid;
} BranchLogicalMapEntry;

typedef struct BranchPhysicalMapKey
{
	Oid			logicalrelid;
	Oid			segmentid;
} BranchPhysicalMapKey;

typedef struct BranchPhysicalMapEntry
{
	BranchPhysicalMapKey key;	/* hash key */
	Oid			physicalrelid;
} BranchPhysicalMapEntry;

static bool BranchCacheCallbacksRegistered = false;
static bool BranchDatabaseEnabledKnown = false;
static bool BranchDatabaseEnabledValue = false;
static bool BranchSessionCacheValid = false;
static char BranchSessionCachedName[NAMEDATALEN];
static bool BranchRelationMapCacheValid = false;
static MemoryContext BranchRelationMapContext = NULL;
static HTAB *BranchLogicalMapCache = NULL;
static HTAB *BranchPhysicalMapCache = NULL;

static void branch_cache_syscache_callback(Datum arg,
											SysCacheIdentifier cacheid,
											uint32 hashvalue);
static void branch_cache_relcache_callback(Datum arg, Oid relid);
static void branch_register_cache_callbacks(void);
static void branch_ensure_relation_map_cache(void);

static void
branch_cache_syscache_callback(Datum arg, SysCacheIdentifier cacheid,
								uint32 hashvalue)
{
	/* A relversion change cannot alter the selected branch coordinate. */
	if (cacheid != BRANCHRELVEROID)
	{
		BranchDatabaseEnabledKnown = false;
		BranchSessionCacheValid = false;
	}
	BranchRelationMapCacheValid = false;
}

static void
branch_cache_relcache_callback(Datum arg, Oid relid)
{
	BranchRelationMapCacheValid = false;
	branch_reset_private_dml_cache();
}

static void
branch_register_cache_callbacks(void)
{
	if (BranchCacheCallbacksRegistered)
		return;

	CacheRegisterSyscacheCallback(BRANCHOID,
								 branch_cache_syscache_callback, (Datum) 0);
	CacheRegisterSyscacheCallback(BRANCHNAME,
								 branch_cache_syscache_callback, (Datum) 0);
	CacheRegisterSyscacheCallback(BRANCHSEGOID,
								 branch_cache_syscache_callback, (Datum) 0);
	CacheRegisterSyscacheCallback(BRANCHRELVEROID,
								 branch_cache_syscache_callback, (Datum) 0);
	CacheRegisterRelcacheCallback(branch_cache_relcache_callback, (Datum) 0);
	BranchCacheCallbacksRegistered = true;
}

static void
branch_ensure_relation_map_cache(void)
{
	HASHCTL		ctl;

	branch_register_cache_callbacks();
	if (BranchRelationMapCacheValid)
		return;

	if (BranchRelationMapContext == NULL)
		BranchRelationMapContext =
			AllocSetContextCreate(CacheMemoryContext,
								  "Branch relation map cache",
								  ALLOCSET_SMALL_SIZES);
	else
		MemoryContextReset(BranchRelationMapContext);

	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(BranchLogicalMapEntry);
	ctl.hcxt = BranchRelationMapContext;
	BranchLogicalMapCache = hash_create("branch physical-to-logical map", 32,
										&ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(BranchPhysicalMapKey);
	ctl.entrysize = sizeof(BranchPhysicalMapEntry);
	ctl.hcxt = BranchRelationMapContext;
	BranchPhysicalMapCache = hash_create("branch logical-to-physical map", 32,
										 &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	BranchRelationMapCacheValid = true;
}

bool
BranchSchemaCopyInProgress(void)
{
	return BranchSchemaCopyDepth > 0;
}

bool
BranchDatabaseIsEnabled(void)
{
	HeapTuple	tuple;
	bool		enabled;

	if (!IsTransactionState() || !OidIsValid(MyDatabaseId))
		return false;
	branch_register_cache_callbacks();
	if (BranchDatabaseEnabledKnown)
		return BranchDatabaseEnabledValue;
	tuple = SearchSysCache1(BRANCHOID, ObjectIdGetDatum(MAIN_BRANCH_OID));
	if (!HeapTupleIsValid(tuple))
		return false;
	enabled = ((Form_pg_branch) GETSTRUCT(tuple))->brhead !=
		ROOT_BRANCH_SEGMENT_OID;
	ReleaseSysCache(tuple);
	BranchDatabaseEnabledValue = enabled;
	BranchDatabaseEnabledKnown = true;
	return enabled;
}

static bool
branch_point_in_interval(const BranchCoordinate *point,
						 const BranchCoordinate *low,
						 const BranchCoordinate *high)
{
	return branchcoord_cmp_internal(low, point) <= 0 &&
		branchcoord_cmp_internal(point, high) < 0;
}

/*
 * Return the newest physical schema version whose binding contains point.
 * Bindings intentionally overlap after nested DDL; the newest binding is the
 * copy-on-write override, while the older one remains the inherited fallback.
 */
static Oid
branch_relation_for_point(Oid logicalrelid, const BranchCoordinate *point)
{
	Relation	relation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;
	Oid			bestversion = InvalidOid;
	Oid			physicalrelid = logicalrelid;
	BranchPhysicalMapKey cachekey;
	BranchPhysicalMapEntry *cacheentry;
	bool		found;
	bool		cacheable;

	cacheable = OidIsValid(MyBranchSegmentId) &&
		branchcoord_cmp_internal(point, &MyBranchPoint) == 0;
	if (cacheable)
	{
		branch_ensure_relation_map_cache();
		cachekey.logicalrelid = logicalrelid;
		cachekey.segmentid = MyBranchSegmentId;
		cacheentry = hash_search(BranchPhysicalMapCache, &cachekey,
								 HASH_FIND, &found);
		if (found)
			return cacheentry->physicalrelid;
	}

	relation = table_open(BranchRelVersionRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_branch_relversion_brvlogical,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(logicalrelid));
	scan = systable_beginscan(relation, BranchRelVersionLogicalIndexId, true,
							 NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_branch_relversion version =
			(Form_pg_branch_relversion) GETSTRUCT(tuple);

		if (version->brvlogical == logicalrelid &&
			branch_point_in_interval(point, &version->brvlow,
									 &version->brvhigh) &&
			(!OidIsValid(bestversion) || version->oid > bestversion))
		{
			bestversion = version->oid;
			physicalrelid = version->brvphysical;
		}
	}
	systable_endscan(scan);
	table_close(relation, AccessShareLock);
	if (cacheable)
	{
		cacheentry = hash_search(BranchPhysicalMapCache, &cachekey,
								 HASH_ENTER, &found);
		cacheentry->physicalrelid = physicalrelid;
	}
	return physicalrelid;
}

/* Translate a physical schema-version OID back to its stable logical OID. */
Oid
BranchLogicalRelationOid(Oid relid)
{
	Relation	relation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;
	Oid			logicalrelid = relid;
	BranchLogicalMapEntry *cacheentry;
	bool		found;

	if (!OidIsValid(relid) || IsBootstrapProcessingMode() ||
		!IsTransactionState() || !BranchDatabaseIsEnabled())
		return relid;

	branch_ensure_relation_map_cache();
	cacheentry = hash_search(BranchLogicalMapCache, &relid, HASH_FIND, &found);
	if (found)
		return cacheentry->logicalrelid;

	relation = table_open(BranchRelVersionRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_branch_relversion_brvphysical,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(relation, BranchRelVersionPhysicalIndexId, true,
							 NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_branch_relversion version =
			(Form_pg_branch_relversion) GETSTRUCT(tuple);

		if (version->brvphysical == relid)
		{
			logicalrelid = version->brvlogical;
			break;
		}
	}
	systable_endscan(scan);
	table_close(relation, AccessShareLock);
	cacheentry = hash_search(BranchLogicalMapCache, &relid, HASH_ENTER, &found);
	cacheentry->logicalrelid = logicalrelid;
	return logicalrelid;
}

/* Called by RangeVarGetRelidExtended after ordinary namespace lookup. */
Oid
BranchResolveRelationOid(Oid relid)
{
	Oid			logicalrelid;

	if (!OidIsValid(relid) || IsBootstrapProcessingMode() ||
		!IsTransactionState() || relid < FirstNormalObjectId ||
		!BranchDatabaseIsEnabled())
		return relid;

	/* An explicitly named internal physical version is already resolved. */
	logicalrelid = BranchLogicalRelationOid(relid);
	if (logicalrelid != relid)
		return relid;

	BranchEnsureSession();
	return branch_relation_for_point(logicalrelid, &MyBranchPoint);
}

Datum
pg_branch_logical_relation(PG_FUNCTION_ARGS)
{
	PG_RETURN_OID(BranchLogicalRelationOid(PG_GETARG_OID(0)));
}

Datum
pg_branch_relation_is_current(PG_FUNCTION_ARGS)
{
	Oid			physicalrelid = PG_GETARG_OID(0);
	Oid			logicalrelid = BranchLogicalRelationOid(physicalrelid);

	if (!BranchDatabaseIsEnabled())
		PG_RETURN_BOOL(true);
	BranchEnsureSession();
	PG_RETURN_BOOL(branch_relation_for_point(logicalrelid, &MyBranchPoint) ==
					 physicalrelid);
}

static bool
branch_schema_version_is_private(Oid logicalrelid, Oid physicalrelid)
{
	Relation	relation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;
	bool		is_private = false;

	/*
	 * Exact ownership is stronger than counting active readers.  A fork moves
	 * the parent's head to a strict subinterval permanently, even if the child
	 * is later dropped.  Matching the owner and both recorded bounds therefore
	 * prevents a formerly shared heap containing not-yet-reclaimed child rows
	 * from becoming eligible for the private fast path again.
	 */
	relation = table_open(BranchRelVersionRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_branch_relversion_brvphysical,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(physicalrelid));
	scan = systable_beginscan(relation, BranchRelVersionPhysicalIndexId, true,
							 NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_branch_relversion version =
			(Form_pg_branch_relversion) GETSTRUCT(tuple);

		if (version->brvlogical == logicalrelid &&
			version->brvphysical == physicalrelid &&
			version->brvbranch == MyBranchId &&
			branchcoord_cmp_internal(&version->brvlow, &MyBranchLow) == 0 &&
			branchcoord_cmp_internal(&version->brvhigh, &MyBranchHigh) == 0)
		{
			is_private = true;
			break;
		}
	}
	systable_endscan(scan);
	table_close(relation, AccessShareLock);
	return is_private;
}

/* The caller must hold a branch lock that prevents a concurrent fork. */
static bool
branch_physical_version_is_private(Oid logicalrelid, Oid physicalrelid)
{
	if (logicalrelid == physicalrelid)
	{
		if (MyBranchLow.hi == 0 && MyBranchLow.lo == 0 &&
			MyBranchHigh.hi == PG_UINT64_MAX &&
			MyBranchHigh.lo == PG_UINT64_MAX)
			return true;

		return branch_schema_version_is_private(logicalrelid, physicalrelid);
	}

	return branch_relation_for_point(logicalrelid,
									 &MyBranchPoint) == physicalrelid &&
		branch_schema_version_is_private(logicalrelid, physicalrelid);
}

static void
branch_reset_private_dml_cache(void)
{
	BranchPrivateDmlLxid = InvalidLocalTransactionId;
	BranchPrivateDmlBranchId = InvalidOid;
	BranchPrivateDmlRelations = NIL;
	BranchSharedDmlRelations = NIL;
}

static bool
branch_relation_is_private_cached(Relation relation)
{
	Oid			physicalrelid = RelationGetRelid(relation);
	Oid			logicalrelid;
	bool		is_private;
	MemoryContext oldcontext;

	if (BranchPrivateDmlLxid != MyProc->vxid.lxid ||
		BranchPrivateDmlBranchId != MyBranchId)
	{
		BranchPrivateDmlLxid = MyProc->vxid.lxid;
		BranchPrivateDmlBranchId = MyBranchId;
		BranchPrivateDmlRelations = NIL;
		BranchSharedDmlRelations = NIL;
	}
	if (list_member_oid(BranchPrivateDmlRelations, physicalrelid))
		return true;
	if (list_member_oid(BranchSharedDmlRelations, physicalrelid))
		return false;

	logicalrelid = BranchLogicalRelationOid(physicalrelid);
	is_private = branch_physical_version_is_private(logicalrelid,
												 physicalrelid);

	oldcontext = MemoryContextSwitchTo(TopTransactionContext);
	if (is_private)
		BranchPrivateDmlRelations =
			lappend_oid(BranchPrivateDmlRelations, physicalrelid);
	else
		BranchSharedDmlRelations =
			lappend_oid(BranchSharedDmlRelations, physicalrelid);
	MemoryContextSwitchTo(oldcontext);
	return is_private;
}

/*
 * A copied physical schema version can use PostgreSQL's ordinary in-place
 * UPDATE/DELETE machinery while exactly one active branch resolves to it.
 * ExecutorStart takes RowExclusiveLock on that branch before rows are visited.
 * CREATE BRANCH takes ShareRowExclusiveLock, so the answer cannot become stale
 * until this transaction ends.
 */
bool
BranchRelationCanModifyInPlace(Relation relation)
{
	if (!BranchRelationIsVersioned(relation))
		return false;
	BranchAcquireLock(RowExclusiveLock);
	return branch_relation_is_private_cached(relation);
}

/*
 * A read needs no branch lock while its snapshot is active.  If a concurrent
 * fork commits after the private check, that snapshot cannot see child writes.
 * The fork's relcache invalidation discards both cached plans and this cache
 * before a later command can use a newer snapshot.
 */
bool
BranchRelationCanReadInPlace(Relation relation)
{
	if (!BranchRelationIsVersioned(relation))
		return false;
	BranchEnsureSession();
	return branch_relation_is_private_cached(relation);
}

static bool
branch_index_is_internal(Relation heaprel, Relation indexrel)
{
	Form_pg_index index = indexrel->rd_index;

	for (int i = 0; i < index->indnatts; i++)
	{
		AttrNumber	attnum = index->indkey.values[i];

		if (attnum > 0 &&
			TupleDescAttr(RelationGetDescr(heaprel), attnum - 1)->attishidden)
			return true;
	}
	return false;
}

/*
 * Clone either required indexes (unique/exclusion) or optional secondary
 * indexes.  Required indexes are built before the schema binding is exposed;
 * secondary indexes are built by BranchIndexWorkerMain after commit.
 */
static bool
branch_clone_indexes(Oid sourcerelid, Oid targetrelid, bool secondary_only)
{
	Relation	sourcerel;
	Relation	targetrel;
	AttrMap    *attmap;
	List	   *indexes;
	ListCell   *lc;
	bool		found = false;

	sourcerel = table_open(sourcerelid, AccessShareLock);
	targetrel = table_open(targetrelid, ShareLock);
	attmap = build_attrmap_by_name(RelationGetDescr(targetrel),
								RelationGetDescr(sourcerel), true);
	indexes = RelationGetIndexList(sourcerel);

	foreach(lc, indexes)
	{
		Relation	indexrel = index_open(lfirst_oid(lc), AccessShareLock);
		Form_pg_index index = indexrel->rd_index;
		bool		required;

		if (!index->indisvalid || !index->indisready ||
			branch_index_is_internal(sourcerel, indexrel))
		{
			index_close(indexrel, AccessShareLock);
			continue;
		}

		required = index->indisunique || index->indisprimary ||
			index->indisexclusion;
		if (secondary_only != !required)
		{
			index_close(indexrel, AccessShareLock);
			continue;
		}

		found = true;
		{
			IndexStmt  *stmt;

			stmt = generateClonedIndexStmt(NULL, indexrel, attmap, NULL);
			stmt->idxname = NULL;
			DefineIndex(NULL, targetrelid, stmt,
						InvalidOid, InvalidOid, InvalidOid, -1,
						true, true, false, false, false);
			CommandCounterIncrement();
		}
		index_close(indexrel, AccessShareLock);
	}

	list_free(indexes);
	free_attrmap(attmap);
	table_close(targetrel, ShareLock);
	table_close(sourcerel, AccessShareLock);
	return found;
}

static void
branch_mark_index_state(Oid versionid, char state)
{
	Relation	relation;
	HeapTuple	tuple;
	Form_pg_branch_relversion version;

	relation = table_open(BranchRelVersionRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(BRANCHRELVEROID,
								ObjectIdGetDatum(versionid));
	if (!HeapTupleIsValid(tuple))
	{
		table_close(relation, RowExclusiveLock);
		return;
	}
	version = (Form_pg_branch_relversion) GETSTRUCT(tuple);
	version->brvindexstate = state;
	CatalogTupleUpdate(relation, &tuple->t_self, tuple);
	heap_freetuple(tuple);
	table_close(relation, RowExclusiveLock);
}

void
BranchIndexWorkerMain(Datum main_arg)
{
	BranchPendingIndexWorker request;
	HeapTuple	tuple;
	Form_pg_branch_relversion version;
	Oid			source;
	Oid			target;

	memcpy(&request, MyBgworkerEntry->bgw_extra, sizeof(request));
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(request.dbid, request.ownerid,
										  BGWORKER_BYPASS_ROLELOGINCHECK);
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());

	tuple = SearchSysCache1(BRANCHRELVEROID,
							ObjectIdGetDatum(request.versionid));
	if (!HeapTupleIsValid(tuple))
	{
		PopActiveSnapshot();
		CommitTransactionCommand();
		proc_exit(0);
	}
	version = (Form_pg_branch_relversion) GETSTRUCT(tuple);
	if (version->brvindexstate != BRANCH_INDEX_STATE_PENDING)
	{
		ReleaseSysCache(tuple);
		PopActiveSnapshot();
		CommitTransactionCommand();
		proc_exit(0);
	}
	source = version->brvsource;
	target = version->brvphysical;
	ReleaseSysCache(tuple);

	/* The relation lock also serializes a following branch-local ALTER. */
	LockRelationOid(target, ShareLock);
	tuple = SearchSysCache1(BRANCHRELVEROID,
							ObjectIdGetDatum(request.versionid));
	if (!HeapTupleIsValid(tuple) ||
		((Form_pg_branch_relversion) GETSTRUCT(tuple))->brvindexstate !=
		BRANCH_INDEX_STATE_PENDING)
	{
		if (HeapTupleIsValid(tuple))
			ReleaseSysCache(tuple);
		PopActiveSnapshot();
		CommitTransactionCommand();
		proc_exit(0);
	}
	ReleaseSysCache(tuple);
	branch_clone_indexes(source, target, true);
	branch_mark_index_state(request.versionid, BRANCH_INDEX_STATE_READY);
	PopActiveSnapshot();
	CommitTransactionCommand();
	proc_exit(0);
}

static void
branch_launch_index_worker(BranchPendingIndexWorker *request)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;

	MemSet(&worker, 0, sizeof(worker));
	snprintf(worker.bgw_name, BGW_MAXLEN,
			 "branch index builder for schema version %u", request->versionid);
	snprintf(worker.bgw_type, BGW_MAXLEN, "branch index builder");
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	strlcpy(worker.bgw_library_name, "postgres", MAXPGPATH);
	strlcpy(worker.bgw_function_name, "BranchIndexWorkerMain", BGW_MAXLEN);
	worker.bgw_main_arg = ObjectIdGetDatum(request->versionid);
	memcpy(worker.bgw_extra, request, sizeof(*request));
	worker.bgw_notify_pid = 0;

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		ereport(WARNING,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("could not start background index builder for branch schema version %u",
						request->versionid),
				 errhint("Increase max_worker_processes; the schema version remains usable with sequential scans.")));
}

static void
branch_launch_gc_worker(BranchPendingGcWorker *request)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;

	MemSet(&worker, 0, sizeof(worker));
	snprintf(worker.bgw_name, BGW_MAXLEN,
			 "branch reclaimer for branch %u", request->branchid);
	snprintf(worker.bgw_type, BGW_MAXLEN, "branch reclaimer");
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	strlcpy(worker.bgw_library_name, "postgres", MAXPGPATH);
	strlcpy(worker.bgw_function_name, "BranchGcWorkerMain", BGW_MAXLEN);
	worker.bgw_main_arg = ObjectIdGetDatum(request->branchid);
	memcpy(worker.bgw_extra, request, sizeof(*request));
	worker.bgw_notify_pid = 0;

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		ereport(WARNING,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("could not start background reclaimer for branch %u",
						request->branchid),
				 errhint("Increase max_worker_processes; a later database session will retry the durable reclamation job.")));
}

static void
branch_index_xact_callback(XactEvent event, void *arg)
{
	ListCell   *lc;

	if (event == XACT_EVENT_COMMIT)
	{
		foreach(lc, BranchPendingIndexWorkers)
			branch_launch_index_worker(lfirst(lc));
		foreach(lc, BranchPendingGcWorkers)
			branch_launch_gc_worker(lfirst(lc));
	}

	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT ||
		event == XACT_EVENT_PREPARE)
	{
		list_free_deep(BranchPendingIndexWorkers);
		BranchPendingIndexWorkers = NIL;
		list_free_deep(BranchPendingGcWorkers);
		BranchPendingGcWorkers = NIL;
	}
}

static void
branch_schedule_index_worker(Oid versionid, Oid ownerid)
{
	MemoryContext oldcontext;
	BranchPendingIndexWorker *request;

	if (!BranchIndexXactCallbackRegistered)
	{
		RegisterXactCallback(branch_index_xact_callback, NULL);
		BranchIndexXactCallbackRegistered = true;
	}

	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	request = palloc_object(BranchPendingIndexWorker);
	request->dbid = MyDatabaseId;
	request->ownerid = ownerid;
	request->versionid = versionid;
	BranchPendingIndexWorkers = lappend(BranchPendingIndexWorkers, request);
	MemoryContextSwitchTo(oldcontext);
}

static void
branch_schedule_gc_worker(Oid branchid)
{
	MemoryContext oldcontext;
	BranchPendingGcWorker *request;
	ListCell   *lc;
	HeapTuple	dbtup;

	foreach(lc, BranchPendingGcWorkers)
	{
		BranchPendingGcWorker *pending = lfirst(lc);

		if (pending->dbid == MyDatabaseId && pending->branchid == branchid)
			return;
	}
	if (!BranchIndexXactCallbackRegistered)
	{
		RegisterXactCallback(branch_index_xact_callback, NULL);
		BranchIndexXactCallbackRegistered = true;
	}

	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	request = palloc_object(BranchPendingGcWorker);
	request->dbid = MyDatabaseId;
	dbtup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	if (!HeapTupleIsValid(dbtup))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	request->ownerid = ((Form_pg_database) GETSTRUCT(dbtup))->datdba;
	ReleaseSysCache(dbtup);
	request->branchid = branchid;
	BranchPendingGcWorkers = lappend(BranchPendingGcWorkers, request);
	MemoryContextSwitchTo(oldcontext);
}

static bool
branch_has_secondary_indexes(Oid relid)
{
	Relation	heaprel = table_open(relid, AccessShareLock);
	List	   *indexes = RelationGetIndexList(heaprel);
	ListCell   *lc;
	bool		found = false;

	foreach(lc, indexes)
	{
		Relation	indexrel = index_open(lfirst_oid(lc), AccessShareLock);
		Form_pg_index index = indexrel->rd_index;

		if (index->indisvalid && index->indisready &&
			!index->indisunique && !index->indisprimary &&
			!index->indisexclusion &&
			!branch_index_is_internal(heaprel, indexrel))
			found = true;
		index_close(indexrel, AccessShareLock);
		if (found)
			break;
	}
	list_free(indexes);
	table_close(heaprel, AccessShareLock);
	return found;
}

/* Finish a pending predecessor before using it as the source of another copy. */
static void
branch_ensure_indexes_ready(Oid physicalrelid)
{
	Relation	relation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;
	Oid			versionid = InvalidOid;
	Oid			source = InvalidOid;

	relation = table_open(BranchRelVersionRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_branch_relversion_brvphysical,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(physicalrelid));
	scan = systable_beginscan(relation, BranchRelVersionPhysicalIndexId, true,
							 NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_branch_relversion version =
			(Form_pg_branch_relversion) GETSTRUCT(tuple);

		if (version->brvphysical == physicalrelid &&
			version->brvindexstate == BRANCH_INDEX_STATE_PENDING)
		{
			versionid = version->oid;
			source = version->brvsource;
			break;
		}
	}
	systable_endscan(scan);
	table_close(relation, AccessShareLock);

	if (OidIsValid(versionid))
	{
		branch_clone_indexes(source, physicalrelid, true);
		branch_mark_index_state(versionid, BRANCH_INDEX_STATE_READY);
		CommandCounterIncrement();
	}
}

static void
branch_execute_spi(const char *sql)
{
	int			status;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed during branch schema copy");
	status = SPI_execute(sql, false, 0);
	if (status < 0)
		elog(ERROR, "branch schema copy command failed: %s", sql);
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed during branch schema copy");
	CommandCounterIncrement();
}

/*
 * Copy visible source tuples into a new, index-free physical heap.  Batching
 * through the table AM avoids per-row executor and SPI overhead, while the
 * freshly-created relation can skip the free-space map.  Source and target
 * have identical descriptors at this point; the user's ALTER runs only after
 * the populated version has been published to the utility command.
 */
static void
branch_bulk_copy_relation(Relation source, Relation target)
{
	AttrMap    *attmap;
	Snapshot	snapshot;
	TableScanDesc scan;
	TupleTableSlot *source_slot;
	TupleTableSlot **target_slots;
	BulkInsertState bistate;
	MemoryContext batch_context;
	MemoryContext oldcontext;
	CommandId	command_id = GetCurrentCommandId(true);
	int			nused = 0;

	/*
	 * Hidden columns are appended when each physical table is created.  Once
	 * a prior schema version has added user columns, those columns precede the
	 * hidden columns in the next copy, so equal natts does not imply equal
	 * physical attribute order.  Map every generation by name before forming
	 * target tuples.
	 */
	attmap = build_attrmap_by_name(RelationGetDescr(source),
								RelationGetDescr(target), false);
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	scan = table_beginscan(source, snapshot, 0, NULL, SO_NONE);
	source_slot = table_slot_create(source, NULL);
	target_slots = palloc0_array(TupleTableSlot *,
								BRANCH_SCHEMA_COPY_BATCH_SIZE);
	for (int i = 0; i < BRANCH_SCHEMA_COPY_BATCH_SIZE; i++)
		target_slots[i] = table_slot_create(target, NULL);
	bistate = GetBulkInsertState();
	batch_context = AllocSetContextCreate(CurrentMemoryContext,
									  "branch schema copy batch",
									  ALLOCSET_DEFAULT_SIZES);

	while (table_scan_getnextslot(scan, ForwardScanDirection, source_slot))
	{
		if (!BranchTupleSlotIsVisible(source, source_slot))
		{
			ExecClearTuple(source_slot);
			continue;
		}

		oldcontext = MemoryContextSwitchTo(batch_context);
		execute_attr_map_slot(attmap, source_slot, target_slots[nused]);
		branch_set_tuple_metadata(target, target_slots[nused],
								  &MyBranchLow, &MyBranchHigh,
								  MyBranchSegmentId, false);
		MemoryContextSwitchTo(oldcontext);
		nused++;
		ExecClearTuple(source_slot);

		if (nused == BRANCH_SCHEMA_COPY_BATCH_SIZE)
		{
			table_multi_insert(target, target_slots, nused, command_id,
							   TABLE_INSERT_SKIP_FSM, bistate);
			for (int i = 0; i < nused; i++)
				ExecClearTuple(target_slots[i]);
			MemoryContextReset(batch_context);
			nused = 0;
			CHECK_FOR_INTERRUPTS();
		}
	}
	if (nused > 0)
	{
		table_multi_insert(target, target_slots, nused, command_id,
						   TABLE_INSERT_SKIP_FSM, bistate);
		for (int i = 0; i < nused; i++)
			ExecClearTuple(target_slots[i]);
	}

	table_finish_bulk_insert(target, TABLE_INSERT_SKIP_FSM);
	FreeBulkInsertState(bistate);
	for (int i = 0; i < BRANCH_SCHEMA_COPY_BATCH_SIZE; i++)
		ExecDropSingleTupleTableSlot(target_slots[i]);
	pfree(target_slots);
	ExecDropSingleTupleTableSlot(source_slot);
	table_endscan(scan);
	UnregisterSnapshot(snapshot);
	MemoryContextDelete(batch_context);
	free_attrmap(attmap);
}

/* Create an index-free PostgreSQL heap with the source's logical row shape. */
static Oid
branch_clone_relation(Oid logicalrelid, Oid sourcerelid, Oid versionid)
{
	Relation	source;
	Relation	target;
	char	   *namespace_name;
	char	   *source_name;
	char		physical_name[NAMEDATALEN];
	char	   *qualified_source;
	char	   *qualified_target;
	StringInfoData sql;
	Oid			namespaceid;
	Oid			targetrelid = InvalidOid;

	source = table_open(sourcerelid, NoLock);
	if (source->rd_rel->relkind != RELKIND_RELATION ||
		source->rd_rel->relispartition)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("branch-local schema changes currently require an ordinary table"),
				 errdetail("Relation \"%s\" is partitioned or is not a heap table.",
						   RelationGetRelationName(source))));

	namespaceid = RelationGetNamespace(source);
	namespace_name = get_namespace_name(namespaceid);
	source_name = pstrdup(RelationGetRelationName(source));
	snprintf(physical_name, sizeof(physical_name),
			 "__pg_branch_v_%u_%u", logicalrelid, versionid);
	qualified_source = quote_qualified_identifier(namespace_name, source_name);
	qualified_target = quote_qualified_identifier(namespace_name, physical_name);

	BranchSchemaCopyDepth++;
	PG_TRY();
	{
		initStringInfo(&sql);
		appendStringInfo(&sql,
						 "CREATE TABLE %s (LIKE %s INCLUDING DEFAULTS INCLUDING GENERATED "
						 "INCLUDING IDENTITY INCLUDING CONSTRAINTS INCLUDING STORAGE "
						 "INCLUDING COMPRESSION) WITH (fillfactor=50)",
						 qualified_target, qualified_source);
		branch_execute_spi(sql.data);
		targetrelid = get_relname_relid(physical_name, namespaceid);
		if (!OidIsValid(targetrelid))
			elog(ERROR, "could not find physical branch relation %s", physical_name);
		target = table_open(targetrelid, NoLock);
		branch_bulk_copy_relation(source, target);
		table_close(target, NoLock);
	}
	PG_FINALLY();
	{
		BranchSchemaCopyDepth--;
	}
	PG_END_TRY();

	table_close(source, NoLock);
	return targetrelid;
}

/*
 * Fork a shared schema version before relation-local DDL.  A private version
 * follows PostgreSQL's regular DDL path directly, preserving instant DDL.
 *
 * CREATE INDEX CONCURRENTLY commits internally.  Its caller requests a
 * session-level database DDL gate so a fork cannot make the target physical
 * version shared between the index's build and validation transactions.
 */
static bool
branch_prepare_relation_ddl(RangeVar *relation, bool missing_ok,
							bool hold_session_lock, bool *copied)
{
	Relation	versionrel;
	Relation	sourcerel;
	Oid			sourcerelid;
	Oid			logicalrelid;
	Oid			targetrelid;
	Oid			versionid;
	Oid			ownerid;
	bool		has_secondary;
	bool		session_lock_held = false;
	Datum		values[Natts_pg_branch_relversion];
	bool		nulls[Natts_pg_branch_relversion];
	HeapTuple	tuple;

	if (BranchSchemaCopyDepth > 0 || IsBootstrapProcessingMode() ||
		!IsTransactionState() ||
		!BranchDatabaseIsEnabled())
		return false;

	/* The predecessor only needs to remain schema-stable while inspected. */
	sourcerelid = RangeVarGetRelid(relation, AccessShareLock, missing_ok);
	if (!OidIsValid(sourcerelid))
		return false;
	logicalrelid = BranchLogicalRelationOid(sourcerelid);
	sourcerel = relation_open(sourcerelid, NoLock);
	if ((sourcerel->rd_rel->relkind != RELKIND_RELATION &&
		 sourcerel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE) ||
		!BranchRelationIsVersioned(sourcerel))
	{
		relation_close(sourcerel, NoLock);
		return false;
	}
	ownerid = sourcerel->rd_rel->relowner;
	if (!object_ownercheck(RelationRelationId, sourcerelid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE,
					   RelationGetRelationName(sourcerel));
	relation_close(sourcerel, NoLock);

	/*
	 * Serialize DDL upgrades of this logical relation independently of other
	 * tables.  The writer-compatible branch lock prevents a fork while private
	 * DDL runs.  Only a real schema copy upgrades the branch lock and waits for
	 * writers.
	 */
	BranchEnsureSession();
	LockDatabaseObject(BranchRelVersionRelationId, logicalrelid,
					   BRANCH_DDL_LOCK_SUBID, ExclusiveLock);
	BranchAcquireLock(RowExclusiveLock);
	branch_ensure_indexes_ready(sourcerelid);
	if (branch_physical_version_is_private(logicalrelid, sourcerelid))
	{
		if (hold_session_lock)
		{
			LOCKTAG		tag;

			SET_LOCKTAG_OBJECT(tag, MyDatabaseId, BranchRelationId, InvalidOid,
							   BRANCH_CONCURRENT_INDEX_LOCK_SUBID);
			(void) LockAcquire(&tag, RowExclusiveLock, true, false);
			session_lock_held = true;
		}
		return session_lock_held;
	}
	BranchAcquireLock(ShareRowExclusiveLock);

	versionrel = table_open(BranchRelVersionRelationId, RowExclusiveLock);
	versionid = GetNewOidWithIndex(versionrel, BranchRelVersionOidIndexId,
								   Anum_pg_branch_relversion_oid);
	targetrelid = branch_clone_relation(logicalrelid, sourcerelid, versionid);

	/* These two indexes are part of the physical version's storage contract. */
	branch_add_storage_index(targetrelid, BRANCH_ROWID_ATTRIBUTE_NAME,
							 BRANCH_LOW_ATTRIBUTE_NAME);
	branch_add_storage_index(targetrelid, BRANCH_WRITER_ATTRIBUTE_NAME,
							 BRANCH_ROWID_ATTRIBUTE_NAME);

	/* Constraint-backed indexes cannot be delayed without allowing bad writes. */
	branch_clone_indexes(sourcerelid, targetrelid, false);
	has_secondary = branch_has_secondary_indexes(sourcerelid);

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, false, sizeof(nulls));
	values[Anum_pg_branch_relversion_oid - 1] = ObjectIdGetDatum(versionid);
	values[Anum_pg_branch_relversion_brvlogical - 1] =
		ObjectIdGetDatum(logicalrelid);
	values[Anum_pg_branch_relversion_brvphysical - 1] =
		ObjectIdGetDatum(targetrelid);
	values[Anum_pg_branch_relversion_brvsource - 1] =
		ObjectIdGetDatum(sourcerelid);
	values[Anum_pg_branch_relversion_brvbranch - 1] =
		ObjectIdGetDatum(MyBranchId);
	values[Anum_pg_branch_relversion_brvowner - 1] =
		ObjectIdGetDatum(ownerid);
	values[Anum_pg_branch_relversion_brvlow - 1] =
		BranchCoordinatePGetDatum(&MyBranchLow);
	values[Anum_pg_branch_relversion_brvhigh - 1] =
		BranchCoordinatePGetDatum(&MyBranchHigh);
	values[Anum_pg_branch_relversion_brvcreated - 1] =
		Int64GetDatum(GetCurrentTimestamp());
	values[Anum_pg_branch_relversion_brvindexstate - 1] =
		CharGetDatum(has_secondary ? BRANCH_INDEX_STATE_PENDING :
					 BRANCH_INDEX_STATE_READY);
	tuple = heap_form_tuple(RelationGetDescr(versionrel), values, nulls);
	CatalogTupleInsert(versionrel, tuple);
	heap_freetuple(tuple);
	table_close(versionrel, RowExclusiveLock);
	CommandCounterIncrement();

	/* Plans on this branch must stop referring to the shared predecessor. */
	BranchRelationMapCacheValid = false;
	CacheInvalidateRelcacheByRelid(sourcerelid);
	ResetPlanCache();
	branch_reset_private_dml_cache();
	if (has_secondary)
		branch_schedule_index_worker(versionid, ownerid);
	if (copied != NULL)
		*copied = true;
	return session_lock_held;
}

void
BranchPrepareAlterTable(AlterTableStmt *stmt, LOCKMODE lockmode)
{
	(void) branch_prepare_relation_ddl(stmt->relation, stmt->missing_ok, false,
									 NULL);
}

bool
BranchPrepareIndexStmt(IndexStmt *stmt)
{
	bool		copied = false;
	bool		session_lock_held;

	/* Preserve CREATE INDEX IF NOT EXISTS as a no-op without copying a heap. */
	if (stmt->if_not_exists && stmt->idxname != NULL)
	{
		Oid			relid = RangeVarGetRelid(stmt->relation, AccessShareLock,
											 false);

		if (OidIsValid(get_relname_relid(stmt->idxname,
										  get_rel_namespace(relid))))
			return false;
	}

	session_lock_held = branch_prepare_relation_ddl(stmt->relation, false,
												 stmt->concurrent, &copied);
	/*
	 * The new target is private and unpublished, so a concurrent build adds
	 * transaction boundaries without admitting any additional useful work.
	 */
	if (copied)
		stmt->concurrent = false;
	return session_lock_held;
}

void
BranchFinishIndexStmt(bool session_lock_held)
{
	LOCKTAG		tag;

	if (!session_lock_held)
		return;
	SET_LOCKTAG_OBJECT(tag, MyDatabaseId, BranchRelationId, InvalidOid,
					   BRANCH_CONCURRENT_INDEX_LOCK_SUBID);
	(void) LockRelease(&tag, RowExclusiveLock, true);
}

int64
BranchNextRowId(void)
{
	Oid			seqid;

	seqid = get_relname_relid("pg_branch_rowid_seq", PG_CATALOG_NAMESPACE);
	if (!OidIsValid(seqid))
		elog(ERROR, "native branching row identity sequence is missing");
	return nextval_internal_parallel_leader(seqid, false);
}

Datum
pg_branch_rowid(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(BranchNextRowId());
}

static HeapTuple
lookup_branch(const char *name, bool missing_ok)
{
	HeapTuple	tuple;

	tuple = SearchSysCache1(BRANCHNAME, CStringGetDatum(name));
	if (HeapTupleIsValid(tuple) &&
		((Form_pg_branch) GETSTRUCT(tuple))->brstate != BRANCH_STATE_ACTIVE)
	{
		ReleaseSysCache(tuple);
		tuple = NULL;
	}
	if (!HeapTupleIsValid(tuple) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" does not exist", name)));
	return tuple;
}

static void
require_database_privilege(AclMode mode)
{
	AclResult	aclresult;

	aclresult = object_aclcheck(DatabaseRelationId, MyDatabaseId,
								GetUserId(), mode);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_DATABASE, get_database_name(MyDatabaseId));
}

void
BranchEnsureSession(void)
{
	bool		had_branch = OidIsValid(MyBranchId);
	bool		same_branch = false;
	HeapTuple	branchtup;
	HeapTuple	segmenttup;
	Form_pg_branch branchform;
	Form_pg_branch_segment segmentform;
	Oid			branchid = InvalidOid;

	if (!IsTransactionState() || !OidIsValid(MyDatabaseId))
		return;

	if (branch_name == NULL || branch_name[0] == '\0')
		branch_name = "main";
	branch_register_cache_callbacks();

	/* Stable sessions need no catalog or lock-manager work on read queries. */
	if (BranchSessionCacheValid && OidIsValid(MyBranchId) &&
		strcmp(BranchSessionCachedName, branch_name) == 0)
	{
		branch_schedule_pending_gc_workers();
		return;
	}

	if (OidIsValid(MyBranchId))
	{
		same_branch = strcmp(BranchSessionCachedName, branch_name) == 0;
		if (same_branch)
			branchid = MyBranchId;

		if (!same_branch && (IsTransactionBlock() || IsSubTransaction()))
			ereport(ERROR,
					(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
					 errmsg("branch cannot be changed inside a transaction block")));
	}

	if (!had_branch || !same_branch)
		require_database_privilege(ACL_CONNECT);
	if (!same_branch)
	{
		branchtup = lookup_branch(branch_name, false);
		branchid = ((Form_pg_branch) GETSTRUCT(branchtup))->oid;
		ReleaseSysCache(branchtup);
	}

	/*
	 * Readers cache this coordinate without a branch-object lock.  If a fork
	 * commits after this lookup, the old point remains a valid MVCC view for
	 * this in-flight statement; sinval refreshes the next command.  Writers
	 * acquire RowExclusiveLock and re-fetch the head in BranchAcquireLock().
	 */
	branchtup = SearchSysCache1(BRANCHOID, ObjectIdGetDatum(branchid));
	if (!HeapTupleIsValid(branchtup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" does not exist", branch_name)));
	branchform = (Form_pg_branch) GETSTRUCT(branchtup);
	if (strcmp(NameStr(branchform->brname), branch_name) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" does not exist", branch_name)));
	if (branchform->brstate != BRANCH_STATE_ACTIVE)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("branch \"%s\" is not active", branch_name)));
	if (same_branch && branchform->brhead == MyBranchSegmentId)
	{
		strlcpy(BranchSessionCachedName, branch_name,
				sizeof(BranchSessionCachedName));
		BranchSessionCacheValid = true;
		ReleaseSysCache(branchtup);
		branch_schedule_pending_gc_workers();
		return;
	}

	segmenttup = SearchSysCache1(BRANCHSEGOID,
								 ObjectIdGetDatum(branchform->brhead));
	if (!HeapTupleIsValid(segmenttup))
		elog(ERROR, "cache lookup failed for branch segment %u", branchform->brhead);
	segmentform = (Form_pg_branch_segment) GETSTRUCT(segmenttup);

	MyBranchId = branchform->oid;
	MyBranchSegmentId = branchform->brhead;
	MyBranchLow = segmentform->brseglow;
	MyBranchHigh = segmentform->brseghigh;
	MyBranchPoint = segmentform->brsegpoint;
	strlcpy(BranchSessionCachedName, branch_name,
			sizeof(BranchSessionCachedName));
	BranchSessionCacheValid = true;
	if (had_branch)
		ResetPlanCache();

	ReleaseSysCache(segmenttup);
	ReleaseSysCache(branchtup);
	branch_schedule_pending_gc_workers();
}

/* Resubmit durable DROP jobs once per backend/database after crash or pressure. */
static void
branch_schedule_pending_gc_workers(void)
{
	Relation	relation;
	TableScanDesc scan;
	TupleTableSlot *slot;

	if (BranchGcRecoveryScannedDbid == MyDatabaseId)
		return;
	BranchGcRecoveryScannedDbid = MyDatabaseId;

	relation = table_open(BranchRelationId, AccessShareLock);
	scan = table_beginscan_catalog(relation, 0, NULL);
	slot = table_slot_create(relation, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, false, NULL);
		Form_pg_branch branch = (Form_pg_branch) GETSTRUCT(tuple);

		if (branch->brstate == BRANCH_STATE_DROPPING)
			branch_schedule_gc_worker(branch->oid);
		ExecClearTuple(slot);
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	table_close(relation, AccessShareLock);
}

Datum
pg_branch_interval_low(PG_FUNCTION_ARGS)
{
	BranchCoordinate *result;

	BranchEnsureSession();
	result = palloc_object(BranchCoordinate);
	*result = MyBranchLow;
	PG_RETURN_BRANCHCOORD_P(result);
}

Datum
pg_branch_interval_high(PG_FUNCTION_ARGS)
{
	BranchCoordinate *result;

	BranchEnsureSession();
	result = palloc_object(BranchCoordinate);
	*result = MyBranchHigh;
	PG_RETURN_BRANCHCOORD_P(result);
}

Datum
pg_branch_writer(PG_FUNCTION_ARGS)
{
	BranchEnsureSession();
	PG_RETURN_OID(MyBranchSegmentId);
}

/* Serialize first-fork catalog conversion with creation of permanent tables. */
void
BranchAcquireActivationLock(LOCKMODE lockmode)
{
	LockDatabaseObject(BranchRelationId, MyDatabaseId, 1, lockmode);
}

/* Refresh a branch head after acquiring a lock that excludes child creation. */
static void
branch_refresh_session_head_locked(void)
{
	HeapTuple	branchtup;
	HeapTuple	segmenttup;
	Form_pg_branch branch;
	Form_pg_branch_segment segment;

	branchtup = SearchSysCache1(BRANCHOID, ObjectIdGetDatum(MyBranchId));
	if (!HeapTupleIsValid(branchtup))
		elog(ERROR, "cache lookup failed for branch %u", MyBranchId);
	branch = (Form_pg_branch) GETSTRUCT(branchtup);
	if (branch->brstate != BRANCH_STATE_ACTIVE)
		elog(ERROR, "branch %u is not active", MyBranchId);
	if (branch->brhead == MyBranchSegmentId)
	{
		ReleaseSysCache(branchtup);
		return;
	}

	segmenttup = SearchSysCache1(BRANCHSEGOID,
							 ObjectIdGetDatum(branch->brhead));
	if (!HeapTupleIsValid(segmenttup))
		elog(ERROR, "cache lookup failed for branch segment %u", branch->brhead);
	segment = (Form_pg_branch_segment) GETSTRUCT(segmenttup);
	MyBranchSegmentId = branch->brhead;
	MyBranchLow = segment->brseglow;
	MyBranchHigh = segment->brseghigh;
	MyBranchPoint = segment->brsegpoint;
	branch_reset_private_dml_cache();
	ResetPlanCache();
	ReleaseSysCache(segmenttup);
	ReleaseSysCache(branchtup);
}

void
BranchAcquireLock(LOCKMODE lockmode)
{
	if (lockmode == RowExclusiveLock &&
		BranchWriteLockLxid == MyProc->vxid.lxid &&
		BranchWriteLockId == MyBranchId)
		return;

	BranchEnsureSession();
	/* Reads rely on MVCC plus the physical relation lock acquired by parser. */
	if (lockmode == AccessShareLock)
		return;
	LockDatabaseObject(BranchRelationId, MyBranchId, 0, lockmode);
	branch_refresh_session_head_locked();
	if (lockmode == RowExclusiveLock)
	{
		BranchWriteLockLxid = MyProc->vxid.lxid;
		BranchWriteLockId = MyBranchId;
	}
}

/* Return palloc'd read points for every branch that can currently be used. */
List *
BranchGetActivePoints(void)
{
	Relation	branchrel;
	TableScanDesc branchscan;
	TupleTableSlot *branchslot;
	List	   *points = NIL;

	branchrel = table_open(BranchRelationId, AccessShareLock);
	branchscan = table_beginscan_catalog(branchrel, 0, NULL);
	branchslot = table_slot_create(branchrel, NULL);
	while (table_scan_getnextslot(branchscan, ForwardScanDirection, branchslot))
	{
		HeapTuple	tuple = ExecFetchSlotHeapTuple(branchslot, false, NULL);
		Form_pg_branch branch = (Form_pg_branch) GETSTRUCT(tuple);
		HeapTuple	segtuple;
		BranchCoordinate *point;

		if (branch->brstate != BRANCH_STATE_ACTIVE)
		{
			ExecClearTuple(branchslot);
			continue;
		}
		segtuple = SearchSysCache1(BRANCHSEGOID,
								 ObjectIdGetDatum(branch->brhead));
		if (!HeapTupleIsValid(segtuple))
			elog(ERROR, "cache lookup failed for branch segment %u",
				 branch->brhead);
		point = palloc_object(BranchCoordinate);
		*point = ((Form_pg_branch_segment) GETSTRUCT(segtuple))->brsegpoint;
		points = lappend(points, point);
		ReleaseSysCache(segtuple);
		ExecClearTuple(branchslot);
	}
	ExecDropSingleTupleTableSlot(branchslot);
	table_endscan(branchscan);
	table_close(branchrel, AccessShareLock);
	return points;
}

static bool
branch_get_attribute_numbers(TupleDesc desc,
							 AttrNumber attnums[BRANCH_HIDDEN_ATTRIBUTE_COUNT])
{
	static const char *const names[BRANCH_HIDDEN_ATTRIBUTE_COUNT] =
	{
		BRANCH_ROWID_ATTRIBUTE_NAME,
		BRANCH_LOW_ATTRIBUTE_NAME,
		BRANCH_HIGH_ATTRIBUTE_NAME,
		BRANCH_WRITER_ATTRIBUTE_NAME,
		BRANCH_DELETED_ATTRIBUTE_NAME
	};
	int			found = 0;

	MemSet(attnums, 0, sizeof(AttrNumber) * BRANCH_HIDDEN_ATTRIBUTE_COUNT);
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);

		if (!attr->attishidden || attr->attisdropped)
			continue;
		for (int j = 0; j < BRANCH_HIDDEN_ATTRIBUTE_COUNT; j++)
		{
			if (strcmp(NameStr(attr->attname), names[j]) == 0)
			{
				attnums[j] = attr->attnum;
				found++;
				break;
			}
		}
	}

	return found == BRANCH_HIDDEN_ATTRIBUTE_COUNT;
}

bool
BranchGetAttributeNumbers(Relation relation,
						  AttrNumber attnums[BRANCH_HIDDEN_ATTRIBUTE_COUNT])
{
	return branch_get_attribute_numbers(RelationGetDescr(relation), attnums);
}

/*
 * Construct implementation fields when a named table row type is read from
 * its user-facing text or binary representation.  Hidden fields are omitted
 * from those representations, just as they are from SELECT * and COPY.
 */
bool
BranchInitializeTupleDescMetadata(TupleDesc desc, Datum *values, bool *nulls)
{
	AttrNumber	attnums[BRANCH_HIDDEN_ATTRIBUTE_COUNT];
	BranchCoordinate *low;
	BranchCoordinate *high;

	if (!branch_get_attribute_numbers(desc, attnums))
		return false;

	BranchEnsureSession();
	low = palloc_object(BranchCoordinate);
	high = palloc_object(BranchCoordinate);
	*low = MyBranchLow;
	*high = MyBranchHigh;
	values[attnums[0] - 1] = Int64GetDatum(BranchNextRowId());
	nulls[attnums[0] - 1] = false;
	values[attnums[1] - 1] = BranchCoordinatePGetDatum(low);
	nulls[attnums[1] - 1] = false;
	values[attnums[2] - 1] = BranchCoordinatePGetDatum(high);
	nulls[attnums[2] - 1] = false;
	values[attnums[3] - 1] = ObjectIdGetDatum(MyBranchSegmentId);
	nulls[attnums[3] - 1] = false;
	values[attnums[4] - 1] = BoolGetDatum(false);
	nulls[attnums[4] - 1] = false;
	return true;
}

bool
BranchRelationIsVersioned(Relation relation)
{
	AttrNumber	attnums[BRANCH_HIDDEN_ATTRIBUTE_COUNT];

	return BranchGetAttributeNumbers(relation, attnums);
}

/* Test logical branch visibility in addition to a caller's MVCC snapshot. */
bool
BranchTupleSlotIsVisible(Relation relation, TupleTableSlot *slot)
{
	AttrNumber	attnums[BRANCH_HIDDEN_ATTRIBUTE_COUNT];
	Datum		lowdatum;
	Datum		highdatum;
	Datum		deleteddatum;
	bool		isnull;
	BranchCoordinate *low;
	BranchCoordinate *high;

	if (!BranchGetAttributeNumbers(relation, attnums))
		return true;

	lowdatum = slot_getattr(slot, attnums[1], &isnull);
	if (isnull)
		return false;
	low = DatumGetBranchCoordinateP(lowdatum);
	highdatum = slot_getattr(slot, attnums[2], &isnull);
	if (isnull)
		return false;
	high = DatumGetBranchCoordinateP(highdatum);
	deleteddatum = slot_getattr(slot, attnums[4], &isnull);
	if (isnull || DatumGetBool(deleteddatum))
		return false;

	return branchcoord_cmp_internal(low, &MyBranchPoint) <= 0 &&
		branchcoord_cmp_internal(&MyBranchPoint, high) < 0;
}

int64
BranchTupleRowId(Relation relation, TupleTableSlot *slot)
{
	AttrNumber	attnums[BRANCH_HIDDEN_ATTRIBUTE_COUNT];
	Datum		rowiddatum;
	bool		isnull;

	if (!BranchGetAttributeNumbers(relation, attnums))
		elog(ERROR, "relation \"%s\" is not versioned",
			 RelationGetRelationName(relation));
	rowiddatum = slot_getattr(slot, attnums[0], &isnull);
	if (isnull)
		elog(ERROR, "null branch row identifier in relation \"%s\"",
			 RelationGetRelationName(relation));
	return DatumGetInt64(rowiddatum);
}

/*
 * Serialize physical rewrites of one logical row.  A 32-bit lock tag is
 * deliberately used as a compact advisory namespace; hash collisions only
 * add harmless serialization and cannot compromise correctness.
 */
void
BranchLockRowIdentity(Relation relation, int64 rowid)
{
	Oid			logicalrelid = BranchLogicalRelationOid(RelationGetRelid(relation));
	uint64		unsigned_rowid = (uint64) rowid;
	uint32		rowlock;
	MemoryContext oldcontext;

	if (BranchRelationLockLxid != MyProc->vxid.lxid)
	{
		BranchRelationLockLxid = MyProc->vxid.lxid;
		BranchRelationWriteLocks = NIL;
		BranchBulkWriteLocks = NIL;
	}

	/* Bulk statements already exclude every writer of this logical table. */
	if (list_member_oid(BranchBulkWriteLocks, logicalrelid))
		return;

	/* Point writers are mutually compatible and coordinate at row granularity. */
	if (!list_member_oid(BranchRelationWriteLocks, logicalrelid))
	{
		LockDatabaseObject(BranchSegmentRelationId, logicalrelid, 1,
						   RowExclusiveLock);
		oldcontext = MemoryContextSwitchTo(TopTransactionContext);
		BranchRelationWriteLocks =
			lappend_oid(BranchRelationWriteLocks, logicalrelid);
		MemoryContextSwitchTo(oldcontext);
	}

	rowlock = (uint32) unsigned_rowid ^ (uint32) (unsigned_rowid >> 32) ^
		logicalrelid;
	LockDatabaseObject(BranchSegmentRelationId, rowlock, 0, ExclusiveLock);
}

/*
 * Install intent locks before a modifying executor starts.  Point statements
 * take mutually compatible RowExclusiveLock intent locks, then coordinate on
 * individual logical row IDs.  A high-cardinality UPDATE/DELETE/MERGE takes
 * one self-conflicting ShareRowExclusiveLock and skips the per-row locks.  It
 * both bounds shared-memory use and preserves concurrency for ordinary point
 * writes on unrelated rows and relations.
 */
void
BranchPrepareExecutorLocks(PlannedStmt *plannedstmt)
{
	Plan	   *plan = plannedstmt->planTree;
	double		estimated_rows = plan ? plan->plan_rows : 0;
	int			row_lock_threshold;
	bool		bulk = false;
	List	   *logicalrelids = NIL;
	MemoryContext oldcontext;
	int			rti = -1;
	ListCell   *lc;

	row_lock_threshold = Min(BRANCH_ROW_LOCK_ESCALATION_CAP,
							 Max(1, max_locks_per_xact /
								 BRANCH_ROW_LOCK_BUDGET_DIVISOR));
	if (plan && IsA(plan, ModifyTable))
	{
		ModifyTable *modify = (ModifyTable *) plan;

		if (outerPlan(plan))
			estimated_rows = outerPlan(plan)->plan_rows;
		bulk = modify->operation != CMD_INSERT &&
			estimated_rows >= row_lock_threshold;
	}

	while ((rti = bms_next_member(plannedstmt->resultRelationRelids, rti)) >= 0)
	{
		RangeTblEntry *rte = list_nth_node(RangeTblEntry,
											 plannedstmt->rtable, rti - 1);
		Relation	relation;
		Oid			logicalrelid;

		if (rte->rtekind != RTE_RELATION)
			continue;
		relation = table_open(rte->relid, NoLock);
		if (BranchRelationCanModifyInPlace(relation))
		{
			table_close(relation, NoLock);
			continue;
		}
		table_close(relation, NoLock);
		logicalrelid = BranchLogicalRelationOid(rte->relid);
		if (!list_member_oid(logicalrelids, logicalrelid))
			logicalrelids = lappend_oid(logicalrelids, logicalrelid);
	}
	list_sort(logicalrelids, list_oid_cmp);

	if (BranchRelationLockLxid != MyProc->vxid.lxid)
	{
		BranchRelationLockLxid = MyProc->vxid.lxid;
		BranchRelationWriteLocks = NIL;
		BranchBulkWriteLocks = NIL;
	}

	foreach(lc, logicalrelids)
	{
		Oid			logicalrelid = lfirst_oid(lc);

		if (bulk && !list_member_oid(BranchBulkWriteLocks, logicalrelid))
		{
			LockDatabaseObject(BranchSegmentRelationId, logicalrelid, 1,
							   ShareRowExclusiveLock);
			oldcontext = MemoryContextSwitchTo(TopTransactionContext);
			BranchBulkWriteLocks = lappend_oid(BranchBulkWriteLocks,
											 logicalrelid);
			if (!list_member_oid(BranchRelationWriteLocks, logicalrelid))
				BranchRelationWriteLocks =
					lappend_oid(BranchRelationWriteLocks, logicalrelid);
			MemoryContextSwitchTo(oldcontext);
		}
		else if (!list_member_oid(BranchRelationWriteLocks, logicalrelid))
		{
			LockDatabaseObject(BranchSegmentRelationId, logicalrelid, 1,
							   RowExclusiveLock);
			oldcontext = MemoryContextSwitchTo(TopTransactionContext);
			BranchRelationWriteLocks =
				lappend_oid(BranchRelationWriteLocks, logicalrelid);
			MemoryContextSwitchTo(oldcontext);
		}
	}
	list_free(logicalrelids);
}

/*
 * Find the one physical version of a logical row visible in the selected
 * database branch.  Normal installations use the internal (rowid, low)
 * btree.  The heap fallback keeps recovery and catalog-transition paths
 * diagnosable if that supporting index is temporarily unavailable.
 */
bool
BranchFindVisibleTuple(Relation relation, int64 rowid,
					   TupleTableSlot *destslot)
{
	AttrNumber	attnums[BRANCH_HIDDEN_ATTRIBUTE_COUNT];
	List	   *indexoids;
	ListCell   *lc;
	Oid			rowidindex = InvalidOid;
	bool		found = false;

	if (!BranchGetAttributeNumbers(relation, attnums))
		return false;

	indexoids = RelationGetIndexList(relation);
	foreach(lc, indexoids)
	{
		Oid			indexoid = lfirst_oid(lc);
		Relation	index = index_open(indexoid, AccessShareLock);

		if (index->rd_rel->relam == BTREE_AM_OID &&
			index->rd_index->indisvalid && index->rd_index->indisready &&
			index->rd_index->indnkeyatts >= 2 &&
			index->rd_index->indkey.values[0] == attnums[0] &&
			index->rd_index->indkey.values[1] == attnums[1] &&
			RelationGetIndexPredicate(index) == NIL &&
			RelationGetIndexExpressions(index) == NIL)
			rowidindex = indexoid;
		index_close(index, AccessShareLock);
		if (OidIsValid(rowidindex))
			break;
	}
	list_free(indexoids);

	if (OidIsValid(rowidindex))
	{
		Relation	index = index_open(rowidindex, AccessShareLock);
		IndexScanDesc scan;
		ScanKeyData key;
		TupleTableSlot *scanslot = table_slot_create(relation, NULL);

		ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_INT8EQ,
					Int64GetDatum(rowid));
		scan = index_beginscan(relation, index, SnapshotSelf, NULL, 1, 0,
							   SO_NONE);
		index_rescan(scan, &key, 1, NULL, 0);
		while (index_getnext_slot(scan, ForwardScanDirection, scanslot))
		{
			if (BranchTupleSlotIsVisible(relation, scanslot))
			{
				if (found)
					elog(ERROR, "multiple visible versions for branch row " INT64_FORMAT,
						 rowid);
				ExecCopySlot(destslot, scanslot);
				found = true;
			}
			ExecClearTuple(scanslot);
		}
		index_endscan(scan);
		ExecDropSingleTupleTableSlot(scanslot);
		index_close(index, AccessShareLock);
	}
	else
	{
		TableScanDesc scan;
		TupleTableSlot *scanslot = table_slot_create(relation, NULL);

		scan = table_beginscan(relation, SnapshotSelf, 0, NULL, SO_NONE);
		while (table_scan_getnextslot(scan, ForwardScanDirection, scanslot))
		{
			Datum		candidate;
			bool		isnull;

			candidate = slot_getattr(scanslot, attnums[0], &isnull);
			if (!isnull && DatumGetInt64(candidate) == rowid &&
				BranchTupleSlotIsVisible(relation, scanslot))
			{
				if (found)
					elog(ERROR, "multiple visible versions for branch row " INT64_FORMAT,
						 rowid);
				ExecCopySlot(destslot, scanslot);
				found = true;
			}
			ExecClearTuple(scanslot);
		}
		table_endscan(scan);
		ExecDropSingleTupleTableSlot(scanslot);
	}

	return found;
}

/*
 * Resolve a plan CTID to the current physical version in this branch while
 * holding the logical-row lock.  This must precede PostgreSQL's physical
 * tuple lock for BEFORE triggers, since interval splitting does not create a
 * CTID chain from one branch's replacement to every sibling fragment.
 */
bool
BranchResolveTupleForUpdate(Relation relation, ItemPointer tid,
							TupleTableSlot *slot, bool *relocated)
{
	bool		would_block;

	/* Preserve the historical writer compatibility used by interval rewrites. */
	return BranchResolveTupleForLock(relation, tid, slot,
									 LockTupleNoKeyExclusive,
									 LockWaitBlock,
									 relocated, &would_block);
}

/* Map PostgreSQL tuple-lock conflicts onto the heavyweight lock table. */
static LOCKMODE
branch_tuple_lockmode(LockTupleMode tuple_lockmode)
{
	switch (tuple_lockmode)
	{
		case LockTupleKeyShare:
			return AccessShareLock;
		case LockTupleShare:
			return ShareLock;
		case LockTupleNoKeyExclusive:
			return ShareUpdateExclusiveLock;
		case LockTupleExclusive:
			return AccessExclusiveLock;
	}
	elog(ERROR, "unrecognized tuple lock mode: %d", (int) tuple_lockmode);
	return NoLock;
}

/* Resolve a logical row while honoring PostgreSQL's tuple-lock semantics. */
bool
BranchResolveTupleForLock(Relation relation, ItemPointer tid,
						  TupleTableSlot *slot,
						  LockTupleMode tuple_lockmode,
						  LockWaitPolicy wait_policy,
						  bool *relocated, bool *would_block)
{
	ItemPointerData originaltid = *tid;
	int64		rowid;
	uint64		unsigned_rowid;
	uint32		rowlock;
	Oid			logicalrelid;
	LOCKMODE	lockmode;

	Assert(BranchRelationIsVersioned(relation));
	*would_block = false;
	BranchAcquireLock(AccessShareLock);
	ExecClearTuple(slot);
	if (!table_tuple_fetch_row_version(relation, tid, SnapshotAny, slot))
		return false;
	rowid = BranchTupleRowId(relation, slot);
	unsigned_rowid = (uint64) rowid;
	logicalrelid = BranchLogicalRelationOid(RelationGetRelid(relation));
	rowlock = (uint32) unsigned_rowid ^ (uint32) (unsigned_rowid >> 32) ^
		logicalrelid;
	lockmode = branch_tuple_lockmode(tuple_lockmode);
	if (wait_policy == LockWaitBlock)
		LockDatabaseObject(BranchSegmentRelationId, rowlock, 0, lockmode);
	else if (!ConditionalLockDatabaseObject(BranchSegmentRelationId, rowlock, 0,
										   lockmode))
	{
		if (wait_policy == LockWaitError)
			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("could not obtain lock on row in relation \"%s\"",
							RelationGetRelationName(relation))));
		*would_block = true;
		return false;
	}

	ExecClearTuple(slot);
	if (!BranchFindVisibleTuple(relation, rowid, slot))
		return false;
	*tid = slot->tts_tid;
	*relocated = !ItemPointerEquals(&originaltid, tid);
	return true;
}

/*
 * BEFORE INSERT triggers operate on the physical tuple descriptor and must
 * not be able to forge engine metadata.  Preserve the identity allocated for
 * the candidate row, then authoritatively stamp the selected branch after all
 * BEFORE triggers have returned.
 */
void
BranchRestoreInsertMetadata(Relation relation, TupleTableSlot *slot, int64 rowid)
{
	AttrNumber	attnums[BRANCH_HIDDEN_ATTRIBUTE_COUNT];

	if (!BranchGetAttributeNumbers(relation, attnums))
		return;
	BranchAcquireLock(RowExclusiveLock);
	ExecMaterializeSlot(slot);
	slot_getallattrs(slot);
	slot->tts_values[attnums[0] - 1] = Int64GetDatum(rowid);
	slot->tts_isnull[attnums[0] - 1] = false;
	branch_set_tuple_metadata(relation, slot, &MyBranchLow, &MyBranchHigh,
							  MyBranchSegmentId, false);
}

static void
branch_set_tuple_metadata(Relation relation, TupleTableSlot *slot,
						  const BranchCoordinate *low,
						  const BranchCoordinate *high, Oid writer,
						  bool deleted)
{
	AttrNumber	attnums[BRANCH_HIDDEN_ATTRIBUTE_COUNT];
	TupleDesc	desc = RelationGetDescr(relation);
	HeapTuple	tuple;
	BranchCoordinate *lowcopy;
	BranchCoordinate *highcopy;

	if (!BranchGetAttributeNumbers(relation, attnums))
		elog(ERROR, "versioned relation \"%s\" lost hidden attributes",
			 RelationGetRelationName(relation));
	ExecMaterializeSlot(slot);
	slot_getallattrs(slot);
	lowcopy = palloc_object(BranchCoordinate);
	highcopy = palloc_object(BranchCoordinate);
	*lowcopy = *low;
	*highcopy = *high;
	slot->tts_values[attnums[1] - 1] =
		BranchCoordinatePGetDatum(lowcopy);
	slot->tts_isnull[attnums[1] - 1] = false;
	slot->tts_values[attnums[2] - 1] =
		BranchCoordinatePGetDatum(highcopy);
	slot->tts_isnull[attnums[2] - 1] = false;
	slot->tts_values[attnums[3] - 1] = ObjectIdGetDatum(writer);
	slot->tts_isnull[attnums[3] - 1] = false;
	slot->tts_values[attnums[4] - 1] = BoolGetDatum(deleted);
	slot->tts_isnull[attnums[4] - 1] = false;
	tuple = heap_form_tuple(desc, slot->tts_values, slot->tts_isnull);
	ExecForceStoreHeapTuple(tuple, slot, true);
	slot->tts_tableOid = RelationGetRelid(relation);
}

/*
 * Implement TRUNCATE as an interval delete for a versioned heap.  The caller
 * already holds AccessExclusiveLock, so a stable MVCC scan can safely replace
 * every tuple visible in the current branch with its outside fragments and a
 * tombstone.  Statement-level TRUNCATE triggers remain managed by
 * ExecuteTruncateGuts(); row-level DELETE triggers are intentionally absent,
 * matching normal PostgreSQL TRUNCATE semantics.
 */
void
BranchTruncateRelation(Relation relation)
{
	AttrNumber	attnums[BRANCH_HIDDEN_ATTRIBUTE_COUNT];
	Snapshot	snapshot;
	TableScanDesc scan;
	TupleTableSlot *slot;
	TupleTableSlot *fragment;
	EState	   *estate;
	ResultRelInfo *relinfo;

	if (!BranchGetAttributeNumbers(relation, attnums))
		elog(ERROR, "relation \"%s\" is not versioned",
			 RelationGetRelationName(relation));
	BranchAcquireLock(RowExclusiveLock);

	estate = CreateExecutorState();
	estate->es_output_cid = GetCurrentCommandId(true);
	relinfo = palloc0_object(ResultRelInfo);
	InitResultRelInfo(relinfo, relation, 0, NULL, 0);
	ExecOpenIndices(relinfo, false);

	snapshot = RegisterSnapshot(GetLatestSnapshot());
	scan = table_beginscan(relation, snapshot, 0, NULL, SO_NONE);
	slot = table_slot_create(relation, NULL);
	fragment = table_slot_create(relation, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		Datum		lowdatum;
		Datum		highdatum;
		Datum		writerdatum;
		bool		isnull;
		BranchCoordinate oldlow;
		BranchCoordinate oldhigh;
		Oid			oldwriter;

		if (!BranchTupleSlotIsVisible(relation, slot))
		{
			ExecClearTuple(slot);
			continue;
		}
		lowdatum = slot_getattr(slot, attnums[1], &isnull);
		Assert(!isnull);
		oldlow = *DatumGetBranchCoordinateP(lowdatum);
		highdatum = slot_getattr(slot, attnums[2], &isnull);
		Assert(!isnull);
		oldhigh = *DatumGetBranchCoordinateP(highdatum);
		writerdatum = slot_getattr(slot, attnums[3], &isnull);
		Assert(!isnull);
		oldwriter = DatumGetObjectId(writerdatum);

		simple_table_tuple_delete(relation, &slot->tts_tid, snapshot);
		if (branchcoord_cmp_internal(&oldlow, &MyBranchLow) < 0)
		{
			ExecCopySlot(fragment, slot);
			branch_set_tuple_metadata(relation, fragment, &oldlow,
								  &MyBranchLow, oldwriter, false);
			table_tuple_insert(relation, fragment, estate->es_output_cid, 0, NULL);
			if (relinfo->ri_NumIndices > 0)
				list_free(ExecInsertIndexTuples(relinfo, estate,
										EIIT_BRANCH_HISTORY, fragment,
										NIL, NULL));
			ExecClearTuple(fragment);
		}
		if (branchcoord_cmp_internal(&MyBranchHigh, &oldhigh) < 0)
		{
			ExecCopySlot(fragment, slot);
			branch_set_tuple_metadata(relation, fragment, &MyBranchHigh,
								  &oldhigh, oldwriter, false);
			table_tuple_insert(relation, fragment, estate->es_output_cid, 0, NULL);
			if (relinfo->ri_NumIndices > 0)
				list_free(ExecInsertIndexTuples(relinfo, estate,
										EIIT_BRANCH_HISTORY, fragment,
										NIL, NULL));
			ExecClearTuple(fragment);
		}
		ExecCopySlot(fragment, slot);
		branch_set_tuple_metadata(relation, fragment, &MyBranchLow,
								  &MyBranchHigh, MyBranchSegmentId, true);
		table_tuple_insert(relation, fragment, estate->es_output_cid, 0, NULL);
		if (relinfo->ri_NumIndices > 0)
			list_free(ExecInsertIndexTuples(relinfo, estate,
									EIIT_BRANCH_HISTORY, fragment, NIL, NULL));
		ExecClearTuple(fragment);
		ExecClearTuple(slot);
	}

	ExecDropSingleTupleTableSlot(fragment);
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	UnregisterSnapshot(snapshot);
	ExecCloseIndices(relinfo);
	FreeExecutorState(estate);
}

static BranchCoordinate
coordinate_midpoint(const BranchCoordinate *low, const BranchCoordinate *high)
{
	BranchCoordinate width;
	BranchCoordinate result;

	if (!branchcoord_sub(high, low, &width))
		elog(ERROR, "invalid branch interval");
	(void) branchcoord_div_u32(&width, 2);
	if (!branchcoord_add(low, &width, &result))
		elog(ERROR, "branch coordinate overflow");
	return result;
}

/*
 * Chronos's default hybrid allocator.  Known fanout uses a stable child
 * width derived from the branch's original interval, while unknown fanout
 * uses a small continuation reserve plus the harmonic policy for siblings.
 *
 * A caller may provide an exact fanout estimate through
 * branch_interval_fanout.  The estimate describes the expected topology, not
 * a hard lifetime limit: benchmark operations and branch replacement after a
 * DROP can consume the planned sibling interval.  When the planned width no
 * longer fits, we fall back to a half split of the remaining interval rather
 * than overflowing the coordinate or reducing the width to zero.
 *
 * A single-child estimate is handled as a path allocation, but it retains a
 * small continuation reserve.  That reserve is what makes a leaf
 * delete-and-recreate safe: DROP is metadata-only and the parent must still
 * have an interval in which the replacement can be published.  A 1/256
 * reserve is negligible for the 128-bit domain and leaves ample capacity for
 * the 128-level spine supported by BranchBench.
 */
static BranchCoordinate
choose_child_width(Oid branchid, Form_pg_branch branch,
					   Form_pg_branch_segment head,
					   int32 fanout)
{
	static const BranchCoordinate one = {0, 1};
	static const BranchCoordinate spine_minimum_reserve = {0, 8};
	static const uint32 spine_reserve_divisor = 256;
	/*
	 * A known fanout is only an estimate.  Keep several additional child
	 * slots after the planned setup: BranchBench times create/delete by
	 * dropping and recreating a child, and real clients may fork again after
	 * the advertised fanout has been reached.  Reserving eight slots keeps
	 * those operations in the same large coordinate interval without making
	 * the allocator dependent on benchmark-specific operation counts.
	 */
	static const uint32 known_fanout_reserve = 8;
	/* 2^56 leaves ample breadth without consuming meaningful 128-bit depth. */
	static const BranchCoordinate unknown_continuation_reserve =
		{0, UINT64CONST(1) << 56};
	static const BranchCoordinate unknown_continuation_minimum =
		{0, (UINT64CONST(1) << 56) + 2};
	BranchCoordinate initial_low = head->brseglow;
	BranchCoordinate initial_high = head->brseghigh;
	BranchCoordinate active;
	BranchCoordinate capacity;
	BranchCoordinate initial;
	BranchCoordinate width;
	Oid			parentid = head->brsegparent;

	/* Recover this branch's original capacity through its continuation chain. */
	while (OidIsValid(parentid))
	{
		HeapTuple	tuple;
		Form_pg_branch_segment parent;
		Oid			nextparent;

		tuple = SearchSysCache1(BRANCHSEGOID, ObjectIdGetDatum(parentid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for branch segment %u", parentid);
		parent = (Form_pg_branch_segment) GETSTRUCT(tuple);
		if (parent->brsegbranch != branchid)
		{
			ReleaseSysCache(tuple);
			break;
		}
		initial_low = parent->brseglow;
		initial_high = parent->brseghigh;
		nextparent = parent->brsegparent;
		ReleaseSysCache(tuple);
		parentid = nextparent;
	}

	/* Leave one coordinate for the branch point/continuation boundary. */
	if (!branchcoord_sub(&head->brseghigh, &head->brseglow, &capacity) ||
		!branchcoord_sub(&capacity, &one, &active) ||
		(active.hi == 0 && active.lo == 0))
		goto exhausted;

	if (fanout == 1)
	{
		BranchCoordinate reserve = active;

		/* Keep a reallocation slot in the parent's continuation segment. */
		(void) branchcoord_div_u32(&reserve, spine_reserve_divisor);
		if (branchcoord_cmp_internal(&reserve,
								 &spine_minimum_reserve) < 0)
			reserve = spine_minimum_reserve;
		if (branchcoord_cmp_internal(&reserve, &active) >= 0 ||
			!branchcoord_sub(&active, &reserve, &width))
		{
			/* Near the end of the domain, retain both sides of the split. */
			width = active;
			(void) branchcoord_div_u32(&width, 2);
		}
	}
	else if (fanout > 1)
	{
		uint32		divisor;

		if ((uint32) fanout > PG_UINT32_MAX - known_fanout_reserve - 1)
			goto exhausted;
		if (!branchcoord_sub(&initial_high, &initial_low, &capacity) ||
			!branchcoord_sub(&capacity, &one, &initial) ||
			(initial.hi == 0 && initial.lo == 0))
			goto exhausted;
		divisor = (uint32) fanout + 1 + known_fanout_reserve;
		width = initial;
		(void) branchcoord_div_u32(&width, divisor);
	}
	else
	{
		uint32		divisor;

		if (branch->brchildcount == 0 &&
			branchcoord_cmp_internal(&active,
								 &unknown_continuation_minimum) >= 0)
		{
			/* Preserve a deep path while leaving room for future siblings. */
			if (!branchcoord_sub(&active, &unknown_continuation_reserve,
								 &width))
				goto exhausted;
		}
		else
		{
			if (branch->brchildcount > PG_UINT32_MAX - 9)
				goto exhausted;
			width = active;
			divisor = 8 + (uint32) branch->brchildcount + 1;
			(void) branchcoord_div_u32(&width, divisor);
		}
	}

	/* Unknown/known fanout > 1 retains two coordinates for a continuation. */
	if (fanout != 1)
	{
		BranchCoordinate maximum;

		if (!branchcoord_sub(&active, &one, &maximum) ||
			!branchcoord_sub(&maximum, &one, &maximum))
			goto exhausted;
		if (branchcoord_cmp_internal(&width, &maximum) > 0)
		{
			/*
			 * A known fanout width is based on the original interval.  Once
			 * setup has consumed that reservation (or a deleted child has
			 * been recreated), use the remaining space rather than attempting
			 * to add a width from the old plan to the current head.
			 */
			width = active;
			(void) branchcoord_div_u32(&width, 2);
			if (branchcoord_cmp_internal(&width, &maximum) > 0)
				width = maximum;
		}
	}

	if ((width.hi == 0 && width.lo == 0) ||
		(fanout != 1 && (width.hi == 0 && width.lo < 2)))
		goto exhausted;
	return width;

exhausted:
	ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("branch interval space is exhausted")));
	pg_unreachable();
}

static void
form_segment_tuple(Relation relation, Oid oid, Oid branchid, Oid parentid,
				   const BranchCoordinate *low, const BranchCoordinate *high,
				   const BranchCoordinate *point, int32 depth, char kind)
{
	Datum		values[Natts_pg_branch_segment];
	bool		nulls[Natts_pg_branch_segment];
	HeapTuple	tuple;

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, false, sizeof(nulls));
	values[Anum_pg_branch_segment_oid - 1] = ObjectIdGetDatum(oid);
	values[Anum_pg_branch_segment_brsegbranch - 1] = ObjectIdGetDatum(branchid);
	values[Anum_pg_branch_segment_brsegparent - 1] = ObjectIdGetDatum(parentid);
	values[Anum_pg_branch_segment_brseglow - 1] =
		BranchCoordinatePGetDatum((BranchCoordinate *) low);
	values[Anum_pg_branch_segment_brseghigh - 1] =
		BranchCoordinatePGetDatum((BranchCoordinate *) high);
	values[Anum_pg_branch_segment_brsegpoint - 1] =
		BranchCoordinatePGetDatum((BranchCoordinate *) point);
	values[Anum_pg_branch_segment_brsegdepth - 1] = Int32GetDatum(depth);
	values[Anum_pg_branch_segment_brsegchildcount - 1] = Int32GetDatum(0);
	values[Anum_pg_branch_segment_brsegkind - 1] = CharGetDatum(kind);

	tuple = heap_form_tuple(RelationGetDescr(relation), values, nulls);
	CatalogTupleInsert(relation, tuple);
	heap_freetuple(tuple);
}

/* Remove all physical versions authored by a branch before its metadata. */
static void
cleanup_branch_rows(List *segmentids)
{
	Relation	classrel;
	TableScanDesc classscan;
	TupleTableSlot *classslot;
	List	   *relids = NIL;
	ListCell   *lc;

	classrel = table_open(RelationRelationId, AccessShareLock);
	classscan = table_beginscan_catalog(classrel, 0, NULL);
	classslot = table_slot_create(classrel, NULL);
	while (table_scan_getnextslot(classscan, ForwardScanDirection, classslot))
	{
		HeapTuple	tuple = ExecFetchSlotHeapTuple(classslot, false, NULL);
		Form_pg_class classform = (Form_pg_class) GETSTRUCT(tuple);

		if (classform->relkind == RELKIND_RELATION &&
			classform->relpersistence != RELPERSISTENCE_TEMP &&
			!IsCatalogNamespace(classform->relnamespace))
			relids = lappend_oid(relids, classform->oid);
		ExecClearTuple(classslot);
	}
	ExecDropSingleTupleTableSlot(classslot);
	table_endscan(classscan);
	table_close(classrel, AccessShareLock);

	foreach(lc, relids)
	{
		Oid			relid = lfirst_oid(lc);
		Relation	relation;
		AttrNumber	attnums[BRANCH_HIDDEN_ATTRIBUTE_COUNT];
		List	   *indexoids;
		ListCell   *ic;
		Oid			writerindex = InvalidOid;

		/*
		 * Concurrent drops collect independent pg_class snapshots.  A physical
		 * schema version selected above can therefore disappear while we wait
		 * for earlier relations in this list.  Once try_table_open() returns a
		 * relation, its RowExclusiveLock protects the remainder of this pass from
		 * concurrent DDL while allowing unrelated branches to keep reading and
		 * writing the shared heap.  A missing relation needs no cleanup.
		 */
		relation = try_table_open(relid, RowExclusiveLock);
		if (relation == NULL)
			continue;

		if (!BranchGetAttributeNumbers(relation, attnums))
		{
			table_close(relation, RowExclusiveLock);
			continue;
		}

		/*
		 * Activation creates (writer, rowid), making reclamation proportional
		 * to the dropped branch's writes rather than to total database size.
		 */
		indexoids = RelationGetIndexList(relation);
		foreach(ic, indexoids)
		{
			Oid			indexoid = lfirst_oid(ic);
			Relation	index = index_open(indexoid, AccessShareLock);

			if (index->rd_rel->relam == BTREE_AM_OID &&
				index->rd_index->indisvalid && index->rd_index->indisready &&
				index->rd_index->indnkeyatts >= 2 &&
				index->rd_index->indkey.values[0] == attnums[3] &&
				index->rd_index->indkey.values[1] == attnums[0] &&
				RelationGetIndexPredicate(index) == NIL &&
				RelationGetIndexExpressions(index) == NIL)
				writerindex = indexoid;
			index_close(index, AccessShareLock);
			if (OidIsValid(writerindex))
				break;
		}
		list_free(indexoids);

		if (OidIsValid(writerindex))
		{
			Relation	index = index_open(writerindex, AccessShareLock);
			TupleTableSlot *slot = table_slot_create(relation, NULL);
			ListCell   *sc;

			foreach(sc, segmentids)
			{
				Oid			segmentid = lfirst_oid(sc);
				IndexScanDesc scan;
				ScanKeyData key;

				ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(segmentid));
				scan = index_beginscan(relation, index, SnapshotSelf, NULL,
									   1, 0, SO_NONE);
				index_rescan(scan, &key, 1, NULL, 0);
				while (index_getnext_slot(scan, ForwardScanDirection, slot))
				{
					simple_table_tuple_delete(relation, &slot->tts_tid,
										  SnapshotSelf);
					ExecClearTuple(slot);
				}
				index_endscan(scan);
			}
			ExecDropSingleTupleTableSlot(slot);
			index_close(index, AccessShareLock);
		}
		else
		{
			TableScanDesc scan;
			TupleTableSlot *slot;
			Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());

			/* Keep a recovery path if an internal supporting index is damaged. */
			scan = table_beginscan(relation, snapshot, 0, NULL, SO_NONE);
			slot = table_slot_create(relation, NULL);
			while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
			{
				Datum		writerdatum;
				bool		isnull;

				writerdatum = slot_getattr(slot, attnums[3], &isnull);
				if (!isnull && list_member_oid(segmentids,
										 DatumGetObjectId(writerdatum)))
					simple_table_tuple_delete(relation, &slot->tts_tid, snapshot);
				ExecClearTuple(slot);
			}
			ExecDropSingleTupleTableSlot(slot);
			table_endscan(scan);
			UnregisterSnapshot(snapshot);
		}
		table_close(relation, RowExclusiveLock);
	}
	list_free(relids);
}

static List *
branch_child_names(Oid parentid)
{
	Relation	relation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;
	List	   *names = NIL;

	relation = table_open(BranchRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_branch_brparent, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(parentid));
	scan = systable_beginscan(relation, BranchParentIndexId, true,
							 NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_branch branch = (Form_pg_branch) GETSTRUCT(tuple);

		if (branch->brstate == BRANCH_STATE_ACTIVE)
			names = lappend(names, pstrdup(NameStr(branch->brname)));
	}
	systable_endscan(scan);
	table_close(relation, AccessShareLock);
	return names;
}

/* Drop physical schema copies owned solely by a branch being removed. */
static void
cleanup_branch_relversions(Oid branchid)
{
	Relation	relation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;
	List	   *versions = NIL;
	ListCell   *lc;

	relation = table_open(BranchRelVersionRelationId, RowExclusiveLock);
	ScanKeyInit(&key, Anum_pg_branch_relversion_brvbranch,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(branchid));
	scan = systable_beginscan(relation, BranchRelVersionBranchIndexId, true,
							 NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_branch_relversion version =
			(Form_pg_branch_relversion) GETSTRUCT(tuple);

		if (version->brvbranch == branchid)
		{
			Oid		   *pair = palloc(sizeof(Oid) * 2);

			pair[0] = version->oid;
			pair[1] = version->brvphysical;
			versions = lappend(versions, pair);
		}
	}
	systable_endscan(scan);

	foreach(lc, versions)
	{
		Oid		   *pair = lfirst(lc);
		HeapTuple	droptuple;
		ObjectAddress object;

		droptuple = SearchSysCacheCopy1(BRANCHRELVEROID,
								ObjectIdGetDatum(pair[0]));
		if (HeapTupleIsValid(droptuple))
		{
			CatalogTupleDelete(relation, &droptuple->t_self);
			heap_freetuple(droptuple);
		}
		CommandCounterIncrement();
		ObjectAddressSet(object, RelationRelationId, pair[1]);
		performDeletion(&object, DROP_CASCADE, PERFORM_DELETION_INTERNAL);
	}
	list_free_deep(versions);
	table_close(relation, RowExclusiveLock);
}

/* Finish one durable metadata tombstone after all old branch users exit. */
static void
branch_reclaim(Oid branchid)
{
	Relation	branchrel;
	Relation	segmentrel;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;
	HeapTuple	branchtup;
	List	   *segmentids = NIL;

	/* Only one worker waits out old readers; recovery resubmissions exit fast. */
	if (!ConditionalLockDatabaseObject(BranchRelationId, branchid, 2,
									ExclusiveLock))
		return;
	LockDatabaseObject(BranchRelationId, branchid, 0, AccessExclusiveLock);
	branchtup = SearchSysCache1(BRANCHOID, ObjectIdGetDatum(branchid));
	if (!HeapTupleIsValid(branchtup))
		return;
	if (((Form_pg_branch) GETSTRUCT(branchtup))->brstate !=
		BRANCH_STATE_DROPPING)
	{
		ReleaseSysCache(branchtup);
		return;
	}
	ReleaseSysCache(branchtup);

	segmentrel = table_open(BranchSegmentRelationId, RowExclusiveLock);
	ScanKeyInit(&key, Anum_pg_branch_segment_brsegbranch,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(branchid));
	scan = systable_beginscan(segmentrel, BranchSegmentBranchIndexId, true,
							 NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		segmentids = lappend_oid(segmentids,
							 ((Form_pg_branch_segment) GETSTRUCT(tuple))->oid);
	systable_endscan(scan);
	table_close(segmentrel, RowExclusiveLock);

	cleanup_branch_rows(segmentids);
	cleanup_branch_relversions(branchid);
	CommandCounterIncrement();

	segmentrel = table_open(BranchSegmentRelationId, RowExclusiveLock);
	scan = systable_beginscan(segmentrel, BranchSegmentBranchIndexId, true,
							 NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		CatalogTupleDelete(segmentrel, &tuple->t_self);
	systable_endscan(scan);
	table_close(segmentrel, RowExclusiveLock);
	list_free(segmentids);
	CommandCounterIncrement();

	branchrel = table_open(BranchRelationId, RowExclusiveLock);
	branchtup = SearchSysCacheCopy1(BRANCHOID, ObjectIdGetDatum(branchid));
	if (HeapTupleIsValid(branchtup))
	{
		CatalogTupleDelete(branchrel, &branchtup->t_self);
		heap_freetuple(branchtup);
	}
	table_close(branchrel, RowExclusiveLock);
	CacheInvalidateRelcacheAll();
}

void
BranchGcWorkerMain(Datum main_arg)
{
	BranchPendingGcWorker request;

	memcpy(&request, MyBgworkerEntry->bgw_extra, sizeof(request));
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(request.dbid, request.ownerid,
										  BGWORKER_BYPASS_ROLELOGINCHECK);
	/* This worker is already servicing the durable queue for its database. */
	BranchGcRecoveryScannedDbid = request.dbid;
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	branch_reclaim(request.branchid);
	PopActiveSnapshot();
	CommitTransactionCommand();
	proc_exit(0);
}

static void
branch_process_utility(Node *node)
{
	PlannedStmt *wrapper = makeNode(PlannedStmt);

	wrapper->commandType = CMD_UTILITY;
	wrapper->canSetTag = false;
	wrapper->utilityStmt = node;
	wrapper->planOrigin = PLAN_STMT_INTERNAL;
	ProcessUtility(wrapper, "<internal database branching activation>", false,
				   PROCESS_UTILITY_SUBCOMMAND, NULL, NULL, None_Receiver, NULL);
}

static ColumnDef *
branch_activation_column(const char *name, Oid typeoid,
						 const char *default_function)
{
	ColumnDef  *column = makeNode(ColumnDef);
	Constraint *constraint = makeNode(Constraint);

	column->colname = pstrdup(name);
	column->typeName = makeTypeNameFromOid(typeoid, -1);
	column->is_local = true;
	column->is_not_null = true;
	column->is_hidden = true;
	column->collOid = InvalidOid;
	column->location = -1;
	constraint->location = -1;
	constraint->contype = CONSTR_DEFAULT;
	if (default_function != NULL)
	{
		constraint->raw_expr = (Node *)
			makeFuncCall(SystemFuncName(pstrdup(default_function)), NIL,
						 COERCE_EXPLICIT_CALL, -1);
	}
	else
	{
		A_Const    *value = makeNode(A_Const);

		value->val.boolval.type = T_Boolean;
		value->val.boolval.boolval = false;
		value->location = -1;
		constraint->raw_expr = (Node *) value;
	}
	column->constraints = list_make1(constraint);
	return column;
}

static RangeVar *
branch_relation_rangevar(Oid relid, bool recurse)
{
	RangeVar   *relation;

	relation = makeRangeVar(get_namespace_name(get_rel_namespace(relid)),
							get_rel_name(relid), -1);
	relation->inh = recurse;
	return relation;
}

static void
branch_add_storage_columns(Oid relid)
{
	AlterTableStmt *alter = makeNode(AlterTableStmt);
	static const struct
	{
		const char *name;
		Oid			typeoid;
		const char *default_function;
	} columns[] =
	{
		{BRANCH_ROWID_ATTRIBUTE_NAME, INT8OID, "pg_branch_rowid"},
		{BRANCH_LOW_ATTRIBUTE_NAME, PG_BRANCH_COORDOID,
			"pg_branch_interval_low"},
		{BRANCH_HIGH_ATTRIBUTE_NAME, PG_BRANCH_COORDOID,
			"pg_branch_interval_high"},
		{BRANCH_WRITER_ATTRIBUTE_NAME, OIDOID, "pg_branch_writer"},
		{BRANCH_DELETED_ATTRIBUTE_NAME, BOOLOID, NULL}
	};

	alter->relation = branch_relation_rangevar(relid, true);
	alter->objtype = OBJECT_TABLE;
	for (int i = 0; i < lengthof(columns); i++)
	{
		AlterTableCmd *command = makeNode(AlterTableCmd);

		command->subtype = AT_AddColumn;
		command->def = (Node *) branch_activation_column(columns[i].name,
												 columns[i].typeoid,
												 columns[i].default_function);
		command->missing_ok = true;
		alter->cmds = lappend(alter->cmds, command);
	}
	branch_process_utility((Node *) alter);
}

static void
branch_add_storage_index(Oid relid, const char *first, const char *second)
{
	Relation	heaprel;
	char	   *oidname;
	const char *label;
	List	   *indexes;
	ListCell   *lc;
	AttrNumber	firstattnum;
	AttrNumber	secondattnum;
	Oid			tablespace;
	IndexStmt  *index = makeNode(IndexStmt);
	IndexElem  *firstelem = makeNode(IndexElem);
	IndexElem  *secondelem = makeNode(IndexElem);

	/* CREATE TABLE's branch transform may already have synthesized it. */
	heaprel = table_open(relid, AccessShareLock);
	tablespace = heaprel->rd_rel->reltablespace;
	if (!OidIsValid(tablespace))
		tablespace = MyDatabaseTableSpace;
	firstattnum = get_attnum(relid, first);
	secondattnum = get_attnum(relid, second);
	indexes = RelationGetIndexList(heaprel);
	foreach(lc, indexes)
	{
		Relation existing = index_open(lfirst_oid(lc), AccessShareLock);
		Form_pg_index definition = existing->rd_index;
		bool		matches;

		matches = existing->rd_rel->relam == BTREE_AM_OID &&
			definition->indisvalid && definition->indisready &&
			definition->indnkeyatts == 2 &&
			definition->indkey.values[0] == firstattnum &&
			definition->indkey.values[1] == secondattnum &&
			RelationGetIndexPredicate(existing) == NIL &&
			RelationGetIndexExpressions(existing) == NIL;
		index_close(existing, AccessShareLock);
		if (matches)
		{
			list_free(indexes);
			table_close(heaprel, AccessShareLock);
			return;
		}
	}
	list_free(indexes);
	table_close(heaprel, AccessShareLock);

	firstelem->name = pstrdup(first);
	firstelem->ordering = SORTBY_DEFAULT;
	firstelem->nulls_ordering = SORTBY_NULLS_DEFAULT;
	secondelem->name = pstrdup(second);
	secondelem->ordering = SORTBY_DEFAULT;
	secondelem->nulls_ordering = SORTBY_NULLS_DEFAULT;

	/*
	 * Table renames do not rename ordinary indexes.  Include the table OID in
	 * internal index names so independently created same-named tables can be
	 * moved into one schema without colliding on our implementation objects.
	 */
	oidname = psprintf("%u", relid);
	label = strcmp(first, BRANCH_ROWID_ATTRIBUTE_NAME) == 0 ?
		"rowid_low_idx" : "writer_rowid_idx";
	index->idxname = ChooseRelationName("__pg_branch", oidname, label,
									 get_rel_namespace(relid), false);
	index->relation = branch_relation_rangevar(relid, false);
	index->accessMethod = pstrdup(DEFAULT_INDEX_TYPE);
	index->tableSpace = get_tablespace_name(tablespace);
	index->indexParams = list_make2(firstelem, secondelem);
	branch_process_utility((Node *) index);
}

/*
 * A base relation created after the branch coordinate has been split cannot
 * use the full-interval shortcut in branch_physical_version_is_private().
 * When the creator is the only active branch, record the relation's exact
 * creation interval.  This proves that the heap has never contained sibling
 * rows without inspecting it.  A later fork changes the creator's bounds, so
 * the same record cannot make a formerly shared heap private again.
 */
static void
branch_record_private_base_version(Oid relid, Oid ownerid)
{
	Relation	branchrel;
	Relation	versionrel;
	TableScanDesc scan;
	TupleTableSlot *slot;
	bool		other_active = false;
	Oid			versionid;
	Datum		values[Natts_pg_branch_relversion];
	bool		nulls[Natts_pg_branch_relversion];
	HeapTuple	tuple;

	BranchEnsureSession();
	if (MyBranchLow.hi == 0 && MyBranchLow.lo == 0 &&
		MyBranchHigh.hi == PG_UINT64_MAX &&
		MyBranchHigh.lo == PG_UINT64_MAX)
		return;

	/* Serialize the proof with a fork from the creating branch. */
	BranchAcquireLock(RowExclusiveLock);
	branchrel = table_open(BranchRelationId, AccessShareLock);
	scan = table_beginscan_catalog(branchrel, 0, NULL);
	slot = table_slot_create(branchrel, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		HeapTuple	branchtuple = ExecFetchSlotHeapTuple(slot, false, NULL);
		Form_pg_branch branch = (Form_pg_branch) GETSTRUCT(branchtuple);

		if (branch->brstate == BRANCH_STATE_ACTIVE &&
			branch->oid != MyBranchId)
		{
			other_active = true;
			break;
		}
		ExecClearTuple(slot);
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	table_close(branchrel, AccessShareLock);
	if (other_active)
		return;

	versionrel = table_open(BranchRelVersionRelationId, RowExclusiveLock);
	versionid = GetNewOidWithIndex(versionrel, BranchRelVersionOidIndexId,
								   Anum_pg_branch_relversion_oid);
	MemSet(values, 0, sizeof(values));
	MemSet(nulls, false, sizeof(nulls));
	values[Anum_pg_branch_relversion_oid - 1] = ObjectIdGetDatum(versionid);
	values[Anum_pg_branch_relversion_brvlogical - 1] = ObjectIdGetDatum(relid);
	values[Anum_pg_branch_relversion_brvphysical - 1] = ObjectIdGetDatum(relid);
	values[Anum_pg_branch_relversion_brvsource - 1] = ObjectIdGetDatum(relid);
	values[Anum_pg_branch_relversion_brvbranch - 1] =
		ObjectIdGetDatum(MyBranchId);
	values[Anum_pg_branch_relversion_brvowner - 1] = ObjectIdGetDatum(ownerid);
	values[Anum_pg_branch_relversion_brvlow - 1] =
		BranchCoordinatePGetDatum(&MyBranchLow);
	values[Anum_pg_branch_relversion_brvhigh - 1] =
		BranchCoordinatePGetDatum(&MyBranchHigh);
	values[Anum_pg_branch_relversion_brvcreated - 1] =
		Int64GetDatum(GetCurrentTimestamp());
	values[Anum_pg_branch_relversion_brvindexstate - 1] =
		CharGetDatum(BRANCH_INDEX_STATE_READY);
	tuple = heap_form_tuple(RelationGetDescr(versionrel), values, nulls);
	CatalogTupleInsert(versionrel, tuple);
	heap_freetuple(tuple);
	table_close(versionrel, RowExclusiveLock);
	CommandCounterIncrement();
}

/* Add the mandatory indexes after CREATE TABLE has assigned the table OID. */
void
BranchCreateStorageIndexes(Oid relid)
{
	Relation	relation;
	char		relkind;
	Oid			ownerid;
	bool		versioned;

	if (BranchSchemaCopyDepth > 0 || !BranchDatabaseIsEnabled())
		return;

	relation = table_open(relid, AccessShareLock);
	relkind = relation->rd_rel->relkind;
	ownerid = relation->rd_rel->relowner;
	versioned = BranchRelationIsVersioned(relation);
	table_close(relation, AccessShareLock);
	if (versioned)
		branch_record_private_base_version(relid, ownerid);
	/* Partitioned roots contain no tuples; their leaf indexes do the work. */
	if (!versioned || relkind != RELKIND_RELATION)
		return;

	branch_add_storage_index(relid, BRANCH_ROWID_ATTRIBUTE_NAME,
								 BRANCH_LOW_ATTRIBUTE_NAME);
	branch_add_storage_index(relid, BRANCH_WRITER_ATTRIBUTE_NAME,
								 BRANCH_ROWID_ATTRIBUTE_NAME);
}

/* Remove the binding before its physical relation leaves pg_class. */
void
BranchForgetPhysicalRelation(Oid relid)
{
	Relation	relation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;

	if (IsBootstrapProcessingMode() || !IsTransactionState() ||
		!BranchDatabaseIsEnabled())
		return;

	relation = table_open(BranchRelVersionRelationId, RowExclusiveLock);
	ScanKeyInit(&key, Anum_pg_branch_relversion_brvphysical,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(relation, BranchRelVersionPhysicalIndexId, true,
							 NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		CatalogTupleDelete(relation, &tuple->t_self);
	systable_endscan(scan);
	table_close(relation, RowExclusiveLock);
}

/*
 * Give main a fresh head with the same interval.  This records activation
 * without consuming any branch-coordinate space.  Freshly initialized
 * databases are already active; this conversion path remains for databases
 * whose catalog still uses the root segment as the main head.
 */
static void
branch_mark_database_enabled(void)
{
	Relation	branchrel;
	Relation	segmentrel;
	HeapTuple	branchtup;
	HeapTuple	segmenttup;
	Form_pg_branch branch;
	Form_pg_branch_segment segment;
	Oid			newsegmentid;

	branchtup = SearchSysCacheCopy1(BRANCHOID,
								ObjectIdGetDatum(MAIN_BRANCH_OID));
	if (!HeapTupleIsValid(branchtup))
		elog(ERROR, "cache lookup failed for main branch");
	branch = (Form_pg_branch) GETSTRUCT(branchtup);
	if (branch->brhead != ROOT_BRANCH_SEGMENT_OID)
	{
		heap_freetuple(branchtup);
		return;
	}
	segmenttup = SearchSysCacheCopy1(BRANCHSEGOID,
								 ObjectIdGetDatum(ROOT_BRANCH_SEGMENT_OID));
	if (!HeapTupleIsValid(segmenttup))
		elog(ERROR, "cache lookup failed for root branch segment");
	segment = (Form_pg_branch_segment) GETSTRUCT(segmenttup);

	branchrel = table_open(BranchRelationId, RowExclusiveLock);
	segmentrel = table_open(BranchSegmentRelationId, RowExclusiveLock);
	newsegmentid = GetNewOidWithIndex(segmentrel, BranchSegmentOidIndexId,
									 Anum_pg_branch_segment_oid);
	form_segment_tuple(segmentrel, newsegmentid, MAIN_BRANCH_OID,
					   ROOT_BRANCH_SEGMENT_OID,
					   &segment->brseglow, &segment->brseghigh,
					   &segment->brsegpoint, segment->brsegdepth,
					   BRANCH_SEGMENT_MUTABLE);
	segment->brsegkind = BRANCH_SEGMENT_RETIRED;
	segment->brsegchildcount++;
	CatalogTupleUpdate(segmentrel, &segmenttup->t_self, segmenttup);
	branch->brhead = newsegmentid;
	CatalogTupleUpdate(branchrel, &branchtup->t_self, branchtup);

	if (MyBranchId == MAIN_BRANCH_OID)
	{
		MyBranchSegmentId = newsegmentid;
		MyBranchLow = segment->brseglow;
		MyBranchHigh = segment->brseghigh;
		MyBranchPoint = segment->brsegpoint;
	}
	heap_freetuple(segmenttup);
	heap_freetuple(branchtup);
	table_close(segmentrel, RowExclusiveLock);
	table_close(branchrel, RowExclusiveLock);
	CommandCounterIncrement();
}

static void
enable_database_branching(void)
{
	Relation	classrel;
	Relation	pubrel;
	SysScanDesc pubscan;
	TableScanDesc scan;
	TupleTableSlot *slot;
	List	   *roots = NIL;
	List	   *tables = NIL;
	List	   *partitioned = NIL;
	ListCell   *lc;

	/*
	 * Logical decoding currently exposes the physical interval rewrite.  Do
	 * not silently change an existing publication's protocol at activation;
	 * publication creation takes the shared activation lock and is rejected
	 * after activation, closing the concurrent-create race as well.
	 */
	pubrel = table_open(PublicationRelationId, AccessShareLock);
	pubscan = systable_beginscan(pubrel, InvalidOid, false, NULL, 0, NULL);
	if (HeapTupleIsValid(systable_getnext(pubscan)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot enable native branching in a database with publications"),
				 errhint("Drop the publications before creating the first branch.")));
	systable_endscan(pubscan);
	table_close(pubrel, AccessShareLock);

	classrel = table_open(RelationRelationId, AccessShareLock);
	scan = table_beginscan_catalog(classrel, 0, NULL);
	slot = table_slot_create(classrel, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, false, NULL);
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tuple);

		if ((form->relkind == RELKIND_RELATION ||
			 form->relkind == RELKIND_PARTITIONED_TABLE) &&
			form->relpersistence != RELPERSISTENCE_TEMP &&
			!IsCatalogNamespace(form->relnamespace) &&
			!IsToastNamespace(form->relnamespace))
		{
			tables = lappend_oid(tables, form->oid);
			if (!has_superclass(form->oid))
				roots = lappend_oid(roots, form->oid);
			if (form->relkind == RELKIND_PARTITIONED_TABLE)
				partitioned = lappend_oid(partitioned, form->oid);
		}
		ExecClearTuple(slot);
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	table_close(classrel, AccessShareLock);
	list_sort(tables, list_oid_cmp);

	/*
	 * Freeze the complete activation inventory before changing any table.
	 * Stable OID order minimizes deadlock risk with concurrent multi-relation
	 * DDL.  The locks remain held to transaction end, making activation appear
	 * atomic to all ordinary relation users.
	 */
	foreach(lc, tables)
	{
		Relation	relation = table_open(lfirst_oid(lc), AccessExclusiveLock);
		TupleDesc	desc = RelationGetDescr(relation);

		for (int i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(desc, i);

			if (!attr->attisdropped &&
				strncmp(NameStr(attr->attname), "__pg_branch_", 12) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_RESERVED_NAME),
						 errmsg("cannot enable native branching for relation \"%s\"",
								RelationGetRelationName(relation)),
						 errdetail("Column name \"%s\" is reserved for native branching.",
								   NameStr(attr->attname))));
		}
		table_close(relation, NoLock);
	}

	foreach(lc, roots)
		branch_add_storage_columns(lfirst_oid(lc));

	/* Partitioned indexes build and attach their leaf indexes recursively. */
	foreach(lc, partitioned)
	{
		Oid relid = lfirst_oid(lc);

		branch_add_storage_index(relid, BRANCH_ROWID_ATTRIBUTE_NAME,
								 BRANCH_LOW_ATTRIBUTE_NAME);
		branch_add_storage_index(relid, BRANCH_WRITER_ATTRIBUTE_NAME,
								 BRANCH_ROWID_ATTRIBUTE_NAME);
	}
	foreach(lc, tables)
	{
		Oid			relid = lfirst_oid(lc);

		if (get_rel_relkind(relid) != RELKIND_RELATION ||
			get_rel_relispartition(relid))
			continue;
		branch_add_storage_index(relid, BRANCH_ROWID_ATTRIBUTE_NAME,
								 BRANCH_LOW_ATTRIBUTE_NAME);
		branch_add_storage_index(relid, BRANCH_WRITER_ATTRIBUTE_NAME,
								 BRANCH_ROWID_ATTRIBUTE_NAME);
	}
	list_free(roots);
	list_free(tables);
	list_free(partitioned);
	branch_mark_database_enabled();
	branch_reset_private_dml_cache();
	BranchDatabaseEnabledKnown = false;
	BranchSessionCacheValid = false;
	BranchRelationMapCacheValid = false;
	CacheInvalidateRelcacheAll();
}

/* Explicit, idempotent activation for setup and administration. */
Datum
pg_branch_enable(PG_FUNCTION_ARGS)
{
	bool		enabled = false;

	require_database_privilege(ACL_CREATE);
	BranchEnsureSession();
	if (BranchDatabaseIsEnabled())
		PG_RETURN_BOOL(false);

	/* Only main exists before activation; serialize with its first fork. */
	BranchAcquireLock(ShareRowExclusiveLock);
	BranchAcquireActivationLock(AccessExclusiveLock);
	if (!BranchDatabaseIsEnabled())
	{
		enable_database_branching();
		enabled = true;
	}
	PG_RETURN_BOOL(enabled);
}

void
CreateBranch(CreateBranchStmt *stmt)
{
	HeapTuple	existing;
	HeapTuple	sourcetup;
	HeapTuple	segmenttup;
	Form_pg_branch source;
	Form_pg_branch_segment oldsegment;
	Relation	branchrel;
	Relation	segmentrel;
	Oid			sourceid;
	Oid			newbranchid;
	Oid			childsegmentid;
	Oid			continuationid;
	BranchCoordinate width;
	BranchCoordinate childhigh;
	BranchCoordinate childpoint;
	BranchCoordinate continuationpoint;
	Datum		values[Natts_pg_branch];
	bool		nulls[Natts_pg_branch];
	HeapTuple	newtuple;
	const char *source_name;

	/*
	 * Take this before catalog lookup establishes an xmin.  A concurrent index
	 * build holds the conflicting lock across its internal transactions and
	 * later waits for older snapshots; looking up the source branch first
	 * would create a lock/snapshot cycle with that wait.
	 */
	if (!ConditionalLockDatabaseObject(BranchRelationId, InvalidOid,
										 BRANCH_CONCURRENT_INDEX_LOCK_SUBID,
										 ShareLock))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("cannot create a branch during a concurrent index build"),
				 errhint("Retry CREATE BRANCH after the index build finishes.")));
	require_database_privilege(ACL_CREATE);
	BranchEnsureSession();

	existing = lookup_branch(stmt->branchname, true);
	if (HeapTupleIsValid(existing))
	{
		ReleaseSysCache(existing);
		if (stmt->if_not_exists)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("branch \"%s\" already exists, skipping",
							stmt->branchname)));
			return;
		}
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("branch \"%s\" already exists", stmt->branchname)));
	}

	source_name = stmt->frombranch ? stmt->frombranch : branch_name;
	sourcetup = lookup_branch(source_name, false);
	source = (Form_pg_branch) GETSTRUCT(sourcetup);
	sourceid = source->oid;
	/*
	 * This is the native replacement for Chronos's external epoch barrier.
	 * Writers hold RowExclusiveLock on their selected branch.  A
	 * ShareRowExclusiveLock waits for those writers and for another fork.
	 * Keeping creation below AccessExclusiveLock also permits existing readers
	 * to finish on their already selected, MVCC-safe point.
	 */
	LockDatabaseObject(BranchRelationId, sourceid, 0, ShareRowExclusiveLock);
	ReleaseSysCache(sourcetup);
	if (!BranchDatabaseIsEnabled())
	{
		BranchAcquireActivationLock(AccessExclusiveLock);
		if (!BranchDatabaseIsEnabled())
			enable_database_branching();
	}

	/* Re-fetch after the lock, so this fork splits the current branch head. */
	sourcetup = SearchSysCacheCopy1(BRANCHOID, ObjectIdGetDatum(sourceid));
	if (!HeapTupleIsValid(sourcetup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" does not exist", source_name)));
	source = (Form_pg_branch) GETSTRUCT(sourcetup);
	if (strcmp(NameStr(source->brname), source_name) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" does not exist", source_name)));
	segmenttup = SearchSysCacheCopy1(BRANCHSEGOID,
									 ObjectIdGetDatum(source->brhead));
	if (!HeapTupleIsValid(segmenttup))
		elog(ERROR, "cache lookup failed for branch segment %u", source->brhead);
	oldsegment = (Form_pg_branch_segment) GETSTRUCT(segmenttup);

	width = choose_child_width(source->oid, source, oldsegment,
								branch_interval_fanout);
	if (!branchcoord_add(&oldsegment->brseglow, &width, &childhigh))
		elog(ERROR, "branch coordinate overflow");
	childpoint = coordinate_midpoint(&oldsegment->brseglow, &childhigh);
	continuationpoint = coordinate_midpoint(&childhigh, &oldsegment->brseghigh);

	branchrel = table_open(BranchRelationId, RowExclusiveLock);
	segmentrel = table_open(BranchSegmentRelationId, RowExclusiveLock);
	newbranchid = GetNewOidWithIndex(branchrel, BranchOidIndexId,
									 Anum_pg_branch_oid);
	childsegmentid = GetNewOidWithIndex(segmentrel, BranchSegmentOidIndexId,
										Anum_pg_branch_segment_oid);

	form_segment_tuple(segmentrel, childsegmentid, newbranchid, source->brhead,
					   &oldsegment->brseglow, &childhigh, &childpoint,
					   oldsegment->brsegdepth + 1, BRANCH_SEGMENT_MUTABLE);
	continuationid = GetNewOidWithIndex(segmentrel, BranchSegmentOidIndexId,
										Anum_pg_branch_segment_oid);
	form_segment_tuple(segmentrel, continuationid, source->oid, source->brhead,
					   &childhigh, &oldsegment->brseghigh, &continuationpoint,
					   oldsegment->brsegdepth, BRANCH_SEGMENT_MUTABLE);

	oldsegment->brsegkind = BRANCH_SEGMENT_RETIRED;
	oldsegment->brsegchildcount += 2;
	CatalogTupleUpdate(segmentrel, &segmenttup->t_self, segmenttup);

	source->brhead = continuationid;
	source->brchildcount++;
	CatalogTupleUpdate(branchrel, &sourcetup->t_self, sourcetup);

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, false, sizeof(nulls));
	values[Anum_pg_branch_oid - 1] = ObjectIdGetDatum(newbranchid);
	values[Anum_pg_branch_brname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->branchname));
	values[Anum_pg_branch_brparent - 1] = ObjectIdGetDatum(source->oid);
	values[Anum_pg_branch_brhead - 1] = ObjectIdGetDatum(childsegmentid);
	values[Anum_pg_branch_browner - 1] = ObjectIdGetDatum(GetUserId());
	values[Anum_pg_branch_brcreated - 1] = Int64GetDatum(GetCurrentTimestamp());
	values[Anum_pg_branch_brchildcount - 1] = Int32GetDatum(0);
	values[Anum_pg_branch_brfanout - 1] = Int32GetDatum(0);
	values[Anum_pg_branch_brkind - 1] = CharGetDatum(BRANCH_KIND_MUTABLE);
	values[Anum_pg_branch_brstate - 1] = CharGetDatum(BRANCH_STATE_ACTIVE);
	newtuple = heap_form_tuple(RelationGetDescr(branchrel), values, nulls);
	CatalogTupleInsert(branchrel, newtuple);
	heap_freetuple(newtuple);

	if (source->oid == MyBranchId)
	{
		MyBranchSegmentId = continuationid;
		MyBranchLow = childhigh;
		MyBranchHigh = oldsegment->brseghigh;
		MyBranchPoint = continuationpoint;
		ResetPlanCache();
	}

	heap_freetuple(segmenttup);
	heap_freetuple(sourcetup);
	table_close(segmentrel, RowExclusiveLock);
	table_close(branchrel, RowExclusiveLock);

	/* Branch points are constants in rewritten plans. */
	branch_reset_private_dml_cache();
	BranchSessionCacheValid = false;
	BranchRelationMapCacheValid = false;
	CacheInvalidateRelcacheAll();
}

void
DropBranch(DropBranchStmt *stmt)
{
	HeapTuple	branchtup;
	Form_pg_branch branch;
	Relation	branchrel;
	Oid			branchid;
	Oid			parentid;
	List	   *children = NIL;
	ListCell   *lc;

	require_database_privilege(ACL_CREATE);
	BranchEnsureSession();
	branchtup = lookup_branch(stmt->branchname, true);
	if (!HeapTupleIsValid(branchtup))
	{
		if (stmt->missing_ok)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("branch \"%s\" does not exist, skipping",
							stmt->branchname)));
			return;
		}
		lookup_branch(stmt->branchname, false);
	}
	branch = (Form_pg_branch) GETSTRUCT(branchtup);
	branchid = branch->oid;
	parentid = branch->brparent;
	if (branchid == 9100)
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("cannot drop the main branch")));
	if (branchid == MyBranchId)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("cannot drop the current branch")));
	ReleaseSysCache(branchtup);

	/*
	 * A drop mutates both the target and its parent's child count.  Lock the
	 * parent first (parents always precede children in allocation order), then
	 * the target.  ShareRowExclusiveLock conflicts with branch writers and
	 * creators while remaining compatible with session readers, avoiding the
	 * same AccessShare-to-AccessExclusive upgrade deadlock as CREATE BRANCH.
	 * Sibling drops and a concurrent fork from the parent are serialized by the
	 * parent lock, so their brchildcount updates cannot race.
	 */
	if (OidIsValid(parentid))
		LockDatabaseObject(BranchRelationId, parentid, 0,
						   ShareRowExclusiveLock);
	LockDatabaseObject(BranchRelationId, branchid, 0, ShareRowExclusiveLock);
	branchtup = SearchSysCache1(BRANCHOID, ObjectIdGetDatum(branchid));
	if (!HeapTupleIsValid(branchtup))
	{
		if (stmt->missing_ok)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("branch \"%s\" does not exist, skipping",
							stmt->branchname)));
			return;
		}
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" does not exist", stmt->branchname)));
	}
	branch = (Form_pg_branch) GETSTRUCT(branchtup);
	Assert(parentid == branch->brparent);
	if (branch->brstate != BRANCH_STATE_ACTIVE ||
		strcmp(NameStr(branch->brname), stmt->branchname) != 0)
	{
		ReleaseSysCache(branchtup);
		if (stmt->missing_ok)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("branch \"%s\" does not exist, skipping",
							stmt->branchname)));
			return;
		}
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" does not exist", stmt->branchname)));
	}
	if (branch->brchildcount != 0 && stmt->behavior == DROP_RESTRICT)
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("branch \"%s\" has child branches", stmt->branchname),
				 errhint("Drop its children first.")));
	if (branch->brchildcount != 0)
		children = branch_child_names(branchid);
	ReleaseSysCache(branchtup);

	foreach(lc, children)
	{
		DropBranchStmt childstmt = {0};

		childstmt.type = T_DropBranchStmt;
		childstmt.branchname = lfirst(lc);
		childstmt.behavior = DROP_CASCADE;
		DropBranch(&childstmt);
		CommandCounterIncrement();
	}
	list_free_deep(children);

	branchrel = table_open(BranchRelationId, RowExclusiveLock);
	branchtup = SearchSysCacheCopy1(BRANCHOID, ObjectIdGetDatum(branchid));
	if (!HeapTupleIsValid(branchtup))
		elog(ERROR, "cache lookup failed for branch %u", branchid);
	branch = (Form_pg_branch) GETSTRUCT(branchtup);
	branch->brstate = BRANCH_STATE_DROPPING;
	/* Free the user-visible unique name while retaining a durable GC job. */
	snprintf(NameStr(branch->brname), NAMEDATALEN, "__pg_branch_gc_%u", branchid);
	CatalogTupleUpdate(branchrel, &branchtup->t_self, branchtup);
	heap_freetuple(branchtup);

	branchtup = SearchSysCacheCopy1(BRANCHOID, ObjectIdGetDatum(parentid));
	if (HeapTupleIsValid(branchtup))
	{
		branch = (Form_pg_branch) GETSTRUCT(branchtup);
		Assert(branch->brchildcount > 0);
		branch->brchildcount--;
		CatalogTupleUpdate(branchrel, &branchtup->t_self, branchtup);
		heap_freetuple(branchtup);
	}

	table_close(branchrel, RowExclusiveLock);
	branch_schedule_gc_worker(branchid);
	branch_reset_private_dml_cache();
	BranchSessionCacheValid = false;
	BranchRelationMapCacheValid = false;

	/* Make every backend discard plans containing a removed branch point. */
	CacheInvalidateRelcacheAll();
}
