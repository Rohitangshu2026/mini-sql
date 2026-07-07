#ifndef TABLE_H
#define TABLE_H

#include<stdint.h>

#include "pager.h"
#include "schema.h"

typedef struct{
    Schema* schema;    /* borrowed until db_close tears the connection down */
    uint32_t num_rows;
    Pager* pager;     /* owns the page cache + backing file */
}Table;

Table* db_open(const char* filename, Schema* schema);
void db_close(Table* table);

#endif
