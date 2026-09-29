#include "table.h"
#include "btree.h"
#include "cursor.h"

#include<stdlib.h>

/*
 * Builds a table around a definition, moving its heap-owned parts over and
 * clearing them in the definition so they can't be freed twice.
 */
Table* table_create(Pager* pager, uint32_t root_page_num, TableDefinition* definition){
    Table* table = malloc(sizeof(Table));
    table->name = definition->name;
    table->schema = definition->schema;
    table->key_column_id = definition->key_column_id;
    table->root_page_num = root_page_num;
    table->sql = definition->sql;
    table->pager = pager;

    definition->name = NULL;
    definition->schema = NULL;
    definition->sql = NULL;
    return table;
}

/*
 * Inserts a row at its sorted position.
 *
 * The key column supplies the key, and table_find resolves the leaf and cell
 * where that key belongs. The duplicate check reads that leaf — the one the
 * cursor landed in — rather than the root: once the root has split, the root
 * is an internal node and no longer the leaf that holds the key.
 *
 * Because table_find returns an insertion point for a key that isn't present,
 * a cursor landing on an existing cell is the signal to inspect it: if the key
 * there matches, the key is already taken and the insert is refused. The
 * bounds check matters — the cursor can legitimately sit one past the last
 * cell, and reading a key there would be off the end of the live cells.
 *
 * There is no capacity check: a full leaf is split inside leaf_node_insert.
 */
bool table_insert(Table* table, const Record* record){
    uint32_t key = (uint32_t)record_get_int(record, table->schema, table->key_column_id);

    Cursor* cursor = table_find(table, key);
    void* leaf = pager_get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(leaf);

    if(cursor->cell_num < num_cells){
        uint32_t key_at_index = *leaf_node_key(leaf, cursor->cell_num, table->schema);
        if(key_at_index == key){
            free(cursor);
            return false;
        }
    }

    leaf_node_insert(table->pager, cursor->page_num, cursor->cell_num, key, record, table->schema);
    free(cursor);
    return true;
}

/* Frees the table's name, schema and text, then the table itself. */
void table_free(Table* table){
    if(table == NULL)
        return;
    free(table->name);
    schema_free(table->schema);
    free(table->sql);
    free(table);
}
