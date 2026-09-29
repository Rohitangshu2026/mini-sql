#include "planner.h"

#include<stdio.h>

/* Keys are non-negative int32 values, so every key lies in [0, KEY_MAX]. */
#define KEY_MAX INT32_MAX

/*
 * A comparison's constant, clamped to just outside the range keys can take.
 * Any value below 0 behaves like -2 and any value above KEY_MAX like
 * KEY_MAX + 2 against every possible key, and the clamp keeps the ±1 the
 * bounds apply to it well away from overflowing.
 */
static int64_t clamp_constant(int64_t value){
    if(value < -2)
        return -2;
    if(value > (int64_t)KEY_MAX + 2)
        return (int64_t)KEY_MAX + 2;
    return value;
}

/* The operator seen from the other side: 5 < id is id > 5. */
static CompareOp flip(CompareOp op){
    switch(op){
        case COMPARE_LESS:          return COMPARE_GREATER;
        case COMPARE_LESS_EQUAL:    return COMPARE_GREATER_EQUAL;
        case COMPARE_GREATER:       return COMPARE_LESS;
        case COMPARE_GREATER_EQUAL: return COMPARE_LESS_EQUAL;
        case COMPARE_EQUAL:
        case COMPARE_NOT_EQUAL:     return op;
    }
    return op;
}

/*
 * Narrows [low, high] by one comparison, if it compares the key column with an
 * integer constant — in either order. != leaves the range alone: it rules out
 * a single key, which the row-by-row check handles.
 */
static void narrow_by_comparison(const BoundExpr* comparison, uint32_t key_column_id,
                                 int64_t* low, int64_t* high){
    const BoundOperand* left = &comparison->left;
    const BoundOperand* right = &comparison->right;
    CompareOp op;
    int64_t value;

    if(left->is_column && left->column_id == key_column_id && !right->is_column && right->type == VALUE_INT){
        op = comparison->op;
        value = clamp_constant(right->integer);
    }
    else if(right->is_column && right->column_id == key_column_id && !left->is_column && left->type == VALUE_INT){
        op = flip(comparison->op);
        value = clamp_constant(left->integer);
    }
    else
        return;

    switch(op){
        case COMPARE_EQUAL:
            if(value > *low)  *low = value;
            if(value < *high) *high = value;
            break;
        case COMPARE_GREATER:
            if(value + 1 > *low) *low = value + 1;
            break;
        case COMPARE_GREATER_EQUAL:
            if(value > *low) *low = value;
            break;
        case COMPARE_LESS:
            if(value - 1 < *high) *high = value - 1;
            break;
        case COMPARE_LESS_EQUAL:
            if(value < *high) *high = value;
            break;
        case COMPARE_NOT_EQUAL:
            break;
    }
}

/*
 * Narrows the range by every comparison that must hold for the whole
 * expression to hold: the expression itself if it's a comparison, and each of
 * its pieces if it's an AND — including ANDs nested in parentheses, which are
 * pieces of the same conjunction. OR and NOT promise nothing about any one
 * comparison, so they narrow nothing.
 */
static void narrow(const BoundExpr* expr, uint32_t key_column_id, int64_t* low, int64_t* high){
    if(expr->kind == EXPR_COMPARISON)
        narrow_by_comparison(expr, key_column_id, low, high);
    else if(expr->kind == EXPR_AND){
        for(uint32_t i = 0; i < expr->num_children; ++i)
            narrow(expr->children[i], key_column_id, low, high);
    }
}

/*
 * Starts from every possible key, [0, KEY_MAX], narrows it by the WHERE, and
 * names the result: nothing left is EMPTY, one key is POINT, nothing narrowed
 * is SCAN, and anything else is RANGE. Bounds are worked in 64 bits and only
 * stored as keys once they're known to lie in [0, KEY_MAX].
 */
Plan plan_select(const BoundExpr* where, uint32_t key_column_id){
    int64_t low = 0;
    int64_t high = KEY_MAX;
    if(where != NULL)
        narrow(where, key_column_id, &low, &high);

    Plan plan = {PLAN_SCAN, 0, 0};
    if(low > high)
        plan.kind = PLAN_EMPTY;
    else if(low == high)
        plan.kind = PLAN_POINT;
    else if(low > 0 || high < KEY_MAX)
        plan.kind = PLAN_RANGE;

    if(plan.kind == PLAN_POINT || plan.kind == PLAN_RANGE){
        plan.low = (uint32_t)low;
        plan.high = (uint32_t)high;
    }
    return plan;
}

/*
 * Describes the plan with its real bounds. A range bounded on one side only
 * shows that side, so "id >= 11" means "from 11 to the last key".
 */
void plan_describe(const Plan* plan, const Table* table, char* buffer, size_t size){
    const char* key = schema_find_column_by_id(table->schema, table->key_column_id)->name;

    switch(plan->kind){
        case PLAN_SCAN:
            snprintf(buffer, size, "SCAN %s", table->name);
            break;
        case PLAN_POINT:
            snprintf(buffer, size, "SEARCH %s USING PRIMARY KEY (%s = %u)", table->name, key, plan->low);
            break;
        case PLAN_RANGE:
            if(plan->low > 0 && plan->high < KEY_MAX)
                snprintf(buffer, size, "SEARCH %s USING PRIMARY KEY (%s >= %u AND %s <= %u)",
                         table->name, key, plan->low, key, plan->high);
            else if(plan->low > 0)
                snprintf(buffer, size, "SEARCH %s USING PRIMARY KEY (%s >= %u)", table->name, key, plan->low);
            else
                snprintf(buffer, size, "SEARCH %s USING PRIMARY KEY (%s <= %u)", table->name, key, plan->high);
            break;
        case PLAN_EMPTY:
            snprintf(buffer, size, "SEARCH %s USING PRIMARY KEY (no row can match)", table->name);
            break;
    }
}
