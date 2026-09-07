/*-------------------------------------------------------------------------
 *
 * pg_branch.h
 *    definition of the native database branch catalog
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_BRANCH_H
#define PG_BRANCH_H

#include "catalog/genbki.h"
#include "catalog/pg_branch_d.h" /* IWYU pragma: export */

BEGIN_CATALOG_STRUCT

CATALOG(pg_branch,9050,BranchRelationId) BKI_ROWTYPE_OID(9051,BranchRelation_Rowtype_Id) BKI_SCHEMA_MACRO
{
	Oid			oid;
	NameData	brname;
	Oid			brparent;
	Oid			brhead;
	Oid			browner BKI_LOOKUP(pg_authid);
	int64		brcreated;
	int32		brchildcount;
	int32		brfanout;
	char		brkind;
	char		brstate;
} FormData_pg_branch;

END_CATALOG_STRUCT

typedef FormData_pg_branch *Form_pg_branch;

DECLARE_UNIQUE_INDEX_PKEY(pg_branch_oid_index, 9052, BranchOidIndexId, pg_branch, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_branch_name_index, 9053, BranchNameIndexId, pg_branch, btree(brname name_ops));
DECLARE_INDEX(pg_branch_parent_index, 9054, BranchParentIndexId, pg_branch, btree(brparent oid_ops));

MAKE_SYSCACHE(BRANCHOID, pg_branch_oid_index, 16);
MAKE_SYSCACHE(BRANCHNAME, pg_branch_name_index, 16);

DECLARE_FOREIGN_KEY_OPT((brparent), pg_branch, (oid));
DECLARE_FOREIGN_KEY((brhead), pg_branch_segment, (oid));

#define BRANCH_KIND_MUTABLE	'm'
#define BRANCH_KIND_TERMINAL	't'
#define BRANCH_STATE_ACTIVE	'a'
#define BRANCH_STATE_DROPPING 'd'

#endif /* PG_BRANCH_H */
