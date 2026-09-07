/*-------------------------------------------------------------------------
 *
 * branchcoord.h
 *	 128-bit coordinates used by native database branches.
 *
 * Branch coordinates are deliberately unsigned.  A branch interval is a
 * half-open range [low, high), and using the full unsigned domain gives the
 * allocator enough room for both broad and deep branch trees without using
 * variable-width NUMERIC datums in every versioned tuple.
 *
 *-------------------------------------------------------------------------
 */
#ifndef BRANCHCOORD_H
#define BRANCHCOORD_H

#include "fmgr.h"

/* Stored in machine byte order; send/receive provide a stable wire format. */
typedef struct BranchCoordinate
{
	uint64		hi;
	uint64		lo;
} BranchCoordinate;

#define BranchCoordinateLessOperator 9022
#define BranchCoordinateLessEqualOperator 9024

#define DatumGetBranchCoordinateP(X) \
	((BranchCoordinate *) DatumGetPointer(X))
#define BranchCoordinatePGetDatum(X) PointerGetDatum(X)
#define PG_GETARG_BRANCHCOORD_P(n) \
	DatumGetBranchCoordinateP(PG_GETARG_DATUM(n))
#define PG_RETURN_BRANCHCOORD_P(x) \
	PG_RETURN_POINTER(x)

extern int branchcoord_cmp_internal(const BranchCoordinate *left,
									const BranchCoordinate *right);
extern bool branchcoord_add_u32(BranchCoordinate *value, uint32 addend);
extern bool branchcoord_sub(const BranchCoordinate *left,
							const BranchCoordinate *right,
							BranchCoordinate *result);
extern bool branchcoord_add(const BranchCoordinate *left,
							const BranchCoordinate *right,
							BranchCoordinate *result);
extern uint32 branchcoord_div_u32(BranchCoordinate *value, uint32 divisor);

#endif							/* BRANCHCOORD_H */
