#include "table.h"
#include "btree.h"

#include<stdlib.h>

/*
 * Opens a connection to the database file and binds it to `schema`. The root is
 * always page 0 — a root split keeps it there by moving the old contents out —
 * so root_page_num never changes. A brand-new file (num_pages == 0) has no root
 * yet, so we materialize page 0 as an empty leaf and mark it as the root; the
 * split relies on that flag to recognize it. An existing file already contains
 * its root and is left untouched.
 */
Table* db_open(const char* filename, Schema* schema){
    Pager* pager = pager_open(filename);

    Table* table = malloc(sizeof(Table));
    table->pager = pager;
    table->schema = schema;
    table->root_page_num = 0;

    if(pager->num_pages == 0){
        void* root_node = pager_get_page(pager, 0);
        initialize_leaf_node(root_node);
        set_node_root(root_node, true);
    }

    return table;
}

/*
 * Closes the connection: flush every resident page to disk, then hand the pager
 * off to be closed and freed. This is also where the borrowed schema is freed,
 * since db_close is the one place that owns tearing the whole connection down.
 */
void db_close(Table* table){
    Pager* pager = table->pager;

    for(uint32_t i = 0; i < pager->num_pages; ++i){
        if(pager->pages[i] == NULL)
            continue;
        pager_flush(pager, i);
        free(pager->pages[i]);
        pager->pages[i] = NULL;
    }

    pager_close(pager);
    schema_free(table->schema);
    free(table);
}
