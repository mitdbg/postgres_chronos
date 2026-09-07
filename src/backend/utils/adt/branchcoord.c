/*-------------------------------------------------------------------------
 *
 * branchcoord.c
 *	I/O, comparison and allocator arithmetic for pg_branch_coord.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "common/hashfn.h"
#include "libpq/pqformat.h"
#include "utils/branchcoord.h"
#include "utils/fmgrprotos.h"

static bool branchcoord_mul_u32(BranchCoordinate *value, uint32 multiplier);

/*
 * Multiply using four base-2^32 limbs.  This is portable to platforms that
 * do not expose a native unsigned 128-bit C type.
 */
static bool
branchcoord_mul_u32(BranchCoordinate *value, uint32 multiplier)
{
	BranchCoordinate original = *value;
	uint32		limbs[4];
	uint64		carry = 0;

	limbs[0] = (uint32) value->lo;
	limbs[1] = (uint32) (value->lo >> 32);
	limbs[2] = (uint32) value->hi;
	limbs[3] = (uint32) (value->hi >> 32);

	for (int i = 0; i < 4; i++)
	{
		uint64		product = (uint64) limbs[i] * multiplier + carry;

		limbs[i] = (uint32) product;
		carry = product >> 32;
	}

	if (carry != 0)
	{
		*value = original;
		return false;
	}

	value->lo = ((uint64) limbs[1] << 32) | limbs[0];
	value->hi = ((uint64) limbs[3] << 32) | limbs[2];
	return true;
}

bool
branchcoord_add_u32(BranchCoordinate *value, uint32 addend)
{
	BranchCoordinate original = *value;
	uint64		oldlo = value->lo;

	value->lo += addend;
	if (value->lo < oldlo)
	{
		value->hi++;
		if (value->hi == 0)
		{
			*value = original;
			return false;
		}
	}
	return true;
}

bool
branchcoord_add(const BranchCoordinate *left,
				const BranchCoordinate *right,
				BranchCoordinate *result)
{
	uint64		low;
	uint64		high;
	uint64		carry;

	low = left->lo + right->lo;
	carry = (low < left->lo);
	high = left->hi + right->hi;
	if (high < left->hi)
		return false;
	if (carry != 0 && ++high == 0)
		return false;

	result->lo = low;
	result->hi = high;
	return true;
}

bool
branchcoord_sub(const BranchCoordinate *left,
				const BranchCoordinate *right,
				BranchCoordinate *result)
{
	if (branchcoord_cmp_internal(left, right) < 0)
		return false;

	result->lo = left->lo - right->lo;
	result->hi = left->hi - right->hi - (left->lo < right->lo);
	return true;
}

/* Divide in base 2^32 and return the remainder. */
uint32
branchcoord_div_u32(BranchCoordinate *value, uint32 divisor)
{
	uint32		limbs[4];
	uint64		remainder = 0;

	Assert(divisor != 0);
	limbs[0] = (uint32) value->lo;
	limbs[1] = (uint32) (value->lo >> 32);
	limbs[2] = (uint32) value->hi;
	limbs[3] = (uint32) (value->hi >> 32);

	for (int i = 3; i >= 0; i--)
	{
		uint64		dividend = (remainder << 32) | limbs[i];

		limbs[i] = (uint32) (dividend / divisor);
		remainder = dividend % divisor;
	}

	value->lo = ((uint64) limbs[1] << 32) | limbs[0];
	value->hi = ((uint64) limbs[3] << 32) | limbs[2];
	return (uint32) remainder;
}

int
branchcoord_cmp_internal(const BranchCoordinate *left,
						 const BranchCoordinate *right)
{
	if (left->hi < right->hi)
		return -1;
	if (left->hi > right->hi)
		return 1;
	if (left->lo < right->lo)
		return -1;
	if (left->lo > right->lo)
		return 1;
	return 0;
}

Datum
branchcoord_in(PG_FUNCTION_ARGS)
{
	const char *input = PG_GETARG_CSTRING(0);
	const char *p = input;
	BranchCoordinate *result = palloc0_object(BranchCoordinate);

	if (*p == '+')
		p++;
	if (*p == '\0')
		goto invalid;

	for (; *p != '\0'; p++)
	{
		if (*p < '0' || *p > '9' ||
			!branchcoord_mul_u32(result, 10) ||
			!branchcoord_add_u32(result, (uint32) (*p - '0')))
			goto invalid;
	}

	PG_RETURN_BRANCHCOORD_P(result);

invalid:
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type pg_branch_coord: \"%s\"",
					input)));
	PG_RETURN_NULL();
}

Datum
branchcoord_out(PG_FUNCTION_ARGS)
{
	BranchCoordinate value = *PG_GETARG_BRANCHCOORD_P(0);
	char		buf[40];
	char	   *end = buf + sizeof(buf) - 1;
	char	   *p = end;

	*end = '\0';
	do
	{
		uint32		remainder = branchcoord_div_u32(&value, 10);

		*--p = (char) ('0' + remainder);
	} while (value.hi != 0 || value.lo != 0);

	PG_RETURN_CSTRING(pstrdup(p));
}

Datum
branchcoord_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	BranchCoordinate *result = palloc_object(BranchCoordinate);

	result->hi = (uint64) pq_getmsgint64(buf);
	result->lo = (uint64) pq_getmsgint64(buf);
	PG_RETURN_BRANCHCOORD_P(result);
}

Datum
branchcoord_send(PG_FUNCTION_ARGS)
{
	const BranchCoordinate *value = PG_GETARG_BRANCHCOORD_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, (int64) value->hi);
	pq_sendint64(&buf, (int64) value->lo);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
branchcoord_cmp(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(branchcoord_cmp_internal(PG_GETARG_BRANCHCOORD_P(0),
										PG_GETARG_BRANCHCOORD_P(1)));
}

#define BRANCHCOORD_BOOL_CMP(name, op) \
Datum \
name(PG_FUNCTION_ARGS) \
{ \
	PG_RETURN_BOOL(branchcoord_cmp_internal(PG_GETARG_BRANCHCOORD_P(0), \
										 PG_GETARG_BRANCHCOORD_P(1)) op 0); \
}

BRANCHCOORD_BOOL_CMP(branchcoord_lt, <)
BRANCHCOORD_BOOL_CMP(branchcoord_le, <=)
BRANCHCOORD_BOOL_CMP(branchcoord_eq, ==)
BRANCHCOORD_BOOL_CMP(branchcoord_ne, !=)
BRANCHCOORD_BOOL_CMP(branchcoord_ge, >=)
BRANCHCOORD_BOOL_CMP(branchcoord_gt, >)

Datum
branchcoord_hash(PG_FUNCTION_ARGS)
{
	const BranchCoordinate *value = PG_GETARG_BRANCHCOORD_P(0);

	PG_RETURN_UINT32(hash_bytes((const unsigned char *) value,
								 sizeof(BranchCoordinate)));
}
