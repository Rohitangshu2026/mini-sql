#include "table.h"
#include "btree.h"
#include "file_header.h"

#include<stdio.h>
#include<stdlib.h>

/* The page a new database puts its table's root on: the first after the header. */
#define NEW_ROOT_PAGE_NUM 1u

/*
 * Opens a connection to the database file and binds it to `schema`.
 *
 * A brand-new file (num_pages == 0) gets its header on page 0 and an empty
 * root leaf on page 1, marked as the root, which the split relies on to
 * recognize it. The two pages are fetched in order, since each new page must
 * be the next one after the end of the file.
 *
 * An existing file must prove it's a database this build can read before
 * anything else happens: its header is validated, and on failure the message
 * is printed and the process exits without writing a single byte, so a file
 * that isn't ours — or is from an older build — is left exactly as it was.
 * Otherwise the root page comes from the header. It never changes afterwards:
 * a root split keeps the root on its page by moving the old contents out.
 */
Table* db_open(const char* filename, Schema* schema){
    Pager* pager = pager_open(filename);

    Table* table = malloc(sizeof(Table));
    table->pager = pager;
    table->schema = schema;

    if(pager->num_pages == 0){
        void* header = pager_get_page(pager, 0);
        file_header_initialize(header, NEW_ROOT_PAGE_NUM);

        void* root_node = pager_get_page(pager, NEW_ROOT_PAGE_NUM);
        initialize_leaf_node(root_node);
        set_node_root(root_node, true);

        table->root_page_num = NEW_ROOT_PAGE_NUM;
        return table;
    }

    void* header = pager_get_page(pager, 0);
    char message[512];
    if(!file_header_validate(header, pager->num_pages, filename, message, sizeof(message))){
        printf("%s\n", message);
        exit(EXIT_FAILURE);
    }
    table->root_page_num = file_header_root_page(header);

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
