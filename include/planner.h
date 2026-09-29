#ifndef PLANNER_H
#define PLANNER_H

#include<stddef.h>
#include<stdint.h>

#include "expression.h"
#include "table.h"

/*
 * How a SELECT reads its table. The only index is the primary key's b-tree,
 * so the choice is between reading one key, a range of keys, nothing at all,
 * or every row.
 */
typedef enum{
    PLAN_SCAN,     /* every row, from the leftmost leaf along the leaf chain */
    PLAN_POINT,    /* the one row whose key is `low` (== `high`), if it exists */
    PLAN_RANGE,    /* keys from `low` to `high`, both inclusive, in order */
    PLAN_EMPTY     /* the conditions contradict each other: read nothing */
}PlanKind;

typedef struct{
    PlanKind kind;
    uint32_t low;     /* PLAN_POINT, PLAN_RANGE: first key to read */
    uint32_t high;    /* PLAN_POINT, PLAN_RANGE: last key to read */
}Plan;

/*
 * Chooses a plan from the conditions on the table's key column. Only the
 * comparisons joined to the rest of the WHERE by AND at its top level narrow
 * the range: anything under an OR or NOT, and anything about another column,
 * is left for the row-by-row check, which always applies the whole WHERE.
 */
Plan plan_select(const BoundExpr* where, uint32_t key_column_id);

/*
 * Describes the plan in EXPLAIN's words — "SCAN users", or "SEARCH users
 * USING PRIMARY KEY (...)" with the actual bounds — into `buffer`.
 */
void plan_describe(const Plan* plan, const Table* table, char* buffer, size_t size);

#endif
