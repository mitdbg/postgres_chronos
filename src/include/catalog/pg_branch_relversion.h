/*-------------------------------------------------------------------------
 *
 * pg_branch_relversion.h
 *    physical relation versions used by branch-local schema changes
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_BRANCH_RELVERSION_H
#define PG_BRANCH_RELVERSION_H

#include "catalog/genbki.h"
#include "catalog/pg_branch_relversion_d.h" /* IWYU pragma: export */
#include "utils/branchcoord.h"

BEGIN_CATALOG_STRUCT

CATALOG(pg_branch_relversion,9070,BranchRelVersionRelationId) BKI_ROWTYPE_OID(9071,BranchRelVersionRelation_Rowtype_Id) BKI_SCHEMA_MACRO
{
	Oid			oid;
	Oid			brvlogical BKI_LOOKUP(pg_class);
	Oid			brvphysical BKI_LOOKUP(pg_class);
	Oid			brvsource BKI_LOOKUP(pg_class);
	Oid			brvbranch BKI_LOOKUP(pg_branch);
	Oid			brvowner BKI_LOOKUP(pg_authid);
	BranchCoordinate brvlow;
	BranchCoordinate brvhigh;
	int64		brvcreated;
	char		brvindexstate;
} FormData_pg_branch_relversion;

END_CATALOG_STRUCT

typedef FormData_pg_branch_relversion *Form_pg_branch_relversion;

DECLARE_UNIQUE_INDEX_PKEY(pg_branch_relversion_oid_index, 9072, BranchRelVersionOidIndexId, pg_branch_relversion, btree(oid oid_ops));
DECLARE_INDEX(pg_branch_relversion_logical_index, 9073, BranchRelVersionLogicalIndexId, pg_branch_relversion, btree(brvlogical oid_ops));
DECLARE_UNIQUE_INDEX(pg_branch_relversion_physical_index, 9074, BranchRelVersionPhysicalIndexId, pg_branch_relversion, btree(brvphysical oid_ops));
DECLARE_INDEX(pg_branch_relversion_branch_index, 9075, BranchRelVersionBranchIndexId, pg_branch_relversion, btree(brvbranch oid_ops));

MAKE_SYSCACHE(BRANCHRELVEROID, pg_branch_relversion_oid_index, 16);

#define BRANCH_INDEX_STATE_PENDING	'p'
#define BRANCH_INDEX_STATE_READY		'r'
#define BRANCH_INDEX_STATE_FAILED	'f'

#endif /* PG_BRANCH_RELVERSION_H */
