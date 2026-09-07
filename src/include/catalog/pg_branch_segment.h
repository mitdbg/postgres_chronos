/*-------------------------------------------------------------------------
 *
 * pg_branch_segment.h
 *    definition of the native branch interval-segment catalog
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_BRANCH_SEGMENT_H
#define PG_BRANCH_SEGMENT_H

#include "catalog/genbki.h"
#include "catalog/pg_branch_segment_d.h" /* IWYU pragma: export */
#include "utils/branchcoord.h"

BEGIN_CATALOG_STRUCT

CATALOG(pg_branch_segment,9060,BranchSegmentRelationId) BKI_ROWTYPE_OID(9061,BranchSegmentRelation_Rowtype_Id) BKI_SCHEMA_MACRO
{
	Oid			oid;
	Oid			brsegbranch;
	Oid			brsegparent;
	BranchCoordinate brseglow;
	BranchCoordinate brseghigh;
	BranchCoordinate brsegpoint;
	int32		brsegdepth;
	int32		brsegchildcount;
	char		brsegkind;
} FormData_pg_branch_segment;

END_CATALOG_STRUCT

typedef FormData_pg_branch_segment *Form_pg_branch_segment;

DECLARE_UNIQUE_INDEX_PKEY(pg_branch_segment_oid_index, 9062, BranchSegmentOidIndexId, pg_branch_segment, btree(oid oid_ops));
DECLARE_INDEX(pg_branch_segment_branch_index, 9063, BranchSegmentBranchIndexId, pg_branch_segment, btree(brsegbranch oid_ops));
DECLARE_INDEX(pg_branch_segment_parent_index, 9064, BranchSegmentParentIndexId, pg_branch_segment, btree(brsegparent oid_ops));

MAKE_SYSCACHE(BRANCHSEGOID, pg_branch_segment_oid_index, 32);

DECLARE_FOREIGN_KEY((brsegbranch), pg_branch, (oid));
DECLARE_FOREIGN_KEY_OPT((brsegparent), pg_branch_segment, (oid));

#define BRANCH_SEGMENT_MUTABLE	'm'
#define BRANCH_SEGMENT_RETIRED	'r'

#endif /* PG_BRANCH_SEGMENT_H */
